/**
 * @file    example_showcase_window.c
 * @brief   M5.5 效果总览窗口：过渡 / 关键帧 / 圆角与圆形揭示 / 径向渐变 / 假翻转 一次看全。
 *
 * 运行：
 *   ./build/example_showcase_window               打开窗口（vsync，60fps）
 *   ./build/example_showcase_window --frames 200  跑 200 帧自动退出
 *   ./build/example_showcase_window --dump a.ppm  最后一帧存成 PPM（CI/文档用）
 *
 * 按键：空格 = 重播，Esc = 退出。鼠标移到 1 号块上可以看到 hover 过渡。
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <qlgl/qlgl.h>

#include "ui_flex_showcase.h"

#define WIN_W 520
#define WIN_H 400

#if !defined(QLGL_ENABLE_GL)
int main(void)
{
    printf("未编译 GL 后端（-DQLGL_BUILD_GL=ON 需要 EGL + GLESv2）\n");
    return 0;
}
#else

static QLGfxGL *s_gfx;
static QLUIRoot *s_ui;

static int dump_ppm(QLGfxGL *gfx, const char *path, int w, int h)
{
    uint8_t *rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
    if (rgba == NULL)
        return 0;
    if (ql_gfx_gl_read_pixels(gfx, rgba, (uint32_t)w * 4u) != QL_OK) {
        free(rgba);
        return 0;
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        free(rgba);
        return 0;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++)
        fwrite(rgba + (size_t)i * 4u, 1, 3, f);
    fclose(f);
    free(rgba);
    return 1;
}

static void sleep_us(long us)
{
    if (us <= 0)
        return;
    struct timespec ts;
    ts.tv_sec = us / 1000000L;
    ts.tv_nsec = (us % 1000000L) * 1000L;
    nanosleep(&ts, NULL);
}

/* 单调微秒，用来给每帧补到 ~16.6ms（窗口没有 vsync 时别让主循环空转） */
static long now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000000L + t.tv_nsec / 1000L;
}

static void restart_all(void)
{
    for (uint16_t i = 0; i < s_ui->node_count; i++)
        (void)ql_ui_anim_restart(s_ui, &s_ui->nodes[i]);
}

static void showcase_sink(const QLInputEvent *ev, void *user)
{
    if (ev->type == QL_INPUT_KEY_DOWN && ev->key == QL_KEY_ESCAPE) {
        if (s_gfx != NULL)
            ql_gfx_gl_close(s_gfx);
        return;
    }
    if (ev->type == QL_INPUT_KEY_DOWN && ev->key == QL_KEY_SPACE) {
        restart_all();
        return;
    }
    ql_ui_input_event((QLUIRoot *)user, ev);
}

int main(int argc, char **argv)
{
    long max_frames = 0;
    const char *dump_path = NULL;
    int dpr = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            max_frames = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--dpr") == 0 && i + 1 < argc)
            dpr = (int)strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
            dump_path = argv[++i];
    }
    if (dpr < 1)
        dpr = 1;
    if (dpr > 4)
        dpr = 4;

    if (qlgl_init() != QL_OK) {
        printf("qlgl_init 失败\n");
        return 1;
    }

    QLGfxGL *gfx = ql_gfx_gl_create("QLGL M5.5 效果总览", WIN_W * dpr, WIN_H * dpr,
                                    QL_GFX_GL_WINDOW);
    if (gfx == NULL) {
        printf("GL 后端创建失败（无 EGL/X11/GLES3）\n");
        qlgl_shutdown();
        return 1;
    }
    ql_gfx_gl_set_scale(gfx, dpr * QL_SCALE_ONE);
    s_gfx = gfx;

    /*
     * 这个窗口**每帧整屏重绘**：毛玻璃要读"背后已经画好的内容"，
     * 而脏区模式下每块脏区都要单独走一遍模糊（一次 blit + 两次全屏 pass），
     * 又因为要读回帧缓冲而打断批次——比老老实实整屏画还慢，而且会一顿一顿的。
     * 所以这里不打开保留式脏区，也不看 ql_ui_consume_dirty。
     */

    {
        const char *env = getenv("QLGL_FONT");
        const char *cands[] = {
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        };
        if (env != NULL && ql_font_set_default_file(env, -1))
            printf("字体：%s\n", env);
        else
            for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++)
                if (ql_font_set_default_file(cands[i], -1)) {
                    printf("字体：%s\n", cands[i]);
                    break;
                }
    }

    QLUIRoot *ui = ql_ui_build(&ui_flex_showcase_doc);
    if (ui == NULL) {
        printf("构建界面失败\n");
        ql_gfx_gl_destroy(gfx);
        qlgl_shutdown();
        return 1;
    }
    s_ui = ui;
    ql_ui_layout(ui, WIN_W, WIN_H);
    ql_input_set_sink(showcase_sink, ui);

    QLDraw2D *d2d = ql_gfx_gl_draw(gfx);

    printf("窗口 %dx%d 设备像素，DPR %d，%u 段动画 / %u 帧；空格重播，Esc 退出\n",
           WIN_W * dpr, WIN_H * dpr, dpr, (unsigned)ui_flex_showcase_doc.anim_count,
           (unsigned)ui_flex_showcase_doc.keyframe_count);

    /*
     * 帧率上限：窗口不一定有 vsync，主循环会跑到几百 fps（白烧 CPU）。
     * 这里按单调时钟把每帧补到 ~16.6ms。
     */
    const long target_us = 16666;

    long frame = 0;
    long drawn_frames = 0;
    long frames_since = 0;
    long t_report_us = now_us();
    int running = 1;

    while (running) {
        const long t_begin = now_us();

        running = ql_gfx_gl_poll(gfx);
        if (!running)
            break;

        ql_input_dispatch();
        if (!running)
            break;

        const uint32_t moving = ql_ui_anim_tick(ui, ql_anim_now_ms());

        /*
         * 整屏重绘：一个 draw call 走完（毛玻璃会额外 flush 一次 → 2 个）。
         * 先把上帧的脏区取走，避免它们无限堆积（这里不用，但清了更省事）。
         */
        {
            QLRect trash[16];
            (void)ql_ui_consume_dirty(ui, trash, 16);
        }

        ql_gfx_gl_begin(gfx, QL_COLOR_RGB(0x0B, 0x0E, 0x13));
        ql_ui_render(ui, d2d);
        ql_gfx_gl_end(gfx);
        drawn_frames++;
        frame++;

        frames_since++;
        const long t_used = now_us() - t_begin;
        sleep_us(target_us - t_used);

        const long t_now = now_us();
        if (t_now - t_report_us >= 1000000L) {
            const double secs = (double)(t_now - t_report_us) / 1000000.0;
            printf("%.1f fps | frame %ld | %u 段动画在跑 | %u draw call | %u 顶点\n",
                   (double)frames_since / secs, frame, (unsigned)moving,
                   ql_gfx_gl_draw_calls(gfx), ql_gfx_gl_vertex_count(gfx));
            t_report_us = t_now;
            frames_since = 0;
        }

        if (max_frames > 0 && frame >= max_frames)
            break;
    }

    if (dump_path != NULL) {
        if (dump_ppm(gfx, dump_path, WIN_W * dpr, WIN_H * dpr))
            printf("已写出 %s\n", dump_path);
        else
            printf("写出 %s 失败\n", dump_path);
    }

    printf("共 %ld 帧，其中重绘 %ld 帧\n", frame, drawn_frames);
    ql_input_set_sink(NULL, NULL);
    ql_ui_free(ui);
    ql_gfx_gl_destroy(gfx);
    qlgl_shutdown();
    return 0;
}
#endif
