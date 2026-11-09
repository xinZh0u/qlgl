/**
 * @file    test_scale_flip.c
 * @brief   `transform: scaleX()` 的真语义：**高度不变、宽度缩放、负值镜像**。
 *
 * 这一期纠正的是一个"看起来像渲染 bug、其实是语义错了"的行为：
 * 原来 `scaleX(0.5)` 会把字号按 min(sx,sy) 整体缩小 —— 字变成了"小一半"，
 * 而正确的 CSS 语义是"压扁"：高度不变、宽度减半。
 *
 * 守住四条：
 *   1. 高度不变（只压宽度）；
 *   2. 负值 = 关于元素中心**水平镜像**（逐像素对拍）；
 *   3. 等比缩放仍然两轴同时缩（那条"换字号"的清晰路径不能被破坏）；
 *   4. 命中测试：镜像仍可点中，压到 0 不可点。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#define W 240
#define H 120

/* 元素：left 40, width 120 → 中心 x = 100 */
#define EL_CX 100

static QLUIDoc *mk(const char *extra, QLUIRoot **out_ui)
{
    static const char *html = "<div id=root><div class=\"f\">翻转AB</div></div>";
    char css[640];
    snprintf(css, sizeof(css),
             "#root { width: 240px; height: 120px; background-color: #000000; }"
             ".f { position: absolute; left: 40px; top: 30px; width: 120px;"
             "     height: 50px; background-color: #FFFFFF; color: #000000;"
             "     font-size: 18px; line-height: 50px; text-align: center; %s }",
             extra);
    QLUIDoc *doc = ql_ui_load_text(html, css);
    if (doc == NULL) {
        printf("  编译失败：%s\n", ql_ui_text_last_error());
        return NULL;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return NULL;
    }
    ql_ui_layout(ui, W, H);
    *out_ui = ui;
    return doc;
}

/* 文字墨迹的范围（排除纯黑背景与纯白元素底色） */
typedef struct { int x0, x1, y0, y1, ink; } Ink;

static Ink scan(QLDispDriver *d)
{
    Ink k = { -1, -1, -1, -1, 0 };
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const ql_color_t c = ql_disp_mem_pixel(d, (ql_coord_t)x, (ql_coord_t)y);
            if (c == 0x000000 || c == 0xFFFFFF)
                continue;
            k.ink++;
            if (k.x0 < 0 || x < k.x0) k.x0 = x;
            if (x > k.x1) k.x1 = x;
            if (k.y0 < 0 || y < k.y0) k.y0 = y;
            if (y > k.y1) k.y1 = y;
        }
    return k;
}

static Ink render_ink(QLUIRoot *ui)
{
    QLDispDriver *d = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
    QLCanvas cv;
    ql_canvas_from_disp(&cv, d);
    ql_canvas_clear(&cv, 0x000000);
    ql_ui_render_canvas(ui, &cv);
    const Ink k = scan(d);
    ql_disp_mem_destroy(d);
    return k;
}

/* 渲染一份并留一帧（镜像对拍用） */
static void render_into(QLUIRoot *ui, ql_color_t *out)
{
    QLDispDriver *d = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
    QLCanvas cv;
    ql_canvas_from_disp(&cv, d);
    ql_canvas_clear(&cv, 0x000000);
    ql_ui_render_canvas(ui, &cv);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            out[y * W + x] = ql_disp_mem_pixel(d, (ql_coord_t)x, (ql_coord_t)y);
    ql_disp_mem_destroy(d);
}

QL_TEST_BEGIN("test_scale_flip")

    /* ---------- 1. scaleX(0.5)：宽减半、**高不变** ---------- */
    {
        QLUIRoot *ui1 = NULL, *ui2 = NULL;
        QLUIDoc *d1 = mk("", &ui1);
        QLUIDoc *d2 = mk("transform: scaleX(0.5);", &ui2);
        QL_CHECK(ui1 != NULL && ui2 != NULL);
        if (ui1 != NULL && ui2 != NULL) {
            const Ink a = render_ink(ui1);
            const Ink b = render_ink(ui2);
            const int w0 = a.x1 - a.x0 + 1, h0 = a.y1 - a.y0 + 1;
            const int w1 = b.x1 - b.x0 + 1, h1 = b.y1 - b.y0 + 1;
            printf("    scaleX(1) → %dx%d，scaleX(0.5) → %dx%d\n", w0, h0, w1, h1);
            QL_CHECK(h1 == h0);                 /* 高度必须一模一样 */
            QL_CHECK(w1 <= (w0 + 1) / 2 + 2);   /* 宽度约减半 */
            QL_CHECK(w1 >= 1);
            /* 竖直中心也不动（压扁是绕中心） */
            QL_CHECK((a.y0 + a.y1) - (b.y0 + b.y1) <= 2 &&
                     (b.y0 + b.y1) - (a.y0 + a.y1) <= 2);
        }
        ql_ui_free(ui1); ql_ui_doc_free(d1);
        ql_ui_free(ui2); ql_ui_doc_free(d2);
    }

    /* ---------- 2. scaleX(-1)：关于元素中心水平镜像 ---------- */
    {
        QLUIRoot *ui1 = NULL, *ui2 = NULL;
        QLUIDoc *d1 = mk("", &ui1);
        QLUIDoc *d2 = mk("transform: scaleX(-1);", &ui2);
        QL_CHECK(ui1 != NULL && ui2 != NULL);
        if (ui1 != NULL && ui2 != NULL) {
            ql_color_t *p0 = (ql_color_t *)malloc(sizeof(ql_color_t) * W * H);
            ql_color_t *p1 = (ql_color_t *)malloc(sizeof(ql_color_t) * W * H);
            QL_CHECK(p0 != NULL && p1 != NULL);
            if (p0 != NULL && p1 != NULL) {
                render_into(ui1, p0);
                render_into(ui2, p1);
                /*
                 * 镜像的预期：把 p0 以 EL_CX 为轴左右翻过来应该等于 p1。
                 * 只比对元素盒子范围内的文字像素（盒子本身对称，边界会差 1px）。
                 */
                /*
                 * 比**墨迹掩码**而不是比灰度值：几何重采样是最近邻 + 覆盖取最大，
                 * 抗锯齿的中间灰必然会差一两级；肉眼看到的是"是不是同一个形状"。
                 */
                int diff = 0, total = 0;
                for (int y = 30; y < 80; y++) {
                    for (int x = 60; x <= 140; x++) {
                        /*
                         * 镜像位置。注意是 `2*cx - 1 - x` 而不是 `2*cx - x`：
                         * 绕整数中心镜像时，目的像素 x 采样的是源像素 2*cx-1-x
                         * （像素**格**的镜像，不是坐标点的镜像）。差这一个像素
                         * 会让整幅图错位，那是测试写错了，不是渲染错了。
                         */
                        const int mx = 2 * EL_CX - 1 - x;
                        if (mx < 0 || mx >= W)
                            continue;
                        const ql_color_t a0 = p0[y * W + mx];
                        const ql_color_t a1 = p1[y * W + x];
                        const int ink0 = (a0 != 0x000000 && a0 != 0xFFFFFF);
                        const int ink1 = (a1 != 0x000000 && a1 != 0xFFFFFF);
                        if (!ink0 && !ink1)
                            continue;
                        total++;
                        if (ink0 != ink1)
                            diff++;
                    }
                }
                printf("    镜像对拍：%d/%d 个文字像素不同\n", diff, total);
                QL_CHECK(total > 20);        /* 确实比到了字 */
                QL_CHECK(diff * 100 <= total * 15);  /* 允许 15% 的边缘取整差异 */
            }
            free(p0);
            free(p1);
        }
        ql_ui_free(ui1); ql_ui_doc_free(d1);
        ql_ui_free(ui2); ql_ui_doc_free(d2);
    }

    /* ---------- 3. scaleX(0)：什么都不画 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("transform: scaleX(0);", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const Ink k = render_ink(ui);
            QL_CHECK_EQ(k.ink, 0);
            /* 命中测试：面积为零，点不中 */
            QL_CHECK(ql_ui_hit_test(ui, EL_CX, 55) != &ui->nodes[1]);
            ql_ui_free(ui);
        }
        ql_ui_doc_free(doc);
    }

    /* ---------- 4. 镜像之后仍然可点（命中测试走逆变换） ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("transform: scaleX(-1);", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK(ql_ui_hit_test(ui, EL_CX, 55) == &ui->nodes[1]);
            ql_ui_free(ui);
        }
        ql_ui_doc_free(doc);
    }

    /* ---------- 5. 等比缩放仍然是两轴同时缩（清晰路径没被破坏） ---------- */
    {
        QLUIRoot *ui1 = NULL, *ui2 = NULL;
        QLUIDoc *d1 = mk("", &ui1);
        QLUIDoc *d2 = mk("transform: scale(0.5);", &ui2);
        QL_CHECK(ui1 != NULL && ui2 != NULL);
        if (ui1 != NULL && ui2 != NULL) {
            const Ink a = render_ink(ui1);
            const Ink b = render_ink(ui2);
            const int w0 = a.x1 - a.x0 + 1, h0 = a.y1 - a.y0 + 1;
            const int w1 = b.x1 - b.x0 + 1, h1 = b.y1 - b.y0 + 1;
            printf("    scale(0.5) → %dx%d（原始 %dx%d）\n", w1, h1, w0, h0);
            QL_CHECK(h1 < h0);            /* 等比缩：高度也变小 */
            QL_CHECK(w1 < w0);
        }
        ql_ui_free(ui1); ql_ui_doc_free(d1);
        ql_ui_free(ui2); ql_ui_doc_free(d2);
    }

    /* ---------- 6. 关键帧动画能走到负值（哨兵不能和 -1 撞车） ---------- */
    {
        static const char *html =
            "<div id=root><div class=\"f\">翻转</div></div>";
        const char *css =
            "#root { width: 240px; height: 120px; background-color: #000000; }"
            ".f { position: absolute; left: 40px; top: 30px; width: 120px;"
            "     height: 50px; background-color: #FFFFFF; color: #000000;"
            "     font-size: 18px; line-height: 50px; text-align: center;"
            "     animation: k 1000ms linear infinite; }"
            "@keyframes k { 0% { transform: scaleX(1); }"
            "  25% { transform: scaleX(0); }  50% { transform: scaleX(-1); }"
            "  75% { transform: scaleX(0); } 100% { transform: scaleX(1); } }";
        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                (void)ql_ui_anim_tick(ui, 0);
                QL_CHECK_EQ((int)ql_ui_node_style(&ui->nodes[1])->sc_x, 1000);
                (void)ql_ui_anim_tick(ui, 250);
                QL_CHECK_EQ((int)ql_ui_node_style(&ui->nodes[1])->sc_x, 0);
                (void)ql_ui_anim_tick(ui, 500);
                /* 这一条就是 sentinel 撞车的现场：以前这里会变回 +1000 */
                QL_CHECK_EQ((int)ql_ui_node_style(&ui->nodes[1])->sc_x, -1000);
                printf("    动画 50%% 处 sc_x = %d\n",
                       (int)ql_ui_node_style(&ui->nodes[1])->sc_x);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

QL_TEST_END()
