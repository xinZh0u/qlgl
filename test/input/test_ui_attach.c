/**
 * @file    test_ui_attach.c
 * @brief   界面层 × 输入队列的接线：`ql_ui_attach_input()` 到底连了什么。
 *
 * 这是"窗口能开、能滚、就是点不动也打不了字"那个 bug 的守门测试。
 * 链路有三段，任何一段断了表现都一样：
 *
 *     平台层  ql_input_push_*()      → 队列
 *     主循环  ql_input_dispatch()    → 事件汇（sink）
 *     界面层  ql_ui_attach_input()   → ql_ui_input_event()
 *
 * 前两段在 test_input_queue / test_input_edit 里测过；这里只测**第三段**：
 * 没接汇时事件被静默丢掉，接上之后指针能命中、键盘能打字。
 */
#include "../ql_test.h"

#define W 300
#define H 200

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

/* 点一下（按下 + 抬起），走队列而不是直接调 ql_ui_input_event */
static void click_via_queue(ql_coord_t x, ql_coord_t y)
{
    (void)ql_input_push_move(x, y, 0);
    (void)ql_input_push_button(x, y, QL_BUTTON_LEFT, 1);
    (void)ql_input_push_button(x, y, QL_BUTTON_LEFT, 0);
    (void)ql_input_dispatch();
}

static void type_via_queue(const char *s)
{
    (void)ql_input_push_text(s);
    (void)ql_input_dispatch();
}

QL_TEST_BEGIN("test_ui_attach")

    const char *html = "<div id=r><input value=\"ab\"></div>";
    const char *css = "#r { width: 300px; height: 200px }"
                      "input { width: 200px; height: 24px; padding: 2px }";

    /* ---------- 1. 不接汇：事件被丢掉（这就是那个 bug 的现场） ---------- */
    {
        ql_input_reset();
        ql_ui_detach_input();

        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                QLUINodeRT *in = find_tag(ui, "input", 0);
                QL_CHECK(in != NULL);

                click_via_queue(in->content_box.x + 2, in->content_box.y + 2);
                /* 没接汇 → 队列排空了，但没人收到，焦点还是空的 */
                QL_CHECK_EQ((int)ql_input_pending(), 0);
                QL_CHECK(ql_ui_focus_node(ui) == NULL);

                /* 键盘同理 */
                type_via_queue("X");
                QL_CHECK(strcmp(ql_ui_input_value(ui, in), "ab") == 0);

                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 2. 接上汇：点击命中输入框、键盘能打字 ---------- */
    {
        ql_input_reset();

        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                ql_ui_attach_input(ui);

                QLUINodeRT *in = find_tag(ui, "input", 0);
                const QLFont *font = ql_ui_node_font(in);
                /* 点在 "ab" 之后 → 光标落到末尾 */
                const ql_coord_t x = ql_font_line_width(font, "ab", 2);
                click_via_queue(in->content_box.x + x, in->content_box.y + 2);

                /* 焦点到位 —— 这就是 attach 之后多出来的效果 */
                QL_CHECK(ql_ui_focus_node(ui) == in);

                type_via_queue("X");
                QL_CHECK(strcmp(ql_ui_input_value(ui, in), "abX") == 0);
                if (ql_ui_input_value(ui, in) != NULL)
                    printf("    attach 后输入 = [%s]\n", ql_ui_input_value(ui, in));

                /* 点界面底部空白：焦点清掉（与浏览器一致） */
                click_via_queue(5, H - 5);
                QL_CHECK(ql_ui_focus_node(ui) == NULL);

                ql_ui_detach_input();
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 3. 接汇后 key 事件也到：End + Backspace 删一个字 ---------- */
    {
        ql_input_reset();

        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                ql_ui_attach_input(ui);

                QLUINodeRT *in = find_tag(ui, "input", 0);
                click_via_queue(in->content_box.x + 2, in->content_box.y + 2);
                (void)ql_input_push_key(QL_KEY_END, 1, 0);
                (void)ql_input_push_key(QL_KEY_END, 0, 0);
                (void)ql_input_push_key(QL_KEY_BACKSPACE, 1, 0);
                (void)ql_input_push_key(QL_KEY_BACKSPACE, 0, 0);
                (void)ql_input_dispatch();
                QL_CHECK(strcmp(ql_ui_input_value(ui, in), "a") == 0);

                ql_ui_detach_input();
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 4. detach 之后事件不再进来 ---------- */
    {
        ql_input_reset();

        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                ql_ui_attach_input(ui);
                QLUINodeRT *in = find_tag(ui, "input", 0);
                click_via_queue(in->content_box.x + 2, in->content_box.y + 2);
                type_via_queue("1");
                QL_CHECK(strcmp(ql_ui_input_value(ui, in), "1ab") == 0);

                ql_ui_detach_input();
                type_via_queue("2");
                QL_CHECK(strcmp(ql_ui_input_value(ui, in), "1ab") == 0);

                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    ql_input_reset();
    ql_ui_detach_input();

QL_TEST_END()
