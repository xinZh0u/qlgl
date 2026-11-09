/**
 * @file    test_textarea.c
 * @brief   CONTROL 包 P5：`<textarea>` 多行编辑。
 *
 * 多行编辑"看起来"只是把单行输入框放大，实际上多出三件事，测试就按这三件分：
 *   1. **渲染**：换行与行首空白要保留（靠 P4a 的 `white-space: pre-wrap`），
 *      内容比盒子高时纵向滚动；
 *   2. **光标**：位置是（视觉行, 行内 x）而不是（单行字节偏移）——
 *      上下键要保住横向位置、Home/End 是本行首尾、点哪落哪；
 *   3. **池**：`<textarea>` 与 `<input>` **分池**（缓冲 512 vs 96），
 *      没有 textarea 的界面零字节。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#define W 360
#define H 240

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

/* 造一条键盘事件 */
static void key(QLUIRoot *ui, uint32_t k, uint8_t mods)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_KEY_DOWN;
    ev.key = k;
    ev.mods = mods;
    ql_ui_input_event(ui, &ev);
}

/* 打一串 ASCII */
static void type(QLUIRoot *ui, const char *s)
{
    for (const char *p = s; *p != '\0'; p++) {
        QLInputEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = QL_INPUT_TEXT;
        ev.text[0] = *p;
        ql_ui_input_event(ui, &ev);
    }
}

/* 按下 → 拖到 (x,y) → 抬起（选区靠这条路径） */
static void drag(QLUIRoot *ui, ql_coord_t x0, ql_coord_t y0, ql_coord_t x1, ql_coord_t y1)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_POINTER_DOWN;
    ev.x = x0;
    ev.y = y0;
    ql_ui_input_event(ui, &ev);
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_POINTER_MOVE;
    ev.x = x1;
    ev.y = y1;
    ql_ui_input_event(ui, &ev);
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_POINTER_UP;
    ev.x = x1;
    ev.y = y1;
    ql_ui_input_event(ui, &ev);
}

static void click(QLUIRoot *ui, ql_coord_t x, ql_coord_t y)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_POINTER_DOWN;
    ev.x = x;
    ev.y = y;
    ql_ui_input_event(ui, &ev);
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_POINTER_UP;
    ev.x = x;
    ev.y = y;
    ql_ui_input_event(ui, &ev);
}

static QLDispDriver *s_disp;

/* --- 假剪贴板：用来"看到"选区内容（不需要新增公开 API） --- */
static char s_clip_buf[512];

static void clip_set(const char *utf8, void *user)
{
    (void)user;
    snprintf(s_clip_buf, sizeof(s_clip_buf), "%s", utf8 != NULL ? utf8 : "");
}

static int clip_get(char *buf, size_t cap, void *user)
{
    (void)user;
    (void)buf;
    (void)cap;
    return 0;   /* 这个测试只关心 set（复制出去），不需要粘贴 */
}

static const QLUIClipboard s_clip = { clip_set, clip_get };

static ql_color_t px(int x, int y)
{
    return ql_disp_mem_pixel(s_disp, (ql_coord_t)x, (ql_coord_t)y);
}

QL_TEST_BEGIN("test_textarea")

    /* ---------- 1. 标签认识 + UA 默认 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><textarea></textarea></div>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *ta = find_tag(ui, "textarea", 0);
            QL_CHECK(ta != NULL);
            if (ta != NULL) {
                QL_CHECK_EQ((int)ta->style->display, (int)QL_UI_DISPLAY_INLINE_BLOCK);
                QL_CHECK_EQ((int)ta->style->white_space, (int)QL_UI_WS_PRE_WRAP);
                QL_CHECK_EQ((int)ta->style->font_family, (int)QL_UI_FAMILY_MONO);
                QL_CHECK(ta->style->border_width > 0);
                QL_CHECK(ta->border_box.h >= 60);
            }
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 2. 池：分池、按需分配 ---------- */
    {
        /* 没有 textarea：arena 为 NULL */
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><input value=a></div>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK(ui->ta_arena == NULL);
            QL_CHECK_EQ((int)ui->ta_count, 0);
            QL_CHECK_EQ((int)ui->input_count, 1);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
        /* 有一个 textarea：arena 分配，且它占另一个池位（input_count 计入） */
        ui = NULL;
        doc = mk("<div id=r><input value=a><textarea></textarea></div>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK(ui->ta_arena != NULL);
            QL_CHECK_EQ((int)ui->ta_count, 1);
            QL_CHECK_EQ((int)ui->input_count, 2);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 3. 初始内容：保留换行、丢开头那一个 '\n' ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><textarea>\nabc\ndef</textarea></div>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *ta = find_tag(ui, "textarea", 0);
            QL_CHECK(ta != NULL);
            QL_CHECK(ta->text != NULL && strcmp(ta->text, "abc\ndef") == 0);
            if (ta->text != NULL)
                printf("    初始值 = %s\n", ta->text);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 4. 回车插入换行、退格跨行合并 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><textarea></textarea></div>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *ta = find_tag(ui, "textarea", 0);
            click(ui, ta->content_box.x + 2, ta->content_box.y + 2);
            type(ui, "ab");
            key(ui, QL_KEY_ENTER, 0);
            type(ui, "cd");
            QL_CHECK(strcmp(ql_ui_input_value(ui, ta), "ab\ncd") == 0);
            if (ql_ui_input_value(ui, ta) != NULL)
                printf("    输入后 = %s\n", ql_ui_input_value(ui, ta));

            /* 退格三次：'d'、'c'、再删掉换行（跨行合并）→ "ab" */
            key(ui, QL_KEY_BACKSPACE, 0);
            key(ui, QL_KEY_BACKSPACE, 0);
            key(ui, QL_KEY_BACKSPACE, 0);
            if (ql_ui_input_value(ui, ta) != NULL)
                printf("    三次退格后 = [%s]\n", ql_ui_input_value(ui, ta));
            QL_CHECK(strcmp(ql_ui_input_value(ui, ta), "ab") == 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 5. 上下键保住横向位置 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><textarea>abcdef\nxyz\n123456</textarea></div>",
                          "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *ta = find_tag(ui, "textarea", 0);
            /* 光标放到第一行第 3 个字符之后 */
            const QLFont *font = ql_ui_node_font(ta);
            const ql_coord_t x3 = ql_font_line_width(font, "abc", 3);
            click(ui, ta->content_box.x + x3, ta->content_box.y + 2);

            /* 下 → 第二行（"xyz"，短行）应停在行尾 */
            key(ui, QL_KEY_DOWN, 0);
            type(ui, "!");
            QL_CHECK(strcmp(ql_ui_input_value(ui, ta), "abcdef\nxyz!\n123456") == 0);
            if (ql_ui_input_value(ui, ta) != NULL)
                printf("    下移后 = %s\n", ql_ui_input_value(ui, ta));

            /* 再下 → 第三行，横向位置回到第 3 列（"12?3456"） */
            key(ui, QL_KEY_DOWN, 0);
            type(ui, "?");
            const char *v = ql_ui_input_value(ui, ta);
            if (v != NULL)
                printf("    再下移后 = %s\n", v);
            /*
             * 注意这里回不到"点下去时那一列"：打过一个字符之后，
             * sticky 列会被**清掉**（浏览器的行为 —— 打字的瞬间，
             * "想去的列"就是当前列了）。所以这里落在第 4 列。
             */
            QL_CHECK(v != NULL && strcmp(v, "abcdef\nxyz!\n1234?56") == 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 6. Home / End 是本行首尾（不是全文） ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><textarea>aa\nbbbb\ncc</textarea></div>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *ta = find_tag(ui, "textarea", 0);
            click(ui, ta->content_box.x + 2, ta->content_box.y + 2);
            /* 到第二行（"bbbb"），End 应落在 'bbbb' 之后，不是全文尾 */
            key(ui, QL_KEY_DOWN, 0);
            key(ui, QL_KEY_END, 0);
            type(ui, "X");
            const char *v = ql_ui_input_value(ui, ta);
            QL_CHECK(v != NULL && strcmp(v, "aa\nbbbbX\ncc") == 0);
            if (v != NULL)
                printf("    End 后 = %s\n", v);

            /* Home 回本行行首 */
            key(ui, QL_KEY_HOME, 0);
            type(ui, "Y");
            v = ql_ui_input_value(ui, ta);
            QL_CHECK(v != NULL && strcmp(v, "aa\nYbbbbX\ncc") == 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 7. 单行输入框的上/下键不被吃掉 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><input value=ab></div>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *in = find_tag(ui, "input", 0);
            click(ui, in->content_box.x + 2, in->content_box.y + 2);
            /* 上下键对单行框没有语义：值不变（交给外层滚动） */
            key(ui, QL_KEY_UP, 0);
            key(ui, QL_KEY_DOWN, 0);
            QL_CHECK(strcmp(ql_ui_input_value(ui, in), "ab") == 0);
            /* Enter 不插换行 */
            key(ui, QL_KEY_ENTER, 0);
            QL_CHECK(strcmp(ql_ui_input_value(ui, in), "ab") == 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 8. 容量：textarea 能吃下远超单行上限的文本 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><textarea></textarea></div>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *ta = find_tag(ui, "textarea", 0);
            click(ui, ta->content_box.x + 2, ta->content_box.y + 2);
            /* 200 个字符：超过 QL_CFG_UI_INPUT_LEN(96)，但远小于 512 */
            char big[201];
            memset(big, 'z', 200);
            big[200] = '\0';
            type(ui, big);
            const char *v = ql_ui_input_value(ui, ta);
            QL_CHECK(v != NULL && strlen(v) == 200);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 9. set_value / value 往返（含换行） ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><textarea></textarea></div>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *ta = find_tag(ui, "textarea", 0);
            QL_CHECK_EQ((int)ql_ui_input_set_value(ui, ta, "p\nq"), (int)QL_OK);
            const char *got = ql_ui_input_value(ui, ta);
            QL_CHECK(got != NULL && strcmp(got, "p\nq") == 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 10. 像素：光标画在对应的行上 ---------- */
    {
        s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
        QL_CHECK(s_disp != NULL);
        if (s_disp != NULL) {
            QLUIRoot *ui = NULL;
            QLUIDoc *doc = mk("<div id=r><textarea>aa\nbb</textarea></div>",
                              "#r { width: 200px } textarea { color: #000000;"
                              " background-color: #FFFFFF }", &ui);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QLUINodeRT *ta = find_tag(ui, "textarea", 0);
                /* 点第二行行首：聚焦 + 光标落在第二行 */
                const QLFont *font = ql_ui_node_font(ta);
                const ql_coord_t lh = font->line_height;
                click(ui, ta->content_box.x + 1, ta->content_box.y + lh + 2);

                QLCanvas cv;
                QL_CHECK_EQ(ql_canvas_from_disp(&cv, s_disp), QL_OK);
                ql_canvas_clear(&cv, 0xFFFFFF);
                ql_ui_render_canvas(ui, &cv);

                /* 第二行的行盒范围内应该有竖线（非白） */
                const QLRect cb = ta->content_box;
                int col_ink = 0;
                for (int y = cb.y + lh + 1; y < cb.y + 2 * lh - 1; y++)
                    if (px(cb.x, y) != 0xFFFFFF)
                        col_ink++;
                QL_CHECK(col_ink > 0);
                printf("    第二行行首竖线像素 %d 个\n", col_ink);
                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
            ql_disp_mem_destroy(s_disp);
        }
    }

    /* ---------- 11. 纵向滚动：内容高于盒子时光标仍能往下走 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><textarea>1\n2\n3\n4\n5\n6</textarea></div>",
                          "#r { width: 200px } textarea { height: 32px }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *ta = find_tag(ui, "textarea", 0);
            click(ui, ta->content_box.x + 1, ta->content_box.y + 1);
            for (int i = 0; i < 5; i++)
                key(ui, QL_KEY_DOWN, 0);
            type(ui, "Z");
            const char *v = ql_ui_input_value(ui, ta);
            QL_CHECK(v != NULL && strchr(v, 'Z') != NULL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 12. blob 往返：textarea 仍然是多行可编辑 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><textarea>m\nn</textarea></div>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            size_t cap = ql_ui_doc_encode(doc, NULL, 0);
            void *buf = (cap > 0) ? malloc(cap) : NULL;
            QL_CHECK(buf != NULL);
            if (buf != NULL) {
                QL_CHECK_EQ((int)ql_ui_doc_encode(doc, buf, cap), (int)cap);
                QLUIDoc *back = ql_ui_doc_load_mem(buf, cap);
                QL_CHECK(back != NULL);
                if (back != NULL) {
                    QLUIRoot *ui2 = ql_ui_build(back);
                    QL_CHECK(ui2 != NULL);
                    if (ui2 != NULL) {
                        ql_ui_layout(ui2, W, H);
                        QLUINodeRT *ta = find_tag(ui2, "textarea", 0);
                        QL_CHECK(ta != NULL);
                        QL_CHECK(ql_ui_input_value(ui2, ta) != NULL &&
                                 strcmp(ql_ui_input_value(ui2, ta), "m\nn") == 0);
                        QL_CHECK(ui2->ta_arena != NULL);
                        ql_ui_free(ui2);
                    }
                    ql_ui_doc_free(back);
                }
                free(buf);
            }
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 13. 内容比盒子高时**裁在框内**（不能画到框外去） ---------- */
    {
        s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
        QL_CHECK(s_disp != NULL);
        if (s_disp != NULL) {
            QLUIRoot *ui = NULL;
            /* 红底：溢出的黑字在红底上肉眼可见，才好断言 */
            QLUIDoc *doc = mk("<div id=r><textarea>1\n2\n3\n4\n5\n6\n7\n8\n9</textarea>"
                              "</div>",
                              "#r { width: 240px; background-color: #FF0000 }"
                              "textarea { width: 200px; height: 48px; padding: 4px;"
                              " background-color: #FFFFFF; color: #000000;"
                              " line-height: 16px }", &ui);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QLCanvas cv;
                QL_CHECK_EQ(ql_canvas_from_disp(&cv, s_disp), QL_OK);
                ql_canvas_clear(&cv, 0xFF0000);
                ql_ui_render_canvas(ui, &cv);

                const QLRect b = ql_ui_node_rect(find_tag(ui, "textarea", 0));
                int leak = 0;
                for (int y = (int)(b.y + b.h) + 1; y < (int)(b.y + b.h) + 8 && y < H; y++)
                    for (int x = (int)b.x; x < (int)(b.x + b.w); x++)
                        if (px(x, y) != 0xFF0000)
                            leak++;
                printf("    框外泄露像素 = %d（应 0）\n", leak);
                QL_CHECK_EQ(leak, 0);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
            ql_disp_mem_destroy(s_disp);
        }
    }

    /* ---------- 14. 打字打到框底要自动滚（光标不能跑到框外） ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><textarea></textarea></div>",
                          "#r { width: 240px } textarea { height: 48px;"
                          " line-height: 16px; padding: 4px }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *ta = find_tag(ui, "textarea", 0);
            click(ui, ta->content_box.x + 2, ta->content_box.y + 2);
            /* 打 10 行：内容远超 48px 高，视图必须跟着滚 */
            for (int i = 0; i < 10; i++) {
                if (i > 0)
                    key(ui, QL_KEY_ENTER, 0);
                type(ui, "abc");
            }
            const char *v = ql_ui_input_value(ui, ta);
            QL_CHECK(v != NULL && strchr(v, '\n') != NULL);

            /*
             * 断言"最后一行在可见范围内"：把光标位置换算成 y，
             * 必须落在内容盒里（这正是 auto-scroll 的目的）。
             */
            s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
            QL_CHECK(s_disp != NULL);
            if (s_disp != NULL) {
                QLCanvas cv;
                QL_CHECK_EQ(ql_canvas_from_disp(&cv, s_disp), QL_OK);
                ql_canvas_clear(&cv, 0xFFFFFF);
                ql_ui_render_canvas(ui, &cv);
                /* 框下方的红区不该有光标竖线（用红底更好判断，这里退一步：
                 * 直接比对内容盒下沿外 1px 处没有非白像素之外的东西） */
                /* 从**边框盒**下沿往外看：content_box 下面还有 padding 和边框，
                 * 拿 content_box 量会把边框本身当成"越界" */
                const QLRect bb = ql_ui_node_rect(ta);
                int outside = 0;
                for (int y = (int)(bb.y + bb.h) + 1; y < (int)(bb.y + bb.h) + 6; y++)
                    for (int x = (int)bb.x; x < (int)(bb.x + bb.w); x++)
                        if (px(x, y) != 0xFFFFFF)
                            outside++;
                printf("    光标/文字越界像素 = %d（应 0）\n", outside);
                QL_CHECK_EQ(outside, 0);
                ql_disp_mem_destroy(s_disp);
            }
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 15. Ctrl+A 全选 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><textarea>hello world</textarea></div>", "", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *ta = find_tag(ui, "textarea", 0);
            click(ui, ta->content_box.x + 2, ta->content_box.y + 2);
            key(ui, 'a', QL_MOD_CTRL);
            type(ui, "X");       /* 全选之后打字应该整串替换 */
            QL_CHECK(strcmp(ql_ui_input_value(ui, ta), "X") == 0);
            if (ql_ui_input_value(ui, ta) != NULL)
                printf("    全选后输入 = [%s]\n", ql_ui_input_value(ui, ta));
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 16. 跨行拖选：选区必须跟着鼠标走 ---------- */
    {
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk("<div id=r><textarea>aaaaaaaaaa\nbbbbbbbbbb\ncccccccccc"
                          "</textarea></div>",
                          "#r { width: 300px } textarea { width: 240px; height: 90px;"
                          " padding: 4px; line-height: 16px }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            ql_ui_set_clipboard(ui, &s_clip, NULL);
            QLUINodeRT *ta = find_tag(ui, "textarea", 0);
            const QLFont *font = ql_ui_node_font(ta);
            const ql_coord_t lh = font->line_height;
            const QLRect cb = ta->content_box;

            /*
             * 第 1 行第 4 列 → 第 3 行第 4 列。
             * 期望选区 = "aaaaaa\nbbbbbbbbbb\ncccc"（6 + 1 + 10 + 1 + 4 = 22 字节）。
             *
             * 这是"拖选走单行公式"那个 bug 的守门用例：以前按 x 当单行算，
             * 会落到第 1 行的第 26 个字符（不存在）或者别处，选区完全对不上。
             */
            const ql_coord_t x4 = ql_font_line_width(font, "aaaa", 4);
            drag(ui, cb.x + x4, cb.y + 2, cb.x + x4, cb.y + 2 * lh + 2);
            (void)ql_ui_clipboard_copy(ui);
            printf("    跨行选区 = [%s]\n", s_clip_buf);
            QL_CHECK(strcmp(s_clip_buf, "aaaaaa\nbbbbbbbbbb\ncccc") == 0);

            /* 反向拖（从下往上）也应该是同一段文本 */
            s_clip_buf[0] = '\0';
            drag(ui, cb.x + x4, cb.y + 2 * lh + 2, cb.x + x4, cb.y + 2);
            (void)ql_ui_clipboard_copy(ui);
            QL_CHECK(strcmp(s_clip_buf, "aaaaaa\nbbbbbbbbbb\ncccc") == 0);

            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 17. 拖出框外自动滚：滚到末尾就停，选区到文本末尾 ---------- */
    {
        const char *txt = "l0\nl1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9";
        char html[256];
        snprintf(html, sizeof(html), "<div id=r><textarea>%s</textarea></div>", txt);
        QLUIRoot *ui = NULL;
        QLUIDoc *doc = mk(html,
                          "#r { width: 300px } textarea { width: 240px; height: 48px;"
                          " padding: 4px; line-height: 16px }", &ui);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            ql_ui_set_clipboard(ui, &s_clip, NULL);
            QLUINodeRT *ta = find_tag(ui, "textarea", 0);
            const QLRect cb = ta->content_box;

            /* 按下在第 1 行行首，然后连续 30 帧把指针停在盒子下方很远处 */
            QLInputEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = QL_INPUT_POINTER_DOWN;
            ev.x = cb.x;
            ev.y = cb.y + 2;
            ql_ui_input_event(ui, &ev);

            for (int i = 0; i < 30; i++) {
                memset(&ev, 0, sizeof(ev));
                ev.type = QL_INPUT_POINTER_MOVE;
                ev.x = cb.x;
                ev.y = cb.y + cb.h + 200;
                ql_ui_input_event(ui, &ev);
            }
            memset(&ev, 0, sizeof(ev));
            ev.type = QL_INPUT_POINTER_UP;
            ev.x = cb.x;
            ev.y = cb.y + cb.h + 200;
            ql_ui_input_event(ui, &ev);

            (void)ql_ui_clipboard_copy(ui);
            /* 滚到末尾就停：选区正好是全文，不会多也不会少 */
            printf("    拖出框外选区长 %d（全文 %d）\n",
                   (int)strlen(s_clip_buf), (int)strlen(txt));
            QL_CHECK(strcmp(s_clip_buf, txt) == 0);

            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 18. 空 textarea 聚焦也要有光标 ---------- */
    {
        s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
        QL_CHECK(s_disp != NULL);
        if (s_disp != NULL) {
            QLUIRoot *ui = NULL;
            QLUIDoc *doc = mk("<div id=r><textarea></textarea></div>",
                              "#r { width: 200px } textarea { color: #000000;"
                              " background-color: #FFFFFF }", &ui);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QLUINodeRT *ta = find_tag(ui, "textarea", 0);
                click(ui, ta->content_box.x + 1, ta->content_box.y + 2);

                QLCanvas cv;
                QL_CHECK_EQ(ql_canvas_from_disp(&cv, s_disp), QL_OK);
                ql_canvas_clear(&cv, 0xFFFFFF);
                ql_ui_render_canvas(ui, &cv);

                const QLRect cb = ta->content_box;
                int ink = 0;
                for (int y = cb.y; y < cb.y + cb.h && y < H; y++)
                    if (px(cb.x, y) != 0xFFFFFF)
                        ink++;
                QL_CHECK(ink >= 4);   /* 空文本：第一行行首也该有竖线 */
                printf("    空框聚焦竖线像素 %d 个\n", ink);
                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
            ql_disp_mem_destroy(s_disp);
        }
    }

    /* ---------- 19. 末尾敲空格：光标要跟着走 ---------- */
    {
        /*
         * 尾随空格以前被 ql_text_wrap 的 trim_line_end 吃掉 → 值插进去了，
         * 但屏幕上什么都不动，看起来像"空格没打上"。
         * 判据：每敲一个空格，光标像素列就往右挪一个空格宽。
         */
        s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
        QL_CHECK(s_disp != NULL);
        if (s_disp != NULL) {
            QLUIRoot *ui = NULL;
            QLUIDoc *doc = mk("<div id=r><textarea>ab</textarea></div>",
                              "#r { width: 200px } textarea { color: #000000;"
                              " background-color: #FFFFFF }", &ui);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QLUINodeRT *ta = find_tag(ui, "textarea", 0);
                const QLRect cb = ta->content_box;
                click(ui, cb.x + cb.w - 2, cb.y + 2);   /* 聚焦并把光标放到行尾 */

                QLCanvas cv;
                QL_CHECK_EQ(ql_canvas_from_disp(&cv, s_disp), QL_OK);
                ql_canvas_clear(&cv, 0xFFFFFF);
                ql_ui_render_canvas(ui, &cv);

                /* 该行最右的墨迹就是光标 */
                int prev = -1;
                for (int x = cb.x; x < cb.x + cb.w; x++)
                    if (px(x, cb.y + 8) != 0xFFFFFF)
                        prev = x;

                type(ui, " ");
                ql_canvas_clear(&cv, 0xFFFFFF);
                ql_ui_render_canvas(ui, &cv);
                int now = -1;
                for (int x = cb.x; x < cb.x + cb.w; x++)
                    if (px(x, cb.y + 8) != 0xFFFFFF)
                        now = x;

                printf("    末尾空格：光标列 %d → %d，值=[%s]\n", prev, now,
                       ql_ui_input_value(ui, ta));
                QL_CHECK(now > prev);                                  /* 光标挪了 */
                QL_CHECK(strcmp(ql_ui_input_value(ui, ta), "ab ") == 0); /* 值也真的加了 */

                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
            ql_disp_mem_destroy(s_disp);
        }
    }

    /* ---------- 20. 内容超高：有滚动条，滚轮能滚 ---------- */
    {
        s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
        QL_CHECK(s_disp != NULL);
        if (s_disp != NULL) {
            char html[512];
            snprintf(html, sizeof(html),
                     "<div id=r><textarea>l0\nl1\nl2\nl3\nl4\nl5\nl6\nl7\n"
                     "l8\nl9\nl10\nl11\nl12\nl13\nl14\nl15\nl16\nl17\n"
                     "l18\nl19</textarea></div>");
            QLUIRoot *ui = NULL;
            QLUIDoc *doc = mk(html,
                              "#r { width: 200px } textarea { width: 200px; height: 60px;"
                              " padding: 4px; color: #000000;"
                              " background-color: #FFFFFF; line-height: 16px }", &ui);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                QLUINodeRT *ta = find_tag(ui, "textarea", 0);
                const QLRect cb = ta->content_box;

                /*
                 * 观测手段两条：
                 *   - 内容盒像素哈希：滚轮真的改画面；
                 *   - 右侧滑块的最上一行：它随滚动位置往下走，而且到末尾就不动了。
                 */
                #define SIGNATURE(out_hash, out_top)                                   \
                    do {                                                               \
                        unsigned h_ = 2166136261u;                                     \
                        int top_ = -1;                                                 \
                        for (int y_ = cb.y; y_ < cb.y + cb.h; y_++)                    \
                            for (int x_ = cb.x; x_ < cb.x + cb.w; x_++) {              \
                                const ql_color_t c_ = px(x_, y_);                      \
                                h_ = (h_ ^ (unsigned)c_) * 16777619u;                  \
                                if (top_ < 0 && x_ >= cb.x + cb.w - 4 &&             \
                                    c_ != 0xFFFFFF && c_ != 0xF6F8FA)                  \
                                    top_ = y_;                                         \
                            }                                                          \
                        *(out_hash) = h_;                                              \
                        *(out_top) = top_;                                             \
                    } while (0)

                QLCanvas cv;
                QL_CHECK_EQ(ql_canvas_from_disp(&cv, s_disp), QL_OK);
                ql_canvas_clear(&cv, 0xFFFFFF);
                ql_ui_render_canvas(ui, &cv);

                unsigned h0 = 0;
                int top0 = -1;
                SIGNATURE(&h0, &top0);
                printf("    超高 textarea：滑块顶 %d\n", top0);
                QL_CHECK(top0 >= cb.y);          /* 有滑块 */

                QLInputEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = QL_INPUT_WHEEL;
                ev.x = cb.x + 10;
                ev.y = cb.y + 10;
                ev.dy = 1;
                ql_ui_input_event(ui, &ev);

                ql_canvas_clear(&cv, 0xFFFFFF);
                ql_ui_render_canvas(ui, &cv);
                unsigned h1 = 0;
                int top1 = -1;
                SIGNATURE(&h1, &top1);
                printf("    滚一格：滑块顶 %d → %d\n", top0, top1);
                QL_CHECK(h1 != h0);              /* 滚轮真的动了画面 */
                QL_CHECK(top1 > top0);           /* 滑块往下走了 */

                /* 滚到底：继续滚不应该再动（不能滚进空白） */
                for (int i = 0; i < 30; i++) {
                    memset(&ev, 0, sizeof(ev));
                    ev.type = QL_INPUT_WHEEL;
                    ev.x = cb.x + 10;
                    ev.y = cb.y + 10;
                    ev.dy = 1;
                    ql_ui_input_event(ui, &ev);
                }
                ql_canvas_clear(&cv, 0xFFFFFF);
                ql_ui_render_canvas(ui, &cv);
                unsigned h2 = 0;
                int top2 = -1;
                SIGNATURE(&h2, &top2);

                memset(&ev, 0, sizeof(ev));
                ev.type = QL_INPUT_WHEEL;
                ev.x = cb.x + 10;
                ev.y = cb.y + 10;
                ev.dy = 1;
                ql_ui_input_event(ui, &ev);
                ql_canvas_clear(&cv, 0xFFFFFF);
                ql_ui_render_canvas(ui, &cv);
                unsigned h3 = 0;
                int top3 = -1;
                SIGNATURE(&h3, &top3);
                QL_CHECK(h3 == h2);              /* 到底就停住 */
                QL_CHECK(top2 > top1);

                /* 末行看得见（滚到底不该是空白） */
                int bottom_ink = 0;
                for (int y = cb.y + cb.h - 16; y < cb.y + cb.h; y++)
                    for (int x = cb.x; x < cb.x + cb.w; x++)
                        if (px(x, y) != 0xFFFFFF)
                            bottom_ink++;
                QL_CHECK(bottom_ink > 0);
                #undef SIGNATURE

                ql_ui_free(ui);
                ql_ui_doc_free(doc);
            }
            ql_disp_mem_destroy(s_disp);
        }
    }

    /* ---------- 21. 光标必须跟着**用过的行距**走（CSS line-height） ---------- */
    {
        /*
         * 以前光标/命中/滚动都用 font->line_height，文本绘制却用 CSS
         * `line-height` —— 两者不等时（19 vs 16）每往下一行就多偏 3px，
         * 表现就是"越是下面的行，光标偏得越厉害"。
         *
         * 判据用**空行**：内容全是换行时，内容盒里唯一的墨迹就是光标，
         * 于是可以精确读出光标的 y 范围。
         */
        s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
        QL_CHECK(s_disp != NULL);
        if (s_disp != NULL) {
            static const int k_lh[2] = { 16, 24 };   /* 一个小于字体行高，一个大于 */
            for (int k = 0; k < 2; k++) {
                char css[192];
                snprintf(css, sizeof(css),
                         "#r { width: 200px } textarea { width: 200px; height: 150px;"
                         " padding: 6px; color: #000000; background-color: #FFFFFF;"
                         " line-height: %dpx }", k_lh[k]);
                QLUIRoot *ui = NULL;
                QLUIDoc *doc = mk("<div id=r><textarea>\n\n\n\n\n</textarea></div>", css, &ui);
                QL_CHECK(ui != NULL);
                if (ui != NULL) {
                    QLUINodeRT *ta = find_tag(ui, "textarea", 0);
                    const QLRect cb = ta->content_box;
                    const QLFont *font = ql_ui_node_font(ta);
                    /* 文本绘制的半行距：line-height 与字体行高不等时上下各分一半 */
                    const ql_coord_t half =
                        (ql_coord_t)(k_lh[k] - (font->ascent + font->descent)) / 2;
                    QLCanvas cv;
                    QL_CHECK_EQ(ql_canvas_from_disp(&cv, s_disp), QL_OK);

                    int tops[2] = { -1, -1 }, bots[2] = { -1, -1 };
                    const int line[2] = { 1, 3 };
                    for (int i = 0; i < 2; i++) {
                        const ql_coord_t cy = cb.y + (ql_coord_t)line[i] * k_lh[k] + 8;
                        ql_ui_pointer_down(ui, cb.x + 1, cy);
                        ql_ui_pointer_up(ui, cb.x + 1, cy);
                        ql_canvas_clear(&cv, 0xFFFFFF);
                        ql_ui_render_canvas(ui, &cv);
                        for (int y = cb.y; y < cb.y + cb.h; y++)
                            for (int x = cb.x; x < cb.x + cb.w; x++)
                                if (px(x, y) != 0xFFFFFF) {
                                    if (tops[i] < 0)
                                        tops[i] = y;
                                    bots[i] = y;
                                }
                    }
                    const int expect_top0 = (int)cb.y + line[0] * k_lh[k] + (int)half;
                    const int expect_top1 = (int)cb.y + line[1] * k_lh[k] + (int)half;
                    printf("    line-height %d：光标行1 顶 %d（应 %d），行3 顶 %d（应 %d）\n",
                           k_lh[k], tops[0], expect_top0, tops[1], expect_top1);
                    QL_CHECK_EQ(tops[0], expect_top0);
                    QL_CHECK_EQ(tops[1], expect_top1);
                    /* 两行之间差的就是 2 × line-height */
                    QL_CHECK_EQ(tops[1] - tops[0], 2 * k_lh[k]);
                    /* 光标高度 = 字形的 ascent+descent（不是字体行高） */
                    QL_CHECK_EQ(bots[0] - tops[0] + 1, (int)(font->ascent + font->descent));

                    ql_ui_free(ui);
                    ql_ui_doc_free(doc);
                }
            }
            ql_disp_mem_destroy(s_disp);
        }
    }

QL_TEST_END()
