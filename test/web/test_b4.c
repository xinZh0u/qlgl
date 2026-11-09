/**
 * @file    test_b4.c
 * @brief   B4 绘制细节第一批：`border-style: dashed/dotted` 与 `outline`。
 *
 * 这两样都是**纯绘制**（不改布局），所以验证分两层：
 *
 *   1. 解析层：关键字 → 枚举、`none` 把宽度归零、`outline` 简写三段任意顺序；
 *   2. 像素层：虚线沿边有"有/无"交替、outline 落在边框盒**外面**、
 *      `outline-offset` 正负都成立、outline 不影响布局。
 *
 * 虚线用"沿顶边数颜色段"来验证，而不是逐点写死坐标 —— 图案是宽度的整数倍，
 * 只要 `段数 >= 2` 且两种颜色都存在，就说明真的断开画了。
 */
#include <stdlib.h>

#include "../ql_test.h"

#define CW 160
#define CH 120
#define BG 0xFFFFFFu

static QLDispDriver *s_disp;
static QLCanvas s_canvas;
static QLUIDoc *s_doc;

static QLUIRoot *render(const char *html, const char *css)
{
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
    s_doc = doc;
    ql_ui_layout(ui, CW, CH);
    if (s_disp == NULL)
        s_disp = ql_disp_mem_create(CW, CH, QL_COLOR_RGB888);
    ql_canvas_from_disp(&s_canvas, s_disp);
    ql_canvas_clear(&s_canvas, BG);
    ql_ui_render_canvas(ui, &s_canvas);
    return ui;
}

static void release(QLUIRoot *ui)
{
    ql_ui_free(ui);
    if (s_doc != NULL) {
        ql_ui_doc_free(s_doc);
        s_doc = NULL;
    }
}

static ql_color_t px(int x, int y)
{
    return ql_disp_mem_pixel(s_disp, x, y);
}

static int is_bg(ql_color_t c)
{
    return QL_COLOR_GET_R(c) == 0xFF && QL_COLOR_GET_G(c) == 0xFF &&
           QL_COLOR_GET_B(c) == 0xFF;
}

static int is_red(ql_color_t c)
{
    return QL_COLOR_GET_R(c) > 200 && QL_COLOR_GET_G(c) < 60 && QL_COLOR_GET_B(c) < 60;
}

static int is_blue(ql_color_t c)
{
    return QL_COLOR_GET_B(c) > 200 && QL_COLOR_GET_R(c) < 60;
}

/* 沿 y 这一行数"多少次从背景切到非背景"——虚线/点线应当切很多次 */
static int count_runs(int y, int x0, int x1, int want_border)
{
    int runs = 0;
    int inside = 0;
    for (int x = x0; x <= x1; x++) {
        const ql_color_t c = px(x, y);
        const int on = want_border ? is_red(c) : is_bg(c);
        if (on && !inside)
            runs++;
        inside = on;
    }
    return runs;
}

static const QLUIStyle *style_of(QLUIRoot *ui, int idx)
{
    return ql_ui_node_style(&ui->nodes[idx]);
}

QL_TEST_BEGIN("test_b4")

    /* ---------- 1. 解析：border-style 关键字 ---------- */
    {
        const char *H = "<div id=b></div>";
        struct {
            const char *css;
            uint8_t     want;
        } cases[] = {
            { "#b { border: 2px solid #f00 }", QL_UI_BORDER_SOLID },
            { "#b { border: 2px dashed #f00 }", QL_UI_BORDER_DASHED },
            { "#b { border: 2px dotted #f00 }", QL_UI_BORDER_DOTTED },
            { "#b { border-style: dashed; border-width: 3px }", QL_UI_BORDER_DASHED },
            { "#b { border-style: dotted }", QL_UI_BORDER_DOTTED },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            QLUIRoot *ui = render(H, cases[i].css);
            QL_CHECK(ui != NULL);
            if (ui == NULL)
                continue;
            QL_CHECK_EQ(style_of(ui, 0)->border_style, cases[i].want);
            release(ui);
        }
    }

    /* ---------- 2. border-style: none 把宽度归零（否则内容盒会缩水） ---------- */
    {
        const char *H = "<div id=r><div id=b></div></div>";
        QLUIRoot *ui = render(H, "#r { background-color: #FFFFFF }"
                                 "#b { width: 40px; height: 20px;"
                                 "     border-width: 6px; border-style: none }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(style_of(ui, 1)->border_width, 0);   /* 宽度归零 */
            QL_CHECK_EQ(style_of(ui, 1)->border_style, QL_UI_BORDER_NONE);
            const QLRect r = ql_ui_node_rect(&ui->nodes[1]);
            QL_CHECK_EQ(r.w, 40);   /* 没有多出 12px */
            QL_CHECK(is_bg(px(r.x + 1, r.y + 1)));   /* 也没画边 */
            release(ui);
        }
    }

    /* ---------- 3. 虚线 / 点线确实断开了 ---------- */
    {
        /*
         * 统一用 `#r` 包一层：绝对定位要相对一个"已经建好的包含块"，
         * 直接挂在根上的 div 会停在 (0,0)（见 ql_ui.c 的 abs 定位前提）。
         */
        const char *H = "<div id=r><div id=b></div></div>";
        const char *HEAD = "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                           "#b { position: absolute; left: 10px; top: 10px;"
                           "     width: 120px; height: 60px;"
                           "     background-color: #FFFFFF;"
                           "     border-color: #FF0000; border-width: 2px;"
                           "     border-style: ";
        char css[512];

        QLUIRoot *ui = NULL;
        QLRect r = ql_rect_make(0, 0, 0, 0);

        /* solid：整条顶边都是红 → 只有 1 段 */
        snprintf(css, sizeof(css), "%ssolid }", HEAD);
        ui = render(H, css);
        QL_CHECK(ui != NULL);
        r = ql_ui_node_rect(&ui->nodes[1]);
        QL_CHECK_EQ(r.x, 10);
        QL_CHECK_EQ(count_runs(r.y, r.x, r.x + r.w - 1, 1), 1);
        release(ui);

        /* dashed：3×w 段 + 2×w 缝 → 120px 上至少 8 段 */
        snprintf(css, sizeof(css), "%sdashed }", HEAD);
        ui = render(H, css);
        QL_CHECK(ui != NULL);
        r = ql_ui_node_rect(&ui->nodes[1]);
        {
            const int dashes = count_runs(r.y, r.x, r.x + r.w - 1, 1);
            const int gaps = count_runs(r.y, r.x, r.x + r.w - 1, 0);
            QL_CHECK(dashes >= 8);
            QL_CHECK(gaps >= 8);
        }
        release(ui);

        /* dotted：1×w 点 + 1×w 缝，比 dashed 密一倍以上 */
        snprintf(css, sizeof(css), "%sdotted }", HEAD);
        ui = render(H, css);
        QL_CHECK(ui != NULL);
        r = ql_ui_node_rect(&ui->nodes[1]);
        {
            const int dots = count_runs(r.y, r.x, r.x + r.w - 1, 1);
            QL_CHECK(dots >= 16);
            /* 左右两条边也在画：竖着数一遍也有点 */
            QL_CHECK(count_runs(r.y + r.h / 2, r.x, r.x + 1, 1) >= 1);
        }
        release(ui);

        /* `border: 2px dashed #f00` 简写一次给全 */
        ui = render(H, "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                       "#b { position: absolute; left: 10px; top: 10px;"
                       "     width: 120px; height: 60px; background-color: #FFFFFF;"
                       "     border: 2px dashed #FF0000 }");
        QL_CHECK(ui != NULL);
        QL_CHECK_EQ(style_of(ui, 1)->border_width, 2);
        QL_CHECK_EQ(style_of(ui, 1)->border_style, QL_UI_BORDER_DASHED);
        r = ql_ui_node_rect(&ui->nodes[1]);
        QL_CHECK(count_runs(r.y, r.x, r.x + r.w - 1, 1) >= 8);
        release(ui);
    }

    /* ---------- 4. outline 画在边框盒**外面**，且不占布局 ---------- */
    {
        const char *H = "<div id=r><div id=b></div></div>";
        const char *HEAD = "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                           "#b { position: absolute; left: 20px; top: 20px;"
                           "     width: 40px; height: 40px;"
                           "     background-color: #FFFFFF;";

        /* 没有 outline 时，盒子外面是背景 */
        QLUIRoot *ui = render(H, "#r { background-color: #FFFFFF }"
                                 "#b { position: absolute; left: 20px; top: 20px;"
                                 "     width: 40px; height: 40px;"
                                 "     background-color: #FFFFFF }");
        QL_CHECK(ui != NULL);
        QL_CHECK(is_bg(px(19, 40)));
        release(ui);

        /* offset = 0：线贴着边框盒外侧 3px，盒子尺寸不变 */
        char css[512];
        snprintf(css, sizeof(css), "%s outline: 3px solid #0000FF }", HEAD);
        ui = render(H, css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLRect r = ql_ui_node_rect(&ui->nodes[1]);
            const int cy = r.y + r.h / 2;
            QL_CHECK_EQ(r.x, 20);      /* 不占布局 */
            QL_CHECK_EQ(r.w, 40);
            QL_CHECK(is_blue(px(r.x - 1, cy)));   /* 外圈 3px：x-3..x-1 */
            QL_CHECK(is_blue(px(r.x - 3, cy)));
            QL_CHECK(is_bg(px(r.x - 4, cy)));     /* 再外面就没有了 */
            QL_CHECK(is_bg(px(r.x, cy)));         /* 盒子内部不受影响 */
            QL_CHECK_EQ(style_of(ui, 1)->outline_width, 3);
            QL_CHECK_EQ(style_of(ui, 1)->outline_style, QL_UI_BORDER_SOLID);
            release(ui);
        }

        /* offset = 4：整圈往外推 4px，中间留空隙 */
        snprintf(css, sizeof(css), "%s outline: 2px solid #0000FF;"
                                   "     outline-offset: 4px }", HEAD);
        ui = render(H, css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLRect r = ql_ui_node_rect(&ui->nodes[1]);
            const int cy = r.y + r.h / 2;
            QL_CHECK_EQ(style_of(ui, 1)->outline_offset, 4);
            QL_CHECK(is_bg(px(r.x - 1, cy)));     /* 紧贴盒子的 4px 是空隙 */
            QL_CHECK(is_bg(px(r.x - 4, cy)));
            QL_CHECK(is_blue(px(r.x - 6, cy)));   /* 外圈 2px：x-6..x-5 */
            QL_CHECK(is_blue(px(r.x - 5, cy)));
            QL_CHECK(is_bg(px(r.x - 7, cy)));
            release(ui);
        }

        /* offset = -3：线压进边框盒里面（CSS 语义） */
        snprintf(css, sizeof(css), "%s outline: 3px solid #0000FF;"
                                   "     outline-offset: -3px }", HEAD);
        ui = render(H, css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLRect r = ql_ui_node_rect(&ui->nodes[1]);
            const int cy = r.y + r.h / 2;
            QL_CHECK(is_blue(px(r.x, cy)));       /* 盒子最外 3px 变成线 */
            QL_CHECK(is_blue(px(r.x + 2, cy)));
            QL_CHECK(is_bg(px(r.x + 3, cy)));
            QL_CHECK(is_bg(px(r.x - 1, cy)));     /* 外面一点都不画 */
            release(ui);
        }

        /* outline-style: none 等价于没写（宽度有值也不画） */
        snprintf(css, sizeof(css), "%s outline-width: 3px; outline-style: none;"
                                   "     outline-color: #0000FF }", HEAD);
        ui = render(H, css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(style_of(ui, 1)->outline_width, 0);
            QL_CHECK_EQ(style_of(ui, 1)->outline_style, QL_UI_BORDER_NONE);
            QL_CHECK(is_bg(px(19, 40)));
            release(ui);
        }
    }

    /* ---------- 5. border 在内、outline 在外，互不覆盖 ---------- */
    {
        QLUIRoot *ui = render("<div id=r><div id=b></div></div>",
                              "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                              "#b { position: absolute; left: 20px; top: 20px;"
                              "     width: 40px; height: 40px;"
                              "     background-color: #FFFFFF;"
                              "     border: 2px solid #FF0000;"
                              "     outline: 2px dotted #0000FF }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLRect r = ql_ui_node_rect(&ui->nodes[1]);
            const int cy = r.y + r.h / 2;
            QL_CHECK(is_red(px(r.x, cy)));        /* 边框盒最外 2px 是边框色 */
            QL_CHECK(is_red(px(r.x + 1, cy)));
            int dots = 0;                          /* 外面是点线（有点/有空隙） */
            for (int x = r.x - 6; x <= r.x - 1; x++)
                if (is_blue(px(x, cy)))
                    dots++;
            QL_CHECK(dots >= 1);
            release(ui);
        }
    }

    /* ---------- 6. outline 能跟着 :focus 走（focus 环的标准写法） ---------- */
    {
        const char *H = "<div id=r><input id=b type='text'></div>";
        const char *CSS = "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                          "#b { position: absolute; left: 20px; top: 20px;"
                          "     width: 60px; height: 20px;"
                          "     background-color: #FFFFFF }"
                          "#b:focus { outline: 2px solid #0000FF }";
        QLUIRoot *ui = render(H, CSS);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLRect r = ql_ui_node_rect(&ui->nodes[1]);
            const int cy = r.y + r.h / 2;
            QL_CHECK(is_bg(px(r.x - 1, cy)));      /* 没焦点：没有环 */

            ql_ui_set_focus_node(ui, &ui->nodes[1]);
            ql_canvas_clear(&s_canvas, BG);
            ql_ui_render_canvas(ui, &s_canvas);
            QL_CHECK(is_blue(px(r.x - 1, cy)));    /* 有焦点：出现 focus 环 */
            QL_CHECK(is_blue(px(r.x - 2, cy)));
            ql_ui_set_focus_node(ui, NULL);
            release(ui);
        }
    }

    if (s_disp != NULL)
        ql_disp_mem_destroy(s_disp);
    s_disp = NULL;

QL_TEST_END()
