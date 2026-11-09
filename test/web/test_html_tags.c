/**
 * @file    test_html_tags.c
 * @brief   标签集与 UA 默认样式。
 *
 * 标签在本架构里只影响三件事：UA 默认样式、`tag` 选择器、调试名。所以这里的
 * 判据全是"编译出来的 QLUIStyle 长什么样" + "布局落在哪"，不看渲染像素
 * （像素那是 golden 的事）。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

/* 编译 + 布局，返回 ui（调用方负责 free）。失败返回 NULL 并打印原因 */
static QLUIRoot *build(const char *html, const char *css, ql_coord_t w, ql_coord_t h,
                       QLUIDoc **out_doc)
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
    ql_ui_layout(ui, w, h);
    *out_doc = doc;
    return ui;
}

QL_TEST_BEGIN("test_html_tags")

    /* ---------- 1. 标签名表与枚举一一对应 ---------- */
    {
        static const char *const expect[] = {
            "div", "section", "p", "span", "b", "i", "button", "h1", "h2", "h3",
            "h4", "h5", "h6", "header", "footer", "nav", "main", "article",
            "aside", "figure", "figcaption", "blockquote", "ul", "ol", "li",
            "hr", "br", "mark", "em", "strong", "u", "s", "input", "select",
            "option",
            /* --- 纯语义标签 --- */
            "html", "body",
            "dl", "dt", "dd", "menu", "address", "hgroup", "search",
            "a", "abbr", "code", "kbd", "samp", "var", "q", "dfn", "cite",
            "data", "time", "small", "bdi", "bdo", "wbr",
            "del", "ins",
            "form", "label", "fieldset", "legend", "optgroup",
            /* --- META 包：解析期跳过，不进树 --- */
            "base", "head", "link", "meta", "style", "title",
            /* --- WIDGET 包 --- */
            "output", "datalist", "progress", "meter", "details", "summary",
            "dialog",
            /* --- IMAGE 包 --- */
            "img", "picture", "source", "map", "area",
            /* --- EMBED 包：只做占位 --- */
            "iframe", "embed", "object", "audio", "video", "track", "canvas",
            "script", "noscript", "slot", "template", "svg", "math",
            "pre", "sub", "sup", "textarea",
        };
        QL_CHECK_EQ((int)QL_UI_TAG_COUNT, (int)(sizeof(expect) / sizeof(expect[0])));
        for (int i = 0; i < (int)QL_UI_TAG_COUNT && i < (int)(sizeof(expect) / sizeof(expect[0])); i++)
            QL_CHECK(strcmp(ql_ui_tag_name((uint16_t)i), expect[i]) == 0);
        QL_CHECK(strcmp(ql_ui_tag_name((uint16_t)QL_UI_TAG_COUNT), "?") == 0);
    }

    /* ---------- 2. 每个标签都能解析，且解析出来的 tag 值正确 ---------- */
    {
        for (int i = 0; i < (int)QL_UI_TAG_COUNT; i++) {
            const char *name = ql_ui_tag_name((uint16_t)i);
            char html[256];
            const int is_void = (strcmp(name, "br") == 0 || strcmp(name, "hr") == 0 ||
                                 strcmp(name, "input") == 0 ||
                                 strcmp(name, "wbr") == 0 ||
                                 strcmp(name, "img") == 0 ||
                                 strcmp(name, "source") == 0 ||
                                 strcmp(name, "area") == 0 ||
                                 strcmp(name, "embed") == 0 ||
                                 strcmp(name, "track") == 0 ||
                                 strcmp(name, "base") == 0 ||
                                 strcmp(name, "link") == 0 ||
                                 strcmp(name, "meta") == 0);
            if (is_void)
                snprintf(html, sizeof(html), "<div id=r><%s></div>", name);
            else
                snprintf(html, sizeof(html), "<div id=r><%s>t</%s></div>", name, name);

            QLUIDoc *doc = NULL;
            QLUIRoot *ui = build(html, "", 200, 200, &doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                /* META 包的标签在解析期就跳过了：树里只剩外面那个 div */
                if (strcmp(ql_ui_tag_name((uint16_t)i), "base") == 0 ||
                    strcmp(ql_ui_tag_name((uint16_t)i), "head") == 0 ||
                    strcmp(ql_ui_tag_name((uint16_t)i), "link") == 0 ||
                    strcmp(ql_ui_tag_name((uint16_t)i), "meta") == 0 ||
                    strcmp(ql_ui_tag_name((uint16_t)i), "style") == 0 ||
                    strcmp(ql_ui_tag_name((uint16_t)i), "title") == 0) {
                    QL_CHECK_EQ((int)doc->node_count, 1);
                } else {
                    QL_CHECK_EQ((int)ui->nodes[1].src->tag, i);
                    QL_CHECK(strcmp(ql_ui_tag_name(ui->nodes[1].src->tag), name) == 0);
                }
                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
        }
    }

    /* ---------- 3. 标题：六级都有字号，且逐级变小 ---------- */
    {
        const char *html = "<div id=r><h1>a</h1><h2>a</h2><h3>a</h3>"
                           "<h4>a</h4><h5>a</h5><h6>a</h6></div>";
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = build(html, "", 300, 400, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            int prev = 999;
            for (int i = 1; i <= 6; i++) {
                const QLUIStyle *st = ui->nodes[i].style;
                QL_CHECK_EQ((int)st->display, (int)QL_UI_DISPLAY_BLOCK);
                const int px = (int)doc->font_sizes[st->font_id];
                QL_CHECK(px < prev);
                prev = px;
            }
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 4. 结构容器都是 block ---------- */
    {
        static const char *const blocks[] = { "header", "footer", "nav", "main",
                                              "article", "aside", "figure",
                                              "figcaption", "blockquote", "li" };
        for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
            char html[128];
            snprintf(html, sizeof(html), "<div id=r><%s>t</%s></div>", blocks[i],
                     blocks[i]);
            QLUIDoc *doc = NULL;
            QLUIRoot *ui = build(html, "", 200, 200, &doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QL_CHECK_EQ((int)ui->nodes[1].style->display, (int)QL_UI_DISPLAY_BLOCK);
                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
        }
    }

    /* ---------- 5. 行内语义标签是 inline ---------- */
    {
        static const char *const inlines[] = { "em", "strong", "u", "s", "mark" };
        for (size_t i = 0; i < sizeof(inlines) / sizeof(inlines[0]); i++) {
            char html[128];
            snprintf(html, sizeof(html), "<div id=r><%s>t</%s></div>", inlines[i],
                     inlines[i]);
            QLUIDoc *doc = NULL;
            QLUIRoot *ui = build(html, "", 200, 200, &doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QL_CHECK_EQ((int)ui->nodes[1].style->display,
                            (int)QL_UI_DISPLAY_INLINE);
                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
        }
    }

    /* ---------- 6. mark 有自己的背景色（现在唯一"看得出区别"的行内标签） ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = build("<div id=r>a<mark>b</mark></div>", "", 200, 100, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLUIStyle *st = ui->nodes[2].style;
            QL_CHECK_EQ((int)st->display, (int)QL_UI_DISPLAY_INLINE);
            QL_CHECK(st->bg_color != 0x000000u);
            QL_CHECK_EQ((int)st->bg_alpha, 255);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 7. hr：块级、1px 高、有颜色（不是默认黑） ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = build("<div id=r><div>a</div><hr><div>b</div></div>", "",
                             200, 200, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLUINodeRT *hr = &ui->nodes[2];
            QL_CHECK(strcmp(ql_ui_tag_name(hr->src->tag), "hr") == 0);
            QL_CHECK_EQ((int)hr->style->display, (int)QL_UI_DISPLAY_BLOCK);
            QL_CHECK_EQ((int)ql_ui_node_rect(hr).h, 1);
            QL_CHECK_EQ((int)ql_ui_node_rect(hr).w, 200);
            QL_CHECK(hr->style->bg_color != 0u);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 8. br：把后面的内容推到下一行 ---------- */
    {
        const char *html = "<div id=r><span>a</span><br><span>b</span></div>";
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = build(html, "", 200, 200, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLRect a = ql_ui_node_rect(&ui->nodes[1]);
            const QLRect b = ql_ui_node_rect(&ui->nodes[3]);
            QL_CHECK_EQ((int)a.y, 0);
            /* 第二段必须严格在第一段下方（一行高） */
            QL_CHECK(b.y >= a.y + a.h);
            QL_CHECK_EQ((int)ql_ui_node_rect(&ui->nodes[2]).h, 0);   /* br 自身不占高 */
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 9. br / hr 是空元素：不需要结束标签，也不能有子节点 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = build("<div id=r>a<br>b<hr>c</div>", "", 200, 200, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
        /*
         * 给空元素补结束标签是**解析错误但不是歧义**：浏览器的做法是忽略它，
         * 这里照做（否则 `<embed></embed>` 这种看着正常的写法会让整页编译不过）。
         * 断言：能编译、且不会因此多出节点。
         */
        QLUIDoc *d1 = ql_ui_load_text("<div id=r>a<br></br>b</div>", "");
        QL_CHECK(d1 != NULL);
        if (d1 != NULL) {
            QLUIRoot *u1 = ql_ui_build(d1);
            QL_CHECK(u1 != NULL);
            if (u1 != NULL) {
                QL_CHECK_EQ((int)u1->node_count, 4);   /* div + span + br + span */
                ql_ui_free(u1);
            }
            ql_ui_doc_free(d1);
        }
        QLUIDoc *d2 = ql_ui_load_text("<div id=r><hr>a</hr></div>", "");
        QL_CHECK(d2 != NULL);
        if (d2 != NULL)
            ql_ui_doc_free(d2);
    }

    /* ---------- 10. ul / ol：左边距 + 子项缩进 ---------- */
    {
        const char *html = "<div id=r><ul><li>a</li><li>b</li></ul></div>";
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = build(html, "", 200, 200, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLUINodeRT *ul = &ui->nodes[1];
            QL_CHECK_EQ((int)ul->style->padding.left, 18);
            /* 列表项从内容盒左边起，宽度 = 容器 - 缩进 */
            const QLRect li = ql_ui_node_rect(&ui->nodes[2]);
            QL_CHECK_EQ((int)li.x, 18);
            QL_CHECK_EQ((int)li.w, 182);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 11. 明确报错，且提示要能指导下一步 ---------- */
    {
        struct { const char *html; const char *needle; } bad[] = {
            { "<div id=r><table><tr><td>x</td></tr></table></div>", "用嵌套的 flex" },

            { "<div id=r><input type=\"date\"></div>", "只支持 text / password" },
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            QLUIDoc *doc = ql_ui_load_text(bad[i].html, "");
            QL_CHECK(doc == NULL);
            if (doc != NULL)
                ql_ui_doc_free(doc);
            else
                QL_CHECK(strstr(ql_ui_text_last_error(), bad[i].needle) != NULL);
        }
    }

    /* ---------- 12. 行内元素之间的空白保留成一个空格 ---------- */
    {
        /*
         * 真实 HTML 里到处都是 `<b>a</b> <i>b</i>`。丢掉中间那段空白就成了 "ab"。
         * 但块级元素之间的空白必须丢（否则每个缩进都变一行）。
         */
        const char *html = "<div id=r><mark>a</mark> <em>b</em></div>";
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = build(html, "", 200, 100, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLRect a = ql_ui_node_rect(&ui->nodes[1]);
            const QLRect b = ql_ui_node_rect(&ui->nodes[3]);
            QL_CHECK(b.x >= ql_rect_right(a));      /* 中间确实有间隙 */
            QL_CHECK(doc->node_count == 4);         /* div + mark + 空格 span + em */
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }

        /* 块级兄弟之间的空白不产生节点 */
        const char *blk = "<div id=r>\n  <div>a</div>\n  <div>b</div>\n</div>";
        doc = NULL;
        ui = build(blk, "", 200, 100, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)doc->node_count, 3);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }

        /* flex 容器里，行内间隔不会变成 flex 项（CSS：只含空白的匿名项不渲染） */
        const char *flex_html =
            "<div id=r><span>aa</span> <span>bb</span></div>";
        doc = NULL;
        ui = build(flex_html, "#r { width: 200px; display: flex; gap: 0; }", 200, 60,
                   &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            /* 三列：aa、隐藏的空格、bb；bb 必须紧贴在 aa 右边（gap=0） */
            const QLRect a = ql_ui_node_rect(&ui->nodes[1]);
            const QLRect b = ql_ui_node_rect(&ui->nodes[3]);
            QL_CHECK_EQ((int)b.x, (int)ql_rect_right(a));
            QL_CHECK_EQ((int)ui->nodes[2].style->display, (int)QL_UI_DISPLAY_NONE);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 13. tag 选择器认识新标签 ---------- */
    {
        const char *html = "<div id=r><header>a</header><article>b</article></div>";
        const char *css = "header { color: #112233 } article { color: #445566 }";
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = build(html, css, 200, 200, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)ui->nodes[1].style->text_color, 0x112233);
            QL_CHECK_EQ((int)ui->nodes[2].style->text_color, 0x445566);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 14. 纯语义标签：UA 默认样式与几个"现在真的有区别"的 ---------- */
    {
        static const char *const inl[] = { "a", "abbr", "code", "kbd", "samp",
                                           "q", "data", "time", "small", "bdi",
                                           "bdo", "del", "ins", "label",
                                           "legend" };
        for (size_t i = 0; i < sizeof(inl) / sizeof(inl[0]); i++) {
            char html[128];
            snprintf(html, sizeof(html), "<div id=r><%s>t</%s></div>", inl[i],
                     inl[i]);
            QLUIDoc *doc = NULL;
            QLUIRoot *ui = build(html, "", 200, 200, &doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QL_CHECK_EQ((int)ui->nodes[1].style->display, (int)QL_UI_DISPLAY_INLINE);
                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
        }

        /* 引用/变量/定义 + <address> 是斜体（合成斜体已经有了） */
        static const char *const ital[] = { "cite", "var", "dfn", "address" };
        for (size_t i = 0; i < sizeof(ital) / sizeof(ital[0]); i++) {
            char html[128];
            snprintf(html, sizeof(html), "<div id=r><%s>t</%s></div>", ital[i],
                     ital[i]);
            QLUIDoc *doc = NULL;
            QLUIRoot *ui = build(html, "", 200, 200, &doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QL_CHECK((ui->nodes[1].style->font_flags & QL_UI_FONT_ITALIC) != 0);
                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
        }

        /* <del> 删除线 / <ins> 下划线 */
        {
            QLUIDoc *doc = NULL;
            QLUIRoot *ui = build("<div id=r><del>a</del><ins>b</ins></div>", "", 200,
                                 200, &doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QL_CHECK((ui->nodes[1].style->font_flags & QL_UI_FONT_LINE_THROUGH) != 0);
                QL_CHECK((ui->nodes[2].style->font_flags & QL_UI_FONT_UNDERLINE) != 0);
                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
        }

        /* 描述列表：<dd> 有缩进，<dt>/<dd> 都是块 */
        {
            QLUIDoc *doc = NULL;
            QLUIRoot *ui = build("<div id=r><dl><dt>t</dt><dd>d</dd></dl></div>", "",
                                 200, 200, &doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QL_CHECK_EQ((int)ui->nodes[2].style->display, (int)QL_UI_DISPLAY_BLOCK);
                QL_CHECK_EQ((int)ui->nodes[3].style->display, (int)QL_UI_DISPLAY_BLOCK);
                QL_CHECK_EQ((int)ui->nodes[3].style->padding.left, 18);
                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
        }

        /* <optgroup> 与 <option> 一样不进布局 */
        {
            QLUIDoc *doc = NULL;
            QLUIRoot *ui = build("<div id=r><select><optgroup><option>x</option>"
                                 "</optgroup></select></div>",
                                 "", 200, 200, &doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QL_CHECK_EQ((int)ui->nodes[2].style->display, (int)QL_UI_DISPLAY_NONE);
                QL_CHECK_EQ((int)ui->nodes[3].style->display, (int)QL_UI_DISPLAY_NONE);
                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
        }

        /* <wbr> 是空元素：不压栈，也不会报"缺少结束标签" */
        {
            QLUIDoc *doc = NULL;
            QLUIRoot *ui = build("<div id=r><span>a</span><wbr><span>b</span></div>",
                                 "", 200, 60, &doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QL_CHECK_EQ((int)doc->node_count, 4);
                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
        }

        /* tag 选择器对新增标签同样有效 */
        {
            QLUIDoc *doc = NULL;
            QLUIRoot *ui = build("<div id=r><abbr>a</abbr><code>b</code></div>",
                                 "abbr { color: #010203 } code { color: #040506 }",
                                 200, 200, &doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QL_CHECK_EQ((int)ui->nodes[1].style->text_color, 0x010203);
                QL_CHECK_EQ((int)ui->nodes[2].style->text_color, 0x040506);
                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
        }
    }

    /* ---------- 15. META 包：整页 HTML 贴进来能编译 ---------- */
    {
        /* 真实页面：doctype + html/head/body + title/meta/link/style 全都在 */
        const char *page =
            "<!DOCTYPE html><html><head>"
            "<meta charset=\"utf-8\"><title>标题</title>"
            "<link rel=\"stylesheet\" href=\"x.css\">"
            "<style>.a { color: #f00 }</style>"
            "</head><body><div id=r>hello</div></body></html>";
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = build(page, "", 200, 80, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            /* html > body > div，元数据一个都不进树 */
            QL_CHECK_EQ((int)doc->node_count, 3);
            QL_CHECK(strcmp(ql_ui_tag_name(ui->nodes[0].src->tag), "html") == 0);
            QL_CHECK(strcmp(ql_ui_tag_name(ui->nodes[1].src->tag), "body") == 0);
            QL_CHECK_STR(ui->nodes[2].text, "hello");
            /* 关键：<style> 的内容没有变成界面上的文字 */
            for (uint16_t i = 0; i < ui->node_count; i++)
                if (ui->nodes[i].text != NULL)
                    QL_CHECK(strstr(ui->nodes[i].text, "color") == NULL);
            QL_CHECK_EQ((int)doc->degraded_tags, 0);
            QL_CHECK(ql_ui_node_rect(&ui->nodes[2]).h > 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }

        /* 元数据写在 body 里同样被跳过（浏览器也这么干） */
        doc = NULL;
        ui = build("<div id=r><style>.a{}</style><span>hi</span></div>", "", 200, 80,
                   &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)doc->node_count, 2);
            QL_CHECK_STR(ui->nodes[1].text, "hi");
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }

        /* <style> 未闭合：明确报错，不猜 */
        {
            QLUIDoc *bad = ql_ui_load_text("<div id=r><style>.a{}</div>", "");
            QL_CHECK(bad == NULL);
            if (bad != NULL)
                ql_ui_doc_free(bad);
            else
                QL_CHECK(strstr(ql_ui_text_last_error(), "未闭合") != NULL);
        }

        /* <title> 的内容里出现 "</head>" 字样也只当文本（raw text 的规矩） */
        doc = NULL;
        ui = build("<div id=r><head><title>a</head>b</title></head><span>hi</span></div>",
                   "", 200, 80, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)doc->node_count, 2);
            QL_CHECK_STR(ui->nodes[1].text, "hi");
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }

        /* 空元素写法：<meta> <link> <base> 不需要结束标签 */
        doc = NULL;
        ui = build("<div id=r><meta><link><base><span>x</span></div>", "", 200, 80,
                   &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)doc->node_count, 2);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

QL_TEST_END()
