/**
 * @file    bench_gl.c
 * @brief   GPU（EGL / GLES3）后端的可视化 benchmark。
 *
 *   ./build/benchmark/bench_gl                    # 开窗口，左上角实时帧率
 *   ./build/benchmark/bench_gl --vsync 0          # 不限帧，测吞吐上限
 *   ./build/benchmark/bench_gl --offscreen --frames 300   # pbuffer 离屏
 *   ./build/benchmark/bench_gl --retained 0       # 关掉保留式 FBO（默认开）
 *
 * 三个数字要分清：
 *   render  begin → end 的 CPU 侧提交时间（顶点攒完 + 上传 + gl 调用）
 *   frame   含 eglSwapBuffers 的整帧 —— 开了 vsync 就是 16.7ms，那是等出来的
 *   fps     滑动窗口平均，`--vsync 0` 下才代表真实吞吐
 *
 * 保留式 FBO 默认**开**：真实应用都该开（帧末整块 blit 到窗口），
 * 关掉能少一次全屏拷贝，对比一下就知道它的成本。离屏模式用不上它。
 */
#include "bench_common.h"

#if !defined(QLGL_ENABLE_GL)

int main(void)
{
    printf("bench_gl：本次构建没有 GL 后端（-DQLGL_BUILD_GL=OFF）\n");
    return 1;
}

#else

#include <qlgl/qlgfx_gl.h>

int main(int argc, char **argv)
{
    BenchOpts o;
    bench_opts_parse(&o, argc, argv);

    int retained = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--retained") == 0 && i + 1 < argc)
            retained = (int)strtol(argv[++i], NULL, 10) != 0;
    }

    if (qlgl_init() != QL_OK)
        return 1;
    ex_setup_locale();
    ex_load_font();

    QLUIDoc *doc = bench_build_scene(o.cards, o.cjk);
    if (doc == NULL) {
        qlgl_shutdown();
        return 1;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }

    const uint32_t flags = o.offscreen ? QL_GFX_GL_OFFSCREEN : QL_GFX_GL_WINDOW;
    QLGfxGL *gfx = ql_gfx_gl_create("QLGL bench · GL", o.w, o.h, flags);
    if (gfx == NULL) {
        printf("创建 GL 上下文失败（无 EGL / 无 GLES3？）\n");
        ql_ui_free(ui);
        ql_ui_doc_free(doc);
        qlgl_shutdown();
        return 1;
    }

    if (!o.offscreen) {
        ql_gfx_gl_set_vsync(gfx, o.vsync);
        if (retained && ql_gfx_gl_set_retained(gfx, 1) != QL_OK)
            printf("（保留式 FBO 创建失败，退回每帧整屏清）\n");
    } else {
        o.frames = (o.frames > 0) ? o.frames : 300;   /* 离屏没有"关窗口" */
    }

    /* GL 后端不注册 QLDispDriver，尺寸只能问它自己（返回值是 CSS px） */
    ql_coord_t cw = o.w, ch = o.h;
    ql_gfx_gl_size(gfx, &cw, &ch);
    ql_ui_layout(ui, cw, ch);
    ql_ui_attach_input(ui);
    if (!o.offscreen)
        ql_gfx_gl_attach_clipboard(gfx, ui);

    QLDraw2D *d2d = ql_gfx_gl_draw(gfx);
    const ql_color_t bg = ex_clear_color(ui);

    BenchFps fps;
    bench_fps_init(&fps);
    BenchHudInfo info;
    memset(&info, 0, sizeof(info));
    info.backend = "GL / EGL";
    info.detail = o.offscreen
                      ? "pbuffer 离屏"
                      : (retained ? "窗口 + 保留式 FBO" : "窗口（无保留式 FBO）");
    info.nodes = (int)ui->node_count;

    printf("GL benchmark：%dx%d，%d 张卡片，%d 个节点，vsync %s，%s\n",
           (int)cw, (int)ch, o.cards, (int)ui->node_count,
           o.vsync ? "开" : "关",
           o.offscreen ? "离屏（无 FBO）" : (retained ? "保留式 FBO" : "无 FBO"));

    const double t_start = bench_now_ms();
    long frames = 0;
    while (ql_gfx_gl_poll(gfx)) {
        if (ql_gfx_gl_resize_pending(gfx)) {
            ql_gfx_gl_size(gfx, &cw, &ch);
            if (cw > 0 && ch > 0)
                ql_ui_layout(ui, cw, ch);
        }
        ql_input_dispatch();
        (void)ql_ui_anim_tick(ui, ql_anim_now_ms());

        const double f0 = bench_now_ms();
        ql_gfx_gl_begin(gfx, bg);
        ql_ui_render(ui, d2d);
        if (o.hud)
            bench_hud_draw(d2d, cw, ch, &fps, &info);
        ql_gfx_gl_end(gfx);
        const double f1 = bench_now_ms();

        frames++;
        info.busy_ms = (double)ql_gfx_gl_frame_us(gfx) / 1000.0;
        info.verts = ql_gfx_gl_vertex_count(gfx);
        info.draws = ql_gfx_gl_draw_calls(gfx);
        bench_fps_add(&fps, f1 - f0);

        if (bench_should_stop(&o, frames, t_start))
            break;
        bench_limit_fps(o.fps_cap, f0);
    }

    /* 离屏时测的是"提交"时间，GPU 可能还在跑；finish 一次才是真实单帧成本 */
    if (o.offscreen)
        ql_gfx_gl_finish(gfx);

    if (o.dump != NULL) {
        const size_t stride = (size_t)o.w * 4u;
        uint8_t *rgba = (uint8_t *)malloc(stride * (size_t)o.h);
        if (rgba != NULL && ql_gfx_gl_read_pixels(gfx, rgba, (uint32_t)stride) == QL_OK) {
            FILE *f = fopen(o.dump, "wb");
            if (f != NULL) {
                fprintf(f, "P6\n%d %d\n255\n", (int)o.w, (int)o.h);
                for (int y = 0; y < (int)o.h; y++)
                    for (int x = 0; x < (int)o.w; x++)
                        fwrite(rgba + (size_t)y * stride + (size_t)x * 4, 1, 3, f);
                fclose(f);
                printf("已写出 %s\n", o.dump);
            }
        }
        free(rgba);
    }

    bench_report("QLGL benchmark · GL", &o, &fps, &info);

    ql_ui_detach_input();
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    ql_gfx_gl_destroy(gfx);
    qlgl_shutdown();
    return 0;
}

#endif /* QLGL_ENABLE_GL */
