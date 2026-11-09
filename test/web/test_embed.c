/**
 * @file    test_embed.c
 * @brief   EMBED 包（13 个只做占位的标签）：解析、内容跳过、盒子、占位提示、blob。
 *
 * 这一包的价值不在"能画"，而在**整页 HTML 贴进来能编译**。所以测试的重心是：
 *   1. 13 个标签全部认识，不报"不支持的标签"；
 *   2. `<script>` / `<template>` / `<svg>` / `<math>` 的**内容一个字节都不进树**
 *      —— 尤其 `<svg>` 里的 `<path>` / `<circle>` 是另一门标记语言，
 *      真去解析就会让整页编译失败；
 *   3. 替换型元素有占位盒（默认 300×150 等），元数据型零盒子；
 *   4. 占位提示（标签名）确实画出来了；
 *   5. 枚举值进 blob 后能原样回来（`QLUITag` 只能末尾追加）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#define W 420
#define H 420

static QLUIDoc *mk(const char *html, const char *css, QLUIRoot **out_ui)
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
    ql_ui_layout(ui, W, H);
    *out_ui = ui;
    return doc;
}

/* 在 doc 的文本池里找子串（用来断言"内容没进池"） */
static int pool_contains(const QLUIDoc *doc, const char *needle)
{
    if (doc->text_pool == NULL)
        return 0;
    for (uint16_t i = 0; i < doc->text_count; i++) {
        const char *s = doc->text_pool + doc->text_offsets[i];
        if (strstr(s, needle) != NULL)
            return 1;
    }
    return 0;
}

/* 按标签名取第 nth 个节点 */
static QLUINodeRT *find_tag(QLUIRoot *ui, const char *name, int nth)
{
    for (uint16_t i = 0; i < ui->node_count; i++) {
        if (strcmp(ql_ui_tag_name(ui->nodes[i].src->tag), name) != 0)
            continue;
        if (nth-- == 0)
            return &ui->nodes[i];
    }
    return NULL;
}

static QLDispDriver *s_disp;

static ql_color_t px(int x, int y)
{
    return ql_disp_mem_pixel(s_disp, (ql_coord_t)x, (ql_coord_t)y);
}

QL_TEST_BEGIN("test_embed")

    /* ---------- 1. 13 个标签都认识，且枚举值正确 ---------- */
    {
        static const char *const k_tags[] = {
            "iframe", "embed", "object", "audio", "video", "track", "canvas",
            "script", "noscript", "slot", "template", "svg", "math",
        };
        for (size_t i = 0; i < sizeof(k_tags) / sizeof(k_tags[0]); i++) {
            /*
             * 枚举值必须等于"表内下标"，这是三张表同序的契约。
             * 这里**按名字查**而不是按算术推：后加的包会追加在枚举末尾，
             * 算术推出来的偏移会跟着变，查名字才不受影响。
             */
            int idx = -1;
            for (int t = 0; t < (int)QL_UI_TAG_COUNT; t++)
                if (strcmp(ql_ui_tag_name((uint16_t)t), k_tags[i]) == 0) {
                    idx = t;
                    break;
                }
            QL_CHECK(idx >= 0);
            QL_CHECK(strcmp(ql_ui_tag_name((uint16_t)idx), k_tags[i]) == 0);

            char html[256];
            /* 空元素 vs 容器：都写成 `<x></x>` 形式，解析器两者都能吃 */
            snprintf(html, sizeof(html), "<div id=r><%s></%s></div>", k_tags[i],
                     k_tags[i]);
            QLUIRoot *ui = NULL;
            QLUIDoc *doc = mk(html, "#r { width: 200px; height: 200px; }", &ui);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QL_CHECK_EQ((int)ui->nodes[1].src->tag, idx);
                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
        }
    }

    /* ---------- 2. 内容整段跳过：script / template / svg / math ---------- */
    {
        /*
         * `<svg>` 里是另一门标记语言（`<path>` / `<circle>` 都不是 HTML 标签），
         * 真去解析就会报"不支持的标签"。这里断言整页能编译、且内容不在树里。
         */
        const char *html =
            "<div id=r>"
            "<script>var x = 1; if (a<b) { document.write('</div>'); }</script>"
            "<template><div>tpl-marker</div></template>"
            "<svg width=10 height=10><circle r=5/><path d='M0 0 L9 9'/></svg>"
            "<math><mi>mi-marker</mi><mo>+</mo></math>"
            "</div>";
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk(html, "#r { width: 200px; height: 200px; }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            /* 池里一个标记都不该有 */
            QL_CHECK(!pool_contains(doc, "var x"));
            QL_CHECK(!pool_contains(doc, "document.write"));
            QL_CHECK(!pool_contains(doc, "tpl-marker"));
            QL_CHECK(!pool_contains(doc, "mi-marker"));

            /* 树里只有 4 个 EMBED 节点，没有子节点 */
            QL_CHECK_EQ((int)doc->node_count, 5);      /* div + 4 */
            for (uint16_t i = 1; i < doc->node_count; i++)
                QL_CHECK(ui->nodes[i].first_child == NULL);

            /* 四个都在树里（节点自己保留，只是内容不进来） */
            QL_CHECK(find_tag(ui, "script", 0) != NULL);
            QL_CHECK(find_tag(ui, "template", 0) != NULL);
            QL_CHECK(find_tag(ui, "svg", 0) != NULL);
            QL_CHECK(find_tag(ui, "math", 0) != NULL);

            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 3. script 的 raw text 语义：内容里的 </div> 不算结束 ---------- */
    {
        const char *html =
            "<div id=r><script>if (a<b) { s = '</div>'; }</script><span>ok</span></div>";
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk(html, "#r { width: 200px; height: 60px; }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            /* span 必须在 div 下面（如果 </div> 被当成真结束标签，结构就散了） */
            QLUINodeRT *sp = find_tag(ui, "span", 0);
            QL_CHECK(sp != NULL);
            if (sp != NULL) {
                QL_CHECK(sp->parent != NULL);
                if (sp->parent != NULL)
                    QL_CHECK_EQ((int)sp->parent->src->tag, (int)QL_UI_TAG_DIV);
            }
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }

        /* 未闭合的 <script> 必须明确报错，不能静默吃掉后面所有内容 */
        QLUIDoc *bad = ql_ui_load_text("<div id=r><script>var x = 1;</div>", "");
        QL_CHECK(bad == NULL);
        if (bad != NULL)
            ql_ui_doc_free(bad);
        QL_CHECK(ql_ui_text_last_error() != NULL);
    }

    /* ---------- 4. 替换型元素：占位盒的默认尺寸 ---------- */
    {
        const char *html =
            "<div id=r>"
            "<iframe></iframe><video></video><canvas></canvas>"
            "<audio></audio><object></object><embed>"
            "</div>";
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk(html, "#r { width: 400px; height: 800px; }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            static const struct {
                const char *tag;
                int w, h;
            } k_expect[] = {
                { "iframe", 302, 152 },   /* 300×150 + 1px 边框双方 */
                { "video", 302, 152 },
                { "canvas", 302, 152 },
                { "object", 302, 152 },
                { "embed", 302, 152 },
                { "audio", 302, 34 },     /* 300×32 + 边框 */
            };
            for (size_t i = 0; i < sizeof(k_expect) / sizeof(k_expect[0]); i++) {
                QLUINodeRT *n = find_tag(ui, k_expect[i].tag, 0);
                QL_CHECK(n != NULL);
                if (n != NULL) {
                    const QLRect r = ql_ui_node_rect(n);
                    QL_CHECK_EQ((int)r.w, k_expect[i].w);
                    QL_CHECK_EQ((int)r.h, k_expect[i].h);
                    QL_CHECK_EQ((int)n->style->display, (int)QL_UI_DISPLAY_BLOCK);
                }
            }
            /* svg / math 是行内的（浏览器里也是） */
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
        QLUIRoot *ui2 = NULL;
        QLUIDoc *doc2 = mk("<div id=r><svg></svg></div>", "", &ui2);
        QL_CHECK(ui2 != NULL);
        if (ui2 != NULL) {
            QLUINodeRT *n = find_tag(ui2, "svg", 0);
            QL_CHECK(n != NULL);
            if (n != NULL) {
                QL_CHECK_EQ((int)n->style->display, (int)QL_UI_DISPLAY_INLINE_BLOCK);
                QL_CHECK_EQ((int)ql_ui_node_rect(n).w, 122);   /* 120 + 边框 */
                QL_CHECK_EQ((int)ql_ui_node_rect(n).h, 62);
            }
            ql_ui_free(ui2);
            ql_ui_doc_free(doc2);
        }
    }

    /* ---------- 5. 元数据型：零盒子 ---------- */
    {
        const char *html =
            "<div id=r><script></script><noscript>ns</noscript><slot></slot>"
            "<template></template><track src=\"a.vtt\"></div>";
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk(html, "#r { width: 200px; height: 200px; }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            static const char *const k_none[] = { "script", "noscript", "slot",
                                                  "template", "track" };
            for (size_t i = 0; i < sizeof(k_none) / sizeof(k_none[0]); i++) {
                QLUINodeRT *n = find_tag(ui, k_none[i], 0);
                QL_CHECK(n != NULL);
                if (n != NULL) {
                    QL_CHECK_EQ((int)n->style->display, (int)QL_UI_DISPLAY_NONE);
                    QL_CHECK_EQ((int)ql_ui_node_rect(n).w, 0);
                    QL_CHECK_EQ((int)ql_ui_node_rect(n).h, 0);
                }
            }
            /* noscript 的内容仍然被解析（文本进池），只是不渲染 */
            QL_CHECK(pool_contains(doc, "ns"));
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 6. 占位提示：标签名真的画在盒子里 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><iframe></iframe></div>",
                          "#r { width: 320px; height: 200px;"
                          "     background-color: #000000; }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
            QL_CHECK(s_disp != NULL);
            QLCanvas cv;
            QL_CHECK_EQ(ql_canvas_from_disp(&cv, s_disp), QL_OK);
            ql_canvas_clear(&cv, 0x000000);
            ql_ui_render_canvas(ui, &cv);

            const QLRect r = ql_ui_node_rect(find_tag(ui, "iframe", 0));
            /* 盒子内部中央应该有非黑像素（= 文字或边框），四角只有边框线 */
            int ink = 0;
            for (int y = (int)r.y; y < (int)(r.y + r.h); y++)
                for (int x = (int)r.x; x < (int)(r.x + r.w); x++)
                    if (px(x, y) != 0)
                        ink++;
            QL_CHECK(ink > 0);
            /* 中央一行有字：取盒子中线上下各 8 行，必须找到成片的非黑像素 */
            int mid_ink = 0;
            const int cy = (int)(r.y + r.h / 2);
            for (int y = cy - 8; y <= cy + 8; y++)
                for (int x = (int)r.x + 2; x < (int)(r.x + r.w) - 2; x++)
                    if (px(x, y) != 0)
                        mid_ink++;
            QL_CHECK(mid_ink > 20);

            /* 角落（边框内侧）不该有字 */
            QL_CHECK_EQ((int)px((int)r.x + 6, (int)r.y + 6), 0);

            ql_disp_mem_destroy(s_disp);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 7. 盒子太小就不画提示（不糊成一团） ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><iframe></iframe></div>",
                          "#r { width: 320px; height: 200px;"
                          "     background-color: #000000; }"
                          "iframe { width: 20px; height: 8px; border-width: 0 }",
                          &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
            QLCanvas cv;
            ql_canvas_from_disp(&cv, s_disp);
            ql_canvas_clear(&cv, 0x000000);
            ql_ui_render_canvas(ui, &cv);
            const QLRect r = ql_ui_node_rect(find_tag(ui, "iframe", 0));
            int ink = 0;
            for (int y = (int)r.y; y < (int)(r.y + r.h); y++)
                for (int x = (int)r.x; x < (int)(r.x + r.w); x++)
                    if (px(x, y) != 0)
                        ink++;
            QL_CHECK_EQ(ink, 0);
            ql_disp_mem_destroy(s_disp);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 8. CSS 覆盖：占位盒的尺寸/边框都能改 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><iframe></iframe></div>",
                          "iframe { width: 80px; height: 40px; border-width: 0;"
                          "         background-color: #223344 }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *n = find_tag(ui, "iframe", 0);
            const QLRect r = ql_ui_node_rect(n);
            QL_CHECK_EQ((int)r.w, 80);
            QL_CHECK_EQ((int)r.h, 40);
            QL_CHECK_EQ((int)n->style->border_w[0], 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 9. blob 往返：枚举值必须原样回来 ---------- */
    {
        const char *html =
            "<div id=r><iframe></iframe><svg></svg><script>var x=1</script>"
            "<audio></audio><math></math></div>";
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk(html, "#r { width: 200px; height: 300px; }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const size_t need = ql_ui_doc_encode(doc, NULL, 0);
            QL_CHECK(need > 0);
            uint8_t *buf = (uint8_t *)ql_mem_alloc(need);
            QL_CHECK(buf != NULL);
            QL_CHECK_EQ(ql_ui_doc_encode(doc, buf, need), need);
            QLUIDoc *back = ql_ui_doc_load_mem(buf, need);
            QL_CHECK(back != NULL);
            if (back != NULL) {
                QL_CHECK_EQ((int)back->node_count, (int)doc->node_count);
                for (uint16_t i = 0; i < doc->node_count; i++)
                    QL_CHECK_EQ((int)back->nodes[i].tag, (int)doc->nodes[i].tag);
                /* 加载回来的 doc 照样能建树、布局 */
                QLUIRoot *ui2 = ql_ui_build(back);
                QL_CHECK(ui2 != NULL);
                if (ui2 != NULL) {
                    ql_ui_layout(ui2, 200, 300);
                    QL_CHECK(find_tag(ui2, "iframe", 0) != NULL);
                    QL_CHECK(find_tag(ui2, "svg", 0) != NULL);
                    QL_CHECK(find_tag(ui2, "math", 0) != NULL);
                    QL_CHECK_EQ((int)find_tag(ui2, "script", 0)->style->display,
                                (int)QL_UI_DISPLAY_NONE);
                    ql_ui_free(ui2);
                }
                ql_ui_doc_free(back);
            }
            ql_mem_free(buf);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 10. 整页 HTML（含 head/style/script/svg）能直接编译 ---------- */
    {
        const char *html =
            "<!DOCTYPE html><html><head>"
            "<meta charset=\"utf-8\"><title>t</title>"
            "<style>.a { color: red }</style>"
            "<script>var y = 2;</script>"
            "</head><body>"
            "<div id=\"r\" class=\"a\"><svg><rect/></svg><p>hello</p></div>"
            "</body></html>";
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk(html, "#r { width: 200px; height: 200px; }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK(pool_contains(doc, "hello"));
            QL_CHECK(!pool_contains(doc, "var y"));
            QL_CHECK(find_tag(ui, "svg", 0) != NULL);
            QL_CHECK(find_tag(ui, "p", 0) != NULL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

QL_TEST_END()
