/**
 * @file    main.c
 * @brief   极简音频播放器：文件名 + 进度条 + 时间 + 播放/暂停。
 *
 *   ./build/example/example_44_audio_player song.mp3
 *   ./build/example/example_44_audio_player song.wav --seconds 5
 *   ./build/example/example_44_audio_player song.mp3 --dump /tmp/player.ppm
 *
 * 演示 `<audio>` 的引擎层（QLMedia）+ 普通 HTML/CSS 控件：
 * 文件名、时间、进度条、播放/暂停全部是 DOM；进度条可点击/拖动跳转
 * （`ql_window_set_event_filter` 做命中与坐标换算），←/→ 各 5 秒。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <qlgl/qlgl.h>

#define W 380
#define H 190

typedef struct {
    QLMedia    *media;
    QLWindow   *win;
    QLUIRoot   *ui;
    QLUINodeRT *bar;      /* <progress> */
    QLUINodeRT *time;     /* 时间文本 */
    QLUINodeRT *play;     /* 播放/暂停按钮 */
    int         last_shown_ms;
    int         dragging;        /* 正在拖进度条 */
    int         drag_ms;         /* 拖动/点击的预览位置（-1 = 没有） */
    char        time_buf[48];
    char        name_buf[96];
} App;

static void fmt_time(char *out, size_t cap, int64_t ms)
{
    if (ms < 0)
        ms = 0;
    const int64_t s = ms / 1000;
    snprintf(out, cap, "%d:%02d", (int)(s / 60), (int)(s % 60));
}

static void update_ui(App *app, int force)
{
    int64_t pos = ql_media_position_ms(app->media);
    if (app->drag_ms >= 0)
        pos = app->drag_ms;          /* 拖动时先显示目标位置，不等 seek 完成 */
    if (!force && pos / 250 == app->last_shown_ms / 250)
        return;
    app->last_shown_ms = (int)pos;

    char cur[16], total[16];
    fmt_time(cur, sizeof(cur), pos);
    fmt_time(total, sizeof(total), ql_media_duration_ms(app->media));
    snprintf(app->time_buf, sizeof(app->time_buf), "%s / %s", cur, total);
    (void)ql_ui_set_text(app->ui, app->time, app->time_buf);

    const int dur = (int)ql_media_duration_ms(app->media);
    (void)ql_ui_progress_set(app->ui, app->bar, (int)pos, dur > 0 ? dur : 1000);

    (void)ql_ui_set_text(app->ui, app->play,
                         (ql_media_state(app->media) == QL_MEDIA_PLAYING) ? "暂停"
                                                                         : "播放");
}

static int64_t duration_ms(const App *app)
{
    const int64_t d = ql_media_duration_ms(app->media);
    return (d > 0) ? d : 1;
}

/* 把 x 换算成进度（毫秒）；进度条之外的坐标夹到两端 */
static int64_t ms_at_x(const App *app, const QLRect r, int32_t x)
{
    if (r.w <= 0)
        return 0;
    int64_t dx = x - r.x;
    if (dx < 0)
        dx = 0;
    if (dx > r.w)
        dx = r.w;
    return dx * duration_ms(app) / r.w;
}

static int on_event(QLUIRoot *ui, const QLInputEvent *ev, void *user)
{
    (void)ui;
    App *app = (App *)user;
    if (app->bar == NULL)
        return 0;

    const QLRect r = ql_ui_node_rect(app->bar);
    /* 纵向多给 6px 的抓取范围，细进度条也好点 */
    const int inside = (ev->x >= r.x && ev->x < r.x + r.w &&
                        ev->y >= r.y - 6 && ev->y < r.y + r.h + 6);

    switch (ev->type) {
    case QL_INPUT_POINTER_DOWN:
        if (!inside)
            return 0;
        app->dragging = 1;
        app->drag_ms = (int)ms_at_x(app, r, ev->x);
        return 1;
    case QL_INPUT_POINTER_MOVE:
        if (!app->dragging)
            return 0;
        app->drag_ms = (int)ms_at_x(app, r, ev->x);
        return 1;
    case QL_INPUT_POINTER_UP:
        if (!app->dragging)
            return 0;
        app->dragging = 0;
        app->drag_ms = (int)ms_at_x(app, r, ev->x);
        return 1;
    case QL_INPUT_KEY_DOWN: {
        const int64_t pos = ql_media_position_ms(app->media);
        if (ev->key == QL_KEY_LEFT) {
            (void)ql_media_seek(app->media, pos > 5000 ? pos - 5000 : 0);
            update_ui(app, 1);
            return 1;
        }
        if (ev->key == QL_KEY_RIGHT) {
            (void)ql_media_seek(app->media, pos + 5000);
            update_ui(app, 1);
            return 1;
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

static void on_play_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)ui;
    (void)node;
    App *app = (App *)user;
    if (ql_media_state(app->media) == QL_MEDIA_PLAYING)
        (void)ql_media_pause(app->media);
    else
        (void)ql_media_play(app->media);          /* ENDED 后 play() = 重播 */
    update_ui(app, 1);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *dump = NULL;
    int seconds = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
            dump = argv[++i];
        else if (path == NULL)
            path = argv[i];
    }
    if (path == NULL) {
        fprintf(stderr, "用法: %s <音频文件> [--seconds N] [--dump out.ppm]\n",
                argv[0]);
        return 2;
    }

    if (qlgl_init() != QL_OK)
        return 1;

    App app;
    memset(&app, 0, sizeof(app));
    app.last_shown_ms = -1000;
    app.drag_ms = -1;

    app.media = ql_media_open(path);
    if (app.media == NULL) {
        fprintf(stderr, "打不开: %s\n", path);
        return 1;
    }

    const char *base = strrchr(path, '/');
    base = (base != NULL) ? base + 1 : path;
    snprintf(app.name_buf, sizeof(app.name_buf), "%s", base);

    QLUIDoc *doc = ql_ui_load_html(
        "<div id=root>"
        "<div id=name class=name></div>"
        "<div id=time class=time>0:00 / 0:00</div>"
        "<progress id=bar value=0 max=1000></progress>"
        "<div class=row><button id=play>暂停</button></div>"
        "<div class=hint>点击或拖动进度条跳转，←/→ 各 5 秒</div>"
        "</div>",
        "#root { width: 100%; height: 100%; padding: 12px; box-sizing: border-box;"
        " background-color: #FFFFFF; color: #1F2328; font-size: 12px; }"
        ".name { font-size: 15px; color: #0969DA; margin-bottom: 8px;"
        " white-space: nowrap; overflow: hidden; }"
        ".time { color: #57606A; margin-bottom: 6px; }"
        "progress { width: 100%; height: 12px; margin-bottom: 10px; }"
        ".row { display: flex; }"
        "button { padding: 6px 14px; border-radius: 6px; background-color: #0969DA;"
        " color: #FFFFFF; border: 1px solid #0969DA; }"
        "button:hover { background-color: #1F7FE8; }"
        ".hint { margin-top: 10px; color: #8C959F; font-size: 11px; }");
    if (doc == NULL) {
        fprintf(stderr, "界面解析失败\n");
        return 1;
    }
    app.ui = ql_ui_build(doc);
    /* root 不拥有 doc：doc 必须活到 ui 释放之后（否则样式指针悬空 → 白屏） */
    if (app.ui == NULL) {
        ql_ui_doc_free(doc);
        return 1;
    }

    QLUINodeRT *name = ql_ui_find_by_id(app.ui, "name");
    if (name != NULL)
        (void)ql_ui_set_text(app.ui, name, app.name_buf);
    app.time = ql_ui_find_by_id(app.ui, "time");
    app.bar = ql_ui_find_by_id(app.ui, "bar");
    app.play = ql_ui_find_by_id(app.ui, "play");
    if (app.play != NULL)
        (void)ql_ui_set_node_click_cb(app.ui, app.play, on_play_click, &app);

    app.win = ql_window_create("QLGL · 音频", W, H, QL_WINDOW_GL);
    if (app.win == NULL)
        app.win = ql_window_create("QLGL · 音频", W, H, QL_WINDOW_CPU);
    if (app.win == NULL) {
        fprintf(stderr, "建窗口失败（没有 GL/X11？）\n");
        ql_ui_free(app.ui);
        ql_ui_doc_free(doc);
        return 1;
    }
    (void)ql_window_bind_ui(app.win, app.ui);
    ql_window_set_event_filter(app.win, on_event, &app);
    ql_media_play(app.media);

    if (dump != NULL) {
        update_ui(&app, 1);
        for (int i = 0; i < 3; i++)
            (void)ql_window_poll(app.win);
        const int ok = ql_window_dump_ppm(app.win, dump);
        printf(ok ? "已写出 %s\n" : "写出 %s 失败\n", dump);
        ql_media_close(app.media);
        ql_window_destroy(app.win);
        ql_ui_free(app.ui);
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return ok ? 0 : 1;
    }

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        (void)ql_media_poll(app.media);

        if (app.drag_ms >= 0 && app.dragging) {
            (void)ql_media_seek(app.media, app.drag_ms);
            app.drag_ms = -1;                   /* 下一帧再按新位置更新 */
        } else if (!app.dragging && app.drag_ms >= 0) {
            (void)ql_media_seek(app.media, app.drag_ms);
            app.drag_ms = -1;
            update_ui(&app, 1);
        }

        update_ui(&app, 0);
        if (!ql_window_poll(app.win))
            break;
        if (seconds > 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - t0.tv_sec >= seconds)
                break;
        }
    }

    printf("欠载: %u\n", (unsigned)ql_media_underruns(app.media));
    ql_media_close(app.media);
    ql_window_destroy(app.win);
    ql_ui_free(app.ui);
    ql_ui_doc_free(doc);
    qlgl_shutdown();
    return 0;
}
