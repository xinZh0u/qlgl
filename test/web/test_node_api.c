/**
 * @file    test_node_api.c
 * @brief   节点级 API：按 id / class 找节点 + 每节点点击回调（冒泡/解绑/blob）。
 *
 * 这两条是为了让应用不再写 `&ui->nodes[2]` 和"全局回调里判断 tag"。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

static int s_node_hits;
static int s_global_hits;
static QLUINodeRT *s_seen;

static void on_node_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)ui;
    (void)user;
    s_node_hits++;
    s_seen = node;
}

static void on_global_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    (void)ui;
    (void)node;
    (void)user;
    s_global_hits++;
}

static void click_at(QLUIRoot *ui, ql_coord_t x, ql_coord_t y)
{
    (void)ql_ui_pointer_move(ui, x, y);
    (void)ql_ui_pointer_down(ui, x, y);
    (void)ql_ui_pointer_up(ui, x, y);
}

QL_TEST_BEGIN("test_node_api")
{
    const char *html =
        "<div id='root'>"
        "<div id='btn' class='btn primary'><span id='label'>OK</span></div>"
        "<div id='log' class='btn'>0</div>"
        "</div>";
    const char *css =
        "#root { width: 200px; height: 100px; }"
        "#btn { width: 80px; height: 40px; background-color: #3366cc; }"
        "#label { color: #ffffff; }"
        "#log { width: 200px; height: 30px; }";

    QLUIDoc *doc = ql_ui_load_text(html, css);
    QL_CHECK(doc != NULL);
    if (doc != NULL) {
        QLUIRoot *ui = ql_ui_build(doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            ql_ui_layout(ui, 200, 100);

            QLUINodeRT *root = ql_ui_find_by_id(ui, "root");
            QLUINodeRT *btn = ql_ui_find_by_id(ui, "btn");
            QLUINodeRT *log = ql_ui_find_by_id(ui, "log");
            QL_CHECK(root != NULL && btn != NULL && log != NULL);
            QL_CHECK(ql_ui_find_by_id(ui, "nope") == NULL);

            /* class 查询：btn 节点同时有 btn/primary 两个 class */
            QL_CHECK(ql_ui_find_by_class(ui, "btn", 0) == btn);
            QL_CHECK(ql_ui_find_by_class(ui, "btn", 1) == log);
            QL_CHECK(ql_ui_find_by_class(ui, "btn", 2) == NULL);
            QL_CHECK(ql_ui_find_by_class(ui, "primary", 0) == btn);
            QL_CHECK(ql_ui_find_by_class(ui, "nope", 0) == NULL);

            /* ---- 节点回调：点子 span 冒泡到按钮 ---- */
            QL_CHECK_EQ(ql_ui_set_node_click_cb(ui, btn, on_node_click, NULL),
                        QL_OK);
            ql_ui_set_click_cb(ui, on_global_click, NULL);
            s_node_hits = s_global_hits = 0;
            s_seen = NULL;

            const QLRect br = ql_ui_node_rect(btn);
            click_at(ui, br.x + br.w / 2, br.y + br.h / 2);
            QL_CHECK_EQ(s_node_hits, 1);
            QL_CHECK_EQ(s_global_hits, 0);   /* 节点绑定优先，且绑定即停止 */
            QL_CHECK(s_seen == btn);         /* 命中 span，回调收到绑定的 btn */

            /* ---- 解绑后回落到全局 ---- */
            QL_CHECK_EQ(ql_ui_set_node_click_cb(ui, btn, NULL, NULL), QL_OK);
            s_node_hits = s_global_hits = 0;
            click_at(ui, br.x + br.w / 2, br.y + br.h / 2);
            QL_CHECK_EQ(s_node_hits, 0);
            QL_CHECK_EQ(s_global_hits, 1);

            /* ---- 非法节点 ---- */
            QL_CHECK_EQ(ql_ui_set_node_click_cb(ui, ui->nodes + ui->node_count,
                                                on_node_click, NULL),
                        QL_ERR_INVAL);

            /* ---- blob 往返后 id / class 仍然有效 ---- */
            {
                const size_t n = ql_ui_doc_encode(doc, NULL, 0);
                QL_CHECK(n > 0);
                uint8_t *blob = (uint8_t *)malloc(n);
                QL_CHECK(blob != NULL);
                if (blob != NULL) {
                    QL_CHECK_EQ(ql_ui_doc_encode(doc, blob, n), n);
                    QLUIDoc *loaded = ql_ui_doc_load_mem(blob, n);
                    QL_CHECK(loaded != NULL);
                    if (loaded != NULL) {
                        QLUIRoot *ui2 = ql_ui_build(loaded);
                        QL_CHECK(ui2 != NULL);
                        if (ui2 != NULL) {
                            QL_CHECK(ql_ui_find_by_id(ui2, "btn") != NULL);
                            QL_CHECK(ql_ui_find_by_class(ui2, "primary", 0) != NULL);
                            QL_CHECK(ql_ui_find_by_class(ui2, "nope", 0) == NULL);
                            ql_ui_free(ui2);
                        }
                        ql_ui_doc_free(loaded);
                    }
                    free(blob);
                }
            }

            ql_ui_free(ui);
        }
        ql_ui_doc_free(doc);
    }
}
QL_TEST_END()
