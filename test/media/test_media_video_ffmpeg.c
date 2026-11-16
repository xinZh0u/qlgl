/**
 * @file    test_media_video_ffmpeg.c
 * @brief   真实视频解码：用系统 ffmpeg 生成 1 秒 mp4，再走 QLMedia 播完。
 *
 * 没有 ffmpeg 工具或编码器就 SKIP（不把二进制样本塞进仓库）。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../ql_test.h"

#if QL_FEAT_MEDIA && QL_FEAT_MEDIA_FFMPEG

#include <qlgl/qlmedia.h>

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

QL_TEST_BEGIN("test_media_video_ffmpeg")

    ql_audio_set_sink(&s_sink);

    char path[128];
    snprintf(path, sizeof(path), "/tmp/qlgl_video_%d.mp4", (int)getpid());
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -loglevel error -f lavfi -i "
             "\"testsrc2=size=64x48:rate=10:duration=1\" -pix_fmt yuv420p "
             "\"%s\" -y >/dev/null 2>&1", path);

    if (system(cmd) != 0) {
        printf("  [SKIP] 没有可用的 ffmpeg/编码器\n");
    } else {
        QLMedia *m = ql_media_open(path);
        QL_CHECK(m != NULL);
        if (m != NULL) {
            ql_coord_t vw = 0, vh = 0;
            QL_CHECK_EQ(ql_media_has_video(m), 1);
            QL_CHECK_EQ(ql_media_video_size(m, &vw, &vh), 1);
            printf("  video = %dx%d, duration = %lld ms\n", (int)vw, (int)vh,
                   (long long)ql_media_duration_ms(m));
            QL_CHECK_EQ((int)vw, 64);
            QL_CHECK_EQ((int)vh, 48);

            QL_CHECK_EQ(ql_media_play(m), QL_OK);
            int frames = 0;
            for (int i = 0; i < 3000; i++) {
                (void)ql_media_poll(m);
                const QLMediaVideoFrame *f = ql_media_acquire_frame(m);
                if (f != NULL) {
                    QL_CHECK(f->px != NULL);
                    frames++;
                    ql_media_frame_release(f);
                }
                if (ql_media_state(m) == QL_MEDIA_ENDED)
                    break;
                sleep_ms(1);
            }
            printf("  frames=%d state=%d\n", frames, (int)ql_media_state(m));
            QL_CHECK(frames >= 5);
            QL_CHECK_EQ(ql_media_state(m), QL_MEDIA_ENDED);
            ql_media_close(m);
        }
        remove(path);

        /*
         * 录屏 WebM：全程只有一个关键帧，seek 必须走"回退到 0 重解"的兜底，
         * 而且要能继续播下去（不能直接 ERROR/END）。
         */
        char wpath[128];
        snprintf(wpath, sizeof(wpath), "/tmp/qlgl_sparse_%d.webm", (int)getpid());
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -loglevel error -f lavfi -i "
                 "\"testsrc2=size=64x48:rate=30:duration=2\" -c:v libvpx -g 9999 "
                 "\"%s\" -y >/dev/null 2>&1", wpath);
        if (system(cmd) == 0) {
            QLMedia *w = ql_media_open(wpath);
            QL_CHECK(w != NULL);
            if (w != NULL) {
                QL_CHECK_EQ(ql_media_play(w), QL_OK);
                sleep_ms(300);
                QL_CHECK_EQ(ql_media_seek(w, 1200), QL_OK);
                sleep_ms(700);
                (void)ql_media_poll(w);
                const QLMediaState st = ql_media_state(w);
                printf("  sparse-webm seek state=%d pos=%lld\n", (int)st,
                       (long long)ql_media_position_ms(w));
                QL_CHECK(st != QL_MEDIA_ERROR);
                QL_CHECK(ql_media_position_ms(w) >= 1000);
                const QLMediaVideoFrame *f = ql_media_acquire_frame(w);
                QL_CHECK(f != NULL);
                if (f != NULL)
                    ql_media_frame_release(f);
                ql_media_close(w);
            }
            remove(wpath);
        } else {
            printf("  [SKIP] 没有 libvpx 编码器（sparse webm 子测试）\n");
        }
    }

    ql_audio_set_sink(NULL);

#else
    printf("  [SKIP] 本构建没有 QL_FEAT_MEDIA / FFmpeg 后端\n");
#endif

QL_TEST_END()
