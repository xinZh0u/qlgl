/**
 * @file    test_dialog.c
 * @brief   `<dialog>`：浮层、遮罩、点外面 / `Esc` 关闭。
 *
 * 浮层是"整棵树画完之后再画一遍"的画法，所以最要紧的两条判据是：
 *   1. 遮罩真的盖住**下面**的内容（含 `ql_ui_render_rect` 只刷脏区时）；
 *   2. 点遮罩被吃掉（不穿透到下面的节点）。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#define W 240
#define H 200

static QLDispDriver *s_disp;
static QLCanvas s_cv;

static const char *k_html =
    "<div id=r><div id=bg>背景</div>"
    "<dialog id=m><div>对话框正文</div></dialog></div>";

static const char *k_css =
    "#r { width: 240px; height: 200px; background-color: #FFFFFF }"
    "#bg { width: 240px; height: 200px; background-color: #FFFFFF }"
    "dialog div { line-height: 20px }";

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

static ql_color_t pixel_at(ql_coord_t x, ql_coord_t y)
{
    return ql_disp_mem_pixel(s_disp, x, y);
}

/* 全量渲染：清成纯白再画 */
static void render_all(QLUIRoot *ui)
{
    ql_canvas_from_disp(&s_cv, s_disp);
    ql_canvas_clear(&s_cv, 0xFFFFFF);
    ql_ui_render_canvas(ui, &s_cv);
}

static void send_key(QLUIRoot *ui, uint32_t key)
{
    QLInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = QL_INPUT_KEY_DOWN;
    ev.key = key;
    ql_ui_input_event(ui, &ev);
}

QL_TEST_BEGIN("test_dialog")

    s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
    QL_CHECK(s_disp != NULL);

    /* ---------- 1. 默认不显示 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(k_html, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *dlg = &ui->nodes[2];
            QL_CHECK_EQ(ql_ui_dialog_open(ui, dlg), 0);
            QL_CHECK_EQ((int)dlg->style->display, (int)QL_UI_DISPLAY_NONE);
            /* 不是 <dialog> 的节点 */
            QL_CHECK_EQ(ql_ui_dialog_open(ui, &ui->nodes[0]), -1);
            QL_CHECK_EQ(ql_ui_dialog_set_open(ui, &ui->nodes[0], 1), QL_ERR_INVAL);
            QL_CHECK_EQ(ql_ui_dialog_close_top(ui), 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 2. open 属性 + 居中 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><dialog open><div>x</div></dialog></div>", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *dlg = &ui->nodes[1];
            QL_CHECK_EQ(ql_ui_dialog_open(ui, dlg), 1);
            const QLRect b = ql_ui_node_rect(dlg);
            QL_CHECK(b.w > 100);                       /* 70% 宽，不是满屏 */
            QL_CHECK(b.w < W);
            /* 中心对齐（允许 ±2px） */
            const int cx = (int)(b.x + b.w / 2);
            const int cy = (int)(b.y + b.h / 2);
            QL_CHECK(cx > W / 2 - 3 && cx < W / 2 + 3);
            QL_CHECK(cy > H / 2 - 3 && cy < H / 2 + 3);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 3. 遮罩：打开后远处变暗 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(k_html, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *dlg = &ui->nodes[2];
            render_all(ui);
            const int before = (int)QL_COLOR_GET_R(pixel_at(2, 2));
            QL_CHECK(before > 0xE0);                   /* 关闭时是白的 */

            QL_CHECK_EQ(ql_ui_dialog_set_open(ui, dlg, 1), QL_OK);
            render_all(ui);
            const int after = (int)QL_COLOR_GET_R(pixel_at(2, 2));
            QL_CHECK(after < before - 40);             /* 被遮罩压暗 */
            QL_CHECK(after > 0x40);                    /* 压暗不是涂黑 */

            /* 对话框自己是白的（不透明） */
            const QLRect b = ql_ui_node_rect(dlg);
            QL_CHECK(QL_COLOR_GET_R(pixel_at(b.x + b.w / 2, b.y + b.h / 2)) > 0xE0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 4. 只刷脏区时遮罩也得到位 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(k_html, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *dlg = &ui->nodes[2];
            QL_CHECK_EQ(ql_ui_dialog_set_open(ui, dlg, 1), QL_OK);
            render_all(ui);

            /* 脏区取左上角一块，离对话框很远 */
            ql_canvas_clear(&s_cv, 0xFFFFFF);
            QLDraw2D d2d;
            QL_CHECK_EQ(ql_draw2d_canvas_init(&d2d, &s_cv), QL_OK);
            ui->root_clip_valid = 0;
            ql_ui_render_rect(ui, &d2d, ql_rect_make(0, 0, 20, 20));
            QL_CHECK(QL_COLOR_GET_R(pixel_at(2, 2)) < 0xC0);   /* 遮罩画到了 */
            /* 脏区外保持原样（我们刚清白了它） */
            QL_CHECK(QL_COLOR_GET_R(pixel_at(W - 2, H - 2)) > 0xC0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 5. 点遮罩关掉，且不穿透 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(k_html, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *dlg = &ui->nodes[2];
            QL_CHECK_EQ(ql_ui_dialog_set_open(ui, dlg, 1), QL_OK);
            QL_CHECK_EQ(ql_ui_pointer_down(ui, 2, 2), 1);   /* 吃掉了这次点击 */
            QL_CHECK_EQ(ql_ui_dialog_open(ui, dlg), 0);

            /* 点对话框内部：不关 */
            QL_CHECK_EQ(ql_ui_dialog_set_open(ui, dlg, 1), QL_OK);
            const QLRect b = ql_ui_node_rect(dlg);
            (void)ql_ui_pointer_down(ui, b.x + b.w / 2, b.y + b.h / 2);
            (void)ql_ui_pointer_up(ui, b.x + b.w / 2, b.y + b.h / 2);
            QL_CHECK_EQ(ql_ui_dialog_open(ui, dlg), 1);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 6. Esc 关掉最上层 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(k_html, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *dlg = &ui->nodes[2];
            QL_CHECK_EQ(ql_ui_dialog_set_open(ui, dlg, 1), QL_OK);
            send_key(ui, QL_KEY_ESCAPE);
            QL_CHECK_EQ(ql_ui_dialog_open(ui, dlg), 0);
            /* 没开的再按 Esc：什么也不发生 */
            send_key(ui, QL_KEY_ESCAPE);
            QL_CHECK_EQ(ql_ui_dialog_open(ui, dlg), 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 7. 打开时 paint_bounds 是整个视口 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk(k_html, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const QLRect before = ql_ui_paint_bounds(ui);
            QL_CHECK_EQ((int)before.w, W);
            QL_CHECK_EQ(ql_ui_dialog_set_open(ui, &ui->nodes[2], 1), QL_OK);
            const QLRect after = ql_ui_paint_bounds(ui);
            QL_CHECK_EQ((int)after.x, 0);
            QL_CHECK_EQ((int)after.y, 0);
            QL_CHECK_EQ((int)after.w, W);
            QL_CHECK_EQ((int)after.h, H);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 8. 没有 <dialog> 的界面：整段逻辑跳过 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><span>x</span></div>", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)ui->dialog_count, 0);
            QL_CHECK_EQ(ql_ui_dialog_close_top(ui), 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    ql_disp_mem_destroy(s_disp);

QL_TEST_END()
