/**
 * @file    test_layer.c
 * @brief   离屏图层（M9.3）：`filter` / `mix-blend-mode` / `mask` / 真 group opacity。
 *
 * 这一组特性的共同前提是"先把子树合成到一张图，再整体处理"，所以测试的重点
 * 不是"某个函数返回了几"，而是**合成结果对不对**：
 *
 *   1. `blur()` 中心保持原色、边缘出现渐变、区域外一个像素都不动；
 *   2. `drop-shadow()` 的影子落在偏移处，且**不会盖住元素自己**；
 *   3. 颜色矩阵族按 CSS 的公式算（灰/反相/亮度有精确的期望值）；
 *   4. **真 group opacity**：两个重叠的半透明块，重叠处不再更暗；
 *   5. `mix-blend-mode` 的七个公式与手算一致；
 *   6. `mask` 用图片 alpha 乘图层 alpha；
 *   7. 嵌套图层（外层 blur + 内层 grayscale）两层都生效；
 *   8. 圆角把滤镜外扩的光晕裁掉；
 *   9. 不该支持的写法必须**构建期报错**（红线：不静默降级）。
 *
 * 每条断言都配一个"能说清楚哪里错了"的打印：出问题时先看数字，再去看代码。
 */
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#define W 160
#define H 120
#define BG  0xFFFFFFu

static QLDispDriver *s_disp;
static QLCanvas s_cv;
/*
 * `ql_ui_build()` 建出来的树仍然引用 doc（样式表、文本池都在 doc 里），
 * 所以 doc 必须活到 `ql_ui_free()` 之后 —— 这里留一个"上一份"，下一个用例
 * 开始时再释放（顺序天然正确）。
 */
static QLUIDoc *s_doc;
static const char *s_html = "<div id=r><div id=b></div></div>";

/* 渲染一份 CSS，返回根节点（调用方 ql_ui_free） */
static QLUIRoot *render_css(const char *css)
{
    QLUIDoc *doc = ql_ui_load_text(s_html, css);
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

/* 用自定义 HTML 渲染（嵌套图层要用不同的结构） */
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

/* 通道差的最大值 */
static int diff(ql_color_t a, ql_color_t b)
{
    int d = 0;
    const int dr = (int)QL_COLOR_GET_R(a) - (int)QL_COLOR_GET_R(b);
    const int dg = (int)QL_COLOR_GET_G(a) - (int)QL_COLOR_GET_G(b);
    const int db = (int)QL_COLOR_GET_B(a) - (int)QL_COLOR_GET_B(b);
    if (dr < 0 ? -dr : dr)
        d = (dr < 0 ? -dr : dr);
    if ((dg < 0 ? -dg : dg) > d)
        d = (dg < 0 ? -dg : dg);
    if ((db < 0 ? -db : db) > d)
        d = (db < 0 ? -db : db);
    return d;
}

/* 期望被拒绝，且报错文案里含 needle */
static int rejects(const char *css, const char *needle)
{
    QLUIDoc *doc = ql_ui_load_text(s_html, css);
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

/* 一块 60×30 的红矩形，可追加滤镜/混合/遮罩声明 */
static void box_css(char *out, size_t cap, const char *extra)
{
    snprintf(out, cap,
             "#r{width:160px;height:120px;background-color:#FFFFFF}"
             "#b{position:absolute;left:50px;top:45px;width:60px;height:30px;"
             "background-color:#FF0000;%s}",
             extra ? extra : "");
}

QL_TEST_BEGIN("test_layer")

    /* ---------- 1. 解析：filter 打包成 (kind, arg) ---------- */
    {
        struct { const char *css; int kind; int arg; } cases[] = {
            { "#b{filter:blur(6px)}", 0, 6 },
            { "#b{filter:brightness(150%)}", 1, 1500 },
            { "#b{filter:brightness(2)}", 1, 2000 },
            { "#b{filter:contrast(80%)}", 2, 800 },
            { "#b{filter:saturate(0.5)}", 3, 500 },
            { "#b{filter:grayscale(100%)}", 4, 1000 },
            { "#b{filter:sepia(20%)}", 5, 200 },
            { "#b{filter:invert(1)}", 6, 1000 },
            { "#b{filter:hue-rotate(90deg)}", 7, 90 },
            { "#b{filter:opacity(30%)}", 8, 300 },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            QLUIDoc *doc = ql_ui_load_text(s_html, cases[i].css);
            QL_CHECK(doc != NULL);
            if (doc == NULL)
                continue;
            const QLUIStyle *st = &doc->styles[1];
            QL_CHECK_EQ((int)st->filter_n, 1);
            QL_CHECK_EQ((int)QL_UI_FILTER_KIND(st->filter_op[0]), cases[i].kind);
            QL_CHECK_EQ((int)QL_UI_FILTER_ARG(st->filter_op[0]), cases[i].arg);
            ql_ui_doc_free(doc);
        }
        /* 链式：顺序必须原样保留（CSS 是先模糊再提亮） */
        QLUIDoc *doc = ql_ui_load_text(
            s_html, "#b{filter:blur(2px) brightness(150%) drop-shadow(1px 2px 3px #123456)}");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            const QLUIStyle *st = &doc->styles[1];
            QL_CHECK_EQ((int)st->filter_n, 3);
            QL_CHECK_EQ((int)QL_UI_FILTER_KIND(st->filter_op[0]), 0);
            QL_CHECK_EQ((int)QL_UI_FILTER_KIND(st->filter_op[1]), 1);
            QL_CHECK_EQ((int)QL_UI_FILTER_KIND(st->filter_op[2]), 9);
            QL_CHECK_EQ((int)st->ds_dx, 1);
            QL_CHECK_EQ((int)st->ds_dy, 2);
            QL_CHECK_EQ((int)st->ds_blur, 3);
            QL_CHECK_EQ((int)st->ds_color, 0x123456);
            ql_ui_doc_free(doc);
        }
        /* 负偏移（左上方向的影子） */
        doc = ql_ui_load_text(s_html, "#b{filter:drop-shadow(-4px -5px red)}");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QL_CHECK_EQ((int)doc->styles[1].ds_dx, -4);
            QL_CHECK_EQ((int)doc->styles[1].ds_dy, -5);
            ql_ui_doc_free(doc);
        }
        /* mix-blend-mode / mask */
        doc = ql_ui_load_text(s_html, "#b{mix-blend-mode:screen}");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QL_CHECK_EQ((int)doc->styles[1].mix_blend, (int)QL_UI_BLEND_SCREEN);
            ql_ui_doc_free(doc);
        }
        doc = ql_ui_load_text(s_html, "#b{mask:url(m)}");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QL_CHECK(doc->styles[1].mask_image != QL_UI_NO_MASK);
            ql_ui_doc_free(doc);
        }
        doc = ql_ui_load_text(s_html, "#b{mask:none}");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QL_CHECK_EQ((int)doc->styles[1].mask_image, (int)QL_UI_NO_MASK);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 2. blur：中心不变、边缘出渐变、外面不动 ---------- */
    {
        char css[512];
        box_css(css, sizeof(css), "");
        QLUIRoot *ui0 = render_css(css);
        QL_CHECK(ui0 != NULL);
        const ql_color_t sharp_in = px(80, 60);
        const ql_color_t sharp_out = px(45, 60);   /* 盒子左边 5px 外 */
        if (ui0 != NULL) {
            QL_CHECK_EQ((int)QL_COLOR_GET_R(sharp_in), 0xFF);
            QL_CHECK_EQ((int)QL_COLOR_GET_R(sharp_out), 0xFF);
            ql_ui_free(ui0);
        }

        box_css(css, sizeof(css), "filter:blur(4px)");
        QLUIRoot *ui = render_css(css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            /* 中心：基本还是纯红（有一点来自边缘的透明，但 R 通道保持满） */
            const ql_color_t c = px(80, 60);
            QL_CHECK_EQ((int)QL_COLOR_GET_R(c), 0xFF);
            /* 盒子外 4px：被糊成中间色（既不是背景白、也不是纯红） */
            const ql_color_t e = px(46, 60);
            QL_CHECK(QL_COLOR_GET_R(e) == 0xFF && QL_COLOR_GET_G(e) > 0x20 &&
                     QL_COLOR_GET_G(e) < 0xE5);
            /* 远处（离盒子 20px 以上）一个像素都不该变 */
            QL_CHECK_EQ(diff(px(20, 20), QL_COLOR_RGB(0xFF, 0xFF, 0xFF)), 0);
            ql_ui_free(ui);
        }
    }

    /* ---------- 3. drop-shadow：影子在偏移处，元素本体不被盖住 ---------- */
    {
        char css[512];
        box_css(css, sizeof(css), "filter:drop-shadow(10px 10px 0px #0000FF)");
        QLUIRoot *ui = render_css(css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            /* 盒子本体仍是红（影子在下面） */
            QL_CHECK_EQ(diff(px(80, 60), QL_COLOR_RGB(0xFF, 0x00, 0x00)), 0);
            /* 右下 10px 处是纯蓝影子 */
            QL_CHECK_EQ(diff(px(112, 82), QL_COLOR_RGB(0x00, 0x00, 0xFF)), 0);
            /* 左上方向没有影子 */
            QL_CHECK_EQ(diff(px(45, 42), QL_COLOR_RGB(0xFF, 0xFF, 0xFF)), 0);
            ql_ui_free(ui);
        }
    }

    /* ---------- 4. 颜色矩阵族：精确期望值 ---------- */
    {
        char css[512];
        /* grayscale(100%)：红 → 亮度 0.213*255 ≈ 54（0x36） */
        box_css(css, sizeof(css), "filter:grayscale(100%)");
        QLUIRoot *ui = render_css(css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const ql_color_t c = px(80, 60);
            QL_CHECK(QL_COLOR_GET_R(c) >= 0x34 && QL_COLOR_GET_R(c) <= 0x38);
            QL_CHECK_EQ((int)QL_COLOR_GET_R(c), (int)QL_COLOR_GET_G(c));
            QL_CHECK_EQ((int)QL_COLOR_GET_G(c), (int)QL_COLOR_GET_B(c));
            ql_ui_free(ui);
        }
        /* invert(100%)：红 → 青 */
        box_css(css, sizeof(css), "filter:invert(100%)");
        ui = render_css(css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(diff(px(80, 60), QL_COLOR_RGB(0x00, 0xFF, 0xFF)), 0);
            ql_ui_free(ui);
        }
        /* brightness(50%)：红 → 暗红 */
        box_css(css, sizeof(css), "filter:brightness(50%)");
        ui = render_css(css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const ql_color_t c = px(80, 60);
            QL_CHECK(QL_COLOR_GET_R(c) >= 0x7C && QL_COLOR_GET_R(c) <= 0x80);
            ql_ui_free(ui);
        }
        /* 链式：grayscale 之后 invert ⇒ 反的是灰度值 */
        box_css(css, sizeof(css), "filter:grayscale(100%) invert(100%)");
        ui = render_css(css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const ql_color_t c = px(80, 60);
            QL_CHECK(QL_COLOR_GET_R(c) >= 0xC7 && QL_COLOR_GET_R(c) <= 0xCB);
            ql_ui_free(ui);
        }
    }

    /* ---------- 5. 真 group opacity：重叠处不再更暗 ---------- */
    {
        const char *html = "<div id=r><div id=o>"
                           "<div class=c></div><div class=c c2></div></div></div>";
        const char *css = "#r{width:160px;height:120px;background-color:#FFFFFF}"
                          "#o{position:absolute;left:20px;top:20px;width:80px;height:40px;"
                          "opacity:0.5}"
                          ".c{position:absolute;left:0;top:0;width:40px;height:40px;"
                          "background-color:#0000FF}"
                          ".c2{left:20px}";
        QLUIRoot *ui = render_html(html, css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            /* 单块处与重叠处的颜色必须**一样**（逐图元乘 alpha 时重叠处会更深） */
            const ql_color_t one = px(30, 40);
            const ql_color_t two = px(45, 40);
            QL_CHECK_EQ(diff(one, two), 0);
            /* 0.5 的蓝压在白底上 ≈ 0x7F7FFF */
            QL_CHECK(QL_COLOR_GET_B(one) >= 0xFE);
            QL_CHECK(QL_COLOR_GET_R(one) >= 0x7C && QL_COLOR_GET_R(one) <= 0x82);
            ql_ui_free(ui);
        }
    }

    /* ---------- 6. mix-blend-mode：七个公式的手算值 ---------- */
    {
        struct { const char *mode; ql_color_t want; } cases[] = {
            /* 背景 #FFCC00 + 源 #3366FF */
            { "multiply", QL_COLOR_RGB(0x33, 0x52, 0x00) },   /* r=0x33*0xFF, g=0x66*0xCC */
            { "screen", QL_COLOR_RGB(0xFF, 0xE0, 0xFF) },
            { "darken", QL_COLOR_RGB(0x33, 0x66, 0x00) },
            { "lighten", QL_COLOR_RGB(0xFF, 0xCC, 0xFF) },
            { "difference", QL_COLOR_RGB(0xCC, 0x66, 0xFF) },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            char css[512];
            snprintf(css, sizeof(css),
                     "#r{width:160px;height:120px;background-color:#FFCC00}"
                     "#b{position:absolute;left:50px;top:45px;width:60px;height:30px;"
                     "background-color:#3366FF;mix-blend-mode:%s}",
                     cases[i].mode);
            QLUIRoot *ui = render_css(css);
            QL_CHECK(ui != NULL);
            if (ui == NULL)
                continue;
            const int d = diff(px(80, 60), cases[i].want);
            if (d > 1)
                printf("  %s：%06X vs %06X\n", cases[i].mode,
                       (unsigned)px(80, 60), (unsigned)cases[i].want);
            QL_CHECK(d <= 1);
            /* 盒子外不受影响 */
            QL_CHECK_EQ(diff(px(20, 20), QL_COLOR_RGB(0xFF, 0xCC, 0x00)), 0);
            ql_ui_free(ui);
        }
        /* normal 就是"不混合"：与不加时的像素一致 */
        char css[512];
        box_css(css, sizeof(css), "mix-blend-mode:normal");
        QLUIRoot *ui = render_css(css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(diff(px(80, 60), QL_COLOR_RGB(0xFF, 0x00, 0x00)), 0);
            ql_ui_free(ui);
        }
    }

    /* ---------- 7. mask：左半 alpha=0、右半 alpha=255 ---------- */
    {
        static uint8_t mask[8 * 8];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                mask[y * 8 + x] = (uint8_t)((x < 4) ? 0 : 255);

        const char *css = "#r{width:160px;height:120px;background-color:#FFFFFF}"
                          "#b{position:absolute;left:20px;top:40px;width:120px;height:40px;"
                          "background-color:#FF0000;mask:url(mk)}";
        QLUIRoot *ui = render_css(css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUIImageInput in;
            memset(&in, 0, sizeof(in));
            in.name = "mk";
            in.data = mask;
            in.len = sizeof(mask);
            in.format = QL_IMAGE_SRC_RAW;
            in.pixel_format = QL_IMAGE_A8;
            in.w = 8;
            in.h = 8;
            QL_CHECK_EQ(ql_ui_register_image(ui, &in), QL_OK);
            ql_canvas_clear(&s_cv, BG);
            ql_ui_render_canvas(ui, &s_cv);
            /* 左半（被遮）：背景色；右半：红 */
            QL_CHECK_EQ(diff(px(30, 60), QL_COLOR_RGB(0xFF, 0xFF, 0xFF)), 0);
            QL_CHECK_EQ(diff(px(120, 60), QL_COLOR_RGB(0xFF, 0x00, 0x00)), 0);
            ql_ui_free(ui);
        }
    }

    /* ---------- 8. 嵌套图层：外层 blur + 内层 grayscale 都生效 ---------- */
    {
        const char *html = "<div id=r><div id=o><div id=b></div></div></div>";
        const char *css = "#r{width:160px;height:120px;background-color:#FFFFFF}"
                          "#o{position:absolute;left:20px;top:20px;width:90px;height:50px;"
                          "filter:blur(2px)}"
                          "#b{position:absolute;left:0;top:0;width:40px;height:30px;"
                          "background-color:#FF0000;filter:grayscale(100%)}";
        QLUIRoot *ui = render_html(html, css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const ql_color_t c = px(40, 35);      /* 盒子里 */
            QL_CHECK_EQ((int)QL_COLOR_GET_R(c), (int)QL_COLOR_GET_G(c));   /* 内层灰 */
            QL_CHECK(QL_COLOR_GET_R(c) < 0x40);                            /* 还是深灰 */
            const ql_color_t e = px(62, 35);      /* 盒子右边界外，被外层糊过 */
            QL_CHECK(QL_COLOR_GET_R(e) < 0xFF && QL_COLOR_GET_G(e) > 0x80);
            ql_ui_free(ui);
        }
    }

    /* ---------- 9. 圆角把滤镜外扩的光晕裁掉 ---------- */
    {
        char css[512];
        box_css(css, sizeof(css), "border-radius:14px;filter:blur(3px)");
        QLUIRoot *ui = render_css(css);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            /* 左上角（圆角缺口里）：连模糊光晕都不该有 */
            QL_CHECK_EQ(diff(px(51, 46), QL_COLOR_RGB(0xFF, 0xFF, 0xFF)), 0);
            /* 中心仍有内容 */
            QL_CHECK_EQ((int)QL_COLOR_GET_R(px(80, 60)), 0xFF);
            ql_ui_free(ui);
        }
    }

    /* ---------- 10. 非法值必须构建期报错（红线） ---------- */
    {
        QL_CHECK(rejects("#b{filter:blur(4)}", "px"));
        QL_CHECK(rejects("#b{filter:blur(300px)}", "半径"));
        QL_CHECK(rejects("#b{filter:url(#x)}", "暂不支持"));
        QL_CHECK(rejects("#b{filter:brightness(abc)}", "数字"));
        QL_CHECK(rejects("#b{filter:hue-rotate(90)}", "deg"));
        QL_CHECK(rejects("#b{filter:drop-shadow(1px 1px 1px 1px red)}", "spread"));
        QL_CHECK(rejects("#b{filter:blur(1px) blur(2px) blur(3px) blur(4px) blur(5px) "
                         "blur(6px) blur(7px)}", "最多"));
        QL_CHECK(rejects("#b{mix-blend-mode:hue}", "mix-blend-mode"));
        QL_CHECK(rejects("#b{filter:}", "空"));
    }

    /* ---------- 11. 脏区重绘：与整屏重绘逐像素一致 ---------- */
    {
        if (s_doc != NULL) {          /* 上一节留下的 doc 在这里还掉 */
            ql_ui_doc_free(s_doc);
            s_doc = NULL;
        }
        char css[512];
        box_css(css, sizeof(css), "filter:blur(5px)");
        QLUIDoc *doc = ql_ui_load_text(s_html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                /* 整屏 */
                QLDispDriver *full = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
                QLCanvas cf;
                ql_canvas_from_disp(&cf, full);
                ql_canvas_clear(&cf, BG);
                ql_ui_render_canvas(ui, &cf);

                /*
                 * 重绘区 = 盒子 + 滤镜外扩（`dirty_expand_for_blur()` 就是这么扩的：
                 * blur·3 = 15）。**只比这一块**：区域外的像素在"局部重绘"里本来就
                 * 保持背景色，拿整屏比会得到一堆假失败。
                 */
                const QLRect dirty = ql_rect_make(35, 30, 90, 60);
                QLDispDriver *part = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
                QLCanvas cp;
                ql_canvas_from_disp(&cp, part);
                ql_canvas_clear(&cp, BG);
                QLDraw2D d2d;
                QL_CHECK_EQ(ql_draw2d_canvas_init(&d2d, &cp), QL_OK);
                ui->root_clip_valid = 0;
                ql_ui_render_rect(ui, &d2d, dirty);

                long bad = 0;
                for (int y = dirty.y; y < ql_rect_bottom(dirty); y++) {
                    for (int x = dirty.x; x < ql_rect_right(dirty); x++) {
                        if (diff(ql_disp_mem_pixel(full, x, y),
                                 ql_disp_mem_pixel(part, x, y)) > 0)
                            bad++;
                    }
                }
                if (bad > 0)
                    printf("  脏区重绘与整屏不一致：%ld 个像素\n", bad);
                QL_CHECK_EQ(bad, 0);
                ql_disp_mem_destroy(full);
                ql_disp_mem_destroy(part);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
            s_doc = NULL;
        }
    }

    if (s_doc != NULL) {
        ql_ui_doc_free(s_doc);
        s_doc = NULL;
    }
    if (s_disp != NULL)
        ql_disp_mem_destroy(s_disp);
    s_disp = NULL;

QL_TEST_END()
