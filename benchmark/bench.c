/**
 * @file    bench.c
 * @brief   可视化性能基准：**一个程序、两个后端、多个维度**。
 *
 *   ./build/benchmark/bench                        # 默认：GL（有的话）+ 全部维度
 *   ./build/benchmark/bench --backend cpu          # 换成 CPU 光栅，同一套场景
 *   ./build/benchmark/bench --scene cards,anim     # 只跑这两个维度
 *   ./build/benchmark/bench --mode dirty           # 只重绘脏区（增量）
 *   ./build/benchmark/bench --unlock-fps           # 解除刷新率上限，看真实吞吐
 *   ./build/benchmark/bench --offscreen --frames 60  # 不开窗口（CI）
 *
 * 三层结构，互不认识：
 *   bench.c          循环 + 后端接口（BenchBackend）
 *   bench_scenes.h   维度表（测什么）—— 加维度只动这里
 *   bench_common.h   命令行 + 统计 + HUD + 结果表
 *
 * 屏幕刷新率是墙：GL 窗口开 vsync 时帧率永远停在 60/144，那不是性能。
 * `--unlock-fps`（= `--vsync 0 --fps-cap 0`）才能看到真实吞吐。
 */
/* 必须在任何系统头之前定义（clock_gettime / nanosleep） */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <locale.h>

#include "bench_scenes.h"

#if defined(QLGL_ENABLE_GL)
#include <qlgl/qlgfx_gl.h>
#endif
#if defined(QLGL_ENABLE_X11)
#include <qlgl/qldisp_x11.h>
#endif
#if defined(QLGL_ENABLE_WAYLAND)
#include <qlgl/qldisp_wayland.h>
#endif

/* ---------------------------- 后端接口 ---------------------------- */

/*
 * 两个后端（CPU canvas / GL）在"怎么清屏、怎么提交、谁提供 QLDraw2D"
 * 这几件事上不同，其余全部共用。把差异收敛到这里，
 * 上面的循环就与后端无关了。
 */
typedef struct BenchBackend BenchBackend;

struct BenchBackend {
    const char *name;    /* "CPU / QLCanvas" */
    const char *detail;  /* "Wayland 窗口" 等 */
    ql_coord_t  cw, ch;  /* CSS px */
    int         can_dirty;
    int         can_vsync;   /* 只有 GL 窗口有 vsync */

    int  (*poll)(BenchBackend *b);                        /* 1 = 继续 */
    /* 表面尺寸变了就重建画布；返回 1 = 建过了（调用方随后要重排） */
    int  (*refresh_surface)(BenchBackend *b);
    void (*layout_size)(BenchBackend *b);                 /* 刷新 cw/ch（CSS px） */
    void (*begin)(BenchBackend *b, ql_color_t bg);        /* 整屏清 */
    /* 只清这几块；返回 1 表示调用方必须整屏重画（首帧/FBO 刚建/不支持局部） */
    int  (*begin_rects)(BenchBackend *b, ql_color_t bg, const QLRect *r, int n);
    QLDraw2D *(*draw)(BenchBackend *b);
    void (*end)(BenchBackend *b);
    unsigned (*verts)(BenchBackend *b);
    unsigned (*draws)(BenchBackend *b);
    double   (*busy_ms)(BenchBackend *b);
    /*
     * 把**最后一帧**读回成 RGB（w*h*3）。返回 0 = 这个后端不支持读回。
     * 自检（--check）用它，**不能再建第二个 canvas draw2d** ——
     * `ql_draw2d_canvas_init()` 背后是一份静态上下文，建第二个会把
     * 正在用的那个 d2d 的 canvas 指针换掉（之后逐帧崩）。
     */
    int  (*snapshot)(BenchBackend *b, uint8_t *rgb, int w, int h);
    void (*destroy)(BenchBackend *b);
};

/* ------------------------------ CPU 后端 ------------------------------ */

typedef struct {
    BenchBackend  base;
    QLDispDriver *disp;
    QLCanvas      canvas;
    QLDraw2D      d2d;
    double        t0;
    double        busy;
    int           kind;   /* 0=内存 1=wayland 2=x11 */
} CpuBackend;

static int cpu_poll(BenchBackend *bb)
{
    CpuBackend *b = (CpuBackend *)bb;
    switch (b->kind) {
#if defined(QLGL_ENABLE_WAYLAND)
    case 1: return ql_disp_wayland_poll(b->disp);
#endif
#if defined(QLGL_ENABLE_X11)
    case 2: return ql_disp_x11_poll(b->disp);
#endif
    default: return 1;
    }
}

static void cpu_layout_size(BenchBackend *bb)
{
    CpuBackend *b = (CpuBackend *)bb;
    bb->cw = ql_disp_css_width(b->disp);
    bb->ch = ql_disp_css_height(b->disp);
}

/* X11 的帧缓冲在 ConfigureNotify 里已经重建了，画布是它的拷贝，得重新取 */
static int cpu_refresh_surface(BenchBackend *bb)
{
    CpuBackend *b = (CpuBackend *)bb;
#if defined(QLGL_ENABLE_X11)
    if (b->kind == 2 && ql_disp_x11_resize_pending(b->disp)) {
        if (ql_canvas_from_disp(&b->canvas, b->disp) != QL_OK)
            return 0;
        if (ql_draw2d_canvas_init(&b->d2d, &b->canvas) != QL_OK)
            return 0;
        cpu_layout_size(bb);
        return 1;
    }
#endif
    return 0;
}

static void cpu_begin(BenchBackend *bb, ql_color_t bg)
{
    CpuBackend *b = (CpuBackend *)bb;
    b->t0 = bench_now_ms();
    ql_canvas_clear(&b->canvas, bg);
}

static int cpu_begin_rects(BenchBackend *bb, ql_color_t bg, const QLRect *r, int n)
{
    CpuBackend *b = (CpuBackend *)bb;
    b->t0 = bench_now_ms();
    if (n <= 0) {
        /* 没有脏区：什么都不清，调用方也不该画（返回 0 = 不是整屏） */
        return 0;
    }
    for (int i = 0; i < n; i++)
        ql_canvas_fill_rect(&b->canvas, r[i], bg);
    return 0;
}

static QLDraw2D *cpu_draw(BenchBackend *bb)
{
    return &((CpuBackend *)bb)->d2d;
}

static void cpu_end(BenchBackend *bb)
{
    CpuBackend *b = (CpuBackend *)bb;
    if (b->disp->flush != NULL)
        b->disp->flush(b->disp, ql_rect_make(0, 0, b->disp->width, b->disp->height));
    if (b->disp->flush_done != NULL)
        b->disp->flush_done(b->disp);
    b->busy = bench_now_ms() - b->t0;
}

static int cpu_snapshot(BenchBackend *bb, uint8_t *rgb, int w, int h)
{
    CpuBackend *b = (CpuBackend *)bb;
    if (b->kind != 0)
        return 0;      /* 窗口驱动没有读回接口：自检退化成"跳过" */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const ql_color_t c = ql_disp_mem_pixel(b->disp, x, y);
            uint8_t *p = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3u;
            p[0] = (uint8_t)QL_COLOR_GET_R(c);
            p[1] = (uint8_t)QL_COLOR_GET_G(c);
            p[2] = (uint8_t)QL_COLOR_GET_B(c);
        }
    return 1;
}

static double cpu_busy_ms(BenchBackend *bb)
{
    return ((CpuBackend *)bb)->busy;
}

static void cpu_destroy(BenchBackend *bb)
{
    CpuBackend *b = (CpuBackend *)bb;
    switch (b->kind) {
#if defined(QLGL_ENABLE_WAYLAND)
    case 1: ql_disp_wayland_destroy(b->disp); break;
#endif
#if defined(QLGL_ENABLE_X11)
    case 2: ql_disp_x11_destroy(b->disp);     break;
#endif
    default: ql_disp_mem_destroy(b->disp);    break;
    }
    free(b);
}

static BenchBackend *cpu_backend_create(const BenchOpts *o)
{
    CpuBackend *b = (CpuBackend *)calloc(1, sizeof(*b));
    if (b == NULL)
        return NULL;

    const char *detail = "内存显示（无窗口）";
    if (!o->offscreen) {
#if defined(QLGL_ENABLE_WAYLAND)
        b->disp = ql_disp_wayland_create("QLGL bench · CPU", o->w, o->h,
                                        QL_COLOR_FORMAT_AUTO);
        if (b->disp != NULL) {
            b->kind = 1;
            detail = "Wayland 窗口";
        }
#endif
#if defined(QLGL_ENABLE_X11)
        if (b->disp == NULL) {
            b->disp = ql_disp_x11_create("QLGL bench · CPU", o->w, o->h,
                                        QL_COLOR_FORMAT_AUTO);
            if (b->disp != NULL) {
                b->kind = 2;
                detail = "X11 窗口";
            }
        }
#endif
    }
    if (b->disp == NULL) {
        b->disp = ql_disp_mem_create(o->w, o->h, QL_COLOR_RGB888);
        if (b->disp == NULL) {
            free(b);
            return NULL;
        }
        printf("（没有窗口，改用内存显示）\n");
    }
    if (ql_canvas_from_disp(&b->canvas, b->disp) != QL_OK ||
        ql_draw2d_canvas_init(&b->d2d, &b->canvas) != QL_OK) {
        cpu_destroy(&b->base);
        return NULL;
    }

    b->base.name = "CPU / QLCanvas";
    b->base.detail = detail;
    b->base.can_dirty = 1;
    b->base.can_vsync = 0;
    b->base.poll = cpu_poll;
    b->base.refresh_surface = cpu_refresh_surface;
    b->base.layout_size = cpu_layout_size;
    b->base.begin = cpu_begin;
    b->base.begin_rects = cpu_begin_rects;
    b->base.draw = cpu_draw;
    b->base.end = cpu_end;
    b->base.verts = NULL;
    b->base.draws = NULL;
    b->base.busy_ms = cpu_busy_ms;
    b->base.snapshot = cpu_snapshot;
    b->base.destroy = cpu_destroy;
    cpu_layout_size(&b->base);
    return &b->base;
}

/* ------------------------------ GL 后端 ------------------------------ */

#if defined(QLGL_ENABLE_GL)

typedef struct {
    BenchBackend base;
    QLGfxGL      *gfx;
    QLDraw2D     *d2d;
} GlBackend;

static int gl_poll(BenchBackend *bb)
{
    GlBackend *b = (GlBackend *)bb;
    if (ql_gfx_gl_resize_pending(b->gfx))
        ql_gfx_gl_size(b->gfx, &bb->cw, &bb->ch);
    return ql_gfx_gl_poll(b->gfx);
}

static void gl_layout_size(BenchBackend *bb)
{
    ql_gfx_gl_size(((GlBackend *)bb)->gfx, &bb->cw, &bb->ch);
}

static void gl_begin(BenchBackend *bb, ql_color_t bg)
{
    ql_gfx_gl_begin(((GlBackend *)bb)->gfx, bg);
}

static int gl_begin_rects(BenchBackend *bb, ql_color_t bg, const QLRect *r, int n)
{
    return ql_gfx_gl_begin_rects(((GlBackend *)bb)->gfx, bg, r, n);
}

static QLDraw2D *gl_draw(BenchBackend *bb)
{
    return ((GlBackend *)bb)->d2d;
}

static void gl_end(BenchBackend *bb)
{
    ql_gfx_gl_end(((GlBackend *)bb)->gfx);
}

static unsigned gl_verts(BenchBackend *bb)
{
    return ql_gfx_gl_vertex_count(((GlBackend *)bb)->gfx);
}

static unsigned gl_draws(BenchBackend *bb)
{
    return ql_gfx_gl_draw_calls(((GlBackend *)bb)->gfx);
}

static double gl_busy_ms(BenchBackend *bb)
{
    return (double)ql_gfx_gl_frame_us(((GlBackend *)bb)->gfx) / 1000.0;
}

/*
 * 读回最后一帧（RGBA → RGB）。离屏 pbuffer 与保留式 FBO 都支持；
 * `ql_gfx_gl_read_pixels` 是同步读，只在自检时调一次，不进热路径。
 */
static int gl_snapshot(BenchBackend *bb, uint8_t *rgb, int w, int h)
{
    GlBackend *b = (GlBackend *)bb;
#if defined(QLGL_ENABLE_GL)
    if (b->gfx == NULL)
        return 0;
    uint8_t *rgba = (uint8_t *)ql_mem_calloc(1, (size_t)w * (size_t)h * 4u);
    if (rgba == NULL)
        return 0;
    if (ql_gfx_gl_read_pixels(b->gfx, rgba, (uint32_t)w * 4u) != QL_OK) {
        ql_mem_free(rgba);
        return 0;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *q = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4u;
            uint8_t *p = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3u;
            p[0] = q[0];
            p[1] = q[1];
            p[2] = q[2];
        }
    ql_mem_free(rgba);
    return 1;
#else
    (void)b; (void)rgb; (void)w; (void)h;
    return 0;
#endif
}

static void gl_destroy(BenchBackend *bb)
{
    GlBackend *b = (GlBackend *)bb;
    if (b->gfx != NULL)
        ql_gfx_gl_destroy(b->gfx);
    free(b);
}

static BenchBackend *gl_backend_create(const BenchOpts *o)
{
    GlBackend *b = (GlBackend *)calloc(1, sizeof(*b));
    if (b == NULL)
        return NULL;

    const uint32_t flags = o->offscreen ? QL_GFX_GL_OFFSCREEN : QL_GFX_GL_WINDOW;
    b->gfx = ql_gfx_gl_create("QLGL bench · GL", o->w, o->h, flags);
    if (b->gfx == NULL) {
        free(b);
        return NULL;
    }
    if (!o->offscreen)
        ql_gfx_gl_set_vsync(b->gfx, o->vsync);
    /* 离屏也开保留式 FBO：这样 CI 里也能测 --mode dirty */
    int retained = o->retained;
    if (retained && ql_gfx_gl_set_retained(b->gfx, 1) != QL_OK) {
        printf("（保留式 FBO 创建失败，退回每帧整屏清）\n");
        retained = 0;
    }
    b->d2d = ql_gfx_gl_draw(b->gfx);

    b->base.name = "GL / EGL";
    b->base.detail = o->offscreen ? "pbuffer 离屏"
                                  : (retained ? "窗口 + 保留式 FBO" : "窗口（无 FBO）");
    /* 局部重绘要保留式 FBO：没有它，swap 之后后缓冲内容没有保证 */
    b->base.can_dirty = retained;
    b->base.can_vsync = !o->offscreen;
    b->base.poll = gl_poll;
    b->base.refresh_surface = NULL;     /* GL 的表面由 begin/end 自己处理 */
    b->base.layout_size = gl_layout_size;
    b->base.begin = gl_begin;
    b->base.begin_rects = gl_begin_rects;
    b->base.draw = gl_draw;
    b->base.end = gl_end;
    b->base.verts = gl_verts;
    b->base.draws = gl_draws;
    b->base.busy_ms = gl_busy_ms;
    b->base.snapshot = gl_snapshot;
    b->base.destroy = gl_destroy;
    gl_layout_size(&b->base);
    return &b->base;
}

#endif /* QLGL_ENABLE_GL */

/* ------------------------------ 跑一个维度 ------------------------------ */

#define MAX_DIRTY 64

static int run_scene(BenchBackend *bk, const BenchScene *sc, const BenchOpts *opts,
                     int items, BenchResult *out)
{
    QLUIDoc *doc = sc->build(opts, items);
    if (doc == NULL)
        return 0;
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return 0;
    }
    ql_ui_layout(ui, bk->cw, bk->ch);
    ql_ui_attach_input(ui);
#if defined(QLGL_ENABLE_GL)
    if (!opts->offscreen && strcmp(bk->name, "GL / EGL") == 0)
        ql_gfx_gl_attach_clipboard(((GlBackend *)bk)->gfx, ui);
#endif

    const ql_color_t bg = 0xFFFFFF;   /* 浅色主题：和场景底色一致 */
    BenchFps fps;
    bench_fps_init(&fps);

    BenchHudInfo info;
    memset(&info, 0, sizeof(info));
    info.backend = bk->name;
    info.scene = sc->id;
    info.detail = bk->detail;
    info.nodes = (int)ui->node_count;

    /* dirty 模式：HUD 自己也是一块每帧都在变的区域 */
    const int use_dirty = (opts->mode == BENCH_MODE_DIRTY) && bk->can_dirty;
    if (opts->mode == BENCH_MODE_DIRTY && !bk->can_dirty)
        printf("（这个后端不支持局部重绘，退回整屏重画）\n");

    printf("\n--- %s（份数 %d，节点 %d）%s%s ---\n", sc->id, items,
           (int)ui->node_count, use_dirty ? " 只重画脏区" : "",
           sc->tick != NULL ? " 含每帧业务动作" : "");

    /*
     * 预热两帧再开始计时：第一帧要建字形图集、上传顶点缓冲、建 FBO，
     * 那是**一次性**成本，混进 p50/min 会把稳态数字糊掉。
     */
    for (int warm = 0; warm < 2; warm++) {
        ql_input_dispatch();
        (void)ql_ui_anim_tick(ui, ql_anim_now_ms());
        if (sc->tick != NULL)
            sc->tick(ui, (uint32_t)warm);
        bk->begin(bk, bg);
        ql_ui_render(ui, bk->draw(bk));
        bk->end(bk);
    }

    long resizes = 0;
    double busy_sum = 0.0;
    unsigned verts_sum = 0, draws_sum = 0;
    long busy_n = 0;
    const double t_start = bench_now_ms();
    long frames = 0;
    ql_coord_t last_w = bk->cw, last_h = bk->ch;
    while (bk->poll(bk)) {
        /*
         * 窗口被拖动缩放：尺寸一变就重排（并重建保留式 FBO、重画整屏）。
         * 漏了这一步，画面会停在旧尺寸上 —— 用户看到的是"半屏白 + 掉帧"。
         */
        if (bk->refresh_surface != NULL)
            (void)bk->refresh_surface(bk);
        bk->layout_size(bk);
        if (bk->cw != last_w || bk->ch != last_h) {
            last_w = bk->cw;
            last_h = bk->ch;
            if (last_w > 0 && last_h > 0) {
                ql_ui_layout(ui, last_w, last_h);
                resizes++;
            }
        }
        /* 整帧从"处理输入"开始算：tick（含动画、滚动重排）也是应用要付的成本 */
        const double f0 = bench_now_ms();
        ql_input_dispatch();
        (void)ql_ui_anim_tick(ui, ql_anim_now_ms());
        if (sc->tick != NULL)
            sc->tick(ui, (uint32_t)frames);

        QLDraw2D *d2d = bk->draw(bk);

        if (!use_dirty) {
            bk->begin(bk, bg);
            ql_ui_render(ui, d2d);
            if (opts->hud)
                bench_hud_draw(d2d, bk->cw, bk->ch, &fps, &info, NULL);
        } else {
            QLRect areas[MAX_DIRTY + 1];
            int n = ql_ui_consume_dirty(ui, areas, MAX_DIRTY);
            if (opts->hud) {
                QLRect hr;
                bench_hud_rect(&hr);
                areas[n++] = hr;
            }
            const int full = bk->begin_rects(bk, bg, areas, n);
            if (full) {
                ql_ui_render(ui, d2d);
                if (opts->hud)
                    bench_hud_draw(d2d, bk->cw, bk->ch, &fps, &info, NULL);
            } else {
                for (int i = 0; i < n; i++)
                    ql_ui_render_rect(ui, d2d, areas[i]);
                if (opts->hud) {
                    QLRect hr;
                    bench_hud_rect(&hr);
                    bench_hud_draw(d2d, bk->cw, bk->ch, &fps, &info, &hr);
                }
            }
        }
        bk->end(bk);
        const double f1 = bench_now_ms();

        frames++;
        info.busy_ms = bk->busy_ms(bk);
        info.verts = (bk->verts != NULL) ? bk->verts(bk) : 0;
        info.draws = (bk->draws != NULL) ? bk->draws(bk) : 0;
        busy_sum += info.busy_ms;
        verts_sum += info.verts;
        draws_sum += info.draws;
        busy_n++;
        bench_fps_add(&fps, f1 - f0);

        int stop = 0;
        if (opts->frames > 0 && frames >= (long)opts->frames)
            stop = 1;
        if (opts->seconds > 0.0 && (bench_now_ms() - t_start) >= opts->seconds * 1000.0)
            stop = 1;
        if (stop)
            break;
        bench_limit_fps(opts->fps_cap, f0);
    }

    if (opts->dump != NULL) {
        char path[512];
        snprintf(path, sizeof(path), "%s.%s.ppm", opts->dump, sc->id);
        QLDispDriver *mem = ql_disp_mem_create(bk->cw, bk->ch, QL_COLOR_RGB888);
        if (mem != NULL) {
            QLCanvas mc;
            if (ql_canvas_from_disp(&mc, mem) == QL_OK) {
                QLDraw2D hd;
                ql_canvas_clear(&mc, bg);
                ql_ui_render_canvas(ui, &mc);
                if (opts->hud && ql_draw2d_canvas_init(&hd, &mc) == QL_OK)
                    bench_hud_draw(&hd, bk->cw, bk->ch, &fps, &info, NULL);
                if (ql_disp_mem_save_ppm(mem, path) == QL_OK)
                    printf("已写出 %s\n", path);
            }
            ql_disp_mem_destroy(mem);
        }
    }

    /*
     * 自检：把**最后一帧**读回来，数一下"非底色采样"和"颜色种类"。
     *
     * 只判"画了东西"这一条，不判具体像素 —— 它要能在任何机器上稳定通过，
     * 目的是拦住"场景静默变成空白"这类功能退化（CI 里跑）。
     * 注意用后端的 snapshot 而不是自己再建一块画布：`ql_draw2d_canvas_init()`
     * 背后是一份静态上下文，建第二个会把正在用的 d2d 换掉（之后逐帧崩）。
     */
    int check_ok = 1;
    if (opts->check) {
        const int cw = (int)bk->cw, chh = (int)bk->ch;
        uint8_t *rgb = (uint8_t *)malloc((size_t)cw * (size_t)chh * 3u);
        if (rgb == NULL) {
            check_ok = 0;
        } else if (bk->snapshot == NULL || !bk->snapshot(bk, rgb, cw, chh)) {
            printf("  [自检跳过] 维度 %s：这个后端不支持读回\n", sc->id);
        } else {
            static uint32_t seen[512];
            int nseen = 0;
            long nonbg = 0;
            const uint32_t bgc = bg & 0xFFFFFFu;
            for (int y = 0; y < chh; y += 4)
                for (int x = 0; x < cw; x += 4) {
                    const uint8_t *p = rgb + ((size_t)y * (size_t)cw + (size_t)x) * 3u;
                    const uint32_t c =
                        ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
                    if (c != bgc)
                        nonbg++;
                    int hit = 0;
                    for (int k = 0; k < nseen; k++)
                        if (seen[k] == c) {
                            hit = 1;
                            break;
                        }
                    if (!hit && nseen < (int)(sizeof(seen) / sizeof(seen[0])))
                        seen[nseen++] = c;
                }
            if (nonbg < 200 || nseen < 8) {
                printf("  [自检失败] 维度 %s 画面几乎是空白"
                       "（非底色采样 %ld 个，颜色种类 %d）\n",
                       sc->id, nonbg, nseen);
                check_ok = 0;
            }
        }
        free(rgb);
    }

    const double busy_avg = (busy_n > 0) ? (busy_sum / (double)busy_n) : 0.0;
    const unsigned verts_avg = (busy_n > 0) ? (unsigned)(verts_sum / (unsigned long)busy_n) : 0;
    const unsigned draws_avg = (busy_n > 0) ? (unsigned)(draws_sum / (unsigned long)busy_n) : 0;
    bench_report_one(bk->name, sc->id, sc->what, opts, items, bk->can_vsync,
                     &fps, &info, busy_avg, verts_avg, draws_avg);
    if (resizes > 0)
        printf("窗口重排  %ld 次（拖动缩放；每次都要重排 + 整屏重画）\n", resizes);

    if (out != NULL) {
        double p50 = 0.0, p95 = 0.0;
        bench_fps_percentiles(&fps, &p50, &p95);
        out->scene = sc->id;
        out->what = sc->what;
        out->items = items;
        out->nodes = (int)ui->node_count;
        out->frames = fps.frames;
        out->fps = bench_fps_value(&fps);
        out->p50 = p50;
        out->p95 = p95;
        out->max_ms = fps.max_ms;
        out->busy_ms = busy_avg;
    }

    ql_ui_detach_input();
    ql_ui_free(ui);
    ql_ui_doc_free(doc);
    return check_ok ? 1 : -1;      /* -1 = 画出来了但自检不过（CI 要红） */
}

/* -------------------------------- main -------------------------------- */

int main(int argc, char **argv)
{
    BenchOpts o;
    bench_opts_parse(&o, argc, argv);

    if (o.list) {
        bench_scene_list();
        return 0;
    }

    if (qlgl_init() != QL_OK)
        return 1;

    /* 字体与 locale：和例程同一套（中文要矢量字体 + setlocale 才有 XIM） */
    setlocale(LC_ALL, "");
    {
        const char *env = getenv("QLGL_FONT");
        int loaded = (env != NULL) && ql_font_set_default_file(env, -1);
        static const char *const cand[] = {
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        };
        for (size_t i = 0; !loaded && i < sizeof(cand) / sizeof(cand[0]); i++)
            loaded = ql_font_set_default_file(cand[i], -1);
        if (!loaded)
            printf("（没找到矢量字体，只用内置 ASCII 位图字体）\n");
    }

    /* ---------- 选后端 ---------- */
    BenchBackend *bk = NULL;
    const int want_gl = (o.backend == NULL) || (strcmp(o.backend, "gl") == 0);
    const int want_cpu = (o.backend == NULL) || (strcmp(o.backend, "cpu") == 0);
#if defined(QLGL_ENABLE_GL)
    if (want_gl)
        bk = gl_backend_create(&o);
#endif
    if (bk == NULL && want_cpu)
        bk = cpu_backend_create(&o);
    if (bk == NULL) {
        printf("没有可用的后端（GL 创建失败，CPU 也失败）\n");
        qlgl_shutdown();
        return 1;
    }
    printf("后端：%s（%s）%s\n", bk->name, bk->detail,
           (o.mode == BENCH_MODE_DIRTY) ? "，只重绘脏区" : "，每帧整屏重画");
    printf("限速：%s\n", bench_limit_desc(&o, bk->can_vsync));

    /* ---------- 选维度 ---------- */
    char scene_buf[256];
    snprintf(scene_buf, sizeof(scene_buf), "%s", o.scenes);
    char *ids[16];
    int nids = 0;
    const int all = (strcmp(scene_buf, "all") == 0);

    BenchResult results[BENCH_SCENE_COUNT];
    int nres = 0;
    int ncheck_fail = 0;

    if (all) {
        for (int i = 0; i < BENCH_SCENE_COUNT; i++) {
            const BenchScene *sc = &k_scenes[i];
            int items = (o.scale > 0) ? o.scale : sc->default_scale;
            if (items > sc->max_scale)
                items = sc->max_scale;
            const int rc = run_scene(bk, sc, &o, items, &results[nres]);
            if (rc != 0)
                nres++;
            if (rc < 0)
                ncheck_fail++;
        }
    } else {
        nids = bench_scene_split(scene_buf, ids, 16);
        for (int i = 0; i < nids; i++) {
            const BenchScene *sc = bench_scene_find(ids[i]);
            if (sc == NULL) {
                printf("未知维度 '%s'，用 --list 看有哪些\n", ids[i]);
                continue;
            }
            int items = (o.scale > 0) ? o.scale : sc->default_scale;
            if (items > sc->max_scale)
                items = sc->max_scale;
            const int rc = run_scene(bk, sc, &o, items, &results[nres]);
            if (rc != 0)
                nres++;
            if (rc < 0)
                ncheck_fail++;
        }
    }

    if (nres > 1)
        bench_report_table(bk->name, (o.mode == BENCH_MODE_DIRTY) ? "dirty" : "full",
                           bench_limit_desc(&o, bk->can_vsync), results, nres);

    bk->destroy(bk);
    qlgl_shutdown();
    if (ncheck_fail > 0)
        printf("\n自检失败 %d 个维度\n", ncheck_fail);
    return (nres > 0 && ncheck_fail == 0) ? 0 : 1;
}
