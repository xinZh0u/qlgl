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

/* 某个矩形里"明显不是背景"的像素个数（判断有没有画出东西） */
static long dark_pixels(QLDispDriver *d, QLRect box)
{
    long n = 0;
    for (ql_coord_t y = box.y; y < ql_rect_bottom(box); y++)
        for (ql_coord_t x = box.x; x < ql_rect_right(box); x++)
            if ((ql_disp_mem_pixel(d, x, y) & 0xFFFFFFu) < 0xE0E0E0u)
                n++;
    return n;
}

/* 矩形里最靠右的深色像素（没有就返回 box.x） */
static ql_coord_t ink_right(QLDispDriver *d, QLRect box)
{
    for (ql_coord_t x = ql_rect_right(box) - 1; x >= box.x; x--)
        for (ql_coord_t y = box.y; y < ql_rect_bottom(box); y++)
            if ((ql_disp_mem_pixel(d, x, y) & 0xFFFFFFu) < 0xE0E0E0u)
                return x;
    return box.x;
}

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

    /* ---------- 1.5 输入法预编辑：不算值、要画出来 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<input id=a>", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *in = &ui->nodes[1];
            (void)ql_ui_pointer_down(ui, in->content_box.x + 2, 12);
            ql_ui_pointer_up(ui, in->content_box.x + 2, 12);
            QL_CHECK(ql_ui_focus_node(ui) == in);

            QLDispDriver *dd = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
            QLCanvas c;
            QL_CHECK_EQ(ql_canvas_from_disp(&c, dd), QL_OK);

            ql_canvas_clear(&c, 0xFFFFFF);
            ql_ui_render_canvas(ui, &c);
            long before = dark_pixels(dd, in->content_box);

            /* 组字中的文本：完全不影响输入框的值，但必须出现在画面上 */
            ql_ui_input_set_preedit(ui, "nihao", 5);
            QL_CHECK_STR(ql_ui_input_value(ui, in), "");
            {
                int cp = -1;
                const char *pre = ql_ui_input_preedit(ui, &cp);
                QL_CHECK(pre != NULL);
                if (pre != NULL)
                    QL_CHECK_STR(pre, "nihao");
                QL_CHECK_EQ(cp, 5);
            }
            ql_canvas_clear(&c, 0xFFFFFF);
            ql_ui_render_canvas(ui, &c);
            const long during = dark_pixels(dd, in->content_box);
            QL_CHECK(during > before);       /* 预编辑真的画出来了 */

            /* 上屏：预编辑清空，值由 QL_INPUT_TEXT 走正常路径进来 */
            ql_ui_input_set_preedit(ui, NULL, 0);
            QL_CHECK(ql_ui_input_preedit(ui, NULL) == NULL);
            ql_canvas_clear(&c, 0xFFFFFF);
            ql_ui_render_canvas(ui, &c);
            QL_CHECK_EQ(dark_pixels(dd, in->content_box), before);

            /* 码点下标 → 字节：中文一个码点 3 字节，切一半会劈坏字符 */
            ql_ui_input_set_preedit(ui, "你好", 1);
            {
                int cp = -1;
                (void)ql_ui_input_preedit(ui, &cp);
                QL_CHECK_EQ(cp, 1);
            }
            ql_ui_input_set_preedit(ui, NULL, 0);

            /*
             * 增量改动（XIM 的 chg_first/chg_length 语义）：
             * 输入法发来的是片段，按整串处理会丢字、还可能劈开半个 UTF-8
             * （半个汉字 → 字体找不到字形 → 显示成方框）。
             */
            {
                int cp = -1;
                const char *pre = NULL;
                /* a) 末尾追加 */
                ql_ui_input_preedit_apply(ui, 0, 0, "ni", 2);
                pre = ql_ui_input_preedit(ui, &cp);
                QL_CHECK(pre != NULL);
                if (pre != NULL)
                    QL_CHECK_STR(pre, "ni");
                QL_CHECK_EQ(cp, 2);

                /* b) 末尾再追加一段 */
                ql_ui_input_preedit_apply(ui, 2, 0, "hao", 5);
                pre = ql_ui_input_preedit(ui, &cp);
                if (pre != NULL)
                    QL_CHECK_STR(pre, "nihao");

                /* c) 替换中间两个码点：ni|ha|o → ni|XX|o */
                ql_ui_input_preedit_apply(ui, 2, 2, "XY", 4);
                pre = ql_ui_input_preedit(ui, NULL);
                if (pre != NULL)
                    QL_CHECK_STR(pre, "niXYo");

                /* d) 删除中间两个码点（insert == NULL） */
                ql_ui_input_preedit_apply(ui, 2, 2, NULL, 2);
                pre = ql_ui_input_preedit(ui, NULL);
                if (pre != NULL)
                    QL_CHECK_STR(pre, "nio");

                /* e) 中文按码点切：2 个汉字插在中间，不能劈坏任何一个 */
                ql_ui_input_set_preedit(ui, "ab", 2);
                ql_ui_input_preedit_apply(ui, 1, 0, "你好", 3);
                pre = ql_ui_input_preedit(ui, &cp);
                if (pre != NULL)
                    QL_CHECK_STR(pre, "a你好b");
                QL_CHECK_EQ(cp, 3);

                /* f) 删掉中间那个汉字（码点 1..2）→ "a好b" */
                ql_ui_input_preedit_apply(ui, 1, 1, NULL, 1);
                pre = ql_ui_input_preedit(ui, NULL);
                if (pre != NULL)
                    QL_CHECK_STR(pre, "a好b");
            }

            /*
             * 输入法塞进来的半截 UTF-8 必须挡在门外。实测 ibus 的 XIMText
             * `length` 是"字符数"不是"字节数"，按字节切一刀就只剩汉字的第一个
             * 字节（0xE4）—— 那半个字符进缓冲后字体找不到字形，屏幕上就是方框。
             */
            {
                ql_ui_input_set_preedit(ui, NULL, 0);
                ql_ui_input_set_preedit(ui, "\xE4", 1);
                QL_CHECK(ql_ui_input_preedit(ui, NULL) == NULL);      /* 半个字：丢掉 */
                ql_ui_input_set_preedit(ui, "a\xE4\xBD", 2);
                QL_CHECK_STR(ql_ui_input_preedit(ui, NULL), "a");     /* 尾巴被切：留 "a" */
                ql_ui_input_preedit_apply(ui, 0, 0, "\xE4\xBD\xA0\xE4", 2);
                QL_CHECK_STR(ql_ui_input_preedit(ui, NULL), "你a");   /* 只收完整的 */
                /* 合法的正常用 */
                ql_ui_input_set_preedit(ui, "\xE4\xBD\xA0", 1);
                QL_CHECK_STR(ql_ui_input_preedit(ui, NULL), "你");
            }
            ql_ui_input_set_preedit(ui, NULL, 0);

            ql_ui_free(ui);
            ql_disp_mem_destroy(dd);
        }
        ql_ui_doc_free(doc);
    }

    /*
     * 组字文本要**插进**正文而不是叠在光标上：有值时后面那截必须被推开。
     * （叠着画会和正文重叠，看起来像"字号不对/糊成一团"。）
     */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<input id=a value=\"abcd\">", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *in = &ui->nodes[1];
            QLDispDriver *dd = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
            QLCanvas c;
            QL_CHECK_EQ(ql_canvas_from_disp(&c, dd), QL_OK);
            (void)ql_ui_pointer_down(ui, in->content_box.x + 1, 12);
            (void)ql_ui_pointer_up(ui, in->content_box.x + 1, 12);

            ql_canvas_clear(&c, 0xFFFFFF);
            ql_ui_render_canvas(ui, &c);
            const ql_coord_t right0 = ink_right(dd, in->content_box);

            ql_ui_input_set_preedit(ui, "汉", 1);
            ql_canvas_clear(&c, 0xFFFFFF);
            ql_ui_render_canvas(ui, &c);
            const ql_coord_t right1 = ink_right(dd, in->content_box);
            /* "汉" 约 14px 宽：后面的 "abcd" 应该被推开这么多 */
            QL_CHECK(right1 >= right0 + 8);

            ql_ui_free(ui);
            ql_disp_mem_destroy(dd);
        }
        ql_ui_doc_free(doc);
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

            /*
             * 输入法定位：`ql_ui_ime_spot()` 给的是"光标左上角 + 高度"。
             * 它必须落在内容盒里、而且跟光标位置一致（候选窗就画在
             * (x, y+h) 那个点上）—— 走的是与绘制光标同一套几何。
             */
            {
                ql_coord_t sx = -1, sy = -1, sh = -1;
                QL_CHECK_EQ(ql_ui_ime_spot(ui, &sx, &sy, &sh), 1);
                QL_CHECK(sx >= in->content_box.x &&
                         sx <= ql_rect_right(in->content_box));
                QL_CHECK(sy >= in->content_box.y &&
                         sy + sh <= ql_rect_bottom(in->content_box));
                QL_CHECK(sh > 0);
                /*
                 * 光标在 "X" 之后：x 应该比内容盒左边右移一个字符宽，
                 * 也就是"spot 跟着光标走"（不是永远贴左边）。
                 */
                QL_CHECK(sx > in->content_box.x);
            }

            /* 点到别处 → 失焦 */
            ql_ui_pointer_down(ui, W - 2, H - 2);
            QL_CHECK(ql_ui_focus_node(ui) == NULL);
            /* 没有聚焦输入框就没有 spot（输入法要据此隐藏候选窗） */
            QL_CHECK_EQ(ql_ui_ime_spot(ui, NULL, NULL, NULL), 0);
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

            /* 聚焦并把光标放到末尾：右边应多出 1px 竖线（点内容盒里靠右的位置） */
            const ql_coord_t tap = in->content_box.x + in->content_box.w - 2;
            ql_ui_pointer_down(ui, tap, 12);
            ql_ui_pointer_up(ui, tap, 12);
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

    /* ---------- 11. 空输入框聚焦：光标照样要画出来 ---------- */
    {
        /*
         * 空值以前会走进"没有文字就整段跳过"的分支，把光标也一起跳过了 ——
         * 清空内容后就看不见焦点在哪。判据：空框聚焦必须有墨迹（那就是光标）。
         */
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<input id=a>", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *in = &ui->nodes[1];
            render(ui);
            const int ink_plain = ink_count();

            ql_ui_pointer_down(ui, in->content_box.x + 1, 12);
            ql_ui_pointer_up(ui, in->content_box.x + 1, 12);
            render(ui);
            printf("    空输入框：未聚焦 %d → 聚焦 %d\n", ink_plain, ink_count());
            QL_CHECK(ink_count() - ink_plain >= 4);   /* 多出来的就是光标竖线 */
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    ql_disp_mem_destroy(s_disp);

QL_TEST_END()
