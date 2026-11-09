/**
 * @file    main.c
 * @brief   热重载：改 index.html / style.css，窗口自己更新（应用不重新编译）。
 *
 *   ./build/example/example_20_live_reload
 *
 * 原理只有一句：**界面是可以在运行期编译的**（`ql_ui_load_text`）。
 * 所以"热重载"不是额外功能，而是"重新读一次文件、重新建一次树"：
 *
 *     stat() 看 mtime 变了吗 → 读文件 → ql_ui_load_text → ql_ui_build
 *                            → 换掉旧的（失败就留着旧的）→ ql_ui_layout
 *
 * 编辑器保存触发 mtime 变化，100ms 内就能看到新界面。
 *
 * 这件事和"设计工具"是同一套东西：把"文件"换成"编辑器里的缓冲区"，
 * 再加一个能点选元素、看盒子的检查器就行 —— 见 doc/对比与取舍.md §12.2。
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "common.h"

#define W 640
#define H 400
#define POLL_MS 100          /* 多久看一次文件（mtime 精度足够） */

/* 两个文件的 mtime（秒 + 纳秒）；返回 0 = 读不到 */
static int stamp_of(const char *path, double *out)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    *out = (double)st.st_mtime;   /* 秒级足够：编辑器保存一定会变 */
    return 1;
}

/* 一份"已编译好的界面"：doc 与 ui 成对，换的时候整对换 */
typedef struct {
    QLUIDoc  *doc;
    QLUIRoot *ui;
    double    html_stamp;
    double    css_stamp;
    int       ms;            /* 上次编译耗时 */
} Loaded;

/*
 * 编译一份界面。失败返回 0 **且不动 out** ——
 * "明确失败优于半个正确"：新文件写坏了，屏幕上应该还是旧的能用的界面。
 */
static int load_ui(Loaded *out, int w, int h)
{
    char hp[1024], cp[1024];
    ex_resolve(hp, sizeof(hp), "index.html");
    ex_resolve(cp, sizeof(cp), "style.css");

    const uint32_t t0 = ql_anim_now_ms();

    /* 读文件 + 编译：`ex_load_ui` 就是"运行期解析"那一句 */
    QLUIDoc *doc = ex_load_ui("index.html", "style.css");
    if (doc == NULL) {
        printf("编译失败（界面保持原样）：%s\n", ql_ui_text_last_error());
        return 0;
    }
    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        printf("建树失败（界面保持原样）\n");
        ql_ui_doc_free(doc);
        return 0;
    }
    ql_ui_layout(ui, w, h);

    /* 只有全部成功才换掉旧的 */
    if (out->ui != NULL) {
        ql_ui_detach_input();
        ql_ui_free(out->ui);
        ql_ui_doc_free(out->doc);
    }
    out->doc = doc;
    out->ui = ui;
    out->ms = (int)(ql_anim_now_ms() - t0);
    stamp_of(hp, &out->html_stamp);
    stamp_of(cp, &out->css_stamp);
    return 1;
}

/* 文件变了吗 */
static int ui_changed(const Loaded *l)
{
    char hp[1024], cp[1024];
    ex_resolve(hp, sizeof(hp), "index.html");
    ex_resolve(cp, sizeof(cp), "style.css");
    double hs = 0, cs = 0;
    if (!stamp_of(hp, &hs) || !stamp_of(cp, &cs))
        return 0;
    return hs != l->html_stamp || cs != l->css_stamp;
}

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;
    ex_setup_locale();
    ex_load_font();

    Loaded cur;
    memset(&cur, 0, sizeof(cur));
    if (!load_ui(&cur, W, H)) {
        qlgl_shutdown();
        return 1;
    }
    printf("首次编译：%d 个节点，%d 个样式，%d ms\n", (int)cur.doc->node_count,
           (int)cur.doc->style_count, cur.ms);

    /* 截图模式（CI 用）：编完直接出一帧，不进循环 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            const int rc = ex_run(cur.ui, W, H, "QLGL · 20 live reload", argc, argv);
            ql_ui_free(cur.ui);
            ql_ui_doc_free(cur.doc);
            qlgl_shutdown();
            return rc;
        }
    }

#if HAS_GL
    QLGfxGL *gfx = ql_gfx_gl_create("QLGL · 20 live reload（改 CSS 试试）", W, H,
                                    QL_GFX_GL_WINDOW);
    if (gfx != NULL) {
        ql_ui_attach_input(cur.ui);
        ql_gfx_gl_attach_clipboard(gfx, cur.ui);
        QLDraw2D *d2d = ql_gfx_gl_draw(gfx);

        ql_coord_t w = W, h = H;
        uint32_t next_poll = ql_anim_now_ms() + POLL_MS;

        while (ql_gfx_gl_poll(gfx)) {
            if (ql_gfx_gl_resize_pending(gfx)) {
                ql_gfx_gl_size(gfx, &w, &h);
                if (w > 0 && h > 0)
                    ql_ui_layout(cur.ui, w, h);
            }
            ql_input_dispatch();

            /* ---- 热重载：文件变了就重新编译 ---- */
            const uint32_t now = ql_anim_now_ms();
            if (now >= next_poll) {
                next_poll = now + POLL_MS;
                if (ui_changed(&cur) && load_ui(&cur, (int)w, (int)h)) {
                    ql_ui_attach_input(cur.ui);      /* 新树要重新接输入 */
                    ql_gfx_gl_attach_clipboard(gfx, cur.ui);
                    printf("重载：%d 个节点，%d 个样式，%d ms\n",
                           (int)cur.doc->node_count, (int)cur.doc->style_count, cur.ms);
                }
            }

            (void)ql_ui_anim_tick(cur.ui, now);
            ql_gfx_gl_begin(gfx, ex_clear_color(cur.ui));
            ql_ui_render(cur.ui, d2d);
            ql_gfx_gl_end(gfx);
        }
        ql_ui_detach_input();
        ql_gfx_gl_destroy(gfx);
        ql_ui_free(cur.ui);
        ql_ui_doc_free(cur.doc);
        qlgl_shutdown();
        return 0;
    }
    printf("（没有可用窗口，改为离屏渲染一帧）\n");
#endif

    ql_ui_layout(cur.ui, W, H);
    if (ex_dump_ppm(cur.ui, W, H, "live_reload.ppm"))
        printf("已写出 live_reload.ppm\n");
    ql_ui_free(cur.ui);
    ql_ui_doc_free(cur.doc);
    qlgl_shutdown();
    return 0;
}
