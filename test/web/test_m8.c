/**
 * @file    test_m8.c
 * @brief   M8：CSS3 补全剩余的 B2 / B3 / B4 项。
 *
 *   B2  `:disabled` / `:checked` / `::placeholder`（含复选框控件）
 *   B3  `word-spacing` / `tab-size` / `text-decoration-*`
 *   B4  `background-image: url()` + `background-size/-position/-repeat`
 *
 * 这些都是"能解析但画错了不会报错"的属性，所以每一条都验到**像素**或
 * **布局尺寸**上，只验"能解析"没有意义。
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

static int is_blue(ql_color_t c)
{
    return QL_COLOR_GET_B(c) > 180 && QL_COLOR_GET_R(c) < 80;
}

static int is_red(ql_color_t c)
{
    return QL_COLOR_GET_R(c) > 180 && QL_COLOR_GET_G(c) < 90 && QL_COLOR_GET_B(c) < 90;
}

/* 4×4 四色 RAW 图：左上红、右上绿、左下蓝、右下白（放大后好判方向） */
static const uint16_t k_raw[4 * 4] = {
    0xF800, 0xF800, 0xF800, 0xF800,     /* 上半红 */
    0xF800, 0xF800, 0xF800, 0xF800,
    0x001F, 0x001F, 0x001F, 0x001F,     /* 下半蓝 */
    0x001F, 0x001F, 0x001F, 0x001F,
};

static void register_pic(QLUIRoot *ui, const char *name)
{
    QLUIImageInput in;
    memset(&in, 0, sizeof(in));
    in.name = name;
    in.data = (const uint8_t *)k_raw;
    in.len = sizeof(k_raw);
    in.w = 4;
    in.h = 4;
    in.format = QL_IMAGE_SRC_RAW;
    in.pixel_format = QL_IMAGE_RGB565;
    QL_CHECK_EQ(ql_ui_register_image(ui, &in), QL_OK);
}

static int rejects(const char *css, const char *needle)
{
    const char *H = "<div id=r><div id=a>x</div></div>";
    QLUIDoc *doc = ql_ui_load_text(H, css);
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

/* 反向断言：这份 CSS **应当**编译通过（用来钉住"别把合法写法一起打掉"） */
static int accepts(const char *css)
{
    const char *H = "<div id=r><div id=a>x</div></div>";
    QLUIDoc *doc = ql_ui_load_text(H, css);
    if (doc == NULL) {
        printf("  本该通过却报错了：%s → %s\n", css, ql_ui_text_last_error());
        return 0;
    }
    ql_ui_doc_free(doc);
    return 1;
}

QL_TEST_BEGIN("test_m8")

    /* ===================== B2-1 `:disabled` 改样式 + 挡住焦点 ===================== */
    {
        QLUIRoot *ui = render(
            "<div id=r><input id=a type='text' value='x'></div>",
            "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
            "#a { position: absolute; left: 10px; top: 10px; width: 60px;"
            "     height: 20px; background-color: #FFFFFF }"
            "#a:disabled { background-color: #0000FF }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            /* 没有 disabled：白底、能拿焦点 */
            QL_CHECK(is_bg(px(40, 20)));
            ql_ui_set_focus_node(ui, &ui->nodes[1]);
            QL_CHECK(ql_ui_focus_node(ui) == &ui->nodes[1]);
            ql_ui_set_focus_node(ui, NULL);
            release(ui);
        }

        ui = render("<div id=r><input id=a type='text' value='x' disabled></div>",
                    "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                    "#a { position: absolute; left: 10px; top: 10px; width: 60px;"
                    "     height: 20px; background-color: #FFFFFF }"
                    "#a:disabled { background-color: #0000FF }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK(is_blue(px(40, 20)));          /* 变体生效 */
            ql_ui_set_focus_node(ui, &ui->nodes[1]);
            QL_CHECK(ql_ui_focus_node(ui) == NULL); /* 拿不到焦点 */
            release(ui);
        }
    }

    /* ===================== B2-2 `:enabled` ===================== */
    {
        QLUIRoot *ui = render(
            "<div id=r><input id=a type='text'><input id=b type='text' disabled></div>",
            "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
            "input { position: absolute; width: 40px; height: 20px;"
            "        background-color: #FFFFFF }"
            "#a { left: 10px; top: 10px } #b { left: 10px; top: 50px }"
            "input:enabled { background-color: #0000FF }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK(is_blue(px(30, 20)));      /* #a enabled */
            QL_CHECK(is_bg(px(30, 60)));        /* #b disabled */
            release(ui);
        }
    }

    /* ===================== B2-3 复选框：默认尺寸 / 勾选切换 / :checked ===================== */
    {
        QLUIRoot *ui = render(
            "<div id=r><input id=a type='checkbox'></div>",
            "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
            "#a { position: absolute; left: 10px; top: 10px;"
            "     background-color: #FFFFFF; border: 1px solid #000000 }"
            "#a:checked { background-color: #0000FF }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLRect r = ql_ui_node_rect(&ui->nodes[1]);
            QL_CHECK_EQ(r.w, 16);    /* UA 默认 16×16（border-box，含 1px 边框） */
            QL_CHECK_EQ(r.h, 16);
            QL_CHECK(is_bg(px(19, 19)));        /* 未勾选：白 */

            /* 点一下 → 勾选 */
            QL_CHECK_EQ(ql_ui_pointer_down(ui, 19, 19), 1);
            (void)ql_ui_pointer_up(ui, 19, 19);
            ql_canvas_clear(&s_canvas, BG);
            ql_ui_render_canvas(ui, &s_canvas);
            QL_CHECK(is_blue(px(19, 19)));      /* :checked 生效 */
            release(ui);
        }
    }

    /* ===================== B2-4 复选框默认 checked + disabled 不可切 ===================== */
    {
        QLUIRoot *ui = render(
            "<div id=r><input id=a type='checkbox' checked disabled></div>",
            "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
            "#a { position: absolute; left: 10px; top: 10px;"
            "     background-color: #FFFFFF }"
            "#a:checked { background-color: #0000FF }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK(is_blue(px(19, 19)));      /* 初始勾选 */
            QL_CHECK_EQ(ql_ui_pointer_down(ui, 19, 19), 1);
            (void)ql_ui_pointer_up(ui, 19, 19);
            ql_canvas_clear(&s_canvas, BG);
            ql_ui_render_canvas(ui, &s_canvas);
            QL_CHECK(is_blue(px(19, 19)));      /* 禁用：点了也不变 */
            release(ui);
        }
    }

    /* ===================== B2-5 `::placeholder` ===================== */
    {
        QLUIRoot *ui = render(
            "<div id=r><input id=a type='text' placeholder='hint'></div>",
            "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
            "#a { position: absolute; left: 10px; top: 10px; width: 80px;"
            "     height: 30px; background-color: #FFFFFF; color: #000000;"
            "     font-size: 12px }"
            "#a::placeholder { color: #FF0000 }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            int red = 0;
            for (int y = 10; y < 40; y++)
                for (int x = 10; x < 90; x++)
                    if (is_red(px(x, y)))
                        red++;
            QL_CHECK(red > 0);      /* 占位文字用 ::placeholder 的红色画出来了 */
            release(ui);
        }
    }

    /* ===================== B3-1 `word-spacing` 只加空格之后 ===================== */
    {
        QLUIRoot *ui = render("<div id=r><div id=a>a b</div></div>",
                              "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                              "#a { position: absolute; left: 10px; top: 10px;"
                              "     font-size: 12px; color: #000000;"
                              "     background-color: #FFFFFF }");
        QL_CHECK(ui != NULL);
        const ql_coord_t w0 = (ui != NULL) ? ql_ui_node_rect(&ui->nodes[1]).w : 0;
        release(ui);

        ui = render("<div id=r><div id=a>a b</div></div>",
                    "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                    "#a { position: absolute; left: 10px; top: 10px;"
                    "     font-size: 12px; color: #000000; word-spacing: 10px;"
                    "     background-color: #FFFFFF }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(ql_ui_node_rect(&ui->nodes[1]).w, w0 + 10);
            release(ui);
        }
    }

    /* ===================== B3-2 `tab-size`：'\t' 折算成 N 个空格宽 ===================== */
    {
        QLUIRoot *ui = render("<div id=r><div id=a>a\tb</div></div>",
                              "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                              "#a { position: absolute; left: 0px; top: 0px; font-size: 12px;"
                              "     color: #000000; white-space: pre }");
        QL_CHECK(ui != NULL);
        const ql_coord_t w_tab0 = (ui != NULL) ? ql_ui_node_rect(&ui->nodes[1]).w : 0;
        release(ui);

        ui = render("<div id=r><div id=a>a\tb</div></div>",
                    "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                    "#a { position: absolute; left: 0px; top: 0px; font-size: 12px;"
                    "     color: #000000; white-space: pre; tab-size: 4 }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const ql_coord_t w4 = ql_ui_node_rect(&ui->nodes[1]).w;
            QL_CHECK(w4 < w_tab0);      /* 4 个空格比默认 8 个窄 */
            QL_CHECK(w4 > 0);
            release(ui);
        }

        ui = render("<div id=r><div id=a>a\tb</div></div>",
                    "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                    "#a { position: absolute; left: 0px; top: 0px; font-size: 12px;"
                    "     color: #000000; white-space: pre; tab-size: 2 }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK(ql_ui_node_rect(&ui->nodes[1]).w < w_tab0);
            release(ui);
        }
    }

    /* ===================== B3-3 `text-decoration-*`：粗细 / 颜色 / 样式 ===================== */
    {
        /* underline 默认 1px 用文字色 */
        QLUIRoot *ui = render("<div id=r><div id=a>UU</div></div>",
                              "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                              "#a { position: absolute; left: 10px; top: 10px;"
                              "     font-size: 14px; color: #000000;"
                              "     text-decoration: underline }");
        QL_CHECK(ui != NULL);
        int plain_ink = 0;
        if (ui != NULL) {
            for (int y = 10; y < 40; y++)
                for (int x = 10; x < 60; x++)
                    if (QL_COLOR_GET_R(px(x, y)) < 120)
                        plain_ink++;
            release(ui);
        }

        ui = render("<div id=r><div id=a>UU</div></div>",
                    "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                    "#a { position: absolute; left: 10px; top: 10px;"
                    "     font-size: 14px; color: #000000;"
                    "     text-decoration: underline; text-decoration-thickness: 4px;"
                    "     text-decoration-color: #FF0000 }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            int red = 0;
            for (int y = 10; y < 40; y++)
                for (int x = 10; x < 60; x++)
                    if (is_red(px(x, y)))
                        red++;
            QL_CHECK(red > 0);              /* 红色粗线画出来了 */
            QL_CHECK(QL_COLOR_GET_R(px(12, 12)) < 255 || plain_ink > 0);
            release(ui);
        }

        /* overline（B3 新增 ）*/
        ui = render("<div id=r><div id=a>OO</div></div>",
                    "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                    "#a { position: absolute; left: 10px; top: 20px;"
                    "     font-size: 14px; color: #000000;"
                    "     text-decoration: overline; text-decoration-color: #0000FF }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            int blue = 0;
            for (int y = 15; y < 40; y++)
                for (int x = 10; x < 60; x++)
                    if (is_blue(px(x, y)))
                        blue++;
            QL_CHECK(blue > 0);
            release(ui);
        }

        /* dotted / double 也真的断开/分开画 */
        ui = render("<div id=r><div id=a>DD</div></div>",
                    "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                    "#a { position: absolute; left: 10px; top: 10px;"
                    "     font-size: 16px; color: #000000;"
                    "     text-decoration: underline; text-decoration-style: dotted;"
                    "     text-decoration-thickness: 2px }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            /* 装饰线的 y 位置跟字体相关，扫一遍找"最像线"的那一行 */
            int best_runs = 0;
            for (int y = 20; y < 40; y++) {
                int runs = 0, inside = 0;
                for (int x = 10; x < 70; x++) {
                    const int dark = (QL_COLOR_GET_R(px(x, y)) < 140);
                    if (dark && !inside)
                        runs++;
                    inside = dark;
                }
                if (runs > best_runs)
                    best_runs = runs;
            }
            QL_CHECK(best_runs >= 3);        /* 点线：断成好几截 */
            release(ui);
        }
    }

    /* ===================== B4-1 `background-image` + `background-size: cover` ===== */
    {
        QLUIRoot *ui = render(
            "<div id=r><div id=a></div></div>",
            "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
            "#a { position: absolute; left: 20px; top: 20px; width: 80px; height: 40px;"
            "     background-image: url(pic); background-size: cover }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            register_pic(ui, "pic");
            ql_ui_layout(ui, CW, CH);
            ql_canvas_clear(&s_canvas, BG);
            ql_ui_render_canvas(ui, &s_canvas);
            /* 4×4 盖满 80×40：上半红、下半蓝 */
            QL_CHECK(is_red(px(25, 25)));
            QL_CHECK(is_red(px(95, 30)));
            QL_CHECK(is_blue(px(25, 55)));
            QL_CHECK(is_blue(px(95, 55)));
            release(ui);
        }
    }

    /* ===================== B4-2 `background-position` 摆位 ===================== */
    {
        QLUIRoot *ui = render(
            "<div id=r><div id=a></div></div>",
            "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
            "#a { position: absolute; left: 10px; top: 10px; width: 40px; height: 40px;"
            "     background-image: url(pic); background-size: 20px 20px;"
            "     background-position: right bottom; background-repeat: no-repeat }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            register_pic(ui, "pic");
            ql_ui_layout(ui, CW, CH);
            ql_canvas_clear(&s_canvas, BG);
            ql_ui_render_canvas(ui, &s_canvas);
            /* 20×20 贴在右下角：左上角是背景（白），右下块里是图（上红下蓝） */
            QL_CHECK(is_bg(px(12, 12)));
            QL_CHECK(is_bg(px(45, 45)) == 0);
            release(ui);
        }
    }

    /* ===================== B4-3 `background-repeat` 平铺满整块 ===================== */
    {
        QLUIRoot *ui = render(
            "<div id=r><div id=a></div></div>",
            "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
            "#a { position: absolute; left: 10px; top: 10px; width: 40px; height: 40px;"
            "     background-image: url(pic); background-size: 10px 10px }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            register_pic(ui, "pic");
            ql_ui_layout(ui, CW, CH);
            ql_canvas_clear(&s_canvas, BG);
            ql_ui_render_canvas(ui, &s_canvas);
            /*
             * 平铺的本质是"盒子被铺满"：所以断言 (a) 盒内没有背景色残留、
             * (b) 两种颜色都出现（=> 确实铺了多块，而不是只画一块拉伸）。
             */
            int bg_left = 0, red = 0, blue = 0;
            for (int y = 10; y < 50; y++)
                for (int x = 10; x < 50; x++) {
                    const ql_color_t c = px(x, y);
                    if (is_bg(c))
                        bg_left++;
                    else if (is_red(c))
                        red++;
                    else if (is_blue(c))
                        blue++;
                }
            QL_CHECK_EQ(bg_left, 0);
            QL_CHECK(red > 100);
            QL_CHECK(blue > 100);
            release(ui);
        }
    }

    /* ===================== 明确报错（不静默忽略） ===================== */
    {
        QL_CHECK(rejects("#a { tab-size: 0.5 }", "tab-size 只支持 1~32 个空格"));
        QL_CHECK(rejects("#a { tab-size: 20px }", "tab-size 只支持 1~32 个空格"));
        QL_CHECK(rejects("#a { text-decoration-style: wavy }",
                         "不支持的 text-decoration-style"));
        QL_CHECK(rejects("#a { text-decoration-thickness: 40px }",
                         "text-decoration-thickness 必须在 1~16px"));
        QL_CHECK(rejects("#a { background-repeat: round }", "background-repeat"));
        QL_CHECK(rejects("#a { background-image: url() }", "没有文件名"));
        QL_CHECK(rejects("#a { word-spacing: 999px }", "word-spacing 超出"));
    }

    /*
     * ============ "只写一条长写"也要报错（回归：以前静默吞掉） ============
     *
     * `transition-timing-function` / `transition-property` 以前整块逻辑都在
     * `if (transition-duration)` 里面，于是**不写 duration 时非法值被静默吃**。
     * 无块 at-rule（`@import`）同理：整份 CSS 只有 `@import` 时以前直接 break。
     */
    {
        /* 非法**缓动名字**仍然报错（cubic-bezier 本身现在支持了） */
        QL_CHECK(rejects("#a { transition-timing-function: bounce }", "不支持的缓动"));
        QL_CHECK(rejects("#a { transition-property: filter }", "transition-property"));
        QL_CHECK(rejects("@import \"x.css\";", "不支持 at-rule"));
        /* 合法值 / 尾部注释与分号不能被误伤 */
        QL_CHECK(accepts("#a { transition-timing-function: ease-out }"));
        QL_CHECK(accepts("#a { transition-property: color, opacity }"));
        QL_CHECK(accepts("#a { color: red } ; /* tail */ "));
        /* `@charset` 是真实 CSS 的第一行，跳过不报错 */
        QL_CHECK(accepts("@charset \"utf-8\"; #a { color: red }"));
    }

    if (s_disp != NULL)
        ql_disp_mem_destroy(s_disp);
    s_disp = NULL;

QL_TEST_END()
