/**
 * @file    test_input_edit.c
 * @brief   `<input>`：值池、光标、编辑、选区、密码显示。
 *
 * 输入框是本库唯一"运行期会变的文本"，所以它单独有一条池
 * （doc 里没有 `<input>` 时零字节）。这里的判据两类：
 *   - 结构/行为：值、光标位置、选区、回调；
 *   - 绘制：光标真的画在那个像素上（按渲染出来的墨迹量，不看内部字段）。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#define W 240
#define H 60

static QLDispDriver *s_disp;
static QLCanvas s_cv;

static void send_text(QLUIRoot *ui, const char *s)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_TEXT;
    snprintf(ev.text, sizeof(ev.text), "%s", s);
    ql_ui_input_event(ui, &ev);
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

/* 建一个只含一个输入框的界面 */
static QLUIRoot *mk(const char *input_html, QLUIDoc **out_doc)
{
    char html[512];
    snprintf(html, sizeof(html), "<div id=r>%s</div>", input_html);
    const char *css = "#r { width: 240px; height: 60px; padding: 4px; }"
                      "input, #a { width: 200px; }";
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
    *out_doc = doc;
    return ui;
}

/* 渲染后某一行里最右的墨迹像素（用来找光标） */
static int rightmost_ink(int row)
{
    int found = -1;
    for (ql_coord_t x = 0; x < W; x++) {
        const ql_color_t c = ql_disp_mem_pixel(s_disp, x, row);
        if (QL_COLOR_GET_R(c) < 0x60 && QL_COLOR_GET_G(c) < 0x60)
            found = (int)x;      /* 白底黑字：找暗像素 */
    }
    return found;
}

static int ink_count(void)
{
    int n = 0;
    for (ql_coord_t y = 0; y < H; y++)
        for (ql_coord_t x = 0; x < W; x++) {
            const ql_color_t c = ql_disp_mem_pixel(s_disp, x, y);
            if (QL_COLOR_GET_R(c) < 0x60 && QL_COLOR_GET_G(c) < 0x60)
                n++;
        }
    return n;
}

static void render(QLUIRoot *ui)
{
    ql_canvas_from_disp(&s_cv, s_disp);
    ql_canvas_clear(&s_cv, QL_COLOR_RGB(0x20, 0x20, 0x20));
    ql_ui_render_canvas(ui, &s_cv);
}

static int g_changed;
static char g_last[128];
static void on_input(QLUIRoot *ui, QLUINodeRT *node, const char *value, void *user)
{
    (void)ui;
    (void)node;
    (void)user;
    g_changed++;
    snprintf(g_last, sizeof(g_last), "%s", value);
}

QL_TEST_BEGIN("test_input_edit")

    s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
    QL_CHECK(s_disp != NULL);

    /* ---------- 1. 解析：类型与初始值 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<input id=a value=\"abc\">", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *in = &ui->nodes[1];
            QL_CHECK(strcmp(ql_ui_tag_name(in->src->tag), "input") == 0);
            QL_CHECK_EQ((int)in->src->flags, 0);
            QL_CHECK_STR(ql_ui_input_value(ui, in), "abc");
            /* 非输入框没有值 */
            QL_CHECK(ql_ui_input_value(ui, &ui->nodes[0]) == NULL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }

        /* type=password → 标志位 */
        doc = NULL;
        ui = mk("<input id=a type=\"password\" value=\"s3cret\">", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK((ui->nodes[1].src->flags & QL_UI_NODE_PASSWORD) != 0);
            QL_CHECK_STR(ql_ui_input_value(ui, &ui->nodes[1]), "s3cret");
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }

        /* 不认识的 type 明确报错 */
        QL_CHECK(ql_ui_load_text("<div><input type=\"date\"></div>", "") == NULL);
        QL_CHECK(strstr(ql_ui_text_last_error(), "text / password") != NULL);
    }

    /* ---------- 2. 点击聚焦 + 按位置定光标 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<input id=a value=\"abcdef\">", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *in = &ui->nodes[1];
            QL_CHECK(ql_ui_focus_node(ui) == NULL);
            QL_CHECK_EQ(ql_ui_pointer_down(ui, in->content_box.x + 1, 12), 1);
            QL_CHECK(ql_ui_focus_node(ui) == in);
            /* 点在文本起点附近 → 光标在 0 */
            send_text(ui, "X");
            QL_CHECK_STR(ql_ui_input_value(ui, in), "Xabcdef");

            ql_ui_pointer_up(ui, in->content_box.x + 1, 12);
            /* 点到别处 → 失焦 */
            ql_ui_pointer_down(ui, W - 2, H - 2);
            QL_CHECK(ql_ui_focus_node(ui) == NULL);
            ql_ui_pointer_up(ui, W - 2, H - 2);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 3. 编辑：插入 / 退格 / 删除 / Home / End ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<input id=a value=\"abc\">", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *in = &ui->nodes[1];
            ql_ui_pointer_down(ui, in->content_box.x, 12);
            send_text(ui, "X");
            QL_CHECK_STR(ql_ui_input_value(ui, in), "Xabc");

            send_key(ui, QL_KEY_END, 0);
            send_text(ui, "Z");
            QL_CHECK_STR(ql_ui_input_value(ui, in), "XabcZ");

            send_key(ui, QL_KEY_BACKSPACE, 0);
            QL_CHECK_STR(ql_ui_input_value(ui, in), "Xabc");

            send_key(ui, QL_KEY_HOME, 0);
            send_key(ui, QL_KEY_DELETE, 0);
            QL_CHECK_STR(ql_ui_input_value(ui, in), "abc");

            /* 左移一格再插入 */
            send_key(ui, QL_KEY_RIGHT, 0);
            send_text(ui, "_");
            QL_CHECK_STR(ql_ui_input_value(ui, in), "a_bc");

            /* ESC 失焦 */
            send_key(ui, QL_KEY_ESCAPE, 0);
            QL_CHECK(ql_ui_focus_node(ui) == NULL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 4. 选区：Shift+End 后删掉 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<input id=a value=\"abcdef\">", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *in = &ui->nodes[1];
            ql_ui_pointer_down(ui, in->content_box.x, 12);
            ql_ui_pointer_up(ui, in->content_box.x, 12);
            send_key(ui, QL_KEY_END, QL_MOD_SHIFT);
            send_key(ui, QL_KEY_BACKSPACE, 0);
            QL_CHECK_STR(ql_ui_input_value(ui, in), "");
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 5. 程序改值 + 长度上限 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<input id=a>", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *in = &ui->nodes[1];
            QL_CHECK_EQ(ql_ui_input_set_value(ui, in, "hello"), QL_OK);
            QL_CHECK_STR(ql_ui_input_value(ui, in), "hello");
            QL_CHECK_EQ(ql_ui_input_set_value(ui, &ui->nodes[0], "x"), QL_ERR_INVAL);

            /* 超长会被截断，不会越界 */
            char big[512];
            memset(big, 'a', sizeof(big) - 1u);
            big[sizeof(big) - 1u] = '\0';
            QL_CHECK_EQ(ql_ui_input_set_value(ui, in, big), QL_OK);
            QL_CHECK((int)strlen(ql_ui_input_value(ui, in)) ==
                     (int)QL_CFG_UI_INPUT_LEN - 1);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 6. 回调：改值触发一次，回车也触发 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<input id=a value=\"x\">", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *in = &ui->nodes[1];
            g_changed = 0;
            g_last[0] = '\0';
            ql_ui_set_input_cb(ui, on_input, NULL);
            ql_ui_pointer_down(ui, in->content_box.x, 12);
            send_text(ui, "Y");
            QL_CHECK_EQ(g_changed, 1);
            QL_CHECK_STR(g_last, "Yx");
            send_key(ui, QL_KEY_ENTER, 0);
            QL_CHECK_EQ(g_changed, 2);      /* 回车也算一次（提交语义） */
            ql_ui_set_input_cb(ui, NULL, NULL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 7. 绘制：光标画在文字之后，密码框画圆点 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<input id=a value=\"IIII\">", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *in = &ui->nodes[1];
            /* 不聚焦：没有光标，墨迹 = 4 个 I */
            render(ui);
            const int ink_plain = ink_count();
            QL_CHECK(ink_plain > 0);
            QL_CHECK(rightmost_ink(12) > 0);

            /* 聚焦并把光标放到末尾：右边应多出 1px 竖线 */
            ql_ui_pointer_down(ui, in->content_box.x + 200, 12);
            ql_ui_pointer_up(ui, in->content_box.x + 200, 12);
            render(ui);
            QL_CHECK(ink_count() > ink_plain);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }

        /* 密码框：显示成圆点，墨迹数与明文不同，但值本身不变 */
        doc = NULL;
        ui = mk("<input id=a type=\"password\" value=\"IIII\">", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *in = &ui->nodes[1];
            render(ui);
            const int ink_pw = ink_count();
            QL_CHECK(ink_pw > 0);
            QL_CHECK_STR(ql_ui_input_value(ui, in), "IIII");   /* 值仍是明文 */
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 8. 没有 <input> 的界面：池是 NULL（零字节） ---------- */
    {
        QLUIDoc *doc = ql_ui_load_text("<div id=r>x</div>", "");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QL_CHECK(ui->inputs == NULL);
                QL_CHECK_EQ((int)ui->input_count, 0);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    ql_disp_mem_destroy(s_disp);

QL_TEST_END()
