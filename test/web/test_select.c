/**
 * @file    test_select.c
 * @brief   `<select>` + `<option>`：覆盖式下拉面板。
 *
 * 两个容易搞错的地方，各有断言守着：
 *   1. `<option>` 必须 `display: none`（它只在面板里画，不进布局）；
 *   2. 面板**不进布局、不受父级 overflow 裁剪**，点它的时候也不能被树里的
 *      节点先抢走命中。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#define W 220
#define H 160

static QLDispDriver *s_disp;
static QLCanvas s_cv;

static const char *k_html =
    "<div id=r><select id=s>"
    "<option>one</option><option>two</option><option>three</option>"
    "</select></div>";

static const char *k_css =
    "#r { width: 220px; height: 160px; padding: 8px; }"
    "#s { width: 180px; }";

static QLUIRoot *mk(QLUIDoc **out_doc)
{
    QLUIDoc *doc = ql_ui_load_text(k_html, k_css);
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

static void click(QLUIRoot *ui, ql_coord_t x, ql_coord_t y)
{
    ql_ui_pointer_down(ui, x, y);
    ql_ui_pointer_up(ui, x, y);
}

/* 白色像素数：面板底板是纯白，用它判断面板有没有画出来 */
static int white_pixels(QLUIRoot *ui)
{
    ql_canvas_from_disp(&s_cv, s_disp);
    ql_canvas_clear(&s_cv, QL_COLOR_RGB(0x10, 0x10, 0x10));
    ql_ui_render_canvas(ui, &s_cv);
    int n = 0;
    for (ql_coord_t y = 0; y < H; y++)
        for (ql_coord_t x = 0; x < W; x++) {
            const ql_color_t c = ql_disp_mem_pixel(s_disp, x, y);
            if (QL_COLOR_GET_R(c) > 0xF0 && QL_COLOR_GET_G(c) > 0xF0 &&
                QL_COLOR_GET_B(c) > 0xF0)
                n++;
        }
    return n;
}

static void send_key(QLUIRoot *ui, uint32_t key, uint8_t mods)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_KEY_DOWN;
    ev.key = key;
    ev.mods = mods;
    ql_ui_input_event(ui, &ev);
}

static void send_text(QLUIRoot *ui, const char *str)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_TEXT;
    snprintf(ev.text, sizeof(ev.text), "%s", str);
    ql_ui_input_event(ui, &ev);
}

static int g_changed;
static char g_last[64];
static void on_change(QLUIRoot *ui, QLUINodeRT *node, const char *value, void *user)
{
    (void)ui;
    (void)node;
    (void)user;
    g_changed++;
    snprintf(g_last, sizeof(g_last), "%s", value);
}

QL_TEST_BEGIN("test_select")

    s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
    QL_CHECK(s_disp != NULL);

    /* ---------- 1. 结构：option 不参与布局，select 显示第一项 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(&doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *s = &ui->nodes[1];
            QL_CHECK(strcmp(ql_ui_tag_name(s->src->tag), "select") == 0);
            QL_CHECK_EQ((int)ql_ui_select_index(ui, s), 0);
            QL_CHECK_STR(s->text, "one");
            /* 三个 option 都是 display: none */
            for (int i = 2; i <= 4; i++)
                QL_CHECK_EQ((int)ui->nodes[i].style->display,
                            (int)QL_UI_DISPLAY_NONE);
            /* 不是 select 的节点返回 -1 */
            QL_CHECK_EQ(ql_ui_select_index(ui, &ui->nodes[0]), -1);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 2. 点击展开 → 点第二项 → 选中并收起 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(&doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *s = &ui->nodes[1];
            const QLRect box = ql_ui_node_rect(s);

            QL_CHECK(ui->open_select == NULL);
            click(ui, box.x + 10, box.y + 10);
            QL_CHECK(ui->open_select != NULL);      /* 展开了 */

            /* 面板贴在下面，行高 = line-height（默认 24） */
            const ql_coord_t row_h = 24;
            click(ui, box.x + 10, ql_rect_bottom(box) + row_h + row_h / 2);
            QL_CHECK_EQ(ql_ui_select_index(ui, s), 1);
            QL_CHECK_STR(s->text, "two");
            QL_CHECK(ui->open_select == NULL);      /* 选完就收起 */
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 3. 点面板外面：只收起，不改选中 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(&doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *s = &ui->nodes[1];
            const QLRect box = ql_ui_node_rect(s);
            click(ui, box.x + 10, box.y + 10);
            QL_CHECK(ui->open_select != NULL);
            click(ui, W - 4, H - 4);
            QL_CHECK(ui->open_select == NULL);
            QL_CHECK_EQ(ql_ui_select_index(ui, s), 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 4. 面板真的画出来了（白色底板），收起后没有 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(&doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *s = &ui->nodes[1];
            const QLRect box = ql_ui_node_rect(s);
            const int closed = white_pixels(ui);
            click(ui, box.x + 10, box.y + 10);
            const int opened = white_pixels(ui);
            QL_CHECK(opened > closed + 1000);       /* 面板是多出来的一大块白 */
            click(ui, box.x + 10, box.y + 10);      /* 再点一次收起 */
            QL_CHECK_EQ(white_pixels(ui), closed);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 5. 程序设值 + 回调 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(&doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *s = &ui->nodes[1];
            g_changed = 0;
            g_last[0] = '\0';
            ql_ui_set_select_cb(ui, on_change, NULL);

            QL_CHECK_EQ(ql_ui_select_set_index(ui, s, 2), QL_OK);
            QL_CHECK_EQ(ql_ui_select_index(ui, s), 2);
            QL_CHECK_STR(s->text, "three");
            QL_CHECK_EQ(g_changed, 1);
            QL_CHECK_STR(g_last, "three");

            /* 越界夹紧，不算变化 */
            QL_CHECK_EQ(ql_ui_select_set_index(ui, s, 99), QL_OK);
            QL_CHECK_EQ(ql_ui_select_index(ui, s), 2);
            QL_CHECK_EQ(g_changed, 1);
            QL_CHECK_EQ(ql_ui_select_set_index(ui, s, -5), QL_OK);
            QL_CHECK_EQ(ql_ui_select_index(ui, s), 0);
            QL_CHECK_EQ(g_changed, 2);

            ql_ui_set_select_cb(ui, NULL, NULL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 6. 没有 option 的 select：不崩、也不可设值 ---------- */
    {
        QLUIDoc *doc = ql_ui_load_text("<div id=r><select id=s></select></div>",
                                       "#r { width: 100px; height: 60px; }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                QL_CHECK_EQ(ql_ui_select_index(ui, &ui->nodes[1]), 0);
                QL_CHECK_EQ(ql_ui_select_set_index(ui, &ui->nodes[1], 0), QL_ERR_INVAL);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 7. 没有 select 的界面：池是 NULL ---------- */
    {
        QLUIDoc *doc = ql_ui_load_text("<div id=r>x</div>", "");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QL_CHECK(ui->selects == NULL);
                QL_CHECK_EQ((int)ui->select_count, 0);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 8. Tab / Shift+Tab 在控件之间循环 ---------- */
    {
        QLUIDoc *doc = ql_ui_load_text(
            "<div id=r><input id=a><select id=s><option>x</option></select>"
            "<input id=b></div>",
            "#r { width: 200px; height: 100px; } input, select { width: 160px; }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                QL_CHECK(ui->focus == NULL);
                send_key(ui, QL_KEY_TAB, 0);
                QL_CHECK(ui->focus == &ui->nodes[1]);   /* a */
                send_text(ui, "hi");                    /* 打字进的是聚焦的那个 */
                QL_CHECK_STR(ql_ui_input_value(ui, &ui->nodes[1]), "hi");
                send_key(ui, QL_KEY_TAB, 0);
                QL_CHECK(ui->focus == &ui->nodes[2]);   /* select */
                send_key(ui, QL_KEY_TAB, 0);
                QL_CHECK(ui->focus == &ui->nodes[4]);   /* b（option 被跳过） */
                send_key(ui, QL_KEY_TAB, 0);            /* 绕回第一个 */
                QL_CHECK(ui->focus == &ui->nodes[1]);
                send_key(ui, QL_KEY_TAB, QL_MOD_SHIFT);
                QL_CHECK(ui->focus == &ui->nodes[4]);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 9. 下拉框的键盘操作 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(&doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *s = &ui->nodes[1];
            ql_ui_set_focus_node(ui, s);
            send_key(ui, QL_KEY_DOWN, 0);
            QL_CHECK_EQ(ql_ui_select_index(ui, s), 1);
            send_key(ui, QL_KEY_UP, 0);
            QL_CHECK_EQ(ql_ui_select_index(ui, s), 0);
            send_key(ui, QL_KEY_UP, 0);     /* 到头就停住，不绕回 */
            QL_CHECK_EQ(ql_ui_select_index(ui, s), 0);
            QL_CHECK(ui->open_select == NULL);
            send_key(ui, QL_KEY_SPACE, 0);  /* 空格展开 */
            QL_CHECK(ui->open_select != NULL);
            send_key(ui, QL_KEY_ENTER, 0);  /* 回车收起 */
            QL_CHECK(ui->open_select == NULL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 10. 点下拉框会给它焦点 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(&doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *s = &ui->nodes[1];
            const QLRect box = ql_ui_node_rect(s);
            click(ui, box.x + 10, box.y + 10);
            QL_CHECK(ui->focus == s);
            /* 有 :focus 样式时能命中（这里只确认状态位点亮） */
            QL_CHECK((s->flags & QL_UI_RT_FOCUS) != 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    ql_disp_mem_destroy(s_disp);

QL_TEST_END()
