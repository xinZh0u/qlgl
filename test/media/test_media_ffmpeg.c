/**
 * @file    test_media_ffmpeg.c
 * @brief   真实解码：生成一段 WAV（44.1k 单声道），用 FFmpeg 后端播完。
 *
 * 不依赖外部工具：WAV 头 + 正弦样本在测试里现写；
 * 声卡用"立即消费"的假 sink，避免 CI/无声卡环境依赖 ALSA。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../ql_test.h"

#if QL_FEAT_MEDIA && QL_FEAT_MEDIA_FFMPEG

#include <qlgl/qlmedia.h>

/* ------------------------------ 假声卡 ------------------------------ */

typedef struct {
    int      rate, channels;
    uint64_t frames;
} FastSink;

static FastSink s_sink;

static int sink_open(void **ctx, int rate, int channels)
{
    s_sink.rate = rate;
    s_sink.channels = channels;
    s_sink.frames = 0;
    *ctx = &s_sink;
    return 0;
}
static void sink_close(void *ctx) { (void)ctx; }
static int sink_write(void *ctx, const int16_t *pcm, int frames)
{
    (void)ctx; (void)pcm;
    s_sink.frames += (uint64_t)frames;
    return frames;
}
static int sink_delay(void *ctx) { (void)ctx; return 0; }
static void sink_flush(void *ctx) { (void)ctx; }

static const QLAudioSink s_fast_sink = {
    .name = "fast",
    .open = sink_open,
    .close = sink_close,
    .write = sink_write,
    .delay_frames = sink_delay,
    .flush = sink_flush,
};

/* ------------------------------ WAV 生成 ------------------------------ */

static void put32(FILE *f, uint32_t v)
{
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f);
    fputc((int)((v >> 24) & 0xFF), f);
}
static void put16(FILE *f, uint16_t v)
{
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
}

static int write_wav(const char *path, int ms, int rate, int channels)
{
    const int frames = rate * ms / 1000;
    const uint32_t data = (uint32_t)frames * (uint32_t)channels * 2u;
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return 0;
    fwrite("RIFF", 1, 4, f);
    put32(f, 36u + data);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    put32(f, 16);
    put16(f, 1);                          /* PCM */
    put16(f, (uint16_t)channels);
    put32(f, (uint32_t)rate);
    put32(f, (uint32_t)(rate * channels * 2));
    put16(f, (uint16_t)(channels * 2));
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, data);
    for (int i = 0; i < frames; i++) {
        const double t = (double)i / (double)rate;
        const int16_t v = (int16_t)(sin(t * 2.0 * 3.14159265358979 * 440.0) * 8000.0);
        for (int c = 0; c < channels; c++)
            put16(f, (uint16_t)v);
    }
    const int ok = (fclose(f) == 0);
    return ok;
}

/* ------------------------------ 测试 ------------------------------ */

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int wait_ended(QLMedia *m, int timeout_ms)
{
    for (int i = 0; i < timeout_ms; i++) {
        (void)ql_media_poll(m);
        if (ql_media_state(m) == QL_MEDIA_ENDED)
            return 1;
        if (ql_media_state(m) == QL_MEDIA_ERROR)
            return 0;
        sleep_ms(1);
    }
    return 0;
}

#endif
QL_TEST_BEGIN("test_media_ffmpeg")
#if QL_FEAT_MEDIA && QL_FEAT_MEDIA_FFMPEG

    ql_audio_set_sink(&s_fast_sink);

    char path[128];
    snprintf(path, sizeof(path), "/tmp/qlgl_media_%d.wav", (int)getpid());
    QL_CHECK_EQ(write_wav(path, 1000, 44100, 1), 1);

    QLMedia *m = ql_media_open(path);
    QL_CHECK(m != NULL);
    if (m != NULL) {
        QL_CHECK_EQ(ql_media_has_audio(m), 1);
        const int64_t dur = ql_media_duration_ms(m);
        printf("  duration = %lld ms\n", (long long)dur);
        QL_CHECK(dur >= 900 && dur <= 1100);

        /* 播完 1 秒 */
        QL_CHECK_EQ(ql_media_play(m), QL_OK);
        QL_CHECK_EQ(wait_ended(m, 3000), 1);
        if (ql_media_state(m) == QL_MEDIA_ERROR)
            printf("  error: %s\n", ql_media_error(m));
        const int64_t pos = ql_media_position_ms(m);
        printf("  ended at = %lld ms\n", (long long)pos);
        QL_CHECK(pos >= 900 && pos <= 1100);

        /* seek 到中间再播完 */
        QL_CHECK_EQ(ql_media_seek(m, 500), QL_OK);
        QL_CHECK_EQ(ql_media_play(m), QL_OK);
        QL_CHECK_EQ(wait_ended(m, 3000), 1);
        QL_CHECK(ql_media_position_ms(m) >= 900);
        QL_CHECK_EQ(ql_media_underruns(m), 0u);

        ql_media_close(m);
    } else {
        printf("  [FAIL] 打不开生成的 WAV（FFmpeg 后端异常）\n");
    }
    remove(path);
    ql_audio_set_sink(NULL);

#else
    printf("  [SKIP] 本构建没有 QL_FEAT_MEDIA / FFmpeg 后端\n");
#endif

QL_TEST_END()
