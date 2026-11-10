/**
 * @file    test_repeat.c
 * @brief   模板实例化 `ql_ui_repeat()`：运行期把一个子树复制 n 份。
 *
 * 契约（都在这儿断言）：
 *   1. 复制后**节点数** = 原 + (n-1)×子树大小，且兄弟链顺序正确；
 *   2. 文本里的 `{i}` 换成 **1 起的序号**（模板那份是 1）；
 *   3. 嵌套结构（子树里还有孩子）整支一起复制，父子关系不串；
 *   4. 样式是**共享只读**的（副本与模板指向同一份 `QLUIStyle`，除非模板改过）；
 *   5. `count == 0` 等价于把模板隐藏（`display: none`），节点数不变；
 *   6. `ql_ui_free()` 之后不留内存（`bytes_live == 0`）。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

static QLUIDoc *s_doc;
static QLUIRoot *s_ui;

static QLUIRoot *build(const char *html, const char *css)
{
    s_doc = ql_ui_load_text(html, css);
    if (s_doc == NULL) {
        printf("  编译失败：%s\n", ql_ui_text_last_error());
        return NULL;
    }
    s_ui = ql_ui_build(s_doc);
    if (s_ui == NULL) {
        ql_ui_doc_free(s_doc);
        s_doc = NULL;
    }
    return s_ui;
}

static void release(void)
{
    if (s_ui != NULL) {
        ql_ui_free(s_ui);
        s_ui = NULL;
    }
    if (s_doc != NULL) {
        ql_ui_doc_free(s_doc);
        s_doc = NULL;
    }
}

/* 第 n 个子节点（从 1 开始数） */
static QLUINodeRT *child(QLUINodeRT *p, int n)
{
    QLUINodeRT *c = (p != NULL) ? p->first_child : NULL;
    while (c != NULL && --n > 0)
        c = c->next_sibling;
    return c;
}

static int count_children(const QLUINodeRT *p)
{
    int n = 0;
    for (const QLUINodeRT *c = p->first_child; c != NULL; c = c->next_sibling)
        n++;
    return n;
}

QL_TEST_BEGIN("repeat")
    /* ============ 1. 平铺：节点数、顺序、{i} ============ */
    {
        QLUIRoot *ui = build("<div id=r><div class=row>第 {i} 行</div></div>",
                             "#r { width: 200px; background: #FFFFFF }"
                             ".row { height: 20px; color: #101010 }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const uint16_t before = ui->node_count;
            QLUINodeRT *root = ql_ui_root_node(ui);
            QLUINodeRT *tpl = root->first_child;
            QL_CHECK(tpl != NULL);

            QL_CHECK_EQ(ql_ui_repeat(ui, tpl, 4), QL_OK);
            QL_CHECK_EQ(ui->node_count, before + 3u);   /* 多 3 份 */
            /* 节点数组会重新分配 → 必须重新取指针（见头文件里的 ⚠️） */
            root = ql_ui_root_node(ui);
            tpl = root->first_child;
            QL_CHECK_EQ(count_children(root), 4);

            for (int i = 1; i <= 4; i++) {
                QLUINodeRT *c = child(root, i);
                QL_CHECK(c != NULL);
                if (c == NULL)
                    continue;
                char want[32];
                snprintf(want, sizeof(want), "第 %d 行", i);
                QL_CHECK(c->text != NULL && strcmp(c->text, want) == 0);
                QL_CHECK_EQ(c->parent, root);
                /* 样式共享（模板没改过样式 → 指针相同） */
                QL_CHECK_EQ(c->style, tpl->style);
                QL_CHECK(c->style_copy == NULL);
            }

            ql_ui_layout(ui, 200, 400);
            QL_CHECK_EQ(root->border_box.h, 80);         /* 4 × 20 */
        }
        release();
    }

    /* ============ 2. 嵌套子树：整支一起复制 ============ */
    {
        QLUIRoot *ui = build(
            "<div id=r><div class=item><span class=k>键 {i}</span>"
            "<span class=v>值 {i}</span></div></div>",
            "#r { width: 300px; background: #FFFFFF }"
            ".item { height: 18px }"
            ".k { color: #202020 } .v { color: #808080 }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *root = ql_ui_root_node(ui);
            QLUINodeRT *tpl = root->first_child;
            const uint16_t sub = 3;                     /* item + 两个 span */
            const uint16_t before = ui->node_count;
            QL_CHECK_EQ(ql_ui_repeat(ui, tpl, 3), QL_OK);
            QL_CHECK_EQ(ui->node_count, before + 2u * sub);
            root = ql_ui_root_node(ui);
            tpl = root->first_child;
            QL_CHECK_EQ(count_children(root), 3);

            for (int i = 1; i <= 3; i++) {
                QLUINodeRT *it = child(root, i);
                QL_CHECK(it != NULL);
                if (it == NULL)
                    continue;
                QL_CHECK_EQ(it->parent, root);
                QL_CHECK_EQ(count_children(it), 2);
                QLUINodeRT *k = it->first_child;
                QLUINodeRT *v = (k != NULL) ? k->next_sibling : NULL;
                QL_CHECK(k != NULL && v != NULL);
                if (k == NULL || v == NULL)
                    continue;
                QL_CHECK_EQ(k->parent, it);
                QL_CHECK_EQ(v->parent, it);
                char wk[32], wv[32];
                snprintf(wk, sizeof(wk), "键 %d", i);
                snprintf(wv, sizeof(wv), "值 %d", i);
                QL_CHECK(k->text != NULL && strcmp(k->text, wk) == 0);
                QL_CHECK(v->text != NULL && strcmp(v->text, wv) == 0);
            }
            ql_ui_layout(ui, 300, 400);
            QL_CHECK_EQ(root->border_box.h, 54);          /* 3 × 18 */
        }
        release();
    }

    /* ============ 3. 同一棵树可以多次调用（各自追加） ============ */
    {
        QLUIRoot *ui = build("<div id=r><div class=a>{i}</div><div class=b>b</div></div>",
                             "#r { width: 200px; background: #FFFFFF }"
                             ".a { height: 10px } .b { height: 10px }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *root = ql_ui_root_node(ui);
            QLUINodeRT *a1 = root->first_child;
            QL_CHECK_EQ(ql_ui_repeat(ui, a1, 3), QL_OK);   /* 3 个 a */
            root = ql_ui_root_node(ui);
            QL_CHECK_EQ(count_children(root), 4);          /* 3 a + 1 b */
            /* 第二次：拿第一个 a 再复制 2 份 */
            QL_CHECK_EQ(ql_ui_repeat(ui, root->first_child, 2), QL_OK);
            root = ql_ui_root_node(ui);
            QL_CHECK_EQ(count_children(root), 5);
            /* 最后那个 b 必须还在（兄弟链没被冲掉） */
            QLUINodeRT *last = child(root, 5);
            QL_CHECK(last != NULL && last->text != NULL &&
                     strcmp(last->text, "b") == 0);
            ql_ui_layout(ui, 200, 200);
            QL_CHECK_EQ(root->border_box.h, 50);
        }
        release();
    }

    /* ============ 4. count == 0：隐藏模板，节点数不变 ============ */
    {
        QLUIRoot *ui = build("<div id=r><div class=a>x</div></div>",
                             "#r { width: 100px; background: #FFFFFF }"
                             ".a { height: 10px }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUIRoot *root_ui = ui;
            QLUINodeRT *root = ql_ui_root_node(ui);
            const uint16_t before = ui->node_count;
            QL_CHECK_EQ(ql_ui_repeat(ui, root->first_child, 0), QL_OK);
            /* 0 份 = 真的删掉那支（节点数减少；不是 display:none） */
            QL_CHECK_EQ(ui->node_count, before - 1u);
            root = ql_ui_root_node(ui);
            QL_CHECK(root->first_child == NULL);
            ql_ui_layout(root_ui, 100, 100);
            QL_CHECK_EQ(root->border_box.h, 0);
        }
        release();
    }

    /* ============ 5. 非法参数 ============ */
    {
        QLUIRoot *ui = build("<div id=r><div class=a>x</div></div>",
                             "#r { width: 100px } .a { height: 10px }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(ql_ui_repeat(NULL, 0, 1), QL_ERR_INVAL);
            QL_CHECK_EQ(ql_ui_repeat(ui, NULL, 1), QL_ERR_INVAL);
            /* 根节点不能当模板 */
            QL_CHECK_EQ(ql_ui_repeat(ui, ql_ui_root_node(ui), 2), QL_ERR_INVAL);
            /* 节点数会溢出 uint16：单个子树的 65535 份 ≈ 65k 节点 */
            QL_CHECK_EQ(ql_ui_repeat(ui, ql_ui_root_node(ui)->first_child, 65535),
                        QL_ERR_FULL);
            /* 失败之后数组必须还是原样（不能半途改坏） */
            QL_CHECK_EQ(ui->node_count, 2);
        }
        release();
    }

    /* ============ 6. 没有 {i} 的子树：文本指针原样借用（零分配） ============ */
    {
        QLUIRoot *ui = build("<div id=r><div class=a>纯文本</div></div>",
                             "#r { width: 100px } .a { height: 10px }");
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *root = ql_ui_root_node(ui);
            const char *orig = root->first_child->text;
            QL_CHECK_EQ(ql_ui_repeat(ui, root->first_child, 3), QL_OK);
            root = ql_ui_root_node(ui);
            QL_CHECK_EQ(root->first_child->next_sibling->text, orig);
            QL_CHECK_EQ(ui->rep_count, 0);      /* 一条字符串都没分配 */
        }
        release();
    }

QL_TEST_END()
