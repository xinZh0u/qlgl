/**
 * @file    test_details.c
 * @brief   `<details>` / `<summary>`：折叠面板。
 *
 * 收起时不用改布局代码 —— 把非 `summary` 的子节点指向一份**共享的隐藏样式**
 * （`display: none`），布局本来就跳过这种节点。所以判据是"子节点的 display 与
 * 面板高度"这两件事。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#define W 200
#define H 200

static const char *k_css =
    "#r { width: 200px; height: 200px }"
    "summary, div { line-height: 20px }";

static QLUIRoot *mk(const char *html, QLUIDoc **out_doc)
{
    QLUIDoc *doc = ql_ui_load_text(html, k_css);
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
    *out_doc = doc;
    return ui;
}

QL_TEST_BEGIN("test_details")

    /* ---------- 1. 默认收起：只看见标题行 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><details><summary>标题</summary>"
                          "<div>正文</div></details></div>", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUIDoc *d = doc;
            QL_CHECK_EQ((int)d->node_count, 4);
            QL_CHECK(strcmp(ql_ui_tag_name(d->nodes[1].tag), "details") == 0);
            QL_CHECK_EQ(ql_ui_details_open(ui, &ui->nodes[1]), 0);
            /* summary 可见，正文被关掉 */
            QL_CHECK(ui->nodes[2].style->display != QL_UI_DISPLAY_NONE);
            QL_CHECK_EQ((int)ui->nodes[3].style->display, (int)QL_UI_DISPLAY_NONE);
            QL_CHECK_EQ((int)ql_ui_node_rect(&ui->nodes[1]).h, 20);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 2. open 属性：初始就是展开的 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><details open><summary>标题</summary>"
                          "<div>正文</div></details></div>", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(ql_ui_details_open(ui, &ui->nodes[1]), 1);
            QL_CHECK(ui->nodes[3].style->display != QL_UI_DISPLAY_NONE);
            QL_CHECK_EQ((int)ql_ui_node_rect(&ui->nodes[1]).h, 40);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 3. set_open：改状态 + 重排 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><details><summary>标题</summary>"
                          "<div>正文</div></details></div>", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *dt = &ui->nodes[1];
            QL_CHECK_EQ(ql_ui_details_set_open(ui, dt, 1), QL_OK);
            QL_CHECK_EQ(ql_ui_details_open(ui, dt), 1);
            QL_CHECK_EQ((int)ql_ui_node_rect(dt).h, 40);   /* 内部已经重排过 */
            QL_CHECK_EQ(ql_ui_details_set_open(ui, dt, 0), QL_OK);
            QL_CHECK_EQ((int)ql_ui_node_rect(dt).h, 20);
            /* 非 <details> 节点 */
            QL_CHECK_EQ(ql_ui_details_open(ui, &ui->nodes[0]), -1);
            QL_CHECK_EQ(ql_ui_details_set_open(ui, &ui->nodes[0], 1), QL_ERR_INVAL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 4. 点标题行切换；点正文不切换 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><details><summary>标题</summary>"
                          "<div>正文</div></details></div>", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *dt = &ui->nodes[1];
            const QLRect sum = ql_ui_node_rect(&ui->nodes[2]);

            ql_ui_pointer_down(ui, sum.x + 4, sum.y + 4);
            ql_ui_pointer_up(ui, sum.x + 4, sum.y + 4);
            QL_CHECK_EQ(ql_ui_details_open(ui, dt), 1);
            QL_CHECK_EQ((int)ql_ui_node_rect(dt).h, 40);

            /* 正文点一下：不该收起 */
            const QLRect body = ql_ui_node_rect(&ui->nodes[3]);
            ql_ui_pointer_down(ui, body.x + 4, body.y + 4);
            ql_ui_pointer_up(ui, body.x + 4, body.y + 4);
            QL_CHECK_EQ(ql_ui_details_open(ui, dt), 1);

            /* 再点标题：收起 */
            ql_ui_pointer_down(ui, sum.x + 4, sum.y + 4);
            ql_ui_pointer_up(ui, sum.x + 4, sum.y + 4);
            QL_CHECK_EQ(ql_ui_details_open(ui, dt), 0);
            QL_CHECK_EQ((int)ql_ui_node_rect(dt).h, 20);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 5. 点 summary 里的行内元素也能切换（往上找到 summary） ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><details><summary><span>标</span>题</summary>"
                          "<div>正文</div></details></div>", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *dt = &ui->nodes[1];
            /* 找到那个 span */
            QLUINodeRT *sp = NULL;
            for (uint16_t i = 0; i < ui->node_count; i++)
                if (ui->nodes[i].src->tag == QL_UI_TAG_SPAN)
                    sp = &ui->nodes[i];
            QL_CHECK(sp != NULL);
            if (sp != NULL) {
                const QLRect r = ql_ui_node_rect(sp);
                ql_ui_pointer_down(ui, r.x + 1, r.y + 1);
                ql_ui_pointer_up(ui, r.x + 1, r.y + 1);
                QL_CHECK_EQ(ql_ui_details_open(ui, dt), 1);
            }
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 6. 多个 details 互不影响 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r>"
                          "<details><summary>a</summary><div>1</div></details>"
                          "<details><summary>b</summary><div>2</div></details></div>",
                          &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(ql_ui_details_set_open(ui, &ui->nodes[1], 1), QL_OK);
            QL_CHECK_EQ(ql_ui_details_open(ui, &ui->nodes[1]), 1);
            QL_CHECK_EQ(ql_ui_details_open(ui, &ui->nodes[4]), 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

QL_TEST_END()
