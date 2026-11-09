/**
 * @file    bench_common.h
 * @brief   benchmark 的公共设施：命令行、计时、帧率统计、HUD、结果表。
 *
 * 与"测什么"（维度）无关 —— 维度在 `bench_scenes.h` 里，一个维度一个
 * `BenchScene` 条目，加新维度只往那张表里加一行。
 * 与后端也无关 —— 后端在 `bench.c` 里收敛成 `BenchBackend` 接口，
 * `--backend cpu|gl` 选。
 */
#ifndef QL_BENCH_COMMON_H
#define QL_BENCH_COMMON_H

#define _POSIX_C_SOURCE 200809L   /* clock_gettime / nanosleep */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <qlgl/qlgl.h>

/* ------------------------------- 时间 ------------------------------- */

static double bench_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ------------------------------ 命令行 ------------------------------ */

/* 重绘策略：整屏 vs 只画脏区 */
typedef enum {
    BENCH_MODE_FULL = 0,   /* 每帧整屏重画：测光栅吞吐（默认） */
    BENCH_MODE_DIRTY = 1,  /* 每帧只清 + 重画脏区：测增量重绘 */
} BenchMode;

typedef struct {
    const char *backend;   /* "cpu" / "gl" / NULL = 自动（有 GL 就用 GL） */
    const char *scenes;    /* "all" 或逗号分隔的维度 id */
    BenchMode   mode;
    int         frames;    /* 每个维度最多跑多少帧（0 = 不限，按时间停） */
    double      seconds;   /* 每个维度跑多少秒（默认 3；<=0 表示不按时间停） */
    ql_coord_t  w, h;
    int         scale;     /* 维度内的"份数"，0 = 用维度自己的默认值 */
    int         hud;
    int         vsync;
    int         offscreen;
    int         retained;
    int         fps_cap;   /* 0 = 不限帧（全速跑吞吐） */
    int         cjk;       /* 1 = 文本用中文（顺带压矢量字体的字形缓存） */
    const char *dump;
    int         list;      /* 只列出维度就退出 */
} BenchOpts;

static void bench_usage(const char *prog)
{
    printf("用法：%s [选项]\n"
           "  --backend cpu|gl   绘制后端（默认 gl；没有 GL 时自动用 cpu）\n"
           "  --scene LIST       'all' 或逗号分隔的维度（默认 all），见 --list\n"
           "  --list             列出所有维度\n"
           "  --mode full|dirty  full=每帧整屏重画（默认），dirty=只画脏区\n"
           "  --scale N          维度内的份数（卡片数/元素数…），0=维度默认\n"
           "  --seconds S        每个维度跑多少**秒**（默认 3；这是主口径）\n"
           "  --frames N         每个维度最多跑 N 帧（默认 0=不限，按时间停）\n"
           "                     两个都是上限，谁先到就停\n"
           "  --size WxH         窗口/画布尺寸（默认 800x480）\n"
           "  --vsync 0|1        GL 垂直同步（默认 1）\n"
           "  --retained 0|1     GL 保留式 FBO（默认 1；--mode dirty 需要它）\n"
           "  --offscreen        不开窗口（CI 友好）\n"
           "  --fps-cap N        限帧（默认 0=全速；N>0 时每帧睡到 1000/N ms）\n"
           "  --unlock-fps       解除刷新率上限（等价 --vsync 0 --fps-cap 0）\n"
           "                     —— 想看超过屏幕帧率的吞吐就用它\n"
           "  --cjk              文本用中文\n"
           "  --no-hud           不画左上角信息\n"
           "  --dump PATH        每个维度退出前导出一帧 PPM（文件名会加维度后缀）\n",
           prog);
}

static void bench_opts_parse(BenchOpts *o, int argc, char **argv)
{
    /* 重定向到文件/管道时 stdout 默认块缓冲：开跑前的几行会憋到退出才出现 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    memset(o, 0, sizeof(*o));
    o->w = 800;
    o->h = 480;
    o->frames = 0;          /* 默认只按时间跑，见 --seconds */
    o->seconds = 3.0;
    o->hud = 1;
    o->vsync = 1;
    o->retained = 1;
    o->scenes = "all";

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const int has_next = (i + 1 < argc);
        if (strcmp(a, "--backend") == 0 && has_next)
            o->backend = argv[++i];
        else if (strcmp(a, "--scene") == 0 && has_next)
            o->scenes = argv[++i];
        else if (strcmp(a, "--list") == 0)
            o->list = 1;
        else if (strcmp(a, "--mode") == 0 && has_next) {
            const char *m = argv[++i];
            o->mode = (strcmp(m, "dirty") == 0) ? BENCH_MODE_DIRTY : BENCH_MODE_FULL;
        } else if (strcmp(a, "--scale") == 0 && has_next)
            o->scale = (int)strtol(argv[++i], NULL, 10);
        else if (strcmp(a, "--frames") == 0 && has_next)
            o->frames = (int)strtol(argv[++i], NULL, 10);
        else if (strcmp(a, "--seconds") == 0 && has_next)
            o->seconds = strtod(argv[++i], NULL);
        else if (strcmp(a, "--size") == 0 && has_next) {
            int w = 0, h = 0;
            if (sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                o->w = (ql_coord_t)w;
                o->h = (ql_coord_t)h;
            }
        } else if (strcmp(a, "--vsync") == 0 && has_next)
            o->vsync = (int)strtol(argv[++i], NULL, 10) != 0;
        else if (strcmp(a, "--retained") == 0 && has_next)
            o->retained = (int)strtol(argv[++i], NULL, 10) != 0;
        else if (strcmp(a, "--offscreen") == 0)
            o->offscreen = 1;
        else if (strcmp(a, "--fps-cap") == 0 && has_next)
            o->fps_cap = (int)strtol(argv[++i], NULL, 10);
        else if (strcmp(a, "--unlock-fps") == 0) {
            /* 屏幕刷新率是墙：GL 窗口不关 vsync 就永远卡在 60/144。
               离屏本来就没有 vsync。 */
            o->vsync = 0;
            o->fps_cap = 0;
        } else if (strcmp(a, "--cjk") == 0)
            o->cjk = 1;
        else if (strcmp(a, "--no-hud") == 0)
            o->hud = 0;
        else if (strcmp(a, "--dump") == 0 && has_next)
            o->dump = argv[++i];
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            bench_usage(argv[0]);
            exit(0);
        }
    }
    /* 两个都是"上限"：谁先到就停。至少要有一个，否则会跑到关窗口 */
    if (o->frames < 0)
        o->frames = 0;
    if (o->seconds < 0.0)
        o->seconds = 0.0;
    if (o->frames == 0 && o->seconds == 0.0)
        o->seconds = 3.0;
}

/* ------------------------------ 帧率统计 ------------------------------ */

#define BENCH_RING 120

typedef struct {
    double v[BENCH_RING];
    int    n;        /* 已填入的个数（≤ BENCH_RING） */
    int    head;
    double sum;

    double last_ms;  /* 最近一帧 */
    double min_ms, max_ms;
    long   frames;
    double total_ms;
} BenchFps;

static void bench_fps_init(BenchFps *f)
{
    memset(f, 0, sizeof(*f));
    f->min_ms = 1e9;
}

/* 记一帧的整帧耗时（毫秒） */
static void bench_fps_add(BenchFps *f, double ms)
{
    f->frames++;
    f->total_ms += ms;
    f->last_ms = ms;
    if (ms < f->min_ms)
        f->min_ms = ms;
    if (ms > f->max_ms)
        f->max_ms = ms;

    if (f->n < BENCH_RING) {
        f->v[f->n++] = ms;
        f->sum += ms;
    } else {
        f->sum -= f->v[f->head];
        f->v[f->head] = ms;
        f->sum += ms;
        f->head = (f->head + 1) % BENCH_RING;
    }
}

/* 滑动窗口的平均帧率（比"最近一帧的倒数"稳得多，读数不跳） */
static double bench_fps_value(const BenchFps *f)
{
    if (f->n == 0 || f->sum <= 0.0)
        return 0.0;
    const double avg_ms = f->sum / (double)f->n;
    return (avg_ms > 0.0) ? (1000.0 / avg_ms) : 0.0;
}

/* 窗口内分位数（p95 才是"卡不卡"的证据，平均值会被 vsync 抹平） */
static void bench_fps_percentiles(const BenchFps *f, double *p50, double *p95)
{
    double tmp[BENCH_RING];
    const int n = (f->n < BENCH_RING) ? f->n : BENCH_RING;
    memcpy(tmp, f->v, sizeof(double) * (size_t)n);
    for (int i = 1; i < n; i++) {
        const double k = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > k) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = k;
    }
    if (p50 != NULL)
        *p50 = (n > 0) ? tmp[n / 2] : 0.0;
    if (p95 != NULL)
        *p95 = (n > 0) ? tmp[(n * 95) / 100] : 0.0;
}

/* -------------------------------- HUD -------------------------------- */

typedef struct {
    const char *backend;    /* "CPU / QLCanvas"、"GL / EGL" */
    const char *scene;      /* 维度名 */
    const char *detail;     /* "X11 窗口"、"pbuffer 离屏"、"dirty" */
    int         nodes;
    unsigned    verts;
    unsigned    draws;
    double      busy_ms;    /* 最近一帧的绘制提交耗时（不含等待） */
} BenchHudInfo;

/* HUD 面板的矩形（dirty 模式下它自己也要进脏区列表） */
static void bench_hud_rect(QLRect *out)
{
    *out = ql_rect_make(6, 6, 236, 14 + 13 * 6);
}

/*
 * 左上角那块半透明面板。**ASCII only** —— 内置位图字体也能画。
 *
 * `clip != NULL` 时（dirty 模式）只允许画这一块：面板里的数字每帧都在变，
 * 所以它的矩形本来就该算进脏区，否则增量重绘会把面板糊成一团。
 */
static void bench_hud_draw(QLDraw2D *d2d, ql_coord_t scr_w, ql_coord_t scr_h,
                           const BenchFps *fps, const BenchHudInfo *info,
                           const QLRect *clip)
{
    if (d2d == NULL || d2d->draw_text == NULL || d2d->fill_round_rect == NULL ||
        d2d->set_clip == NULL)
        return;

    enum { PAD = 7, LH = 13, BOX_W = 236 };

    char lines[6][96];
    const double fps_v = bench_fps_value(fps);
    double p50 = 0.0, p95 = 0.0;
    bench_fps_percentiles(fps, &p50, &p95);

    snprintf(lines[0], sizeof(lines[0]), "QLGL bench  %s", info->backend);
    snprintf(lines[1], sizeof(lines[1]), "%s", info->scene);
    snprintf(lines[2], sizeof(lines[2]), "fps %5.1f  frame %6.2f ms",
             fps_v, fps->last_ms);
    snprintf(lines[3], sizeof(lines[3]), "render %5.2f  p95 %5.2f  max %5.2f",
             info->busy_ms, p95, fps->max_ms);
    if (info->draws > 0)
        snprintf(lines[4], sizeof(lines[4]), "nodes %d  verts %u  draws %u",
                 info->nodes, info->verts, info->draws);
    else   /* CPU 光栅没有顶点/draw call，别印一排 0 让人以为坏了 */
        snprintf(lines[4], sizeof(lines[4]), "nodes %d  cpu raster", info->nodes);
    snprintf(lines[5], sizeof(lines[5]), "%s",
             (info->detail != NULL) ? info->detail : "");

    /* HUD 必须自己设裁剪：界面渲染结束后 d2d 的裁剪不保证是整屏 */
    const QLRect area = (clip != NULL) ? *clip : ql_rect_make(0, 0, scr_w, scr_h);
    d2d->set_clip(d2d, area);

    QLRect panel;
    bench_hud_rect(&panel);
    /* 浅色主题：白底 + 一条淡边框，深色字 */
    d2d->fill_round_rect(d2d, panel, 6, 0xF2F5F9, 244);
    if (d2d->stroke_round_rect != NULL)
        d2d->stroke_round_rect(d2d, panel, 6, 1, 0xBFC7D1);

    const QLFont *font = ql_font_mono(11);
    if (font == NULL)
        return;

    for (int i = 0; i < 6; i++) {
        if (lines[i][0] == '\0')
            continue;
        QLTextStyle ts;
        memset(&ts, 0, sizeof(ts));
        ts.color = (i == 0) ? 0x0969DA : 0x24292F;
        ts.line_h = LH;
        const QLRect lb = ql_rect_make(panel.x + PAD, panel.y + PAD + LH * i,
                                       BOX_W - PAD * 2, LH);
        d2d->draw_text(d2d, font, lines[i], -1, lb, &ts);
    }
}

/* ------------------------------ 结果汇总 ------------------------------ */

/* 一个维度跑完的结论（多维度时最后汇总成一张表） */
typedef struct {
    const char *scene;
    const char *what;      /* 这个维度测什么（一句话） */
    int         items;     /* 份数（卡片数/元素数…） */
    int         nodes;
    long        frames;
    double      fps;
    double      p50, p95, max_ms;
    double      busy_ms;
} BenchResult;

/*
 * 一行说明"为什么帧率停在某个数"——屏幕刷新率是墙，不是性能。
 * `can_vsync` = 这个后端有没有垂直同步（CPU 光栅没有，别吓人）。
 */
static const char *bench_limit_desc(const BenchOpts *o, int can_vsync)
{
    if (o->fps_cap > 0)
        return "--fps-cap 限帧";
    if (o->offscreen)
        return "无（离屏没有 vsync，全速）";
    if (!can_vsync)
        return "无（这个后端没有 vsync，全速）";
    if (o->vsync)
        return "vsync（受屏幕刷新率限制；--unlock-fps 可解除）";
    return "无（全速，可超过屏幕帧率）";
}

static void bench_report_one(const char *backend, const char *scene, const char *what,
                             const BenchOpts *o, int items, int can_vsync,
                             const BenchFps *f, const BenchHudInfo *info,
                             double busy_avg, unsigned verts_avg, unsigned draws_avg)
{
    if (f->frames <= 0) {
        printf("%s：没有跑任何帧\n", scene);
        return;
    }
    double p50 = 0.0, p95 = 0.0;
    bench_fps_percentiles(f, &p50, &p95);

    printf("\n=== %s ===\n", scene);
    printf("测什么      %s\n", (what != NULL) ? what : "-");
    printf("后端        %s（%s），%s\n", backend,
           (info->detail != NULL) ? info->detail : "-",
           (o->mode == BENCH_MODE_DIRTY) ? "只重绘脏区" : "每帧整屏重画");
    printf("规模        %dx%d，份数 %d，节点 %d\n",
           (int)o->w, (int)o->h, items, info->nodes);
    printf("限速        %s\n", bench_limit_desc(o, can_vsync));
    printf("时长        %.2f s，%ld 帧（平均 %.0f 帧/秒）\n",
           f->total_ms / 1000.0, f->frames,
           (f->total_ms > 0.0) ? ((double)f->frames * 1000.0 / f->total_ms) : 0.0);
    printf("平均帧率    %.2f fps（整帧 %.2f ms）\n", bench_fps_value(f),
           f->total_ms / (double)f->frames);
    printf("帧时间      p50 %.2f / p95 %.2f / max %.2f ms\n", p50, p95, f->max_ms);
    if (draws_avg > 0)
        printf("绘制提交    %.3f ms/帧（均值），%u 顶点，%u draw call\n",
               busy_avg, verts_avg, draws_avg);
    else
        printf("绘制提交    %.3f ms/帧（均值）\n", busy_avg);

    if (o->vsync && bench_fps_value(f) < 55.0)
        printf("提示        帧率低于 55：开了 vsync 就是这个数（等出来的），"
               "看 p95/render 才知道真实成本\n");
}

/* 跑了多个维度时的汇总表 */
static void bench_report_table(const char *backend, const char *mode,
                               const char *limit_desc,
                               const BenchResult *r, int n)
{
    if (n <= 0)
        return;
    printf("\n================ 汇总（%s，%s）================\n", backend, mode);
    printf("限速：%s\n\n", limit_desc);
    printf("%-10s %6s %6s %9s %7s %7s %7s  %s\n",
           "维度", "份数", "节点", "fps", "p50", "p95", "render", "测什么");
    printf("--------------------------------------------------------------"
           "----------------------------\n");
    for (int i = 0; i < n; i++)
        printf("%-10s %6d %6d %9.1f %7.2f %7.2f %7.2f  %s\n",
               r[i].scene, r[i].items, r[i].nodes, r[i].fps,
               r[i].p50, r[i].p95, r[i].busy_ms,
               (r[i].what != NULL) ? r[i].what : "");
    printf("\n单位：p50/p95/render 都是毫秒。对照：1/60 s = 16.67 ms。\n");
}

/* 限速：把这一帧睡到目标帧率 */
static void bench_limit_fps(int cap, double frame_start_ms)
{
    if (cap <= 0)
        return;
    const double target = 1000.0 / (double)cap;
    const double spent = bench_now_ms() - frame_start_ms;
    if (spent < target) {
        struct timespec ts;
        const double sleep_ms = target - spent;
        ts.tv_sec = (time_t)(sleep_ms / 1000.0);
        ts.tv_nsec = (long)((sleep_ms - (double)ts.tv_sec * 1000.0) * 1e6);
        nanosleep(&ts, NULL);
    }
}

#endif /* QL_BENCH_COMMON_H */
