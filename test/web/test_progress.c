/**
 * @file    test_progress.c
 * @brief   `<progress>` / `<meter>`：value/max → 0..1000 的完成度。
 *
 * 只存"多满"不存 value/max —— 布局与绘制都只需要这个数。所以这里的判据是
 * 三件事：属性折得对不对、set 的夹紧、以及"填充真的按比例变宽"（数像素）。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#define W 200
#define H 120

static QLDispDriver *s_disp;
static QLCanvas s_cv;

static QLUIRoot *mk(const char *html, const char *css, QLUIDoc **out_doc)
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
    *out_doc = doc;
    return ui;
}

/* 填充像素数（填充色 #0969DA 的蓝） */
static int blue_pixels(QLUIRoot *ui)
{
    ql_canvas_from_disp(&s_cv, s_disp);
    ql_canvas_clear(&s_cv, 0x000000);
    ql_ui_render_canvas(ui, &s_cv);
    int n = 0;
    for (ql_coord_t y = 0; y < H; y++)
        for (ql_coord_t x = 0; x < W; x++) {
            const ql_color_t c = ql_disp_mem_pixel(s_disp, x, y);
            const int r = QL_COLOR_GET_R(c), g = QL_COLOR_GET_G(c), b = QL_COLOR_GET_B(c);
            if (b > 0x80 && b > r + 0x40 && b > g + 0x30)
                n++;
        }
    return n;
}

static const char *k_css =
    "#r { width: 200px; height: 120px; background-color: #FFFFFF }"
    "progress, meter { width: 100px }";

QL_TEST_BEGIN("test_progress")

    s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
    QL_CHECK(s_disp != NULL);

    /* ---------- 1. value/max 折成完成度 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><progress value=\"70\" max=\"100\"></progress>"
                          "<meter value=\"1\" max=\"4\"></meter>"
                          "<progress></progress>"
                          "<progress value=\"200\" max=\"100\"></progress></div>",
                          k_css, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(ql_ui_progress_permille(ui, &ui->nodes[1]), 700);
            QL_CHECK_EQ(ql_ui_progress_permille(ui, &ui->nodes[2]), 250);
            QL_CHECK_EQ(ql_ui_progress_permille(ui, &ui->nodes[3]), -2);   /* 不确定 */
            QL_CHECK_EQ(ql_ui_progress_permille(ui, &ui->nodes[4]), 1000); /* 夹紧 */
            /* 不是控件的节点返回 -1 */
            QL_CHECK_EQ(ql_ui_progress_permille(ui, &ui->nodes[0]), -1);
            /* 那串数字只是运输工具，build 之后就清掉了 */
            for (uint16_t i = 0; i < ui->node_count; i++)
                QL_CHECK(ui->nodes[i].text == NULL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 2. 没有 max 时按 100 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><progress value=\"25\"></progress></div>", k_css,
                          &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ(ql_ui_progress_permille(ui, &ui->nodes[1]), 250);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 3. set：换算与夹紧 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><meter value=\"0\" max=\"10\"></meter></div>",
                          k_css, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *m = &ui->nodes[1];
            QL_CHECK_EQ(ql_ui_progress_set(ui, m, 5, 10), QL_OK);
            QL_CHECK_EQ(ql_ui_progress_permille(ui, m), 500);
            QL_CHECK_EQ(ql_ui_progress_set(ui, m, -3, 10), QL_OK);   /* 负数夹到 0 */
            QL_CHECK_EQ(ql_ui_progress_permille(ui, m), 0);
            QL_CHECK_EQ(ql_ui_progress_set(ui, m, 999, 10), QL_OK);  /* 超过 max */
            QL_CHECK_EQ(ql_ui_progress_permille(ui, m), 1000);
            QL_CHECK_EQ(ql_ui_progress_set(ui, m, 1, 0), QL_OK);     /* max<=0 视为 100 */
            QL_CHECK_EQ(ql_ui_progress_permille(ui, m), 10);
            QL_CHECK_EQ(ql_ui_progress_set(ui, &ui->nodes[0], 1, 2), QL_ERR_INVAL);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 4. 填充真的按比例变宽（数像素） ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><progress value=\"0\" max=\"100\"></progress>"
                          "</div>", k_css, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QLUINodeRT *p = &ui->nodes[1];
            (void)ql_ui_progress_set(ui, p, 0, 100);
            const int zero = blue_pixels(ui);
            (void)ql_ui_progress_set(ui, p, 50, 100);
            const int half = blue_pixels(ui);
            (void)ql_ui_progress_set(ui, p, 100, 100);
            const int full = blue_pixels(ui);
            QL_CHECK(half > zero + 100);
            QL_CHECK(full > half + 100);
            /* 半个大致就是一半（允许 15% 误差：填充是整像素矩形） */
            QL_CHECK(half * 2 > full * 85 / 100);
            QL_CHECK(half * 2 < full * 115 / 100);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 5. 不确定进度：画固定 1/3 ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><progress></progress></div>", k_css, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK(ql_ui_progress_permille(ui, &ui->nodes[1]) == -2);
            const int ink = blue_pixels(ui);
            QL_CHECK(ink > 100);          /* 有东西，但不是满的 */
            QL_CHECK(ink < 100 * 8);      /* 100px 宽 × 8px 高 = 满格 800 */
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 6. output / datalist ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><output>42</output>"
                          "<datalist><option>a</option></datalist></div>",
                          k_css, &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK_EQ((int)ui->nodes[1].style->display, (int)QL_UI_DISPLAY_INLINE);
            QL_CHECK_STR(ui->nodes[1].text, "42");
            QL_CHECK_EQ((int)ui->nodes[2].style->display, (int)QL_UI_DISPLAY_NONE);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    /* ---------- 7. 没有 progress/meter 时池是 NULL ---------- */
    {
        QLUIDoc *doc = NULL;
        QLUIRoot *ui = mk("<div id=r><span>x</span></div>", "", &doc);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            QL_CHECK(ui->progresses == NULL);
            QL_CHECK_EQ((int)ui->progress_count, 0);
            ql_ui_free(ui);
            ql_ui_doc_free(doc);
        }
    }

    ql_disp_mem_destroy(s_disp);

QL_TEST_END()
