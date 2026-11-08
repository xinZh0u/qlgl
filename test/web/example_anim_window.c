/**
 * @file    example_anim_window.c
 * @brief   动效演示窗口：CSS @keyframes + transform，实时看到滑入/脉冲/提示闪烁。
 *
 * 运行：
 *   ./build/example_anim_window              打开窗口（vsync，60fps）
 *   ./build/example_anim_window --frames 600 跑 600 帧自动退出（适合截图/CI）
 *   ./build/example_anim_window --dpr 2      2x 设备像素比
 *
 * 按键：空格 = 重播全部动画，Esc = 退出。
 *
 * 这个程序同时是"动画 → 脏区 → 局部重绘"这条链路的活样例：
 * 每帧只重绘 `ql_ui_consume_dirty` 给出的那几块，没有动画时直接跳过整帧。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <qlgl/qlgl.h>

#include "ui_flex_anim.h"

#define WIN_W 360
#define WIN_H 260

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

/* 把当前 framebuffer 存成 PPM（不依赖任何截图工具，CI/文档用） */
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

/* 重播所有动画（对没有动画的节点返回 QL_ERR_INVAL，这里直接忽略） */
static void restart_all(void)
{
    for (uint16_t i = 0; i < s_ui->node_count; i++)
        (void)ql_ui_anim_restart(s_ui, &s_ui->nodes[i]);
}

static void anim_sink(const QLInputEvent *ev, void *user)
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

    QLGfxGL *gfx = ql_gfx_gl_create("QLGL 动效演示", WIN_W * dpr, WIN_H * dpr,
                                    QL_GFX_GL_WINDOW);
    if (gfx == NULL) {
        printf("GL 后端创建失败（无 EGL/X11/GLES3）\n");
        qlgl_shutdown();
        return 1;
    }
    ql_gfx_gl_set_scale(gfx, dpr * QL_SCALE_ONE);
    s_gfx = gfx;

    /*
     * 保留式脏区：窗口是双缓冲，交换后后缓冲内容不保证保留，所以只重画脏区
     * 必须先把"上一帧"存在自己的 FBO 里（见 ql_gfx_gl_set_retained 的注释）。
     */
    if (ql_gfx_gl_set_retained(gfx, 1) != QL_OK)
        printf("注意：保留式脏区不可用，退回每帧整屏重绘\n");

    /* 有矢量字体就用它（中文 + 任意字号） */
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

    QLUIRoot *ui = ql_ui_build(&ui_flex_anim_doc);
    if (ui == NULL) {
        printf("构建界面失败\n");
        ql_gfx_gl_destroy(gfx);
        qlgl_shutdown();
        return 1;
    }
    s_ui = ui;
    ql_ui_layout(ui, WIN_W, WIN_H);
    ql_input_set_sink(anim_sink, ui);

    QLDraw2D *d2d = ql_gfx_gl_draw(gfx);

    printf("窗口 %dx%d 设备像素，DPR %d，%u 个动画段 / %u 帧；空格重播，Esc 退出\n",
           WIN_W * dpr, WIN_H * dpr, dpr, (unsigned)ui_flex_anim_doc.anim_count,
           (unsigned)ui_flex_anim_doc.keyframe_count);

    long frame = 0;
    long drawn_frames = 0;      /* 真正重绘过的帧数（静止时应远小于 frame） */
    uint32_t last_dirty_rects = 0;
    int running = 1;

    while (running) {
        running = ql_gfx_gl_poll(gfx);
        if (!running)
            break;

        ql_input_dispatch();
        if (!running)
            break;

        /*
         * 1. 推进动画。返回值 0 = 这一帧没有任何东西在动
         *    （本页面有 infinite 动画，所以几乎总是非 0）。
         */
        const uint32_t moving = ql_ui_anim_tick(ui, ql_anim_now_ms());

        /* 2. 取走脏区；没有脏区就不提交这一帧 */
        QLRect areas[16];
        int n = ql_ui_consume_dirty(ui, areas, 16);

        if (n == 0 && frame > 0) {
            /* 静止：不提交、不重绘，只等一小会儿（有 vsync 时靠驱动限速） */
            sleep_us(4000);
        } else {
            /*
             * 只清 + 重画脏区。返回 1 表示这一帧必须整屏重画
             * （首帧 / 窗口改尺寸 / 保留模式不可用），这时忽略 areas。
             */
            int full = ql_gfx_gl_begin_rects(gfx, QL_COLOR_RGB(0x08, 0x0A, 0x10),
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

        last_dirty_rects = (uint32_t)n;
        frame++;

        if (frame % 60 == 0) {
            printf("frame %ld | %u 段动画在跑 | %u 块脏区 | %u 顶点 | %u draw call\n",
                   frame, (unsigned)moving, (unsigned)last_dirty_rects,
                   ql_gfx_gl_vertex_count(gfx), ql_gfx_gl_draw_calls(gfx));
        }

        if (max_frames > 0 && frame >= max_frames)
            break;
    }

    if (dump_path != NULL) {
        if (dump_ppm(gfx, dump_path, WIN_W * dpr, WIN_H * dpr))
            printf("已保存 %s（%dx%d）\n", dump_path, WIN_W * dpr, WIN_H * dpr);
        else
            printf("保存 %s 失败\n", dump_path);
    }
    printf("共 %ld 帧，其中重绘 %ld 帧\n", frame, drawn_frames);

    s_ui = NULL;
    ql_ui_free(ui);
    ql_gfx_gl_destroy(gfx);
    qlgl_shutdown();
    return 0;
}
#endif
