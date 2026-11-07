/**
 * @file    example_gl_window.c
 * @brief   GPU（GLES3/EGL）窗口示例：HTML/CSS 编译出的界面 + 实时动画。
 *
 * 运行：
 *   ./build/example_gl_window              打开 GL 窗口，Esc/q/关闭按钮退出
 *   ./build/example_gl_window --frames 300 跑 300 帧自动退出
 *
 * 按 F 键（或程序内每 60 帧）打印一次帧时间统计。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <qlgl/qlgl.h>

#include "ui_demo.h"

#define WIN_W 320
#define WIN_H 240

#if !defined(QLGL_ENABLE_GL)
int main(void)
{
    printf("未编译 GL 后端（-DQLGL_BUILD_GL=ON 需要 EGL + GLESv2）\n");
    return 0;
}
#else

#if defined(__linux__)
extern int nanosleep(const struct timespec *req, struct timespec *rem);
#endif

static void sleep_us(long us)
{
    struct timespec ts;
    ts.tv_sec = us / 1000000L;
    ts.tv_nsec = (us % 1000000L) * 1000L;
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    long max_frames = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            max_frames = strtol(argv[++i], NULL, 10);
    }

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
    printf("GL 后端就绪 %dx%d\n", WIN_W, WIN_H);

    QLUIRoot *ui = ql_ui_build(&ui_demo_doc);
    if (ui == NULL) {
        printf("构建界面失败\n");
        ql_gfx_gl_destroy(gfx);
        qlgl_shutdown();
        return 1;
    }
    ql_ui_layout(ui, WIN_W, WIN_H);

    QLDraw2D *d2d = ql_gfx_gl_draw(gfx);

    long frame = 0;
    double frame_time_sum = 0;
    long frame_time_n = 0;
    int running = 1;

    while (running) {
        running = ql_gfx_gl_poll(gfx);
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

        sleep_us(16666);      /* 约 60 fps */
    }

    printf("共 %ld 帧\n", frame);
    ql_ui_free(ui);
    ql_gfx_gl_destroy(gfx);
    qlgl_shutdown();
    return 0;
}
#endif
