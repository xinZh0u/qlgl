/**
 * @file    example_gl_window.c
 * @brief   GPU（GLES3/EGL）窗口示例：HTML/CSS 编译出的界面 + 实时动画。
 *
 * 运行：
 *   ./build/example_gl_window                 打开 GL 窗口（vsync，通常 60fps）
 *   ./build/example_gl_window --frames 300    跑 300 帧自动退出
 *   ./build/example_gl_window --bench 600     关闭 vsync + 不休眠，测真实上限
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <qlgl/qlgl.h>

#include "ui_demo.h"

#define WIN_W 320
#define WIN_H 320

#if !defined(QLGL_ENABLE_GL)
int main(void)
{
    printf("未编译 GL 后端（-DQLGL_BUILD_GL=ON 需要 EGL + GLESv2）\n");
    return 0;
}
#else

#if defined(__linux__)
extern int nanosleep(const struct timespec *req, struct timespec *rem);
extern int clock_gettime(int clk_id, struct timespec *tp);
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#endif

static void sleep_us(long us)
{
    struct timespec ts;
    ts.tv_sec = us / 1000000L;
    ts.tv_nsec = (us % 1000000L) * 1000L;
    nanosleep(&ts, NULL);
}

/* ---- 输入：Esc 退出，其余转给 QLUI ---- */
static QLGfxGL *s_gfx;

static void demo_sink(const QLInputEvent *ev, void *user)
{
    if (ev->type == QL_INPUT_KEY_DOWN && ev->key == QL_KEY_ESCAPE) {
        if (s_gfx != NULL)
            ql_gfx_gl_close(s_gfx);
        return;
    }
    ql_ui_input_event((QLUIRoot *)user, ev);
}

int main(int argc, char **argv)
{
    long max_frames = 0;
    long bench_frames = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            max_frames = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc)
            bench_frames = strtol(argv[++i], NULL, 10);
    }
    const int bench = (bench_frames > 0);

    if (qlgl_init() != QL_OK) {
        printf("qlgl_init 失败\n");
        return 1;
    }

    QLGfxGL *gfx = ql_gfx_gl_create("QLGL GPU Demo", WIN_W, WIN_H, QL_GFX_GL_WINDOW);
    if (gfx == NULL) {
        printf("GL 后端创建失败（无 EGL/X11/GLES3）\n");
        qlgl_shutdown();
        return 1;
    }
    s_gfx = gfx;
    printf("GL 后端就绪 %dx%d\n", WIN_W, WIN_H);

    /* 有矢量字体就用它（中文 + 任意字号），没有自动回落内置位图字体 */
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

    QLUIRoot *ui = ql_ui_build(&ui_demo_doc);
    if (ui == NULL) {
        printf("构建界面失败\n");
        ql_gfx_gl_destroy(gfx);
        qlgl_shutdown();
        return 1;
    }
    ql_ui_layout(ui, WIN_W, WIN_H);

    /* 事件：Esc 关窗，其余交给 QLUI（驱动 :hover/:active + click_cb） */
    ql_input_set_sink(demo_sink, ui);

    if (bench) {
        ql_gfx_gl_set_vsync(gfx, 0);      /* 关掉垂直同步，测上限 */
        max_frames = bench_frames;
    }

    QLDraw2D *d2d = ql_gfx_gl_draw(gfx);

    long frame = 0;
    double frame_time_sum = 0;
    long frame_time_n = 0;
    int running = 1;

    struct timespec wall0;
    clock_gettime(CLOCK_MONOTONIC, &wall0);

    while (running) {
        running = ql_gfx_gl_poll(gfx);
        if (!running)
            break;

        ql_input_dispatch();
        if (!running)
            break;

        /* 每 5 帧改一次文本，证明运行期改内容 + GPU 重绘 */
        if (frame % 5 == 0) {
            QLUINodeRT *pct = NULL;
            char buf[32];
            for (uint16_t i = 0; i < ui->node_count; i++) {
                if (strncmp(ql_ui_node_text(&ui->nodes[i]), "50% width box", 5) == 0) {
                    pct = &ui->nodes[i];
                    break;
                }
            }
            if (pct != NULL) {
                snprintf(buf, sizeof(buf), "frame %ld", frame);
                ql_ui_set_text(ui, pct, buf);
            }
        }

        ql_gfx_gl_begin(gfx, QL_COLOR_RGB(0x08, 0x0A, 0x10));

        /* 1. 界面（HTML/CSS 编译而来） */
        ql_ui_render(ui, d2d);

        /* 2. 直接用绘制接口画一个动画条，证明 GPU 路径可用 */
        ql_coord_t bar_w = (ql_coord_t)((frame % 120) * 2);
        if (bar_w > WIN_W - 40)
            bar_w = WIN_W - 40;
        d2d->fill_round_rect(d2d, ql_rect_make(20, WIN_H - 16, WIN_W - 40, 8), 4,
                             QL_COLOR_RGB(0x2E, 0x38, 0x4A), 255);
        d2d->fill_round_rect(d2d, ql_rect_make(20, WIN_H - 16, bar_w, 8), 4,
                             QL_COLOR_RGB(0xF0, 0xB0, 0x30), 255);

        ql_gfx_gl_end(gfx);

        frame_time_sum += (double)ql_gfx_gl_frame_us(gfx);
        frame_time_n++;
        if (frame % 60 == 0) {
            printf("frame %ld | %.0f us/帧 | %u 顶点 | %u draw call\n",
                   frame, frame_time_sum / (double)frame_time_n,
                   ql_gfx_gl_vertex_count(gfx), ql_gfx_gl_draw_calls(gfx));
            frame_time_sum = 0;
            frame_time_n = 0;
        }

        frame++;
        if (max_frames > 0 && frame >= max_frames)
            break;

        if (!bench)
            sleep_us(16666);      /* 约 60 fps */
    }

    struct timespec wall1;
    clock_gettime(CLOCK_MONOTONIC, &wall1);
    double elapsed = (double)(wall1.tv_sec - wall0.tv_sec) +
                     (double)(wall1.tv_nsec - wall0.tv_nsec) / 1e9;
    if (elapsed > 0.0 && frame > 0) {
        printf("共 %ld 帧，墙钟 %.3f s → %.1f fps（%.3f ms/帧）%s\n",
               frame, elapsed, (double)frame / elapsed,
               elapsed * 1000.0 / (double)frame,
               bench ? " [无 vsync 上限模式]" : " [vsync]");
    } else {
        printf("共 %ld 帧\n", frame);
    }
    ql_ui_free(ui);
    ql_gfx_gl_destroy(gfx);
    qlgl_shutdown();
    return 0;
}
#endif
