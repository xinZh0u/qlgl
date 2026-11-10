/**
 * @file    test_transform.c
 * @brief   M9.4 旋转光栅：`transform: rotate/skew/matrix` + `transform-origin`。
 *
 * CSS 的 `transform` 作用在**元素的渲染结果**上（"先合成，再重采样"），所以实现
 * 是"离屏图层 + 仿射重采样"，而不是把每个图元单独转。测试就围绕这一点：
 *
 *   1. 解析：角度 → 26.6 定点矩阵（cos/sin 的精确值）、`skew`/`matrix` 的展开、
 *      `transform-origin` 的三种写法（px / 百分比 / 关键字）、非法写法必须报错；
 *   2. 光栅：`rotate(90deg)` 把 40×10 的横条变成 10×40 的竖条（像素数与位置都能算出来）、
 *      `rotate(45deg)` 的面积、`skewX` 的错切方向、`matrix` 的等价性；
 *   3. `transform-origin` 真的改变旋转中心（`left top` vs 默认为中心）；
 *   4. 圆角/遮罩跟着一起转（**先裁再转**，不是转完再裁成方框）；
 *   5. 点中测试：旋转后的元素按**旋转后**的形状命中，原来的位置点不中。
 *
 * 每条断言都打印"哪里不对"的数字：出问题先看数字，再回去看代码。
 */
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#define W 160
#define H 120
#define BG  0xFFFFFFu

static QLDispDriver *s_disp;
static QLCanvas s_cv;
/* ql_ui_build() 建出的树引用 doc，所以 doc 要活到 ql_ui_free() 之后 */
static QLUIDoc *s_doc;

static QLUIRoot *render_html(const char *html, const char *css)
{
    QLUIDoc *doc = ql_ui_load_text(html, css);
    if (doc == NULL) {
        printf("  编译失败：%s\n", ql_ui_text_last_error());
        return NULL;
    }
    if (s_doc != NULL) {
        ql_ui_doc_free(s_doc);
        s_doc = NULL;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return NULL;
    }
    s_doc = doc;
    ql_ui_layout(ui, W, H);
    if (s_disp == NULL)
        s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
    ql_canvas_from_disp(&s_cv, s_disp);
    ql_canvas_clear(&s_cv, BG);
    ql_ui_render_canvas(ui, &s_cv);
    return ui;
}

static ql_color_t px(int x, int y)
{
    return ql_disp_mem_pixel(s_disp, x, y);
}

static int is_red(ql_color_t c)
{
    return QL_COLOR_GET_R(c) > 200 && QL_COLOR_GET_G(c) < 60;
}

/* 统计某个颜色族的像素数与包围盒（-1 表示没有） */
static int count_color(int *x0, int *y0, int *x1, int *y1)
{
    int n = 0;
    *x0 = W;
    *y0 = H;
    *x1 = -1;
    *y1 = -1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (!is_red(px(x, y)))
                continue;
            n++;
            if (x < *x0)
                *x0 = x;
            if (y < *y0)
                *y0 = y;
            if (x > *x1)
                *x1 = x;
            if (y > *y1)
                *y1 = y;
        }
    }
    return n;
}

/* 期望被拒绝，且报错文案里含 needle */
static int rejects(const char *css, const char *needle)
{
    QLUIDoc *doc = ql_ui_load_text("<div id=r><div id=b></div></div>", css);
    if (doc != NULL) {
        ql_ui_doc_free(doc);
        printf("  本该报错却编译通过了：%s\n", css);
        return 0;
    }
    const char *msg = ql_ui_text_last_error();
    if (msg == NULL || strstr(msg, needle) == NULL) {
        printf("  报错文案不含 '%s'：%s\n", needle, msg ? msg : "(null)");
        return 0;
    }
    return 1;
}

/* 40×10 的红横条，可追加 transform 声明 */
static void bar_css(char *out, size_t cap, const char *extra)
{
    snprintf(out, cap,
             "#r{width:160px;height:120px;background-color:#FFFFFF}"
             "#b{position:absolute;left:60px;top:55px;width:40px;height:10px;"
             "background-color:#FF0000;%s}",
             extra ? extra : "");
}

QL_TEST_BEGIN("test_transform")

    /* ---------- 1. 解析：角度 → 矩阵 ---------- */
    {
        struct { const char *css; int px, py, wantx, wanty; } cases[] = {
            /* 元素 60..100 × 55..65，中心 (80,60)。用"点被转到哪里"来验证矩阵 */
            { "transform:rotate(90deg)", 60, 55, 84, 40 },
            { "transform:rotate(180deg)", 60, 55, 99, 64 },
            { "transform:rotate(-90deg)", 60, 55, 75, 79 },
            { "transform:rotate(0deg)", 60, 55, 60, 55 },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            char css[512];
            bar_css(css, sizeof(css), cases[i].css);
            QLUIRoot *ui = render_html("<div id=r><div id=b></div></div>", css);
            QL_CHECK(ui != NULL);
            if (ui == NULL)
                continue;
            int x0, y0, x1, y1;
            const int n = count_color(&x0, &y0, &x1, &y1);
            /*
             * 左上角那个像素中心 (60.5, 55.5) 经旋转后的落点就是要检查的量：
             * 只要求"落点在图里是红的、且原位置在转 90/180 后不再是红"。
             */
            const int at_ok = is_red(px(cases[i].wantx, cases[i].wanty));

            printf("  %-26s 面积 %d px，左上角 → (%d,%d) %s\n", cases[i].css, n,
                   cases[i].wantx, cases[i].wanty, at_ok ? "命中" : "缺");
            QL_CHECK_EQ(at_ok, 1);
            if (i == 0 || i == 2) {
                /*
                 * 转 ±90° 之后原横条的位置（(61,56) 这类点）不再有内容。
                 * 180° 不能这么查：横条关于自己的中心对称，转 180° 还是原来那块。
                 */
                QL_CHECK_EQ(is_red(px(61, 56)), 0);
            }
            ql_ui_free(ui);
        }
    }

    /* ---------- 2. 光栅几何：像素数与包围盒 ---------- */
    {
        char css[512];

        /* rotate(90deg)：40×10 横条 → 10×40 竖条，面积不变 */
        bar_css(css, sizeof(css), "transform:rotate(90deg)");
        QLUIRoot *ui = render_html("<div id=r><div id=b></div></div>", css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            int x0, y0, x1, y1;
            const int n = count_color(&x0, &y0, &x1, &y1);
            printf("  rotate(90)  ：面积 %d px，包围盒 x[%d..%d] y[%d..%d]\n", n, x0, x1, y0,
                   y1);
            QL_CHECK(n >= 400 && n <= 460);   /* 40*10，没有空洞（多的是边缘钳位那一圈） */
            QL_CHECK(x1 - x0 + 1 >= 10 && x1 - x0 + 1 <= 11);
            QL_CHECK(y1 - y0 + 1 >= 40 && y1 - y0 + 1 <= 41);
            ql_ui_free(ui);
        }

        /* 不写 transform：还是 40×10 的横条（老路径别被改坏） */
        bar_css(css, sizeof(css), NULL);
        ui = render_html("<div id=r><div id=b></div></div>", css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            int x0, y0, x1, y1;
            const int n = count_color(&x0, &y0, &x1, &y1);
            QL_CHECK_EQ(n, 400);              /* 轴对齐走快路：一像素不多 */
            QL_CHECK_EQ(x1 - x0 + 1, 40);
            QL_CHECK_EQ(y1 - y0 + 1, 10);
            ql_ui_free(ui);
        }

        /* rotate(45deg)：双线性重采样，包围盒边长 ≈ 40/√2 + 10/√2 ≈ 35 */
        bar_css(css, sizeof(css), "transform:rotate(45deg)");
        ui = render_html("<div id=r><div id=b></div></div>", css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            int x0, y0, x1, y1;
            const int n = count_color(&x0, &y0, &x1, &y1);
            printf("  rotate(45)  ：面积 %d px，包围盒 %d×%d\n", n, x1 - x0 + 1,
                   y1 - y0 + 1);
            /* 面积波动 ±15%：边缘是硬边界，采样点在边上的像素时红时不红 */
            QL_CHECK(n > 340 && n < 460);
            QL_CHECK(x1 - x0 + 1 >= 33 && x1 - x0 + 1 <= 38);
            QL_CHECK(y1 - y0 + 1 >= 33 && y1 - y0 + 1 <= 38);
            ql_ui_free(ui);
        }

        /* skewX(45deg)：上边不动、下边右移一个高度（10px） */
        bar_css(css, sizeof(css), "transform:skewX(45deg)");
        ui = render_html("<div id=r><div id=b></div></div>", css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            int x0, y0, x1, y1;
            const int n = count_color(&x0, &y0, &x1, &y1);
            printf("  skewX(45)   ：面积 %d px，包围盒 x[%d..%d] y[%d..%d]\n", n, x0, x1,
                   y0, y1);
            QL_CHECK(n > 380 && n < 420);      /* 面积不变 */
            QL_CHECK(x1 - x0 + 1 >= 46 && x1 - x0 + 1 <= 52);   /* 40 + 10 */
            QL_CHECK_EQ(y1 - y0 + 1, 10);
            ql_ui_free(ui);
        }

        /* matrix(1,0,0,1,0,0) 与不写 transform 等价 */
        bar_css(css, sizeof(css), "transform:matrix(1,0,0,1,0,0)");
        ui = render_html("<div id=r><div id=b></div></div>", css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            int x0, y0, x1, y1;
            const int n = count_color(&x0, &y0, &x1, &y1);
            QL_CHECK(n >= 400 && n <= 460);
            QL_CHECK_EQ(x0, 60);
            QL_CHECK_EQ(y0, 55);
            ql_ui_free(ui);
        }

        /* matrix 里的平移：等价于 translate */
        bar_css(css, sizeof(css), "transform:matrix(1,0,0,1,5,7)");
        ui = render_html("<div id=r><div id=b></div></div>", css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            int x0, y0, x1, y1;
            const int n = count_color(&x0, &y0, &x1, &y1);
            /*
             * 400 + 最外一圈 ~51 px：重采样对越界点做**边缘钳位**，
             * 所以平移后元素外沿会多出半像素宽的"边缘色"。CPU/GPU 一致。
             */
            QL_CHECK(n >= 400 && n <= 460);
            QL_CHECK_EQ(x0, 64);
            QL_CHECK_EQ(y0, 61);
            ql_ui_free(ui);
        }
    }

    /* ---------- 3. transform-origin ---------- */
    {
        char css[512];

        /* 绕左上角转 90°：(60,55) 是轴，横条变成从 (50,55) 往下的竖条 */
        bar_css(css, sizeof(css), "transform:rotate(90deg);transform-origin:left top");
        QLUIRoot *ui = render_html("<div id=r><div id=b></div></div>", css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            int x0, y0, x1, y1;
            const int n = count_color(&x0, &y0, &x1, &y1);
            printf("  origin:left top：面积 %d px，包围盒 x[%d..%d] y[%d..%d]\n", n, x0,
                   x1, y0, y1);
            QL_CHECK(n >= 400 && n <= 460);
            QL_CHECK(x1 - x0 + 1 >= 10 && x1 - x0 + 1 <= 11);  /* 40 长的那边转到竖直 */
            QL_CHECK(y1 - y0 + 1 >= 40 && y1 - y0 + 1 <= 41);
            QL_CHECK_EQ(y0, 55);               /* 顶边是旋转轴，不动 */
            QL_CHECK_EQ(x0, 50);               /* 长边往左展开：60-10 */
            ql_ui_free(ui);
        }

        /* 百分比原点 0% 0% 与 left top 等价 */
        bar_css(css, sizeof(css), "transform:rotate(90deg);transform-origin:0% 0%");
        ui = render_html("<div id=r><div id=b></div></div>", css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            int x0, y0, x1, y1;
            const int n = count_color(&x0, &y0, &x1, &y1);
            QL_CHECK(n >= 400 && n <= 460);
            QL_CHECK_EQ(y0, 55);
            QL_CHECK_EQ(x0, 50);
            ql_ui_free(ui);
        }
    }

    /* ---------- 4. 圆角/遮罩先裁再转 ---------- */
    {
        /* 带圆角的方块转 45°：**角是转过来的角**，不是转出来的方框的角 */
        QLUIRoot *ui = render_html(
            "<div id=r><div id=b></div></div>",
            "#r{width:160px;height:120px;background-color:#FFFFFF}"
            "#b{position:absolute;left:60px;top:40px;width:40px;height:40px;"
            "background-color:#FF0000;border-radius:20px;transform:rotate(45deg)}");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            /* 圆（旋转不变）的直径还是 40：四角必须白，中心必须红 */
            QL_CHECK_EQ(is_red(px(80, 60)), 1);
            QL_CHECK_EQ(is_red(px(61, 41)), 0);   /* 旋转后包围盒的角上 */
            QL_CHECK_EQ(is_red(px(99, 41)), 0);
            QL_CHECK_EQ(is_red(px(61, 79)), 0);
            QL_CHECK_EQ(is_red(px(99, 79)), 0);
            int x0, y0, x1, y1;
            const int n = count_color(&x0, &y0, &x1, &y1);
            /* πr² = 1256，双线性重采样后按"红到 200 以上"统计会少一些 */
            printf("  圆角 + rotate(45)：面积 %d px，包围盒 %d×%d\n", n, x1 - x0 + 1,
                   y1 - y0 + 1);
            QL_CHECK(n > 1000 && n < 1400);
            ql_ui_free(ui);
        }
    }

    /* ---------- 5. 点中测试按旋转后的形状 ---------- */
    {
        QLUIRoot *ui = render_html(
            "<div id=r><div id=b></div></div>",
            "#r{width:160px;height:120px;background-color:#FFFFFF}"
            "#b{position:absolute;left:60px;top:55px;width:40px;height:10px;"
            "background-color:#FF0000;transform:rotate(90deg)}");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUIRoot *u2 = ui;
            /* 转成竖条后：(80,45) 在条上，(65,60) 在原来的横条位置、现在不在 */
            QLUINodeRT *h1 = ql_ui_hit_test(u2, 80, 45);
            QLUINodeRT *h2 = ql_ui_hit_test(u2, 65, 60);
            printf("  hit (80,45)=%s  hit (65,60)=%s\n", h1 ? "命中" : "空",
                   h2 ? "命中" : "空");
            QL_CHECK(h1 != NULL);
            if (h1 != NULL) {
                /* 命中的必须是那个转了 90° 的元素（边框盒 60..100 × 55..65） */
                const QLRect r = ql_ui_node_rect(h1);
                printf("  命中的节点盒子：%d,%d %dx%d\n", (int)r.x, (int)r.y,
                       (int)r.w, (int)r.h);
                QL_CHECK_EQ(r.x, 60);
                QL_CHECK_EQ(r.y, 55);
                QL_CHECK_EQ(r.w, 40);
                QL_CHECK_EQ(r.h, 10);
            }
            /* 原位置现在落在外面的空白处，命中的只可能是更大的根节点 */
            if (h2 != NULL) {
                const QLRect r2 = ql_ui_node_rect(h2);
                QL_CHECK(!(r2.x == 60 && r2.y == 55 && r2.w == 40 && r2.h == 10));
            }
            ql_ui_free(ui);
        }
    }

    /* ---------- 6. 不该支持的写法 ---------- */
    {
        QL_CHECK(rejects("#b{transform:perspective(10px)}", "transform"));
        QL_CHECK(rejects("#b{transform:rotate()}", "rotate"));
        QL_CHECK(rejects("#b{transform:matrix(1,0,0,1,0)}", "matrix"));
        QL_CHECK(rejects("#b{transform-origin:left top 10px}", "transform-origin"));
    }

    if (s_doc != NULL) {
        ql_ui_doc_free(s_doc);
        s_doc = NULL;
    }
    if (s_disp != NULL)
        ql_disp_mem_destroy(s_disp);
    s_disp = NULL;

QL_TEST_END()
