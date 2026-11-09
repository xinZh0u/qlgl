/**
 * @file    test_clipboard.c
 * @brief   剪贴板：Ctrl+C/X/V 与 `QLUIClipboard` 后端接口（含异步粘贴）。
 *
 * 平台层的 X11 实现是异步的（X11 剪贴板是点对点协议，内容下一帧才到），
 * 所以这里用一个**假后端**把两条路都测到：
 *   1. 同步返回内容（后端立刻有）；
 *   2. 第一次返回 0、第二次才给（模拟 SelectionNotify 晚到）。
 */
#include <stdio.h>
#include <string.h>

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

/* --- 假后端 --- */
static char s_clip[256];
static int  s_clip_ready;      /* 1 = 立刻给内容；0 = 第一次催还没到 */
static int  s_set_calls;

static void fake_set(const char *utf8, void *user)
{
    (void)user;
    s_set_calls++;
    snprintf(s_clip, sizeof(s_clip), "%s", utf8 != NULL ? utf8 : "");
}

static int fake_get(char *buf, size_t cap, void *user)
{
    (void)user;
    if (!s_clip_ready)
        return 0;              /* 模拟"内容还在路上" */
    const size_t n = strlen(s_clip);
    const size_t k = (n < cap - 1u) ? n : cap - 1u;
    memcpy(buf, s_clip, k);
    buf[k] = '\0';
    return 1;
}

static QLUIClipboard s_cb = { fake_set, fake_get };

static void key(QLUIRoot *ui, uint32_t k, uint8_t mods)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_KEY_DOWN;
    ev.key = k;
    ev.mods = mods;
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
}

QL_TEST_BEGIN("test_clipboard")

    const char *html = "<div id=r><input value=\"hello\"></div>";
    const char *css = "#r { width: 300px; height: 200px }"
                      "input { width: 240px; height: 24px; padding: 2px }";

    /* ---------- 1. 没装后端：快捷键什么也不做，也不报错 ---------- */
    {
        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                QLUINodeRT *in = find_tag(ui, "input", 0);
                click(ui, in->content_box.x + 2, in->content_box.y + 2);
                key(ui, 'c', QL_MOD_CTRL);
                key(ui, 'v', QL_MOD_CTRL);
                QL_CHECK(strcmp(ql_ui_input_value(ui, in), "hello") == 0);
                QL_CHECK_EQ((int)ql_ui_clipboard_copy(ui), (int)QL_ERR_UNSUPPORTED);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 2. 复制全文（没有选区时） ---------- */
    {
        s_set_calls = 0;
        s_clip[0] = '\0';
        s_clip_ready = 1;
        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                ql_ui_set_clipboard(ui, &s_cb, NULL);
                QLUINodeRT *in = find_tag(ui, "input", 0);
                click(ui, in->content_box.x + 2, in->content_box.y + 2);
                key(ui, 'c', QL_MOD_CTRL);
                QL_CHECK_EQ(s_set_calls, 1);
                QL_CHECK(strcmp(s_clip, "hello") == 0);
                if (s_set_calls == 1)
                    printf("    复制到剪贴板 = [%s]\n", s_clip);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 3. 剪切：复制 + 删掉 ---------- */
    {
        s_set_calls = 0;
        s_clip[0] = '\0';
        s_clip_ready = 1;
        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                ql_ui_set_clipboard(ui, &s_cb, NULL);
                QLUINodeRT *in = find_tag(ui, "input", 0);
                click(ui, in->content_box.x + 2, in->content_box.y + 2);
                (void)ql_ui_input_set_value(ui, in, "abc");
                /* 全选不了（没有 Ctrl+A），用"没有选区就复制全文"的语义：
                 * 剪切在没有选区时只复制、不清空 —— 与"全选再剪切"区分开 */
                key(ui, 'x', QL_MOD_CTRL);
                QL_CHECK(strcmp(s_clip, "abc") == 0);
                QL_CHECK(strcmp(ql_ui_input_value(ui, in), "abc") == 0);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 4. 粘贴（同步到达） ---------- */
    {
        s_clip_ready = 1;
        snprintf(s_clip, sizeof(s_clip), "XYZ");
        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                ql_ui_set_clipboard(ui, &s_cb, NULL);
                QLUINodeRT *in = find_tag(ui, "input", 0);
                click(ui, in->content_box.x + 2, in->content_box.y + 2);
                key(ui, 'v', QL_MOD_CTRL);
                QL_CHECK(strcmp(ql_ui_input_value(ui, in), "XYZhello") == 0);
                if (ql_ui_input_value(ui, in) != NULL)
                    printf("    粘贴后 = [%s]\n", ql_ui_input_value(ui, in));
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 5. 粘贴（异步：第一帧没到，anim_tick 之后才落地） ---------- */
    {
        s_clip_ready = 0;
        snprintf(s_clip, sizeof(s_clip), "异步");
        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                ql_ui_set_clipboard(ui, &s_cb, NULL);
                QLUINodeRT *in = find_tag(ui, "input", 0);
                click(ui, in->content_box.x + 2, in->content_box.y + 2);
                key(ui, 'v', QL_MOD_CTRL);
                /* 还没到：值不变，但请求已经记下了 */
                QL_CHECK(strcmp(ql_ui_input_value(ui, in), "hello") == 0);
                s_clip_ready = 1;              /* 内容到了 */
                (void)ql_ui_anim_tick(ui, 0);  /* 主循环每帧都会调 */
                QL_CHECK(strcmp(ql_ui_input_value(ui, in), "异步hello") == 0);
                if (ql_ui_input_value(ui, in) != NULL)
                    printf("    异步粘贴后 = [%s]\n", ql_ui_input_value(ui, in));
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 6. 没有焦点：快捷键不崩、也不动任何东西 ---------- */
    {
        s_clip_ready = 1;
        snprintf(s_clip, sizeof(s_clip), "Q");
        QLUIDoc *doc = ql_ui_load_text(html, css);
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *ui = ql_ui_build(doc);
            QL_CHECK(ui != NULL);
            if (ui != NULL) {
                ql_ui_layout(ui, W, H);
                ql_ui_set_clipboard(ui, &s_cb, NULL);
                QL_CHECK(ql_ui_focus_node(ui) == NULL);
                key(ui, 'v', QL_MOD_CTRL);
                key(ui, 'c', QL_MOD_CTRL);
                QLUINodeRT *in = find_tag(ui, "input", 0);
                QL_CHECK(strcmp(ql_ui_input_value(ui, in), "hello") == 0);
                ql_ui_free(ui);
            }
            ql_ui_doc_free(doc);
        }
    }

QL_TEST_END()
