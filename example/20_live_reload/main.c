/**
 * @file    main.c
 * @brief   热重载：改 index.html / style.css，窗口自己更新（应用不重新编译）。
 *
 *   ./build/example/example_20_live_reload
 *   ./build/example/example_20_live_reload --dump /tmp/x.ppm   # CI：出一帧
 *
 * 原理只有一句：**界面是可以在运行期编译的**（`ql_ui_load_html()`）。
 * 所以"热重载"不是额外功能，而是"重新读一次文件、重新建一次树"：
 *
 *     stat() 看 mtime 变了吗 → 读文件 → ql_ui_load_html → ql_ui_build
 *                            → ql_window_set_ui（失败就留着旧的）
 *
 * 编辑器保存触发 mtime 变化，100ms 内就能看到新界面。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <qlgl/qlgl.h>

#define W 640
#define H 400
#define POLL_MS 100          /* 多久看一次文件（mtime 精度足够） */

static uint32_t s_next_poll;

/* 两个文件的 mtime；返回 0 = 读不到 */
static int stamp_of(const char *path, double *out)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    *out = (double)st.st_mtime;   /* 秒级足够：编辑器保存一定会变 */
    return 1;
}

typedef struct {
    QLUIDoc  *doc;
    QLUIRoot *ui;
    double    html_stamp;
    double    css_stamp;
} Loaded;

static void paths(char *hp, size_t hcap, char *cp, size_t ccap)
{
    snprintf(hp, hcap, "%s/index.html", QL_EX_DIR);
    snprintf(cp, ccap, "%s/style.css", QL_EX_DIR);
}

/*
 * 编译一份界面到 `out`（不碰旧的那份）。失败返回 0，"明确失败优于半个正确"：
 * 新文件写坏了，屏幕上应该还是旧的能用的界面。
 */
static int build_ui(Loaded *out, int w, int h)
{
    char hp[1024], cp[1024];
    paths(hp, sizeof(hp), cp, sizeof(cp));

    char *html = ql_file_read_text(hp);
    char *css = ql_file_read_text(cp);
    if (html == NULL || css == NULL) {
        printf("读不到 %s / %s（界面保持原样）\n", hp, cp);
        ql_mem_free(html);
        ql_mem_free(css);
        return 0;
    }

    QLUIDoc *doc = ql_ui_load_html(html, css);
    ql_mem_free(html);
    ql_mem_free(css);
    if (doc == NULL) {
        printf("编译失败（界面保持原样）：%s\n", ql_ui_text_last_error());
        return 0;
    }

    QLUIRoot *ui = ql_ui_build(doc);
    if (ui == NULL) {
        ql_ui_doc_free(doc);
        printf("建树失败（界面保持原样）\n");
        return 0;
    }
    ql_ui_layout(ui, w, h);

    memset(out, 0, sizeof(*out));
    out->doc = doc;
    out->ui = ui;
    stamp_of(hp, &out->html_stamp);
    stamp_of(cp, &out->css_stamp);
    return 1;
}

static int ui_changed(const Loaded *l)
{
    char hp[1024], cp[1024];
    paths(hp, sizeof(hp), cp, sizeof(cp));
    double hs = 0, cs = 0;
    if (!stamp_of(hp, &hs) || !stamp_of(cp, &cs))
        return 0;
    return hs != l->html_stamp || cs != l->css_stamp;
}

/* 每帧钩子：到点看一次文件；变了就编译 → 换 UI（失败保留旧的） */
static int on_frame(QLUIRoot *ui, QLWindow *win, void *user)
{
    (void)ui;
    Loaded *cur = (Loaded *)user;
    const uint32_t now = ql_anim_now_ms();
    if ((int32_t)(now - s_next_poll) < 0)
        return 0;
    s_next_poll = now + POLL_MS;

    if (!ui_changed(cur))
        return 0;

    ql_coord_t w = W, h = H;
    ql_window_size(win, &w, &h);
    Loaded next;
    if (!build_ui(&next, w, h))
        return 0;

    QLUIRoot *old = cur->ui;
    QLUIDoc *old_doc = cur->doc;
    *cur = next;
    ql_window_set_ui(win, cur->ui);   /* 先摘旧 root 的输入汇 */
    ql_ui_free(old);                  /* 再释放旧树（doc 一起） */
    ql_ui_doc_free(old_doc);
    printf("重载：%d 个节点，%d 个样式\n", (int)cur->doc->node_count,
           (int)cur->doc->style_count);
    return 1;                         /* 整屏重画 */
}

int main(int argc, char **argv)
{
    const char *dump = NULL;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
            dump = argv[++i];

    if (qlgl_init() != QL_OK)
        return 1;

    Loaded cur;
    if (!build_ui(&cur, W, H)) {
        qlgl_shutdown();
        return 1;
    }

    if (dump != NULL) {
        const int ok = ql_ui_dump_ppm(cur.ui, W, H, dump);
        printf(ok ? "已写出 %s\n" : "写出 %s 失败\n", dump);
        ql_ui_free(cur.ui);
        ql_ui_doc_free(cur.doc);
        qlgl_shutdown();
        return ok ? 0 : 1;
    }

    QLWindow *win = ql_window_create("QLGL · 20 live reload（改 CSS 试试）", W, H,
                                     QL_WINDOW_GL);
    if (win == NULL) {
        printf("（没有可用窗口，改为离屏渲染一帧）\n");
        if (ql_ui_dump_ppm(cur.ui, W, H, "live_reload.ppm"))
            printf("已写出 live_reload.ppm\n");
        ql_ui_free(cur.ui);
        ql_ui_doc_free(cur.doc);
        qlgl_shutdown();
        return 0;
    }

    ql_window_set_frame_cb(win, on_frame, &cur);
    ql_window_bind_ui(win, cur.ui);
    s_next_poll = ql_anim_now_ms() + POLL_MS;
    const int rc = ql_window_run(win);
    ql_window_destroy(win);
    ql_ui_free(cur.ui);
    ql_ui_doc_free(cur.doc);
    qlgl_shutdown();
    return rc;
}
