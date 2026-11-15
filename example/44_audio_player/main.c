/**
 * @file    main.c
 * @brief   `<audio>` 的引擎层最小示例：用 QLMedia 播一个本地文件。
 *
 *   ./build/example/example_44_audio_player song.mp3
 *   ./build/example/example_44_audio_player song.wav --seconds 3
 *
 * 只演示 API，不做界面：播放 → 每 0.5 秒打印位置 → 播完/到点退出。
 * 结束时会打印欠载计数（正常应为 0）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <qlgl/qlgl.h>

static void on_event(QLMedia *m, QLMediaEventType ev, void *user)
{
    (void)user;
    switch (ev) {
    case QL_MEDIA_EV_READY:
        printf("[event] ready: %lld ms\n", (long long)ql_media_duration_ms(m));
        break;
    case QL_MEDIA_EV_STATE:
        printf("[event] state=%d\n", (int)ql_media_state(m));
        break;
    case QL_MEDIA_EV_ENDED:
        printf("[event] ended\n");
        break;
    case QL_MEDIA_EV_ERROR:
        printf("[event] error: %s\n", ql_media_error(m));
        break;
    default:
        break;
    }
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int seconds = 10;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = atoi(argv[++i]);
        else if (path == NULL)
            path = argv[i];
    }
    if (path == NULL) {
        fprintf(stderr, "用法: %s <音频文件> [--seconds N]\n", argv[0]);
        return 2;
    }

    if (qlgl_init() != QL_OK)
        return 1;

    QLMedia *m = ql_media_open(path);
    if (m == NULL) {
        fprintf(stderr, "打不开: %s\n", path);
        return 1;
    }
    printf("时长: %lld ms，音频: %s，视频: %s\n",
           (long long)ql_media_duration_ms(m),
           ql_media_has_audio(m) ? "有" : "无",
           ql_media_has_video(m) ? "有" : "无");
    ql_media_set_event_cb(m, on_event, NULL);

    if (ql_media_play(m) != QL_OK) {
        fprintf(stderr, "播放失败: %s\n", ql_media_error(m));
        ql_media_close(m);
        return 1;
    }

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int64_t last_print = -500;

    for (;;) {
        (void)ql_media_poll(m);

        clock_gettime(CLOCK_MONOTONIC, &now);
        const int64_t elapsed = (now.tv_sec - t0.tv_sec) * 1000 +
                                (now.tv_nsec - t0.tv_nsec) / 1000000;
        const int64_t pos = ql_media_position_ms(m);

        if (pos / 500 != last_print / 500) {
            last_print = pos;
            printf("  %6.2f / %6.2f s\n", pos / 1000.0,
                   ql_media_duration_ms(m) / 1000.0);
            fflush(stdout);
        }
        if (ql_media_state(m) == QL_MEDIA_ENDED)
            break;
        if (ql_media_state(m) == QL_MEDIA_ERROR) {
            fprintf(stderr, "错误: %s\n", ql_media_error(m));
            break;
        }
        if (elapsed >= (int64_t)seconds * 1000)
            break;

        struct timespec ts = { 0, 10 * 1000000L };   /* 10ms */
        nanosleep(&ts, NULL);
    }

    printf("欠载: %u\n", (unsigned)ql_media_underruns(m));
    ql_media_close(m);
    qlgl_shutdown();
    return 0;
}
