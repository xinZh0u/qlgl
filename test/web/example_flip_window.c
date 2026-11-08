/**
 * @file    example_flip_window.c
 * @brief   假 3D 翻转演示窗口：纯 2D 的 scaleX/scaleY，看得见"卡片翻面"。
 *
 * 运行：
 *   ./build/example_flip_window               打开窗口（vsync，60fps）
 *   ./build/example_flip_window --frames 200  跑 200 帧自动退出
 *   ./build/example_flip_window --dump a.ppm  最后一帧存成 PPM（CI/文档用）
 *
 * 按键：空格 = 重播，Esc = 退出。
 *
 * 这里没有 3D、没有矩阵栈：`scaleX: 1 → 0 → 1` 就是把元素在水平方向压到零宽再展开。
 * 圆角和文字随之按 min(sx, sy) 收缩（见 QLUI阅读指南 §5.4.3）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <qlgl/qlgl.h>

#include "ui_flex_flipdemo.h"

#define WIN_W 420
#define WIN_H 240

#if !defined(QLGL_ENABLE_GL)
int main(void)
{
    printf("未编译 GL 后端（-DQLGL_BUILD_GL=ON 需要 EGL + GLESv2）\n");
    return 0;
}
#else

static QLGfxGL *s_gfx;
static QLUIRoot *s_ui;

#if defined(__linux__)
extern int nanosleep(const struct timespec *req, struct timespec *rem);
#endif

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
    struct timespec ts;
    ts.tv_sec = us / 1000000L;
    ts.tv_nsec = (us % 1000000L) * 1000L;
    nanosleep(&ts, NULL);
}

static void restart_all(void)
{
    for (uint16_t i = 0; i < s_ui->node_count; i++)
        (void)ql_ui_anim_restart(s_ui, &s_ui->nodes[i]);
}

static void flip_sink(const QLInputEvent *ev, void *user)
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

    QLGfxGL *gfx = ql_gfx_gl_create("QLGL 假翻转演示", WIN_W * dpr, WIN_H * dpr,
                                    QL_GFX_GL_WINDOW);
    if (gfx == NULL) {
        printf("GL 后端创建失败（无 EGL/X11/GLES3）\n");
        qlgl_shutdown();
        return 1;
    }
    ql_gfx_gl_set_scale(gfx, dpr * QL_SCALE_ONE);
    s_gfx = gfx;

    if (ql_gfx_gl_set_retained(gfx, 1) != QL_OK)
        printf("注意：保留式脏区不可用，退回每帧整屏重绘\n");

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

    QLUIRoot *ui = ql_ui_build(&ui_flex_flipdemo_doc);
    if (ui == NULL) {
        printf("构建界面失败\n");
        ql_gfx_gl_destroy(gfx);
        qlgl_shutdown();
        return 1;
    }
    s_ui = ui;
    ql_ui_layout(ui, WIN_W, WIN_H);
    ql_input_set_sink(flip_sink, ui);

    QLDraw2D *d2d = ql_gfx_gl_draw(gfx);

    printf("窗口 %dx%d 设备像素，DPR %d，%u 段动画 / %u 帧；空格重播，Esc 退出\n",
           WIN_W * dpr, WIN_H * dpr, dpr, (unsigned)ui_flex_flipdemo_doc.anim_count,
           (unsigned)ui_flex_flipdemo_doc.keyframe_count);

    long frame = 0;
    long drawn_frames = 0;
    int running = 1;

    while (running) {
        running = ql_gfx_gl_poll(gfx);
        if (!running)
            break;

        ql_input_dispatch();
        if (!running)
            break;

        const uint32_t moving = ql_ui_anim_tick(ui, ql_anim_now_ms());

        QLRect areas[16];
        int n = ql_ui_consume_dirty(ui, areas, 16);

        if (n == 0 && frame > 0) {
            sleep_us(4000);
        } else {
            int full = ql_gfx_gl_begin_rects(gfx, QL_COLOR_RGB(0x0E, 0x11, 0x16),
                                             areas, n);
            if (full) {
                ql_ui_render(ui, d2d);
            } else {
                for (int i = 0; i < n; i++)
                    ql_ui_render_rect(ui, d2d, areas[i]);
            }
            ql_gfx_gl_end(gfx);
            drawn_frames++;
        }

        frame++;

        if (frame % 60 == 0)
            printf("frame %ld | %u 段动画在跑 | %u 块脏区 | %u draw call\n", frame,
                   (unsigned)moving, (unsigned)n, ql_gfx_gl_draw_calls(gfx));

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
