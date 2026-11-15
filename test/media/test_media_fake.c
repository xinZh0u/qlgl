/**
 * @file    test_media_fake.c
 * @brief   QLMedia 核心（状态机/时钟/事件/暂停/seek/音量/循环）——假后端。
 *
 * 不依赖 FFmpeg、也不碰声卡：后端按"时长 + 采样率"生成常量 PCM，
 * sink 只数帧、可选地睡一点（模拟设备节奏）。M1 的核心逻辑全在这里被覆盖。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../ql_test.h"

#if QL_FEAT_MEDIA

#include <qlgl/qlmedia.h>

/* ------------------------------ 假后端 ------------------------------ */

typedef struct {
    int     rate, channels;
    int64_t frames_total;
    int64_t frames_out;
    int     seeks;
    int64_t last_seek_ms;
    int16_t value;
} FakeBe;

static volatile int s_be_rate = 48000;
static volatile int s_be_channels = 2;
static volatile int s_be_ms = 2000;
static volatile int16_t s_be_value = 8000;

static void *be_open(const char *src, char *err, size_t cap)
{
    (void)src; (void)err; (void)cap;
    FakeBe *b = (FakeBe *)ql_mem_calloc(1, sizeof(*b));
    if (b == NULL)
        return NULL;
    b->rate = s_be_rate;
    b->channels = s_be_channels;
    b->frames_total = (int64_t)s_be_ms * b->rate / 1000;
    b->value = s_be_value;
    return b;
}

static void be_close(void *h) { ql_mem_free(h); }

static int64_t be_duration(void *h)
{
    FakeBe *b = (FakeBe *)h;
    return b->frames_total * 1000 / b->rate;
}

static int be_has_audio(void *h) { (void)h; return 1; }
static int be_has_video(void *h) { (void)h; return 0; }
static int be_rate(void *h) { return ((FakeBe *)h)->rate; }
static int be_channels(void *h) { return ((FakeBe *)h)->channels; }

static int be_read(void *h, int16_t *buf, int max_frames)
{
    FakeBe *b = (FakeBe *)h;
    int64_t left = b->frames_total - b->frames_out;
    if (left <= 0)
        return 0;
    int n = (int)((left < max_frames) ? left : max_frames);
    for (int i = 0; i < n * b->channels; i++)
        buf[i] = b->value;
    b->frames_out += n;
    return n;
}

static QLErr be_seek(void *h, int64_t ms)
{
    FakeBe *b = (FakeBe *)h;
    b->seeks++;
    b->last_seek_ms = ms;
    b->frames_out = ms * b->rate / 1000;
    if (b->frames_out > b->frames_total)
        b->frames_out = b->frames_total;
    return QL_OK;
}

static const QLMediaBackend s_fake_be = {
    .name = "fake",
    .open = be_open,
    .close = be_close,
    .duration_ms = be_duration,
    .has_audio = be_has_audio,
    .has_video = be_has_video,
    .rate = be_rate,
    .channels = be_channels,
    .read_audio = be_read,
    .seek = be_seek,
};

/* ------------------------------ 假声卡 ------------------------------ */

typedef struct {
    int      rate;
    int      channels;
    int      pace_us;        /* 每次 write 睡多久（0 = 立即消费，测试跑得快） */
    int      paused;
    uint64_t frames;
    int16_t  last_sample;
} FakeSink;

static FakeSink s_sink;
static volatile int s_pace_us;   /* 每次 write 睡多少微秒（0 = 立即消费） */

static int sink_open(void **ctx, int rate, int channels)
{
    memset(&s_sink, 0, sizeof(s_sink));
    s_sink.rate = rate;
    s_sink.channels = channels;
    s_sink.pace_us = s_pace_us;
    *ctx = &s_sink;
    return 0;
}

static void sink_close(void *ctx) { (void)ctx; }

static int sink_write(void *ctx, const int16_t *pcm, int frames)
{
    FakeSink *s = (FakeSink *)ctx;
    if (s->paused)
        return 0;
    if (frames > 0)
        s->last_sample = pcm[0];
    s->frames += (uint64_t)frames;
    if (s->pace_us > 0) {
        struct timespec ts = { 0, (long)s->pace_us * 1000L };
        nanosleep(&ts, NULL);
    }
    return frames;
}

static int sink_delay(void *ctx) { (void)ctx; return 0; }
static int sink_pause(void *ctx, int paused)
{
    ((FakeSink *)ctx)->paused = paused;
    return 0;
}
static void sink_flush(void *ctx) { (void)ctx; }
static uint32_t sink_underruns(void *ctx) { (void)ctx; return 0; }

static const QLAudioSink s_fake_sink = {
    .name = "fake",
    .open = sink_open,
    .close = sink_close,
    .write = sink_write,
    .delay_frames = sink_delay,
    .pause = sink_pause,
    .flush = sink_flush,
    .underruns = sink_underruns,
};

/* ------------------------------ 事件与等待 ------------------------------ */

static int s_ev_ready, s_ev_state, s_ev_ended, s_ev_error;

static void on_event(QLMedia *m, QLMediaEventType ev, void *user)
{
    (void)m; (void)user;
    switch (ev) {
    case QL_MEDIA_EV_READY: s_ev_ready++; break;
    case QL_MEDIA_EV_STATE: s_ev_state++; break;
    case QL_MEDIA_EV_ENDED: s_ev_ended++; break;
    case QL_MEDIA_EV_ERROR: s_ev_error++; break;
    default: break;
    }
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int wait_state(QLMedia *m, QLMediaState want, int timeout_ms)
{
    for (int i = 0; i < timeout_ms; i++) {
        (void)ql_media_poll(m);
        if (ql_media_state(m) == want)
            return 1;
        sleep_ms(1);
    }
    return 0;
}

#endif
QL_TEST_BEGIN("test_media_fake")
#if QL_FEAT_MEDIA

    ql_media_set_backend(&s_fake_be);
    ql_audio_set_sink(&s_fake_sink);

    /* ---------- 1. 打开 + 播完 + 事件 ---------- */
    s_ev_ready = s_ev_state = s_ev_ended = s_ev_error = 0;
    s_pace_us = 0;                    /* 立即消费 */
    s_be_ms = 2000;

    QLMedia *m = ql_media_open("fake://basic");
    QL_CHECK(m != NULL);
    if (m != NULL) {
        QL_CHECK_EQ(ql_media_state(m), QL_MEDIA_READY);
        QL_CHECK_EQ((int)ql_media_duration_ms(m), 2000);
        QL_CHECK_EQ(ql_media_has_audio(m), 1);
        QL_CHECK_EQ(ql_media_has_video(m), 0);
        ql_media_set_event_cb(m, on_event, NULL);

        QL_CHECK_EQ(ql_media_play(m), QL_OK);
        QL_CHECK_EQ(ql_media_state(m), QL_MEDIA_PLAYING);
        QL_CHECK_EQ(wait_state(m, QL_MEDIA_ENDED, 3000), 1);
        QL_CHECK_EQ((int)ql_media_position_ms(m), 2000);
        QL_CHECK(s_ev_ready >= 1);
        QL_CHECK(s_ev_ended >= 1);
        QL_CHECK_EQ(s_ev_error, 0);
        ql_media_close(m);
    }

    /* ---------- 2. 暂停冻结时钟，恢复后继续 ---------- */
    s_pace_us = 1000;                        /* 每次 write 睡 1ms，放大到可观察 */
    s_be_ms = 60000;
    m = ql_media_open("fake://pause");
    QL_CHECK(m != NULL);
    if (m != NULL) {
        QL_CHECK_EQ(ql_media_play(m), QL_OK);
        sleep_ms(30);
        QL_CHECK_EQ(ql_media_pause(m), QL_OK);
        const int64_t p1 = ql_media_position_ms(m);
        QL_CHECK(p1 > 0);
        sleep_ms(60);
        const int64_t p2 = ql_media_position_ms(m);
        QL_CHECK_EQ((int)p2, (int)p1);       /* 暂停后位置不动 */

        QL_CHECK_EQ(ql_media_play(m), QL_OK);   /* 恢复 = play() */
        sleep_ms(40);
        const int64_t p3 = ql_media_position_ms(m);
        QL_CHECK(p3 > p2);
        ql_media_close(m);
    }

    /* ---------- 3. seek ---------- */
    s_pace_us = 1000;
    s_be_ms = 20000;
    m = ql_media_open("fake://seek");
    QL_CHECK(m != NULL);
    if (m != NULL) {
        QL_CHECK_EQ(ql_media_play(m), QL_OK);
        sleep_ms(20);
        QL_CHECK_EQ(ql_media_seek(m, 10000), QL_OK);
        sleep_ms(30);
        const int64_t pos = ql_media_position_ms(m);
        QL_CHECK(pos >= 10000);
        QL_CHECK(pos < 19000);
        ql_media_close(m);
    }

    /* ---------- 4. 音量 / 静音 ---------- */
    s_pace_us = 0;
    s_be_ms = 200;
    m = ql_media_open("fake://volume");
    QL_CHECK(m != NULL);
    if (m != NULL) {
        QL_CHECK_EQ(ql_media_set_volume(m, 50), QL_OK);
        QL_CHECK_EQ(ql_media_play(m), QL_OK);
        QL_CHECK_EQ(wait_state(m, QL_MEDIA_ENDED, 2000), 1);
        const int half = (int)s_sink.last_sample;
        QL_CHECK(half > 3500 && half < 4500);   /* 8000 * 50% */

        ql_media_seek(m, 0);
        ql_media_set_muted(m, 1);
        QL_CHECK_EQ(ql_media_play(m), QL_OK);
        QL_CHECK_EQ(wait_state(m, QL_MEDIA_ENDED, 2000), 1);
        QL_CHECK_EQ((int)s_sink.last_sample, 0);
        ql_media_close(m);
    }

    /* ---------- 5. loop：播完自动回 0，不发 ENDED ---------- */
    s_pace_us = 0;
    s_be_ms = 100;
    s_ev_ended = 0;
    m = ql_media_open("fake://loop");
    QL_CHECK(m != NULL);
    if (m != NULL) {
        ql_media_set_event_cb(m, on_event, NULL);
        ql_media_set_loop(m, 1);
        QL_CHECK_EQ(ql_media_play(m), QL_OK);
        sleep_ms(80);
        (void)ql_media_poll(m);
        QL_CHECK(ql_media_state(m) == QL_MEDIA_PLAYING);
        QL_CHECK_EQ(s_ev_ended, 0);
        ql_media_set_loop(m, 0);
        ql_media_close(m);
    }

    /* ---------- 6. 打开失败：明确报错，不崩 ---------- */
    s_be_ms = 100;
    {
        /* 让 open 失败：src 为空 */
        QL_CHECK(ql_media_open(NULL) == NULL);
        QL_CHECK(ql_media_open("") == NULL);
    }

    ql_audio_set_sink(NULL);
    ql_media_set_backend(NULL);

#else
    printf("  [SKIP] 本构建没有 QL_FEAT_MEDIA\n");
#endif

QL_TEST_END()
