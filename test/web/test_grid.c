/**
 * @file    test_grid.c
 * @brief   B6：CSS Grid 布局 + `border-image` 九宫格。
 *
 * Grid 是**布局**能力，所以断言基本都落在"盒子在哪、多大"上：fr 的分配、
 * `repeat()` 展开、`minmax()` 的上下限、自动放置换行、跨格、具名区域、
 * 对齐、隐式轨道、负线号。另有若干"必须报错"的用例把边界钉死
 * （具名线、`repeat(auto-fill)`、区域不是矩形、`grid-column` 用在非 grid 父级上）。
 *
 * `border-image` 是纯绘制，所以验到**像素**：四角/四边/中间三处颜色。
 */
#include <stdlib.h>
#include <string.h>

#include "../ql_test.h"

#define CW 320
#define CH 240

typedef struct { int x, y, w, h; } Box;

static QLUIDoc *s_doc;
static QLUIRoot *s_ui;
static QLDispDriver *s_disp;
static QLCanvas s_canvas;

/* 编译 + 布局；返回 root（失败返回 NULL） */
static QLUIRoot *lay(const char *html, const char *css, int vw, int vh)
{
    QLUIDoc *doc = ql_ui_load_text(html, css);
    if (doc == NULL)
        return NULL;
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        return NULL;
    }
    s_doc = doc;
    s_ui = ui;
    ql_ui_layout(ui, (ql_coord_t)vw, (ql_coord_t)vh);
    return ui;
}

static void drop(void)
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

/* 按文档顺序取节点盒子 */
static Box box_of(QLUIRoot *ui, int idx)
{
    Box b = { 0, 0, 0, 0 };
    if (ui == NULL || idx < 0 || idx >= (int)ui->node_count)
        return b;
    const QLRect r = ql_ui_node_rect(&ui->nodes[idx]);
    b.x = r.x;
    b.y = r.y;
    b.w = r.w;
    b.h = r.h;
    return b;
}

static int rejects(const char *html, const char *css, const char *needle)
{
    QLUIDoc *doc = ql_ui_load_text(html, css);
    if (doc != NULL) {
        ql_ui_doc_free(doc);
        printf("  本该报错却编译通过了：%s\n", css);
        return 0;
    }
    const char *msg = ql_ui_text_last_error();
    if (msg == NULL || strstr(msg, needle) == NULL) {
        printf("  报错文案不含 '%s'：%s\n", needle, msg ? msg : "(null)");
        return 0;
    }
    return 1;
}

/* 渲染一张图（border-image 用） */
static int render_int(const char *html, const char *css, int vw, int vh)
{
    if (lay(html, css, vw, vh) == NULL)
        return 0;
    if (s_disp == NULL)
        s_disp = ql_disp_mem_create(vw, vh, QL_COLOR_RGB888);
    ql_canvas_from_disp(&s_canvas, s_disp);
    ql_canvas_clear(&s_canvas, 0x000000);
    ql_ui_render_canvas(s_ui, &s_canvas);
    return 1;
}

static ql_color_t px(int x, int y)
{
    return ql_disp_mem_pixel(s_disp, x, y);
}

static int near(ql_color_t c, int r, int g, int b, int tol)
{
    const int dr = (int)QL_COLOR_GET_R(c) - r;
    const int dg = (int)QL_COLOR_GET_G(c) - g;
    const int db = (int)QL_COLOR_GET_B(c) - b;
    return dr >= -tol && dr <= tol && dg >= -tol && dg <= tol && db >= -tol &&
           db <= tol;
}

QL_TEST_BEGIN("test_grid")

    /* ============ 1. fr 分配：1fr 2fr 在 300 宽里 = 100 / 200 ============ */
    {
        QLUIRoot *ui = lay("<div id=r><div id=g>"
                           "<div id=a></div><div id=b></div></div></div>",
                           "#r { width: 300px; }"
                           "#g { display: grid; grid-template-columns: 1fr 2fr; }"
                           "#a, #b { height: 10px; }",
                           320, 240);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const Box a = box_of(ui, 2);
            const Box b = box_of(ui, 3);
            QL_CHECK_EQ(a.x, 0);
            QL_CHECK_EQ(a.w, 100);
            QL_CHECK_EQ(b.x, 100);
            QL_CHECK_EQ(b.w, 200);
        }
        drop();
    }

    /* ============ 2. 固定 + gap：两列 80/1fr，gap 10 ============ */
    {
        QLUIRoot *ui = lay("<div id=r><div id=g>"
                           "<div id=a></div><div id=b></div></div></div>",
                           "#r { width: 300px; }"
                           "#g { display: grid; grid-template-columns: 80px 1fr;"
                           "     column-gap: 10px; }"
                           "#a, #b { height: 10px; }",
                           320, 240);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const Box a = box_of(ui, 2);
            const Box b = box_of(ui, 3);
            QL_CHECK_EQ(a.w, 80);
            QL_CHECK_EQ(b.x, 90);
            QL_CHECK_EQ(b.w, 210);
        }
        drop();
    }

    /* ============ 3. repeat(3, 1fr) + 自动换行（第 4 项落到第二行） ============ */
    {
        QLUIRoot *ui = lay("<div id=r><div id=g>"
                           "<div class=c></div><div class=c></div><div class=c></div>"
                           "<div class=c></div></div></div>",
                           "#r { width: 300px; }"
                           "#g { display: grid; grid-template-columns: repeat(3, 1fr);"
                           "     row-gap: 5px; }"
                           ".c { height: 20px; }",
                           320, 240);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const Box c0 = box_of(ui, 2);
            const Box c3 = box_of(ui, 5);
            QL_CHECK_EQ(c0.w, 100);
            QL_CHECK_EQ(c3.w, 100);
            QL_CHECK_EQ(c3.x, 0);
            QL_CHECK_EQ(c3.y, 25);      /* 20 + 5 行间距 */
            const Box gbox = box_of(ui, 1);
            QL_CHECK_EQ(gbox.h, 45);    /* 两行 20 + 5 */
        }
        drop();
    }

    /* ============ 4. minmax(60px, auto) 与百分比列 ============ */
    {
        QLUIRoot *ui = lay("<div id=r><div id=g>"
                           "<div class=c>x</div><div class=c>y</div>"
                           "<div class=c>z</div></div></div>",
                           "#r { width: 400px; }"
                           "#g { display: grid;"
                           "     grid-template-columns: minmax(60px, auto) 1fr 25%; }"
                           ".c { height: 10px; }",
                           420, 240);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const Box a = box_of(ui, 2);
            const Box b = box_of(ui, 3);
            const Box c = box_of(ui, 4);
            QL_CHECK(a.w >= 60);        /* minmax 的下限生效 */
            QL_CHECK_EQ(c.w, 100);      /* 25% × 400 */
            QL_CHECK_EQ(b.x, a.w);
            QL_CHECK_EQ(b.w, 400 - a.w - 100);
        }
        drop();
    }

    /* ============ 5. auto 列取内容宽度 ============ */
    {
        QLUIRoot *ui = lay("<div id=r><div id=g>"
                           "<div id=a>AB</div><div id=b>CD</div></div></div>",
                           "#r { width: 300px; }"
                           "#g { display: grid; grid-template-columns: auto 1fr; }",
                           320, 240);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const Box a = box_of(ui, 2);
            const Box b = box_of(ui, 3);
            QL_CHECK(a.w > 0);
            QL_CHECK(a.w < 100);        /* 两个字母宽，不是撑满 */
            QL_CHECK_EQ(b.x, a.w);
            QL_CHECK_EQ(a.w + b.w, 300);
        }
        drop();
    }

    /* ============ 6. grid-column: 1 / span 2 跨列 ============ */
    {
        QLUIRoot *ui = lay("<div id=r><div id=g>"
                           "<div id=a></div><div class=c></div>"
                           "<div class=c></div></div></div>",
                           "#r { width: 300px; }"
                           "#g { display: grid; grid-template-columns: 1fr 1fr;"
                           "     column-gap: 0px; }"
                           "#a { grid-column: 1 / span 2; height: 12px; }"
                           ".c { height: 10px; }",
                           320, 240);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const Box a = box_of(ui, 2);
            QL_CHECK_EQ(a.w, 300);
            QL_CHECK_EQ(a.h, 12);
        }
        drop();
    }

    /* ============ 7. grid-row: span 2 把高度摊到两行 ============ */
    {
        QLUIRoot *ui = lay("<div id=r><div id=g>"
                           "<div id=tall></div><div class=c></div>"
                           "<div class=c></div></div></div>",
                           "#r { width: 300px; }"
                           "#g { display: grid; grid-template-columns: 1fr 1fr; }"
                           "#tall { grid-row: span 2; height: 60px; }"
                           ".c { height: 20px; }",
                           320, 240);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const Box t = box_of(ui, 2);
            QL_CHECK_EQ(t.h, 60);       /* 跨两行，拉伸到两行总和 */
            const Box gbox = box_of(ui, 1);
            QL_CHECK_EQ(gbox.h, 60);    /* 行高被摊成 30 + 30 */
        }
        drop();
    }

    /* ============ 8. grid-template-areas + grid-area: 名字 ============ */
    {
        QLUIRoot *ui = lay("<div id=r><div id=g>"
                           "<div id=hd>h</div><div id=sb>s</div>"
                           "<div id=mn>m</div><div id=ft>f</div></div></div>",
                           "#r { width: 200px; }"
                           "#g { display: grid;"
                           "     grid-template-areas: \"hd hd\" \"sb mn\" \"ft ft\";"
                           "     grid-template-columns: 60px 1fr;"
                           "     grid-template-rows: 10px 40px 10px; }"
                           "#hd { grid-area: hd; }"
                           "#sb { grid-area: sb; }"
                           "#mn { grid-area: mn; }"
                           "#ft { grid-area: ft; }",
                           220, 240);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const Box hd = box_of(ui, 2);
            const Box sb = box_of(ui, 3);
            const Box mn = box_of(ui, 4);
            const Box ft = box_of(ui, 5);
            QL_CHECK_EQ(hd.w, 200);     /* 跨两列 */
            QL_CHECK_EQ(hd.h, 10);
            QL_CHECK_EQ(sb.x, 0);
            QL_CHECK_EQ(sb.y, 10);
            QL_CHECK_EQ(sb.w, 60);
            QL_CHECK_EQ(mn.x, 60);
            QL_CHECK_EQ(mn.w, 140);
            QL_CHECK_EQ(mn.h, 40);
            QL_CHECK_EQ(ft.y, 50);
            QL_CHECK_EQ(ft.w, 200);
        }
        drop();
    }

    /* ============ 9. 负线号 -1 = 最后一条线 ============ */
    {
        QLUIRoot *ui = lay("<div id=r><div id=g>"
                           "<div class=c></div><div id=last></div></div></div>",
                           "#r { width: 300px; }"
                           "#g { display: grid; grid-template-columns: 1fr 1fr 1fr; }"
                           "#last { grid-column: -2 / -1; height: 5px; }"
                           ".c { height: 5px; }",
                           320, 240);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const Box last = box_of(ui, 3);
            QL_CHECK_EQ(last.x, 200);   /* 第三列 */
            QL_CHECK_EQ(last.w, 100);
        }
        drop();
    }

    /* ============ 10. justify-items / align-items ============ */
    {
        QLUIRoot *ui = lay("<div id=r><div id=g><div id=a></div></div></div>",
                           "#r { width: 200px; }"
                           "#g { display: grid; grid-template-columns: 200px;"
                           "     grid-template-rows: 100px;"
                           "     justify-items: center; align-items: end; }"
                           "#a { width: 40px; height: 20px; }",
                           220, 240);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const Box a = box_of(ui, 2);
            QL_CHECK_EQ(a.w, 40);       /* 没被拉伸 */
            QL_CHECK_EQ(a.x, 80);       /* (200-40)/2 */
            QL_CHECK_EQ(a.y, 80);       /* 底部对齐 100-20 */
        }
        drop();
    }

    /* ============ 11. 隐式行用 grid-auto-rows ============ */
    {
        QLUIRoot *ui = lay("<div id=r><div id=g>"
                           "<div class=c></div><div class=c></div></div></div>",
                           "#r { width: 100px; }"
                           "#g { display: grid; grid-template-columns: 100px;"
                           "     grid-auto-rows: 30px; }"
                           ".c { }",
                           120, 240);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const Box c1 = box_of(ui, 3);
            QL_CHECK_EQ(c1.h, 30);
            QL_CHECK_EQ(c1.y, 30);
        }
        drop();
    }

    /* ============ 12. 嵌套 grid（内层按外层给的宽度定尺） ============ */
    {
        QLUIRoot *ui = lay("<div id=r><div id=outer><div id=inner>"
                           "<div class=c></div><div class=c></div>"
                           "</div></div></div>",
                           "#r { width: 300px; }"
                           "#outer { display: grid; grid-template-columns: 1fr; }"
                           "#inner { display: grid; grid-template-columns: 1fr 1fr; }"
                           ".c { height: 10px; }",
                           320, 240);
        QL_CHECK(ui != NULL);
        if (ui != NULL) {
            const Box a = box_of(ui, 3);
            const Box b = box_of(ui, 4);
            QL_CHECK_EQ(a.w, 150);
            QL_CHECK_EQ(b.x, 150);
            QL_CHECK_EQ(b.w, 150);
        }
        drop();
    }

    /* ================== 13. 必须报错的边界 ================== */
    {
        const char *H = "<div id=r><div id=g><div id=a></div></div></div>";
        QL_CHECK(rejects(H,
                         "#g { display: grid; grid-template-columns: [a] 1fr; }",
                         "具名网格线"));
        QL_CHECK(rejects(H,
                         "#g { display: grid;"
                         "     grid-template-columns: repeat(auto-fill, 1fr); }",
                         "auto-fill"));
        QL_CHECK(rejects(H,
                         "#g { display: grid;"
                         "     grid-template-areas: \"a b\" \"a\"; }",
                         "列数必须相同"));
        QL_CHECK(rejects(H,
                         "#g { display: grid;"
                         "     grid-template-areas: \"a a\" \"a b\";"
                         "     } #a { grid-area: a; }",
                         "不是矩形"));
        QL_CHECK(rejects(H, "#g { display: grid } #a { grid-column: 1 }",
                         "直接子元素"));
        QL_CHECK(rejects(H,
                         "#g { display: grid; grid-template-columns: 1fr; }"
                         "#a { grid-column: 1 / 1; }",
                         "必须在起始位置之后"));
        QL_CHECK(rejects(H, "#g { display: grid; grid: auto-flow / 1fr 1fr; }",
                         "不支持 `grid` 简写"));
        QL_CHECK(rejects(H,
                         "#g { display: grid; grid-template-areas: \"x\"; }"
                         "#a { grid-area: nope; }",
                         "不认识的区域名"));
        /* 合法写法不能被误伤 */
        {
            QLUIDoc *doc = ql_ui_load_text(
                H, "#g { display: grid; grid-template-columns: repeat(2, 1fr); }"
                   "#a { grid-column: 1 / -1; }");
            QL_CHECK(doc != NULL);
            if (doc != NULL)
                ql_ui_doc_free(doc);
        }
    }

    /* ================== 14. border-image 像素 ================== */
    {
        /*
         * 源图 6×6：四角红、四边绿、中间蓝；`slice: 2` 意思是"边 2px"。
         * 盒子 60×40，`border-width: 2px`，所以：
         *   左上角 (1,1) → 红；顶边中点 (30,0) → 绿；中间 (30,20) → 蓝。
         */
        static uint16_t src[6 * 6];
        for (int y = 0; y < 6; y++)
            for (int x = 0; x < 6; x++) {
                const int corner = (x < 2 || x >= 4) && (y < 2 || y >= 4);
                const int edge = (x < 2 || x >= 4) || (y < 2 || y >= 4);
                src[y * 6 + x] = corner ? 0xF800u : (edge ? 0x07E0u : 0x001Fu);
            }
        char html[256];
        snprintf(html, sizeof(html),
                 "<div id=r><div id=b>bi</div></div>");
        const char *css =
            "#r { width: 60px; height: 40px; }"
            "#b { width: 60px; height: 40px; box-sizing: border-box;"
            "     border-width: 2px; border-style: solid; border-color: #000000;"
            "     border-image: url(frame) 2;"
            "     border-image-slice: 2; }";
        QL_CHECK(render_int(html, css, 100, 80));
        QLUIImageInput in;
        memset(&in, 0, sizeof(in));
        in.name = "frame";
        in.data = (const uint8_t *)src;
        in.len = sizeof(src);
        in.w = 6;
        in.h = 6;
        in.format = QL_IMAGE_SRC_RAW;
        in.pixel_format = QL_IMAGE_RGB565;
        QL_CHECK_EQ(ql_ui_register_image(s_ui, &in), QL_OK);
        ql_canvas_clear(&s_canvas, 0x000000);
        ql_ui_render_canvas(s_ui, &s_canvas);

        QL_CHECK(near(px(1, 1), 255, 0, 0, 24));       /* 左上角 = 红 */
        QL_CHECK(near(px(30, 0), 0, 255, 0, 24));      /* 顶边 = 绿 */
        QL_CHECK(near(px(30, 20), 0, 0, 255, 24));     /* 中间 = 蓝 */
        QL_CHECK(near(px(58, 38), 255, 0, 0, 24));     /* 右下角 = 红 */

        /* 有 border-image 必须写 slice */
        QL_CHECK(rejects("<div id=r><div id=b></div></div>",
                         "#b { border-image: url(x); }", "border-image-slice"));
        QL_CHECK(rejects("<div id=r><div id=b></div></div>",
                         "#b { border-image: url(x) 2;"
                         "     border-image-repeat: squish; }",
                         "border-image-repeat"));
    }

    if (s_disp != NULL) {
        ql_disp_mem_destroy(s_disp);
        s_disp = NULL;
    }
    drop();

QL_TEST_END()
