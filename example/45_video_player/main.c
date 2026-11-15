/**
 * @file    main.c
 * @brief   极简视频播放器：`<video>` + 文件名/时间/可调进度条/播放暂停。
 *
 *   ./build/example/example_45_video_player clip.mp4
 *   ./build/example/example_45_video_player clip.mp4 --seconds 5
 *   ./build/example/example_45_video_player clip.mp4 --dump /tmp/video.ppm
 *
 * 演示 M2：QLMedia 解出 RGBA 帧 → `ql_ui_bind_media()` 绑到 `<video>` 节点 →
 * 每来新帧把节点标脏，主循环只重绘那一块。控件仍然是普通 HTML/CSS。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <qlgl/qlgl.h>

#define W 480
#define H 360

typedef struct {
    QLMedia    *media;
    QLWindow   *win;
    QLUIRoot   *ui;
    QLUINodeRT *video;
    QLUINodeRT *bar;
    QLUINodeRT *time;
    QLUINodeRT *play;
    int         last_shown_ms;
    int         dragging;
    int         drag_ms;
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
        pos = app->drag_ms;
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

static int64_t ms_at_x(const App *app, QLRect r, int32_t x)
{
    if (r.w <= 0)
        return 0;
    int64_t dx = x - r.x;
    if (dx < 0) dx = 0;
    if (dx > r.w) dx = r.w;
    return dx * duration_ms(app) / r.w;
}

static int on_event(QLUIRoot *ui, const QLInputEvent *ev, void *user)
{
    (void)ui;
    App *app = (App *)user;
    if (app->bar == NULL)
        return 0;
    const QLRect r = ql_ui_node_rect(app->bar);
    const int inside = (ev->x >= r.x && ev->x < r.x + r.w &&
                        ev->y >= r.y - 6 && ev->y < r.y + r.h + 6);

    switch (ev->type) {
    case QL_INPUT_POINTER_DOWN:
        if (!inside) return 0;
        app->dragging = 1;
        app->drag_ms = (int)ms_at_x(app, r, ev->x);
        return 1;
    case QL_INPUT_POINTER_MOVE:
        if (!app->dragging) return 0;
        app->drag_ms = (int)ms_at_x(app, r, ev->x);
        return 1;
    case QL_INPUT_POINTER_UP:
        if (!app->dragging) return 0;
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

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)ui;
    App *app = (App *)user;
    if (node != app->play)
        return;
    if (ql_media_state(app->media) == QL_MEDIA_PLAYING)
        (void)ql_media_pause(app->media);
    else
        (void)ql_media_play(app->media);
    update_ui(app, 1);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *dump = NULL;
    const char *size = NULL;
    int seconds = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
            dump = argv[++i];
        else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc)
            size = argv[++i];
        else if (path == NULL)
            path = argv[i];
    }
    if (path == NULL) {
        fprintf(stderr, "用法: %s <视频文件> [--seconds N] [--dump out.ppm]\n", argv[0]);
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
    if (!ql_media_has_video(app.media))
        fprintf(stderr, "提示：这个文件没有视频轨（只出声）\n");

    const char *base = strrchr(path, '/');
    base = (base != NULL) ? base + 1 : path;
    snprintf(app.name_buf, sizeof(app.name_buf), "%s", base);

    QLUIDoc *doc = ql_ui_load_html(
        "<div id=root>"
        "<div class=name></div>"
        "<video></video>"
        "<div class=time>0:00 / 0:00</div>"
        "<progress value=0 max=1000></progress>"
        "<div class=row><button>暂停</button></div>"
        "</div>",
        "#root { width: 100%; height: 100%; padding: 10px;"
        " box-sizing: border-box; display: flex; flex-direction: column;"
        " background-color: #FFFFFF; color: #1F2328; font-size: 12px; }"
        ".name { font-size: 14px; color: #0969DA; margin-bottom: 6px;"
        " white-space: nowrap; overflow: hidden; }"
        "video { flex: 1 1 0px; width: 100%; height: auto;"
        " background-color: #000000; margin-bottom: 8px; }"
        ".time { color: #57606A; margin-bottom: 4px; }"
        "progress { width: 100%; height: 12px; margin-bottom: 8px; }"
        ".row { display: flex; }"
        "button { padding: 6px 14px; border-radius: 6px; background-color: #0969DA;"
        " color: #FFFFFF; border: 1px solid #0969DA; }"
        "button:hover { background-color: #1F7FE8; }");
    if (doc == NULL) {
        fprintf(stderr, "界面解析失败\n");
        return 1;
    }
    app.ui = ql_ui_build(doc);
    if (app.ui == NULL) {
        ql_ui_doc_free(doc);
        return 1;
    }

    QLUINodeRT *name = ql_ui_find_by_tag(app.ui, QL_UI_TAG_DIV, 1);
    if (name != NULL)
        (void)ql_ui_set_text(app.ui, name, app.name_buf);
    app.time = ql_ui_find_by_tag(app.ui, QL_UI_TAG_DIV, 2);
    app.bar = ql_ui_find_by_tag(app.ui, QL_UI_TAG_PROGRESS, 0);
    app.play = ql_ui_find_by_tag(app.ui, QL_UI_TAG_BUTTON, 0);
    app.video = ql_ui_find_by_tag(app.ui, QL_UI_TAG_VIDEO, 0);
    ql_ui_set_click_cb(app.ui, on_click, &app);
    if (app.video != NULL)
        (void)ql_ui_bind_media(app.ui, app.video, app.media);

    app.win = ql_window_create("QLGL · 视频", W, H, QL_WINDOW_GL);
    if (app.win == NULL)
        app.win = ql_window_create("QLGL · 视频", W, H, QL_WINDOW_CPU);
    if (app.win == NULL) {
        fprintf(stderr, "建窗口失败（没有 GL/X11？）\n");
        return 1;
    }
    (void)ql_window_bind_ui(app.win, app.ui);
    if (size != NULL) {
        int sw = 0, sh = 0;
        if (sscanf(size, "%dx%d", &sw, &sh) == 2 && sw > 0 && sh > 0)
            (void)ql_window_resize(app.win, sw, sh);
    }
    ql_window_set_event_filter(app.win, on_event, &app);
    ql_media_play(app.media);

    if (dump != NULL) {
        /* 给解码线程一点时间出第一帧，然后导一帧 */
        uint32_t serial = 0;
        for (int i = 0; i < 60; i++) {
            (void)ql_media_poll(app.media);
            const uint32_t s = ql_media_video_serial(app.media);
            if (s != serial) {
                serial = s;
                ql_ui_invalidate(app.ui, app.video);
            }
            update_ui(&app, i == 0);
            (void)ql_window_poll(app.win);
            struct timespec ts = { 0, 10 * 1000000L };
            nanosleep(&ts, NULL);
        }
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
    uint32_t serial = 0;

    for (;;) {
        (void)ql_media_poll(app.media);

        const uint32_t s = ql_media_video_serial(app.media);
        if (s != serial) {
            serial = s;
            ql_ui_invalidate(app.ui, app.video);   /* 只重绘视频那一块 */
        }

        if (app.drag_ms >= 0) {
            (void)ql_media_seek(app.media, app.drag_ms);
            app.drag_ms = -1;
            update_ui(&app, 1);
        } else {
            update_ui(&app, 0);
        }

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
