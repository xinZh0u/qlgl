/**
 * @file    test_media_video_fake.c
 * @brief   QLMedia 视频侧：帧池/发布/暂停/seek（假后端，不依赖 ffmpeg）。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../ql_test.h"

#if QL_FEAT_MEDIA

#include <qlgl/qlmedia.h>

typedef struct {
    int     rate;
    int64_t total;      /* 总帧数 */
    int64_t done;       /* 已产出 */
    int     seeks;
    int64_t seek_ms;
} FakeBe;

static volatile int s_frames = 20;      /* 20fps */
static volatile int s_ms = 1000;

static void *be_open(const char *src, char *err, size_t cap)
{
    (void)src; (void)err; (void)cap;
    FakeBe *b = (FakeBe *)ql_mem_calloc(1, sizeof(*b));
    if (b == NULL)
        return NULL;
    b->rate = s_frames;
    b->total = (int64_t)s_ms * b->rate / 1000;
    return b;
}
static void be_close(void *h) { ql_mem_free(h); }
static int64_t be_duration(void *h)
{
    FakeBe *b = (FakeBe *)h;
    return b->total * 1000 / b->rate;
}
static int be_has_audio(void *h) { (void)h; return 0; }
static int be_has_video(void *h) { (void)h; return 1; }
static int be_rate(void *h) { (void)h; return 48000; }
static int be_channels(void *h) { (void)h; return 2; }
static int be_read_audio(void *h, int16_t *buf, int frames)
{
    (void)h; (void)buf; (void)frames;
    return -1;
}
static int be_video_size(void *h, int *w, int *out_h)
{
    (void)h;
    if (w != NULL) *w = 64;
    if (out_h != NULL) *out_h = 48;
    return 0;
}
static int be_read_video(void *h, uint8_t *dst, int cap, int *w, int *hh,
                         int64_t *pts)
{
    FakeBe *b = (FakeBe *)h;
    if (b->done >= b->total)
        return 0;
    if (cap < 64 * 48 * 4)
        return -1;
    memset(dst, (int)(b->done % 256), 64 * 48 * 4);
    *w = 64;
    *hh = 48;
    *pts = b->done * 1000000 / b->rate;
    b->done++;
    return 1;
}
static QLErr be_seek(void *h, int64_t ms)
{
    FakeBe *b = (FakeBe *)h;
    b->seeks++;
    b->seek_ms = ms;
    b->done = ms * b->rate / 1000;
    if (b->done > b->total)
        b->done = b->total;
    return QL_OK;
}

static const QLMediaBackend s_be = {
    .name = "fake-video",
    .open = be_open, .close = be_close,
    .duration_ms = be_duration, .has_audio = be_has_audio,
    .has_video = be_has_video, .rate = be_rate, .channels = be_channels,
    .read_audio = be_read_audio, .video_size = be_video_size,
    .read_video = be_read_video, .seek = be_seek,
};

static int sink_open(void **ctx, int rate, int ch)
{
    (void)rate; (void)ch;
    *ctx = (void *)1;
    return 0;
}
static void sink_close(void *ctx) { (void)ctx; }
static int sink_write(void *ctx, const int16_t *pcm, int frames)
{
    (void)ctx; (void)pcm;
    return frames;
}
static int sink_delay(void *ctx) { (void)ctx; return 0; }
static void sink_flush(void *ctx) { (void)ctx; }
static const QLAudioSink s_sink = {
    .name = "fast", .open = sink_open, .close = sink_close,
    .write = sink_write, .delay_frames = sink_delay, .flush = sink_flush,
};

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

QL_TEST_BEGIN("test_media_video_fake")

    ql_media_set_backend(&s_be);
    ql_audio_set_sink(&s_sink);

    QLMedia *m = ql_media_open("fake://video");
    QL_CHECK(m != NULL);
    if (m != NULL) {
        ql_coord_t vw = 0, vh = 0;
        QL_CHECK_EQ(ql_media_has_video(m), 1);
        QL_CHECK_EQ(ql_media_video_size(m, &vw, &vh), 1);
        QL_CHECK_EQ((int)vw, 64);
        QL_CHECK_EQ((int)vh, 48);
        QL_CHECK_EQ((int)ql_media_duration_ms(m), 1000);

        QL_CHECK_EQ(ql_media_play(m), QL_OK);

        int frames = 0;
        int64_t last_pts = -1;
        int serial = 0;
        for (int i = 0; i < 3000; i++) {
            (void)ql_media_poll(m);
            const QLMediaVideoFrame *f = ql_media_acquire_frame(m);
            if (f != NULL) {
                QL_CHECK_EQ(f->w, 64);
                QL_CHECK_EQ(f->h, 48);
                QL_CHECK(f->px != NULL);
                QL_CHECK(f->pts_us >= last_pts);
                last_pts = f->pts_us;
                frames++;
                ql_media_frame_release(f);
            }
            const int s = (int)ql_media_video_serial(m);
            if (s > serial)
                serial = s;
            if (ql_media_state(m) == QL_MEDIA_ENDED)
                break;
            sleep_ms(1);
        }
        QL_CHECK(frames >= 5);
        QL_CHECK(serial >= 5);
        /* 纯视频（没有音轨）也要走系统时钟，位置不能永远停在 0 */
        QL_CHECK(ql_media_position_ms(m) > 100);
        QL_CHECK_EQ(ql_media_state(m), QL_MEDIA_ENDED);
        printf("  frames=%d serial=%d last_pts=%lld\n", frames, serial,
               (long long)last_pts);
        ql_media_close(m);
    }

    /* 暂停：serial 不再增长 */
    m = ql_media_open("fake://pause");
    QL_CHECK(m != NULL);
    if (m != NULL) {
        QL_CHECK_EQ(ql_media_play(m), QL_OK);
        for (int i = 0; i < 200; i++) {
            if (ql_media_video_serial(m) >= 3)
                break;
            sleep_ms(5);
        }
        QL_CHECK_EQ(ql_media_pause(m), QL_OK);
        const uint32_t s1 = ql_media_video_serial(m);
        sleep_ms(120);
        QL_CHECK_EQ((int)ql_media_video_serial(m), (int)s1);
        QL_CHECK_EQ(ql_media_play(m), QL_OK);
        sleep_ms(120);
        QL_CHECK(ql_media_video_serial(m) > s1);
        /* 暂停后拖动：也要把目标位置的帧显示出来（seek preview） */
        QL_CHECK_EQ(ql_media_pause(m), QL_OK);
        QL_CHECK_EQ(ql_media_seek(m, 700), QL_OK);
        sleep_ms(150);
        {
            const QLMediaVideoFrame *pf = ql_media_acquire_frame(m);
            QL_CHECK(pf != NULL);
            if (pf != NULL) {
                printf("  paused-seek pts=%lld\n", (long long)pf->pts_us);
                QL_CHECK(pf->pts_us >= 600000);
                ql_media_frame_release(pf);
            }
        }
        ql_media_close(m);
    }

    /* seek：位置立即生效，之后的帧 pts 紧跟目标 */
    m = ql_media_open("fake://seek");
    QL_CHECK(m != NULL);
    if (m != NULL) {
        QL_CHECK_EQ(ql_media_play(m), QL_OK);
        sleep_ms(80);
        QL_CHECK_EQ(ql_media_seek(m, 500), QL_OK);
        sleep_ms(150);
        const QLMediaVideoFrame *f = ql_media_acquire_frame(m);
        QL_CHECK(f != NULL);
        if (f != NULL) {
            printf("  after seek pts=%lld\n", (long long)f->pts_us);
            QL_CHECK(f->pts_us >= 400000);
            ql_media_frame_release(f);
        }
        ql_media_close(m);
    }

    ql_audio_set_sink(NULL);
    ql_media_set_backend(NULL);

#else
    printf("  [SKIP] 本构建没有 QL_FEAT_MEDIA\n");
#endif

QL_TEST_END()
