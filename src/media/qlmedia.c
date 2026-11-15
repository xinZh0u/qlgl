/**
 * @file    qlmedia.c
 * @brief   QLMedia 核心：状态机、时钟、工作线程、事件队列。
 *
 * 这一层不认识文件格式，也不认识音频设备：
 *   - 解码交给 `QLMediaBackend`（默认 ffmpeg）；
 *   - 出声交给 `QLAudioSink`（默认 alsa；打不开就退化成静音 + 真实时钟）。
 *
 * M1 只做音频。线程模型：
 *
 *     UI 线程: ql_media_play/pause/seek/position/poll（只投递命令）
 *     工作线程: read_audio → 音量 → sink->write（阻塞）→ 累计帧数
 *     设备:     ALSA 自己的线程/中断（我们只在 write 里等）
 *
 * 时钟：`position = base_ms + (已写帧数 − 设备延迟) / 采样率`。
 * 暂停时把当前位置写进 base_ms 并清零帧计数；seek 时基点是目标位置。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <qlgl/qlgl.h>

#include "qlmedia_internal.h"

#if !QL_FEAT_MEDIA

/* ----- 关闭媒体能力：保留符号，明确报"不支持" ----- */
static const char s_unsupported[] = "media support is disabled (QL_FEAT_MEDIA=0)";

QLMedia *ql_media_open(const char *src) { (void)src; return NULL; }
void ql_media_close(QLMedia *m) { (void)m; }
QLErr ql_media_play(QLMedia *m) { (void)m; return QL_ERR_UNSUPPORTED; }
QLErr ql_media_pause(QLMedia *m) { (void)m; return QL_ERR_UNSUPPORTED; }
QLErr ql_media_seek(QLMedia *m, int64_t ms) { (void)m; (void)ms; return QL_ERR_UNSUPPORTED; }
QLErr ql_media_set_volume(QLMedia *m, int percent)
{
    (void)m; (void)percent;
    return QL_ERR_UNSUPPORTED;
}
void ql_media_set_muted(QLMedia *m, int muted) { (void)m; (void)muted; }
void ql_media_set_loop(QLMedia *m, int loop) { (void)m; (void)loop; }
int64_t ql_media_duration_ms(const QLMedia *m) { (void)m; return 0; }
int64_t ql_media_position_ms(const QLMedia *m) { (void)m; return 0; }
QLMediaState ql_media_state(const QLMedia *m) { (void)m; return QL_MEDIA_ERROR; }
int ql_media_has_audio(const QLMedia *m) { (void)m; return 0; }
int ql_media_has_video(const QLMedia *m) { (void)m; return 0; }
uint32_t ql_media_underruns(const QLMedia *m) { (void)m; return 0; }
const char *ql_media_error(const QLMedia *m)
{
    (void)m;
    return s_unsupported;
}
void ql_media_set_event_cb(QLMedia *m, ql_media_event_cb cb, void *user)
{
    (void)m; (void)cb; (void)user;
}
int ql_media_poll(QLMedia *m) { (void)m; return 1; }

void ql_media_set_backend(const QLMediaBackend *backend) { (void)backend; }
const QLMediaBackend *ql_media_backend(void) { return NULL; }
void ql_audio_set_sink(const QLAudioSink *sink) { (void)sink; }
const QLAudioSink *ql_audio_sink(void) { return NULL; }
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
const QLMediaBackend *ql_media_backend_default(void) { return NULL; }
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
const QLAudioSink *ql_audio_sink_default(void) { return NULL; }

#else /* QL_FEAT_MEDIA */

#include <pthread.h>
#include <stdlib.h>

/* ------------------------------ 默认后端/设备 ------------------------------ */

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
const QLMediaBackend *ql_media_backend_default(void)
{
#if QL_FEAT_MEDIA_FFMPEG
    return ql_media_backend_ffmpeg();
#else
    return NULL;
#endif
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
const QLAudioSink *ql_audio_sink_default(void)
{
    return ql_audio_sink_alsa();
}

static const QLMediaBackend *s_backend_override;
static const QLAudioSink    *s_sink_override;

void ql_media_set_backend(const QLMediaBackend *backend) { s_backend_override = backend; }
const QLMediaBackend *ql_media_backend(void)
{
    return (s_backend_override != NULL) ? s_backend_override : ql_media_backend_default();
}

void ql_audio_set_sink(const QLAudioSink *sink) { s_sink_override = sink; }
const QLAudioSink *ql_audio_sink(void)
{
    return (s_sink_override != NULL) ? s_sink_override : ql_audio_sink_default();
}

/* ------------------------------ 静音设备 ------------------------------ */

typedef struct {
    int rate;
    uint32_t underruns;
    int paused;
} QLNullSink;

static void null_sleep_ms(int64_t ms)
{
    if (ms <= 0)
        return;
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
}

static int null_open(void **ctx, int rate, int channels)
{
    (void)channels;
    QLNullSink *n = (QLNullSink *)ql_mem_calloc(1, sizeof(*n));
    if (n == NULL)
        return -1;
    n->rate = (rate > 0) ? rate : 48000;
    *ctx = n;
    return 0;
}

static void null_close(void *ctx) { ql_mem_free(ctx); }

static int null_write(void *ctx, const int16_t *pcm, int frames)
{
    (void)pcm;
    QLNullSink *n = (QLNullSink *)ctx;
    if (n == NULL)
        return -1;
    /* 真实时钟：按采样率睡够这些帧的时间（暂停时由调用方保证不会被调用） */
    null_sleep_ms((int64_t)frames * 1000 / (n->rate > 0 ? n->rate : 48000));
    return frames;
}

static int null_delay(void *ctx) { (void)ctx; return 0; }

static int null_pause(void *ctx, int paused)
{
    QLNullSink *n = (QLNullSink *)ctx;
    if (n != NULL)
        n->paused = paused;
    return 0;
}

static void null_flush(void *ctx) { (void)ctx; }

static uint32_t null_underruns(void *ctx)
{
    QLNullSink *n = (QLNullSink *)ctx;
    return (n != NULL) ? n->underruns : 0;
}

static const QLAudioSink s_null_sink = {
    .name = "null",
    .open = null_open,
    .close = null_close,
    .write = null_write,
    .delay_frames = null_delay,
    .pause = null_pause,
    .flush = null_flush,
    .underruns = null_underruns,
};

/* ------------------------------ 结构 ------------------------------ */

typedef struct {
    uint8_t type;      /* QLMediaEventType */
} QLMediaEvent;

struct QLMedia {
    const QLMediaBackend *be;
    void                 *be_ctx;
    const QLAudioSink    *sink;
    void                 *sink_ctx;

    pthread_t       thread;
    int             thread_started;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             quit;

    int             playing;     /* 用户希望出声 */
    int             paused;      /* 用户暂停 */
    int             loop;
    int             volume;      /* 0..100 */
    int             muted;

    int             seek_req;
    int64_t         seek_ms;
    int             flush_req;   /* 暂停时让工作线程丢掉设备缓冲 */

    QLMediaState    state;
    int64_t         duration_ms;
    int             has_audio;
    int             has_video;
    int             vw, vh;
    QLMediaVideoFrame vframes[QL_CFG_MEDIA_VIDEO_FRAMES];
    uint8_t        *vmem;
    size_t          vframe_bytes;
    QLMediaVideoFrame *latest;      /* 最新发布的帧（不含引用） */
    QLMediaVideoFrame *vpending;    /* 已解码、等 pts 的帧 */
    int             video_eof;
    uint32_t        vserial;
    int64_t         play_start_us;  /* 无音频时用系统时钟做基准 */
    int             rate;
    int             channels;

    /* 时钟：base_ms + (frames - device_delay) / rate */
    int64_t         base_ms;
    uint64_t        frames;
    uint32_t        underruns;

    QLMediaEvent    evq[QL_CFG_MEDIA_EVENTS];
    int             ev_head, ev_tail, ev_count;
    ql_media_event_cb cb;
    void             *cb_user;

    char            err[160];
};

/* ------------------------------ 事件 ------------------------------ */

static void ev_push(QLMedia *m, QLMediaEventType type)
{
    /* 调用方持锁 */
    if (m->ev_count >= QL_CFG_MEDIA_EVENTS) {
        m->ev_head = (m->ev_head + 1) % QL_CFG_MEDIA_EVENTS;   /* 丢最旧 */
        m->ev_count--;
    }
    m->evq[m->ev_tail].type = (uint8_t)type;
    m->ev_tail = (m->ev_tail + 1) % QL_CFG_MEDIA_EVENTS;
    m->ev_count++;
}

static void set_state(QLMedia *m, QLMediaState st)
{
    if (m->state != st) {
        m->state = st;
        ev_push(m, QL_MEDIA_EV_STATE);
    }
}

/* ------------------------------ 时钟 ------------------------------ */

static int64_t pos_locked(QLMedia *m)
{
    int64_t f = (int64_t)m->frames;
    if (m->sink != NULL && m->sink_ctx != NULL && m->sink->delay_frames != NULL)
        f -= m->sink->delay_frames(m->sink_ctx);
    if (f < 0)
        f = 0;
    return m->base_ms + (f * 1000) / (m->rate > 0 ? m->rate : 48000);
}

int64_t ql_media_position_ms(const QLMedia *cm)
{
    if (cm == NULL)
        return 0;
    QLMedia *m = (QLMedia *)cm;
    pthread_mutex_lock(&m->mu);
    int64_t pos;
    if (m->state == QL_MEDIA_PLAYING) {
        pos = pos_locked(m);
    } else if (m->state == QL_MEDIA_ENDED && m->duration_ms > 0) {
        pos = m->duration_ms;
    } else {
        pos = m->base_ms;
    }
    pthread_mutex_unlock(&m->mu);
    return pos;
}

static int64_t now_us_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* 音画同步用：有音频就以"耳朵听到的位置"为准，否则用系统时钟 */
static int64_t clock_ms_locked(QLMedia *m)
{
    if (m->has_audio && m->sink_ctx != NULL)
        return pos_locked(m);
    int64_t e = (now_us_mono() - m->play_start_us) / 1000;
    if (e < 0)
        e = 0;
    return m->base_ms + e;
}

static QLMediaVideoFrame *frame_free_slot(QLMedia *m)
{
    /* 调用方持锁 */
    for (int i = 0; i < QL_CFG_MEDIA_VIDEO_FRAMES; i++) {
        QLMediaVideoFrame *f = &m->vframes[i];
        if (f->px != NULL && f != m->latest && !f->in_use && f->refs == 0)
            return f;
    }
    return NULL;
}

static void frame_publish(QLMedia *m, QLMediaVideoFrame *f)
{
    pthread_mutex_lock(&m->mu);
    f->in_use = 0;
    m->latest = f;
    m->vserial++;
    pthread_mutex_unlock(&m->mu);
}

static void frame_drop(QLMedia *m, QLMediaVideoFrame *f)
{
    (void)m;
    f->in_use = 0;
}

/* 解码/呈现一帧；在 worker 里、不持锁调用 */
static void video_step(QLMedia *m)
{
    if (m->vpending != NULL) {
        pthread_mutex_lock(&m->mu);
        const int live = m->playing && !m->paused;
        const int64_t clock = clock_ms_locked(m);
        pthread_mutex_unlock(&m->mu);

        if (!live)
            return;
        const int64_t pts = m->vpending->pts_us;
        if (pts <= clock * 1000 + 5000)
            frame_publish(m, m->vpending);
        else if (pts < clock * 1000 - 80000)
            frame_drop(m, m->vpending);
        else
            return;                 /* 还早：先让音频线程/时钟走 */
        m->vpending = NULL;
    }

    if (m->vpending != NULL)
        return;

    pthread_mutex_lock(&m->mu);
    QLMediaVideoFrame *f = frame_free_slot(m);
    if (f != NULL)
        f->in_use = 1;
    pthread_mutex_unlock(&m->mu);
    if (f == NULL)
        return;                     /* 池满：等 UI 放掉旧帧 */

    int w = 0, h = 0;
    int64_t pts = 0;
    const int r = m->be->read_video(m->be_ctx, (uint8_t *)f->px,
                                    (int)m->vframe_bytes, &w, &h, &pts);
    if (r <= 0) {
        f->in_use = 0;
        if (r < 0) {
            pthread_mutex_lock(&m->mu);
            snprintf(m->err, sizeof(m->err), "video decode error");
            set_state(m, QL_MEDIA_ERROR);
            ev_push(m, QL_MEDIA_EV_ERROR);
            m->playing = 0;
            pthread_mutex_unlock(&m->mu);
        } else {
            m->video_eof = 1;
            if (!m->has_audio) {
                pthread_mutex_lock(&m->mu);
                m->base_ms = m->duration_ms;
                m->playing = 0;
                set_state(m, QL_MEDIA_ENDED);
                ev_push(m, QL_MEDIA_EV_ENDED);
                pthread_mutex_unlock(&m->mu);
            }
        }
        return;
    }

    f->w = w;
    f->h = h;
    f->stride = (uint32_t)w * 4u;
    f->pts_us = pts;
    f->format = QL_IMAGE_RGBA8888;
    m->vpending = f;
}

const QLMediaVideoFrame *ql_media_acquire_frame(QLMedia *m)
{
    if (m == NULL)
        return NULL;
    pthread_mutex_lock(&m->mu);
    QLMediaVideoFrame *f = m->latest;
    if (f != NULL && f->px != NULL)
        f->refs++;
    else
        f = NULL;
    pthread_mutex_unlock(&m->mu);
    return f;
}

void ql_media_frame_release(const QLMediaVideoFrame *cf)
{
    if (cf == NULL)
        return;
    QLMediaVideoFrame *f = (QLMediaVideoFrame *)cf;
    QLMedia *m = (QLMedia *)f->owner;
    if (m == NULL)
        return;
    pthread_mutex_lock(&m->mu);
    if (f->refs > 0)
        f->refs--;
    pthread_mutex_unlock(&m->mu);
}

uint32_t ql_media_video_serial(const QLMedia *cm)
{
    if (cm == NULL)
        return 0;
    QLMedia *m = (QLMedia *)cm;
    pthread_mutex_lock(&m->mu);
    const uint32_t s = m->vserial;
    pthread_mutex_unlock(&m->mu);
    return s;
}

int ql_media_video_size(const QLMedia *m, ql_coord_t *w, ql_coord_t *h)
{
    if (m == NULL || !m->has_video)
        return 0;
    if (w != NULL) *w = (ql_coord_t)m->vw;
    if (h != NULL) *h = (ql_coord_t)m->vh;
    return 1;
}

int64_t ql_media_duration_ms(const QLMedia *m) { return (m != NULL) ? m->duration_ms : 0; }
QLMediaState ql_media_state(const QLMedia *m) { return (m != NULL) ? m->state : QL_MEDIA_ERROR; }
int ql_media_has_audio(const QLMedia *m) { return (m != NULL) ? m->has_audio : 0; }
int ql_media_has_video(const QLMedia *m) { return (m != NULL) ? m->has_video : 0; }
uint32_t ql_media_underruns(const QLMedia *m) { return (m != NULL) ? m->underruns : 0; }
const char *ql_media_error(const QLMedia *m) { return (m != NULL) ? m->err : ""; }

/* ------------------------------ 工作线程 ------------------------------ */

static void apply_volume(int16_t *pcm, int samples, int volume, int muted)
{
    if (muted || volume <= 0) {
        memset(pcm, 0, (size_t)samples * sizeof(int16_t));
        return;
    }
    if (volume >= 100)
        return;
    for (int i = 0; i < samples; i++) {
        int32_t v = ((int32_t)pcm[i] * volume) / 100;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        pcm[i] = (int16_t)v;
    }
}

static void *media_thread(void *arg)
{
    QLMedia *m = (QLMedia *)arg;
    int16_t *buf = (int16_t *)ql_mem_alloc((size_t)QL_CFG_MEDIA_FRAMES *
                                           (size_t)m->channels * sizeof(int16_t));
    if (buf == NULL) {
        pthread_mutex_lock(&m->mu);
        snprintf(m->err, sizeof(m->err), "out of memory (audio buffer)");
        set_state(m, QL_MEDIA_ERROR);
        ev_push(m, QL_MEDIA_EV_ERROR);
        pthread_mutex_unlock(&m->mu);
        return NULL;
    }

    for (;;) {
        pthread_mutex_lock(&m->mu);
        while (!m->quit && !m->seek_req && !m->flush_req &&
               (!m->playing || m->paused))
            pthread_cond_wait(&m->cv, &m->mu);
        if (m->quit)
            break;

        if (m->flush_req) {
            m->flush_req = 0;
            pthread_mutex_unlock(&m->mu);
            if (m->sink->flush != NULL)
                m->sink->flush(m->sink_ctx);
            continue;                 /* 回到循环顶部重新加锁 */
        }

        const int do_seek = m->seek_req;
        const int64_t target = m->seek_ms;
        const int do_loop = m->loop;
        m->seek_req = 0;
        pthread_mutex_unlock(&m->mu);

        if (do_seek && m->be->seek != NULL) {
            (void)m->be->seek(m->be_ctx, target);
            if (m->sink->flush != NULL)
                m->sink->flush(m->sink_ctx);
            if (m->vpending != NULL) {
                m->vpending->in_use = 0;
                m->vpending = NULL;
            }
            m->video_eof = 0;
            pthread_mutex_lock(&m->mu);
            m->play_start_us = now_us_mono();
            pthread_mutex_unlock(&m->mu);
        }

        if (m->has_video)
            video_step(m);

        if (!m->has_audio) {
            /* 纯视频：没有阻塞写设备，靠 5ms 的节拍等下一帧 */
            struct timespec ts = { 0, 5 * 1000000L };
            nanosleep(&ts, NULL);
            continue;
        }

        const int want = m->has_video ? (QL_CFG_MEDIA_FRAMES / 2)
                                      : QL_CFG_MEDIA_FRAMES;
        const int got = m->be->read_audio(m->be_ctx, buf, want);
        if (got > 0) {
            pthread_mutex_lock(&m->mu);
            const int vol = m->volume;
            const int muted = m->muted;
            pthread_mutex_unlock(&m->mu);

            apply_volume(buf, got * m->channels, vol, muted);
            const int wrote = m->sink->write(m->sink_ctx, buf, got);

            pthread_mutex_lock(&m->mu);
            if (wrote > 0)
                m->frames += (uint64_t)wrote;
            else
                m->underruns++;
            pthread_mutex_unlock(&m->mu);
        } else if (got == 0) {
            /* EOF */
            if (do_loop) {
                if (m->be->seek != NULL)
                    (void)m->be->seek(m->be_ctx, 0);
                if (m->sink->flush != NULL)
                    m->sink->flush(m->sink_ctx);
                pthread_mutex_lock(&m->mu);
                m->base_ms = 0;
                m->frames = 0;
                pthread_mutex_unlock(&m->mu);
            } else {
                pthread_mutex_lock(&m->mu);
                m->base_ms = pos_locked(m);
                m->frames = 0;
                m->playing = 0;
                set_state(m, QL_MEDIA_ENDED);
                ev_push(m, QL_MEDIA_EV_ENDED);
                pthread_mutex_unlock(&m->mu);
            }
        } else {
            pthread_mutex_lock(&m->mu);
            snprintf(m->err, sizeof(m->err), "decode error");
            set_state(m, QL_MEDIA_ERROR);
            ev_push(m, QL_MEDIA_EV_ERROR);
            m->playing = 0;
            pthread_mutex_unlock(&m->mu);
        }
    }

    ql_mem_free(buf);
    return NULL;
}

static int ensure_thread(QLMedia *m)
{
    if (m->thread_started)
        return 1;
    if (pthread_create(&m->thread, NULL, media_thread, m) != 0) {
        snprintf(m->err, sizeof(m->err), "cannot create media thread");
        return 0;
    }
    m->thread_started = 1;
    return 1;
}

/* ------------------------------ 公开 API ------------------------------ */

QLMedia *ql_media_open(const char *src)
{
    if (src == NULL || src[0] == '\0')
        return NULL;
    const QLMediaBackend *be = ql_media_backend();
    if (be == NULL || be->open == NULL) {
        QL_LOGE("media: no decode backend (build with QLGL_BUILD_MEDIA / FFmpeg)");
        return NULL;
    }

    QLMedia *m = (QLMedia *)ql_mem_calloc(1, sizeof(*m));
    if (m == NULL)
        return NULL;
    m->be = be;
    m->sink = ql_audio_sink();
    m->volume = 100;
    m->state = QL_MEDIA_OPENING;
    pthread_mutex_init(&m->mu, NULL);
    pthread_cond_init(&m->cv, NULL);

    m->be_ctx = be->open(src, m->err, sizeof(m->err));
    if (m->be_ctx == NULL) {
        if (m->err[0] == '\0')
            snprintf(m->err, sizeof(m->err), "cannot open \"%s\"", src);
        QL_LOGE("media: %s", m->err);
        pthread_cond_destroy(&m->cv);
        pthread_mutex_destroy(&m->mu);
        ql_mem_free(m);
        return NULL;
    }

    m->duration_ms = (be->duration_ms != NULL) ? be->duration_ms(m->be_ctx) : 0;
    m->has_audio = (be->has_audio != NULL) ? be->has_audio(m->be_ctx) : 1;
    m->has_video = (be->has_video != NULL) ? be->has_video(m->be_ctx) : 0;
    m->rate = (be->rate != NULL) ? be->rate(m->be_ctx) : 48000;
    if (m->rate <= 0)
        m->rate = 48000;
    m->channels = (be->channels != NULL) ? be->channels(m->be_ctx) : 2;
    if (m->channels <= 0 || m->channels > 8)
        m->channels = 2;

    /* 视频帧池：打开时就按解码尺寸分配，播放期零分配 */
    if (m->has_video) {
        int vw = 0, vh = 0;
        if (be->video_size == NULL || be->read_video == NULL ||
            be->video_size(m->be_ctx, &vw, &vh) != 0 || vw <= 0 || vh <= 0 ||
            vw > QL_CFG_MEDIA_MAX_DIM || vh > QL_CFG_MEDIA_MAX_DIM) {
            QL_LOGW("media: video %dx%d rejected (limit %d)", vw, vh,
                    QL_CFG_MEDIA_MAX_DIM);
            m->has_video = 0;
        } else {
            m->vw = vw;
            m->vh = vh;
            m->vframe_bytes = (size_t)vw * (size_t)vh * 4u;
            m->vmem = (uint8_t *)ql_mem_alloc(m->vframe_bytes *
                                              QL_CFG_MEDIA_VIDEO_FRAMES);
            if (m->vmem == NULL) {
                QL_LOGW("media: no memory for video frames (%ux%u)", vw, vh);
                m->has_video = 0;
            } else {
                for (int i = 0; i < QL_CFG_MEDIA_VIDEO_FRAMES; i++) {
                    QLMediaVideoFrame *f = &m->vframes[i];
                    memset(f, 0, sizeof(*f));
                    f->px = m->vmem + (size_t)i * m->vframe_bytes;
                    f->w = vw;
                    f->h = vh;
                    f->stride = (uint32_t)vw * 4u;
                    f->format = QL_IMAGE_RGBA8888;
                    f->owner = m;
                }
            }
        }
    }

    m->state = QL_MEDIA_READY;
    ev_push(m, QL_MEDIA_EV_READY);
    return m;
}

void ql_media_close(QLMedia *m)
{
    if (m == NULL)
        return;
    if (m->thread_started) {
        pthread_mutex_lock(&m->mu);
        m->quit = 1;
        m->playing = 0;
        pthread_cond_broadcast(&m->cv);
        pthread_mutex_unlock(&m->mu);
        pthread_join(m->thread, NULL);
        m->thread_started = 0;
    }
    if (m->sink != NULL && m->sink_ctx != NULL)
        m->sink->close(m->sink_ctx);
    if (m->be != NULL && m->be->close != NULL)
        m->be->close(m->be_ctx);
    ql_mem_free(m->vmem);
    pthread_cond_destroy(&m->cv);
    pthread_mutex_destroy(&m->mu);
    ql_mem_free(m);
}

QLErr ql_media_play(QLMedia *m)
{
    if (m == NULL)
        return QL_ERR_INVAL;
    if (m->state == QL_MEDIA_ERROR)
        return QL_ERR_IO;

    pthread_mutex_lock(&m->mu);

    if (m->sink_ctx == NULL) {
        if (m->sink == NULL)
            m->sink = &s_null_sink;
        if (m->sink->open(&m->sink_ctx, m->rate, m->channels) != 0) {
            QL_LOGW("media: cannot open audio sink \"%s\", falling back to silence",
                    (m->sink != NULL && m->sink->name != NULL) ? m->sink->name : "?");
            m->sink = &s_null_sink;
            if (m->sink->open(&m->sink_ctx, m->rate, m->channels) != 0) {
                snprintf(m->err, sizeof(m->err), "cannot open any audio sink");
                set_state(m, QL_MEDIA_ERROR);
                ev_push(m, QL_MEDIA_EV_ERROR);
                pthread_mutex_unlock(&m->mu);
                return QL_ERR_IO;
            }
        }
    }

    if (!ensure_thread(m)) {
        set_state(m, QL_MEDIA_ERROR);
        ev_push(m, QL_MEDIA_EV_ERROR);
        pthread_mutex_unlock(&m->mu);
        return QL_ERR_NOMEM;
    }

    if (m->state == QL_MEDIA_ENDED) {
        m->seek_req = 1;
        m->seek_ms = 0;
        m->base_ms = 0;
        m->frames = 0;
    }
    /* 从暂停恢复：先 seek 回暂停点，避免设备里残留的缓冲造成跳帧 */
    if (m->paused) {
        m->seek_req = 1;
        m->seek_ms = m->base_ms;
    }

    m->playing = 1;
    m->paused = 0;
    m->play_start_us = now_us_mono();
    set_state(m, QL_MEDIA_PLAYING);
    pthread_cond_broadcast(&m->cv);
    pthread_mutex_unlock(&m->mu);
    return QL_OK;
}

QLErr ql_media_pause(QLMedia *m)
{
    if (m == NULL)
        return QL_ERR_INVAL;
    pthread_mutex_lock(&m->mu);
    if (m->state != QL_MEDIA_PLAYING) {
        pthread_mutex_unlock(&m->mu);
        return QL_OK;
    }

    const int64_t pos = pos_locked(m);
    m->base_ms = pos;
    m->frames = 0;
    m->paused = 1;
    set_state(m, QL_MEDIA_PAUSED);
    m->flush_req = 1;                 /* 由工作线程去丢设备缓冲（不跨线程碰 ALSA） */
    pthread_cond_broadcast(&m->cv);
    pthread_mutex_unlock(&m->mu);
    return QL_OK;
}

QLErr ql_media_seek(QLMedia *m, int64_t ms)
{
    if (m == NULL || ms < 0)
        return QL_ERR_INVAL;
    if (m->be == NULL || m->be->seek == NULL)
        return QL_ERR_UNSUPPORTED;

    pthread_mutex_lock(&m->mu);
    if (m->duration_ms > 0 && ms > m->duration_ms)
        ms = m->duration_ms;
    m->seek_req = 1;
    m->seek_ms = ms;
    m->base_ms = ms;
    m->frames = 0;
    if (m->state == QL_MEDIA_ENDED)
        set_state(m, m->playing ? QL_MEDIA_PLAYING : QL_MEDIA_READY);
    pthread_cond_broadcast(&m->cv);
    pthread_mutex_unlock(&m->mu);
    return QL_OK;
}

QLErr ql_media_set_volume(QLMedia *m, int percent)
{
    if (m == NULL)
        return QL_ERR_INVAL;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    pthread_mutex_lock(&m->mu);
    m->volume = percent;
    pthread_mutex_unlock(&m->mu);
    return QL_OK;
}

void ql_media_set_muted(QLMedia *m, int muted)
{
    if (m == NULL)
        return;
    pthread_mutex_lock(&m->mu);
    m->muted = muted ? 1 : 0;
    pthread_mutex_unlock(&m->mu);
}

void ql_media_set_loop(QLMedia *m, int loop)
{
    if (m == NULL)
        return;
    pthread_mutex_lock(&m->mu);
    m->loop = loop ? 1 : 0;
    pthread_mutex_unlock(&m->mu);
}

void ql_media_set_event_cb(QLMedia *m, ql_media_event_cb cb, void *user)
{
    if (m == NULL)
        return;
    pthread_mutex_lock(&m->mu);
    m->cb = cb;
    m->cb_user = user;
    pthread_mutex_unlock(&m->mu);
}

int ql_media_poll(QLMedia *m)
{
    if (m == NULL)
        return 0;

    QLMediaEvent events[QL_CFG_MEDIA_EVENTS];
    int n = 0;
    ql_media_event_cb cb;
    void *cb_user;

    pthread_mutex_lock(&m->mu);
    cb = m->cb;
    cb_user = m->cb_user;
    while (m->ev_count > 0 && n < QL_CFG_MEDIA_EVENTS) {
        events[n++] = m->evq[m->ev_head];
        m->ev_head = (m->ev_head + 1) % QL_CFG_MEDIA_EVENTS;
        m->ev_count--;
    }
    pthread_mutex_unlock(&m->mu);

    for (int i = 0; i < n; i++) {
        if (cb != NULL)
            cb(m, (QLMediaEventType)events[i].type, cb_user);
    }
    return 1;
}

#endif /* QL_FEAT_MEDIA */
