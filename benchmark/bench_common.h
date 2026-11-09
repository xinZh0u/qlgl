/**
 * @file    bench_common.h
 * @brief   可视化 benchmark 共用的部分：压力场景、帧率统计、左上角 HUD。
 *
 * 两个可执行文件（`bench_cpu` / `bench_gl`）唯一的区别是"把界面画到哪"：
 *
 *   bench_cpu   QLCanvas（CPU 光栅）→ X11/Wayland 窗口，或内存显示（无窗口）
 *   bench_gl    QLDraw2D（EGL/GLES3）→ GL 窗口，或 pbuffer 离屏
 *
 * 场景、计时、HUD 都在这里，两个 main 只负责后端相关的十几行 —— 这样
 * "GL 比 CPU 快多少"才是同一个场景下的可比数字，而不是两套不同的界面。
 *
 * 参考 LVGL 的 benchmark：一个固定场景每帧整屏重画，左上角实时显示帧率。
 * 整屏重画是刻意的 —— 它测的是**光栅吞吐**（填充率 + 文本 + 圆角/阴影/渐变），
 * 局部重绘能力另有 `test_gl_dirty` 与例程 05 覆盖。
 */
#ifndef QL_BENCH_COMMON_H
#define QL_BENCH_COMMON_H

#define _POSIX_C_SOURCE 200809L   /* clock_gettime / nanosleep */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <qlgl/qlgl.h>

/* 复用例程里的字体加载 / locale / 清除色：同一套样板，别抄第二份 */
#include "../example/common.h"

/* ------------------------------- 时间 ------------------------------- */

static double bench_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ------------------------------ 命令行 ------------------------------ */

typedef struct {
    int       frames;      /* 跑多少帧后退出；0 = 一直跑到关窗口 */
    double    seconds;     /* 跑多少秒后退出；0 = 不限时 */
    ql_coord_t w, h;
    int       cards;       /* 场景里的卡片数（每张 3 个节点） */
    int       hud;         /* 1 = 左上角显示帧率 */
    int       vsync;
    int       offscreen;   /* 1 = 不开窗口（CI / 无显示器） */
    int       cjk;         /* 1 = 文本用中文（顺带压矢量字体的字形缓存） */
    int       fps_cap;     /* >0 = 每帧睡到这个帧率（看效果用；0 = 不限速） */
    const char *dump;      /* 非 NULL = 退出前导出一帧 PPM */
} BenchOpts;

static void bench_usage(const char *prog)
{
    printf("用法：%s [选项]\n"
           "  --frames N     跑 N 帧后退出（默认 0 = 关窗口为止）\n"
           "  --seconds S    跑 S 秒后退出\n"
           "  --size WxH     窗口/画布尺寸（默认 800x480）\n"
           "  --cards N      压力场景里的卡片数（默认 120，每张 3 节点）\n"
           "  --cjk          文本用中文\n"
           "  --no-hud       不画左上角帧率\n"
           "  --vsync 0|1    垂直同步（GL 默认 1；CPU 后端无此概念）\n"
           "  --offscreen    不开窗口，离屏跑（打印结果用，CI 友好）\n"
           "  --fps-cap N    限到 N fps（默认 0 = 全速，跑满吞吐）\n"
           "  --dump PATH    退出前导出一帧 PPM\n",
           prog);
}

static void bench_opts_parse(BenchOpts *o, int argc, char **argv)
{
    /* 重定向到文件/管道时 stdout 默认是块缓冲：开跑前的几行会憋到退出才出现 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    memset(o, 0, sizeof(*o));
    o->w = 800;
    o->h = 480;
    o->cards = 120;
    o->hud = 1;
    o->vsync = 1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const int has_next = (i + 1 < argc);
        if (strcmp(a, "--frames") == 0 && has_next)
            o->frames = (int)strtol(argv[++i], NULL, 10);
        else if (strcmp(a, "--seconds") == 0 && has_next)
            o->seconds = strtod(argv[++i], NULL);
        else if (strcmp(a, "--size") == 0 && has_next) {
            int w = 0, h = 0;
            if (sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                o->w = (ql_coord_t)w;
                o->h = (ql_coord_t)h;
            }
        } else if (strcmp(a, "--cards") == 0 && has_next)
            o->cards = (int)strtol(argv[++i], NULL, 10);
        else if (strcmp(a, "--cjk") == 0)
            o->cjk = 1;
        else if (strcmp(a, "--no-hud") == 0)
            o->hud = 0;
        else if (strcmp(a, "--vsync") == 0 && has_next)
            o->vsync = (int)strtol(argv[++i], NULL, 10) != 0;
        else if (strcmp(a, "--offscreen") == 0)
            o->offscreen = 1;
        else if (strcmp(a, "--fps-cap") == 0 && has_next)
            o->fps_cap = (int)strtol(argv[++i], NULL, 10);
        else if (strcmp(a, "--dump") == 0 && has_next)
            o->dump = argv[++i];
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            bench_usage(argv[0]);
            exit(0);
        }
    }
    if (o->cards < 1)
        o->cards = 1;
    if (o->cards > 1200)
        o->cards = 1200;      /* QL_CFG_UI_MAX_NODES 是 4096，一张卡 3 节点 */
}

/* ------------------------------ 压力场景 ------------------------------ */

/*
 * 为什么是"一堆卡片"：它同时压住四条最贵的路径 ——
 *   - 圆角矩形填充（每张卡一块，带抗锯齿）
 *   - 渐变（hero 面板 + 每 3 张卡一片，GPU 走顶点插值、CPU 走逐像素）
 *   - 阴影（hero 的 box-shadow，SDF）
 *   - 文本（每张卡两行，走字形图集）
 * 再叠一个 `@keyframes` 滑动的条 —— 保证每帧画面真的在变，
 * 否则测出来的数字不真实。
 */
static QLUIDoc *bench_build_scene(int cards, int cjk)
{
    static const char *k_css =
        "#root { width: 100%; height: 100%; box-sizing: border-box;"
        "        display: flex; flex-wrap: wrap; gap: 8px;"
        "        background-color: #0E1116; padding: 10px;"
        "        font-size: 11px; color: #E6EDF3; }"
        ".hero { width: 100%; height: 44px; box-sizing: border-box;"
        "        border-radius: 10px; box-shadow: 0px 4px 10px #000000;"
        "        background-image: linear-gradient(to right, #2E6BE6, #7A5AF8); }"
        ".card { width: 122px; height: 62px; box-sizing: border-box; padding: 6px;"
        "        border-radius: 8px; border-width: 1px; border-color: #30363D;"
        "        background-color: #161B22; }"
        ".c2 { background-color: #1B2430; border-color: #3D4854; }"
        ".c3 { background-image: linear-gradient(to bottom, #1F2933, #131A21); }"
        ".t { font-size: 12px; color: #E6EDF3; }"
        ".s { font-size: 10px; color: #8B949E; }"
        "#bar { width: 120px; height: 8px; border-radius: 4px;"
        "       background-color: #2FB8A6;"
        "       animation: slide 1200ms ease-in-out infinite alternate; }"
        "@keyframes slide { from { transform: translate(0px, 0px); }"
        "                   to   { transform: translate(320px, 0px); } }";

    static const char *k_cjk_title[] = { "温度", "压力", "流量", "转速" };
    static const char *k_en_title[] = { "Item", "Temp", "Flow", "RPM" };

    const size_t cap = (size_t)cards * 260u + 4096u;
    char *html = (char *)malloc(cap);
    if (html == NULL)
        return NULL;

    size_t n = 0;
    n += (size_t)snprintf(html + n, cap - n, "<div id=\"root\"><div class=\"hero\"></div>");

    for (int i = 0; i < cards; i++) {
        const char *cls = (i % 3 == 1) ? "card c2" : ((i % 3 == 2) ? "card c3" : "card");
        char t[64], s[64];
        if (cjk) {
            snprintf(t, sizeof(t), "%s #%d", k_cjk_title[i % 4], i);
            snprintf(s, sizeof(s), "%d.%d 单位", (i * 7) % 100, i % 10);
        } else {
            snprintf(t, sizeof(t), "%s %d", k_en_title[i % 4], i);
            snprintf(s, sizeof(s), "%d.%d unit", (i * 7) % 100, i % 10);
        }
        n += (size_t)snprintf(html + n, cap - n,
                              "<div class=\"%s\"><div class=\"t\">%s</div>"
                              "<div class=\"s\">%s</div></div>",
                              cls, t, s);
    }
    n += (size_t)snprintf(html + n, cap - n, "<div id=\"bar\"></div></div>");

    QLUIDoc *doc = ql_ui_load_text(html, k_css);
    if (doc == NULL)
        printf("场景编译失败：%s\n", ql_ui_text_last_error());
    free(html);
    return doc;
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
    double total_ms; /* 所有帧耗时之和 */
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

/* -------------------------------- HUD -------------------------------- */

typedef struct {
    const char *backend;    /* "CPU / QLCanvas" 或 "GL / EGL" */
    const char *detail;     /* 例如 "X11 窗口"、"pbuffer 离屏" */
    int         nodes;
    unsigned    verts;
    unsigned    draws;
    double      busy_ms;    /* 最近一帧"绘制提交"耗时（不含 swap/等待） */
} BenchHudInfo;

/*
 * 左上角那块半透明面板。**ASCII only** —— 内置位图字体也能画，
 * 没装矢量字体的机器上 benchmark 一样能看。
 */
static void bench_hud_draw(QLDraw2D *d2d, ql_coord_t scr_w, ql_coord_t scr_h,
                           const BenchFps *fps, const BenchHudInfo *info)
{
    if (d2d == NULL || d2d->draw_text == NULL || d2d->fill_round_rect == NULL ||
        d2d->set_clip == NULL)
        return;

    enum { PAD = 7, LH = 13, BOX_W = 232 };

    char lines[5][96];
    const double fps_v = bench_fps_value(fps);
    snprintf(lines[0], sizeof(lines[0]), "QLGL bench  %s", info->backend);
    snprintf(lines[1], sizeof(lines[1]), "fps %5.1f  frame %6.2f ms",
             fps_v, fps->last_ms);
    snprintf(lines[2], sizeof(lines[2]), "render %5.2f  min %.2f  max %.2f ms",
             info->busy_ms, fps->min_ms, fps->max_ms);
    if (info->draws > 0)
        snprintf(lines[3], sizeof(lines[3]), "nodes %d  verts %u  draws %u",
                 info->nodes, info->verts, info->draws);
    else   /* CPU 光栅没有顶点/draw call，别印一排 0 让人以为坏了 */
        snprintf(lines[3], sizeof(lines[3]), "nodes %d  cpu raster", info->nodes);
    snprintf(lines[4], sizeof(lines[4]), "%s",
             (info->detail != NULL) ? info->detail : "");

    /* HUD 必须自己设裁剪：界面渲染结束后 d2d 的裁剪不保证是整屏 */
    d2d->set_clip(d2d, ql_rect_make(0, 0, scr_w, scr_h));

    const QLRect panel = ql_rect_make(6, 6, BOX_W, PAD * 2 + LH * 5);
    d2d->fill_round_rect(d2d, panel, 6, 0x0B0E13, 208);

    const QLFont *font = ql_font_mono(11);
    if (font == NULL)
        return;

    for (int i = 0; i < 5; i++) {
        if (lines[i][0] == '\0')
            continue;
        QLTextStyle ts;
        memset(&ts, 0, sizeof(ts));
        /* 第一行（标题）用亮青色，其余灰白 —— 一眼分出"是什么"和"数字" */
        ts.color = (i == 0) ? 0x7CE0FF : 0xC9D1D9;
        ts.line_h = LH;
        const QLRect lb = ql_rect_make(panel.x + PAD, panel.y + PAD + LH * i,
                                       BOX_W - PAD * 2, LH);
        d2d->draw_text(d2d, font, lines[i], -1, lb, &ts);
    }
}

/* ------------------------------ 结果汇总 ------------------------------ */

/*
 * 退出时打一张表：滑动窗口的 fps 会被 vsync 抹平，而 p50/p95/min/max
 * 才是"卡不卡"的证据。全部用毫秒，方便和 `1/60 = 16.67` 对照。
 */
static void bench_report(const char *name, const BenchOpts *o, const BenchFps *f,
                         const BenchHudInfo *info)
{
    if (f->frames <= 0) {
        printf("%s：没有跑任何帧\n", name);
        return;
    }

    /* 排序求分位数（窗口最多 BENCH_RING 条，插入排序足够） */
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
    const double p50 = (n > 0) ? tmp[n / 2] : 0.0;
    const double p95 = (n > 0) ? tmp[(n * 95) / 100] : 0.0;

    printf("\n=== %s ===\n", name);
    printf("后端        %s（%s）\n", info->backend,
           (info->detail != NULL) ? info->detail : "-");
    printf("尺寸        %dx%d，卡片 %d，节点 %d\n",
           (int)o->w, (int)o->h, o->cards, info->nodes);
    printf("帧数        %ld（合计 %.2f s）\n", f->frames, f->total_ms / 1000.0);
    printf("平均帧率    %.2f fps（整帧 %.2f ms）\n", bench_fps_value(f),
           f->total_ms / (double)f->frames);
    printf("帧时间      p50 %.2f / p95 %.2f / min %.2f / max %.2f ms\n",
           p50, p95, f->min_ms, f->max_ms);
    if (info->draws > 0)
        printf("绘制提交    %.3f ms/帧，%u 顶点，%u draw call\n",
               info->busy_ms, info->verts, info->draws);
    else
        printf("绘制提交    %.3f ms/帧\n", info->busy_ms);

    if (f->last_ms > 0.0 && (1000.0 / f->last_ms) < 55.0)
        printf("提示        帧率低于 55：开了 vsync 的话这不是性能问题，"
               "用 --vsync 0 测吞吐\n");
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

/* 跑多少帧 / 多少秒该停 */
static int bench_should_stop(const BenchOpts *o, long frames, double t_start)
{
    if (o->frames > 0 && frames >= (long)o->frames)
        return 1;
    if (o->seconds > 0.0 && (bench_now_ms() - t_start) >= o->seconds * 1000.0)
        return 1;
    return 0;
}

#endif /* QL_BENCH_COMMON_H */
