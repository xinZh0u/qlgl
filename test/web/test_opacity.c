/**
 * @file    test_opacity.c
 * @brief   `opacity`：元素整体不透明度（背景/边框/阴影/文字/图片一起淡出）。
 *
 * 五条契约：
 *   1. 构建期解析成 0..255（`opacity: 0.5` → 128 左右），越界报错；
 *   2. `opacity: 1` 与不写完全等价（逐像素相同）；
 *   3. `opacity: 0` 整棵子树不画；0.5 与底色的混合结果落在中间；
 *   4. **顺着子树往下乘**：父级 0.5 + 子级 0.5 = 0.25；
 *   5. 文字与边框也一起淡（不是只淡背景那种"半个正确"）。
 *
 * 语义差别写在 QLUIStyle.opacity 的注释里：这是"逐图元降透明"，
 * 严格等价于 CSS 的 group opacity 需要离屏图层（尚未做）。
 */
#include "../ql_test.h"

#if defined(QLGL_ENABLE_GL)
#include <qlgl/qlgfx_gl.h>
#endif

#define W 120
#define H 80
#define BG 0xFFFFFFu      /* 白底 */

static QLDispDriver *s_disp;
static QLCanvas s_canvas;
static QLUIDoc *s_doc;      /* build 不接管所有权：先 release(ui) 再放 doc */

/* 画一份界面到白底画布，返回 disp 供查询 */
static QLUIRoot *render(const char *html, const char *css)
{
    QLUIDoc *doc = ql_ui_load_text(html, css);
    if (doc == NULL) {
        printf("  编译失败：%s\n", ql_ui_text_last_error());
        return NULL;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    /*
     * **不能在这里放 doc**：`ql_ui_build` 只是引用它（ql_ui_free 之后才能
     * ql_ui_doc_free），节点文本、字体表都还指着 doc 的内存。
     */
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return NULL;
    }
    s_doc = doc;
    ql_ui_layout(ui, W, H);
    if (s_disp == NULL)
        s_disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
    ql_canvas_from_disp(&s_canvas, s_disp);
    ql_canvas_clear(&s_canvas, BG);
    ql_ui_render_canvas(ui, &s_canvas);
    return ui;
}

/* 释放一份界面：先运行期树，再 doc（所有权约定见 QLUI API 手册） */
static void release(QLUIRoot *ui)
{
    ql_ui_free(ui);
    if (s_doc != NULL) {
        ql_ui_doc_free(s_doc);
        s_doc = NULL;
    }
}

static ql_color_t px(int x, int y)
{
    return ql_disp_mem_pixel(s_disp, x, y);
}

/* 单通道差 */
static int near(ql_color_t a, ql_color_t b, int tol)
{
    int dr = (int)QL_COLOR_GET_R(a) - (int)QL_COLOR_GET_R(b);
    int dg = (int)QL_COLOR_GET_G(a) - (int)QL_COLOR_GET_G(b);
    int db = (int)QL_COLOR_GET_B(a) - (int)QL_COLOR_GET_B(b);
    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (db < 0) db = -db;
    return dr <= tol && dg <= tol && db <= tol;
}

QL_TEST_BEGIN("test_opacity")

    const char *html = "<div id=r><div id=b></div></div>";
    /* 纯蓝块，便于算混合 */
    const ql_color_t blue = QL_COLOR_RGB(0x00, 0x00, 0xFF);

    /* ---------- 1. 不写 opacity：原色 ---------- */
    QLUIRoot *ui = render(html, "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                                "#b { position: absolute; left: 20px; top: 20px;"
                                "     width: 40px; height: 40px;"
                                "     background-color: #0000FF }");
    QL_CHECK(ui != NULL);
    const ql_color_t plain = px(40, 40);
    QL_CHECK(near(plain, blue, 2));
    release(ui);

    /* ---------- 2. opacity: 1 与不写逐像素相同 ---------- */
    ui = render(html, "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                      "#b { position: absolute; left: 20px; top: 20px;"
                      "     width: 40px; height: 40px; background-color: #0000FF;"
                      "     opacity: 1 }");
    QL_CHECK(ui != NULL);
    QL_CHECK_EQ(px(40, 40), plain);
    release(ui);

    /* ---------- 3. opacity: 0.5 → 与白底各半 ---------- */
    ui = render(html, "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                      "#b { position: absolute; left: 20px; top: 20px;"
                      "     width: 40px; height: 40px; background-color: #0000FF;"
                      "     opacity: 0.5 }");
    QL_CHECK(ui != NULL);
    {
        const ql_color_t half = px(40, 40);
        printf("  opacity:0.5 的蓝块 → %06X（红/绿抬到 ≈128，蓝仍 255）\n",
               (unsigned)half);
        QL_CHECK(QL_COLOR_GET_R(half) > 110 && QL_COLOR_GET_R(half) < 145);
        QL_CHECK(QL_COLOR_GET_G(half) > 110 && QL_COLOR_GET_G(half) < 145);
        QL_CHECK_EQ(QL_COLOR_GET_B(half), 0xFFu);
    }
    release(ui);

    /* ---------- 4. opacity: 0 → 什么都没画 ---------- */
    ui = render(html, "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                      "#b { position: absolute; left: 20px; top: 20px;"
                      "     width: 40px; height: 40px; background-color: #0000FF;"
                      "     opacity: 0 }");
    QL_CHECK(ui != NULL);
    QL_CHECK_EQ(px(40, 40), QL_COLOR_RGB(0xFF, 0xFF, 0xFF));
    release(ui);

    /* ---------- 5. 顺着子树往下乘：0.5 × 0.5 = 0.25 ---------- */
    ui = render("<div id=r><div id=p><div id=c></div></div></div>",
                "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
                "#p { position: absolute; left: 10px; top: 10px; width: 60px;"
                "     height: 60px; opacity: 0.5 }"
                "#c { width: 100%; height: 100%; background-color: #0000FF;"
                "     opacity: 0.5 }");
    QL_CHECK(ui != NULL);
    {
        const ql_color_t q = px(40, 40);
        printf("  父 0.5 × 子 0.5 的蓝块 → 红 %d（应 ≈ 191）\n",
               (int)QL_COLOR_GET_R(q));
        QL_CHECK(QL_COLOR_GET_R(q) > 170 && QL_COLOR_GET_R(q) < 210);
    }
    release(ui);

    /* ---------- 6. 文字与边框也一起淡（不是只淡背景） ---------- */
    {
        const char *t_html = "<div id=r><div id=t>HHHH</div></div>";
        const char *t_css =
            "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
            "#t { position: absolute; left: 4px; top: 4px; width: 110px;"
            "     height: 60px; font-size: 16px; color: #000000;"
            "     border-width: 2px; border-color: #000000";
        char css_op[512];

        /* 基准：不透明 */
        snprintf(css_op, sizeof(css_op), "%s }", t_css);
        QLUIRoot *base = render(t_html, css_op);
        QL_CHECK(base != NULL);
        int ink_dark = 0;
        for (int y = 10; y < 26; y++)
            for (int x = 8; x < 70; x++)
                if (QL_COLOR_GET_R(px(x, y)) < 60)
                    ink_dark++;
        const ql_color_t border_base = px(4, 30);
        release(base);
        printf("  不透明时：深色墨迹 %d 个，左边框 %06X\n", ink_dark,
               (unsigned)border_base);
        QL_CHECK(ink_dark > 0);
        QL_CHECK(QL_COLOR_GET_R(border_base) < 40);

        /* 加了 opacity: 0.4 */
        snprintf(css_op, sizeof(css_op), "%s; opacity: 0.4 }", t_css);
        QLUIRoot *faded = render(t_html, css_op);
        QL_CHECK(faded != NULL);
        int ink_dark2 = 0;
        for (int y = 10; y < 26; y++)
            for (int x = 8; x < 70; x++)
                if (QL_COLOR_GET_R(px(x, y)) < 60)
                    ink_dark2++;
        const ql_color_t border_faded = px(4, 30);
        printf("  opacity:0.4 时：深色墨迹 %d 个（应接近 0），左边框 %06X\n",
               ink_dark2, (unsigned)border_faded);
        QL_CHECK(ink_dark2 < ink_dark);
        /* 边框从近黑淡成中灰：R 通道应明显抬高 */
        QL_CHECK(QL_COLOR_GET_R(border_faded) > QL_COLOR_GET_R(border_base) + 40);
        release(faded);
    }

    /*
     * ---------- 7. GPU 侧同样要淡（两条后端语义一致） ----------
     * 只做性质断言：白底上的蓝块淡成 0.5 之后，红通道应落在中间值附近。
     */
#if defined(QLGL_ENABLE_GL)
    {
        QLUIDoc *doc = ql_ui_load_text(html,
            "#r { width: 100%; height: 100%; background-color: #FFFFFF }"
            "#b { position: absolute; left: 20px; top: 20px; width: 40px;"
            "     height: 40px; background-color: #0000FF; opacity: 0.5 }");
        QL_CHECK(doc != NULL);
        if (doc != NULL) {
            QLUIRoot *gu = ql_ui_build(doc);
            QL_CHECK(gu != NULL);
            if (gu != NULL) {
                ql_ui_layout(gu, W, H);
                QLGfxGL *gfx = ql_gfx_gl_create("opacity", W, H,
                                                QL_GFX_GL_OFFSCREEN);
                if (gfx == NULL) {
                    printf("  [SKIP] GL：无法创建上下文\n");
                } else {
                    ql_gfx_gl_begin(gfx, QL_COLOR_RGB(0xFF, 0xFF, 0xFF));
                    ql_ui_render(gu, ql_gfx_gl_draw(gfx));
                    ql_gfx_gl_end(gfx);
                    ql_gfx_gl_finish(gfx);
                    uint8_t *rgba = (uint8_t *)ql_mem_calloc(1, (size_t)W * H * 4u);
                    QL_CHECK(rgba != NULL);
                    if (rgba != NULL) {
                        QL_CHECK_EQ(ql_gfx_gl_read_pixels(gfx, rgba, W * 4), QL_OK);
                        const uint8_t *p = rgba + ((size_t)40 * W + 40u) * 4u;
                        printf("  GPU 侧 opacity:0.5 的蓝块 → R=%d G=%d B=%d\n",
                               p[0], p[1], p[2]);
                        QL_CHECK(p[0] > 100 && p[0] < 160);
                        QL_CHECK(p[2] > 240);
                        ql_mem_free(rgba);
                    }
                    ql_gfx_gl_destroy(gfx);
                }
                ql_ui_free(gu);
            }
            ql_ui_doc_free(doc);
        }
    }
#endif

    if (s_disp != NULL) {
        ql_disp_mem_destroy(s_disp);
        s_disp = NULL;
    }

QL_TEST_END()
