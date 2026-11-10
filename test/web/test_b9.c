/**
 * @file    test_b9.c
 * @brief   B9：`clip-path` 的四种形状 + 字体变体 + L1 小件（blob v34）。
 *
 * 覆盖：
 *   clip-path   circle / ellipse / inset / polygon（解析 + 真的裁到像素）
 *   font-variant-caps: small-caps / font-variant-numeric: tabular-nums
 *   text-align: justify
 *   box-shadow 的 spread
 *   border-radius: a / b（椭圆圆角，绘制侧取两分量平均，见文档）
 *   font 简写 / word-wrap 别名 / :empty / revert
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#define CW 120
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
    const uint8_t *row = s_canvas.buffer + (uint32_t)y * s_canvas.stride;
    const uint8_t *p = row + (size_t)x * 3u;
    return (ql_color_t)(((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]);
}

/* 期望被拒绝，且报错文案里含 needle */
static int rejects(const char *html, const char *css, const char *needle)
{
    QLUIDoc *doc = ql_ui_load_text(html, css);
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

/*
 * 取第 n 个 <div> 的样式（0 = 文档里第一个 div）。
 * QLUINode 里没有 id 字段（构建期把 id 折进了选择器），所以按文档顺序取最稳。
 */
static const QLUIStyle *style_n(QLUIDoc *doc, int n)
{
    /* 第 0 个 div 是测试用的包裹层，跳过；n 从 1 开始数测试里的元素 */
    int seen = 0;
    for (uint16_t i = 0; i < doc->node_count; i++) {
        const QLUINode *nd = &doc->nodes[i];
        if (nd->tag != QL_UI_TAG_DIV)
            continue;
        if (seen++ == 0)
            continue;
        if (n-- == 0)
            return &doc->styles[nd->style];
    }
    return NULL;
}

QL_TEST_BEGIN("b9")
    /* ============ 1. clip-path 四种形状的解析 ============ */
    {
        QLUIDoc *doc = ql_ui_load_text(
            "<div id=w><div id=a></div><div id=b></div><div id=c></div>"
            "<div id=d></div><div id=e></div></div>",
            "#a { clip-path: circle(40%) }"
            "#b { clip-path: ellipse(30% 20%) }"
            "#c { clip-path: inset(10% 20% 30% 40%) }"
            "#d { clip-path: polygon(0 0, 100% 0, 50% 100%) }"
            "#e { clip-path: none }");
        QL_CHECK(doc != NULL);
        if (doc == NULL) {
            printf("    %s\n", ql_ui_text_last_error());
        } else {
            QL_CHECK_EQ(style_n(doc, 0)->clip_kind, QL_UI_CLIP_CIRCLE);
            QL_CHECK_EQ(style_n(doc, 0)->clip_r, 400);

            const QLUIStyle *sb = style_n(doc, 1);
            QL_CHECK_EQ(sb->clip_kind, QL_UI_CLIP_ELLIPSE);
            QL_CHECK_EQ(sb->clip_box[0].value, 30);
            QL_CHECK_EQ(sb->clip_box[0].unit, QL_UI_UNIT_PERCENT);
            QL_CHECK_EQ(sb->clip_box[1].value, 20);

            const QLUIStyle *sc = style_n(doc, 2);
            QL_CHECK_EQ(sc->clip_kind, QL_UI_CLIP_INSET);
            /* 上右下左 */
            QL_CHECK_EQ(sc->clip_box[0].value, 10);
            QL_CHECK_EQ(sc->clip_box[1].value, 20);
            QL_CHECK_EQ(sc->clip_box[2].value, 30);
            QL_CHECK_EQ(sc->clip_box[3].value, 40);

            const QLUIStyle *sd = style_n(doc, 3);
            QL_CHECK_EQ(sd->clip_kind, QL_UI_CLIP_POLYGON);
            QL_CHECK_EQ(sd->clip_pt_n, 3);
            QL_CHECK(sd->clip_pts != QL_UI_NO_CLIP_PTS);
            QL_CHECK(doc->clip_pt_count >= 3);
            if (doc->clip_pts != NULL) {
                QL_CHECK_EQ(doc->clip_pts[sd->clip_pts + 1].x.value, 100);  /* 第 2 点 x */
                QL_CHECK_EQ(doc->clip_pts[sd->clip_pts + 2].y.value, 100);  /* 第 3 点 y */
            }

            QL_CHECK_EQ(style_n(doc, 4)->clip_kind, QL_UI_CLIP_NONE);
            ql_ui_doc_free(doc);
        }
    }

    /* 1 个 / 2 个 / 3 个值的 inset 展开规则 */
    {
        QLUIDoc *doc = ql_ui_load_text(
            "<div id=w><div id=a></div><div id=b></div><div id=c></div></div>",
            "#a { clip-path: inset(5%) }"
            "#b { clip-path: inset(5% 10%) }"
            "#c { clip-path: inset(5% 10% 15%) }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            const QLUIStyle *a = style_n(doc, 0);
            QL_CHECK_EQ(a->clip_box[0].value, 5);
            QL_CHECK_EQ(a->clip_box[1].value, 5);
            QL_CHECK_EQ(a->clip_box[2].value, 5);
            QL_CHECK_EQ(a->clip_box[3].value, 5);
            const QLUIStyle *b2 = style_n(doc, 1);
            QL_CHECK_EQ(b2->clip_box[0].value, 5);
            QL_CHECK_EQ(b2->clip_box[1].value, 10);
            QL_CHECK_EQ(b2->clip_box[2].value, 5);   /* 下 = 上 */
            QL_CHECK_EQ(b2->clip_box[3].value, 10);  /* 左 = 右 */
            const QLUIStyle *c2 = style_n(doc, 2);
            QL_CHECK_EQ(c2->clip_box[2].value, 15);
            QL_CHECK_EQ(c2->clip_box[3].value, 10);  /* 左 = 右 */
            ql_ui_doc_free(doc);
        }
    }

    /* 同一份多边形顶点要去重（只存一份） */
    {
        QLUIDoc *doc = ql_ui_load_text(
            "<div id=w><div id=a></div><div id=b></div></div>",
            "#a, #b { clip-path: polygon(0 0, 100% 0, 50% 100%) }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QL_CHECK_EQ(style_n(doc, 0)->clip_pts, style_n(doc, 1)->clip_pts);
            QL_CHECK_EQ(doc->clip_pt_count, 3);   /* 3 个顶点，只存一次 */
            ql_ui_doc_free(doc);
        }
    }

    /* ============ 2. clip-path 真的裁到像素 ============ */
    /* inset：四边各裁掉 25% ⇒ 只有中间 50% 那块有颜色 */
    {
        QLUIRoot *ui = render("<div id=w><div id=a></div></div>",
                              "#w { position: absolute; left: 0; top: 0; width: 120px;"
                              "     height: 120px }\n"
                              "#a { position: absolute; left: 20px; top: 20px;"
                              "     width: 80px; height: 80px; background: #FF0000;"
                              "     clip-path: inset(25%) }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(px(60, 60) & 0xFFFFFFu, 0xFF0000u);   /* 中心：在 */
            QL_CHECK_EQ(px(24, 60) & 0xFFFFFFu, BG);          /* 左侧外：被裁 */
            QL_CHECK_EQ(px(96, 60) & 0xFFFFFFu, BG);          /* 右侧外 */
            QL_CHECK_EQ(px(60, 24) & 0xFFFFFFu, BG);          /* 上侧外 */
            QL_CHECK_EQ(px(60, 96) & 0xFFFFFFu, BG);          /* 下侧外 */
        }
        release(ui);
    }

    /* ellipse：左右两端（椭圆外的角落）被裁掉，中心保留 */
    {
        QLUIRoot *ui = render("<div id=w><div id=a></div></div>",
                              "#w { position: absolute; left: 0; top: 0; width: 120px;"
                              "     height: 120px }\n"
                              "#a { position: absolute; left: 20px; top: 20px;"
                              "     width: 80px; height: 80px; background: #00A000;"
                              "     clip-path: ellipse(50% 50%) }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(px(60, 60) & 0xFFFFFFu, 0x00A000u);   /* 中心 */
            /* 左中：椭圆的边界恰好在 x=20，取 21 应该在（AA 边缘留 1px） */
            QL_CHECK_EQ(px(26, 60) & 0xFFFFFFu, 0x00A000u);
            /* 左上角：椭圆外 */
            QL_CHECK_EQ(px(22, 22) & 0xFFFFFFu, BG);
            /* 右上角同理 */
            QL_CHECK_EQ(px(98, 22) & 0xFFFFFFu, BG);
        }
        release(ui);
    }

    /* polygon：三角形 ⇒ 左下角在形状外 */
    {
        QLUIRoot *ui = render(
            "<div id=w><div id=a></div></div>",
            "#w { position: absolute; left: 0; top: 0; width: 120px; height: 120px }\n"
            "#a { position: absolute; left: 20px; top: 20px;"
            "     width: 80px; height: 80px; background: #0000FF;"
            "     clip-path: polygon(0 0, 100% 0, 50% 100%) }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(px(60, 30) & 0xFFFFFFu, 0x0000FFu);   /* 上部中间：在 */
            QL_CHECK_EQ(px(60, 92) & 0xFFFFFFu, 0x0000FFu);   /* 底部尖端附近：在 */
            QL_CHECK_EQ(px(24, 92) & 0xFFFFFFu, BG);          /* 左下角：外 */
            QL_CHECK_EQ(px(96, 92) & 0xFFFFFFu, BG);          /* 右下角：外 */
            /*
             * 抗锯齿带宽度：斜边上"既不是背景也不是纯色"的像素。
             * 解析覆盖率只有 ±0.5px 的过渡带（每行 1~3 个），早先的
             * "64×64 蒙版图按边框盒拉伸"版本这里是 10+ 个（糊成一片）。
             */
            {
                int worst = 0;
                for (int y = 22; y < 98; y++) {
                    int cnt = 0;
                    for (int x = 20; x < 100; x++) {
                        const uint32_t v = (uint32_t)px(x, y) & 0xFFFFFFu;
                        if (v != BG && v != 0x0000FFu)
                            cnt++;
                    }
                    if (cnt > worst)
                        worst = cnt;
                }
                QL_CHECK(worst > 0);        /* 确实有边缘 */
                QL_CHECK(worst <= 3);       /* 而且边缘很窄（≤ 3px） */
            }
        }
        release(ui);
    }

    /* ============ 3. 字体变体 ============ */
    {
        QLUIDoc *doc = ql_ui_load_text(
            "<div id=w><div id=a></div><div id=b></div><div id=c></div></div>",
            "#a { font-variant-caps: small-caps }"
            "#b { font-variant-numeric: tabular-nums }"
            "#c { font-variant-caps: normal; font-variant-numeric: normal }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QL_CHECK_EQ(style_n(doc, 0)->variant_flags & QL_UI_VARIANT_SMALL_CAPS,
                        QL_UI_VARIANT_SMALL_CAPS);
            QL_CHECK_EQ(style_n(doc, 0)->variant_flags & QL_UI_VARIANT_TABULAR_NUMS, 0);
            QL_CHECK_EQ(style_n(doc, 1)->variant_flags & QL_UI_VARIANT_TABULAR_NUMS,
                        QL_UI_VARIANT_TABULAR_NUMS);
            QL_CHECK_EQ(style_n(doc, 2)->variant_flags, 0);
            ql_ui_doc_free(doc);
        }
    }
    QL_CHECK(rejects("<div id=a></div>", "#a { font-variant-caps: petite-caps }",
                     "font-variant-caps"));

    /* ============ 4. text-align: justify ============ */
    {
        QLUIDoc *doc = ql_ui_load_text(
            "<div id=w><div id=a>hello world foo bar baz</div></div>",
            "#a { width: 100px; text-align: justify; white-space: normal }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QL_CHECK_EQ(style_n(doc, 0)->text_align, QL_TEXT_ALIGN_JUSTIFY);
            ql_ui_doc_free(doc);
        }
    }

    /* ============ 5. box-shadow 的 spread ============ */
    {
        QLUIDoc *doc = ql_ui_load_text("<div id=w><div id=a></div></div>",
                                       "#a { box-shadow: 0 0 0 10px #FF0000 }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QL_CHECK_EQ(style_n(doc, 0)->shadow[0].spread, 10);
            QL_CHECK_EQ(style_n(doc, 0)->shadow[0].blur, 0);
            ql_ui_doc_free(doc);
        }
    }
    /* spread 真的把阴影放大：盒子 20×20 放在 40,40，spread 10 ⇒ 30,30 处有阴影 */
    {
        QLUIRoot *ui = render("<div id=w><div id=a></div></div>",
                              "#w { position: absolute; left: 0; top: 0; width: 120px;"
                              "     height: 120px }\n"
                              "#a { position: absolute; left: 40px; top: 40px;"
                              "     width: 20px; height: 20px; background: #000000;"
                              "     box-shadow: 0 0 0 10px #FF0000 }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(px(35, 50) & 0xFFFFFFu, 0xFF0000u);   /* 阴影环里 */
            QL_CHECK_EQ(px(50, 50) & 0xFFFFFFu, 0x000000u);   /* 自己的背景 */
        }
        release(ui);
    }

    /* ============ 6. 椭圆圆角（a / b） ============ */
    {
        QLUIDoc *doc = ql_ui_load_text("<div id=w><div id=a></div></div>",
                                       "#a { border-radius: 10px / 20px }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QL_CHECK_EQ(style_n(doc, 0)->radius[0], 10);
            QL_CHECK_EQ(style_n(doc, 0)->radius_v[0], 20);
            ql_ui_doc_free(doc);
        }
    }

    /* ============ 7. font 简写 ============ */
    {
        QLUIDoc *doc = ql_ui_load_text(
            "<div id=w><div id=a>BoldItalic</div></div>",
            "#a { font: italic bold 20px/28px monospace }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            const QLUIStyle *s = style_n(doc, 0);
            QL_CHECK_EQ(s->font_flags & QL_UI_FONT_ITALIC, QL_UI_FONT_ITALIC);
            QL_CHECK_EQ(s->font_flags & QL_UI_FONT_BOLD, QL_UI_FONT_BOLD);
            QL_CHECK_EQ(s->line_h, 28);
            QL_CHECK_EQ(s->font_family, 1);   /* monospace */
            ql_ui_doc_free(doc);
        }
    }
    QL_CHECK(rejects("<div id=a></div>", "#a { font: bold monospace }", "缺少字号"));

    /* ============ 8. word-wrap 别名 ============ */
    {
        QLUIDoc *doc = ql_ui_load_text("<div id=w><div id=a>aaaaaaaaaa</div></div>",
                                       "#a { word-wrap: break-word }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QL_CHECK_EQ(style_n(doc, 0)->word_break, QL_UI_WORDBREAK_ALL);
            ql_ui_doc_free(doc);
        }
    }

    /* ============ 9. :empty ============ */
    {
        /*
         * CSS 的定义：`:empty` 匹配"没有子元素、也没有非空文本"的元素 ——
         * **只有空白**的元素算 empty（它不产生任何盒子）。
         */
        QLUIDoc *doc = ql_ui_load_text(
            "<div id=w><div id=a class=e></div><div id=b class=e>  </div>"
            "<div id=c class=e>x</div></div>",
            ".e:empty { background: #00FF00 }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QL_CHECK_EQ(style_n(doc, 0)->bg_color & 0xFFFFFFu, 0x00FF00u);  /* 真空 */
            QL_CHECK_EQ(style_n(doc, 1)->bg_color & 0xFFFFFFu, 0x00FF00u);  /* 只有空白 */
            QL_CHECK_EQ(style_n(doc, 2)->bg_color & 0xFFFFFFu, 0x000000u);  /* 有文本 */
            ql_ui_doc_free(doc);
        }
    }
    {
        QLUIDoc *doc = ql_ui_load_text(
            "<div id=w><div id=a class=e></div><div id=b class=e><i></i></div></div>",
            ".e:empty { background: #00FF00 }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QL_CHECK_EQ(style_n(doc, 0)->bg_color & 0xFFFFFFu, 0x00FF00u);
            /* b 有子元素 ⇒ 不匹配 */
            QL_CHECK_EQ(style_n(doc, 1)->bg_color & 0xFFFFFFu, 0x000000u);
            ql_ui_doc_free(doc);
        }
    }

    /* ============ 10. revert 关键字（颜色位置） ============ */
    {
        QLUIDoc *doc = ql_ui_load_text("<div id=w><div id=a></div></div>",
                                       "#a { color: revert }");
        QL_CHECK(doc != NULL);
        if (doc != NULL)
            ql_ui_doc_free(doc);
    }

    /* ============ 11. 各种非法写法 ============ */
    QL_CHECK(rejects("<div id=a></div>", "#a { clip-path: circle(200%) }",
                     "0~100"));
    QL_CHECK(rejects("<div id=a></div>", "#a { clip-path: ellipse(50%) }",
                     "ellipse()"));
    QL_CHECK(rejects("<div id=a></div>", "#a { clip-path: polygon(0px 0px, 1px 1px, 2px) }",
                     "每个顶点"));
    QL_CHECK(rejects("<div id=a></div>", "#a { clip-path: bowtie(1) }", "clip-path"));
    QL_CHECK(rejects("<div id=a></div>", "#a { box-shadow: 0 0 0 0 0 #f00 }",
                     "2~4 个长度"));

    if (s_disp != NULL) {
        ql_disp_mem_destroy(s_disp);
        s_disp = NULL;
    }

QL_TEST_END()
