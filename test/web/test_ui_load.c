/**
 * @file    test_ui_load.c
 * @brief   高层加载入口：`ql_ui_load_html_file()` / `ql_ui_load_html()`。
 *
 * hello world 只写一行 `QLUIRoot *ui = ql_ui_load_html_file(...)`，所以这条路径
 * 的契约必须钉住：
 *   1. 读不到文件 / CSS 解析失败 → NULL（不返回半成品）；
 *   2. 返回的 root **持有** doc（`ql_ui_free()` 一并释放，内存归零）；
 *   3. 只 build、不布局 —— 尺寸由 `ql_ui_layout()` / `ql_window_bind_ui()` 决定；
 *   4. `ql_ui_load_html()` 是 `ql_ui_load_text()` 的别名，返回独立 doc。
 */
#include <stdio.h>
#include <string.h>

#include "../ql_test.h"

#define HTML_PATH "/tmp/qlgl_test_load.html"
#define CSS_PATH  "/tmp/qlgl_test_load.css"

static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return 0;
    const size_t n = strlen(text);
    const int ok = (fwrite(text, 1, n, f) == n);
    fclose(f);
    return ok;
}

QL_TEST_BEGIN("test_ui_load")

    /* 1. 文件 → root（doc 归 root） */
    QL_CHECK(write_file(HTML_PATH,
                        "<div id=root><div class=t>Hello</div></div>"));
    QL_CHECK(write_file(CSS_PATH,
                        "#root { width: 100%; height: 100%; background-color: #102030; }"
                        ".t { color: #FFFFFF; font-size: 12px; }"));

    QLUIRoot *ui = ql_ui_load_html_file(HTML_PATH, CSS_PATH);
    QL_CHECK(ui != NULL);
    if (ui != NULL) {
        QL_CHECK(ui->doc != NULL);
        QL_CHECK_EQ(ui->owns_doc, 1);
        QL_CHECK(ui->node_count >= 2);
        QL_CHECK_EQ(ui->width, 0);          /* 只 build，不布局 */
        QL_CHECK_EQ(ui->height, 0);

        ql_ui_layout(ui, 120, 80);
        QL_CHECK_EQ(ui->width, 120);
        QL_CHECK_EQ(ui->height, 80);
        ql_ui_free(ui);                     /* doc 一起释放 */
    }

    /* 2. 文件不存在 → NULL */
    QL_CHECK(ql_ui_load_html_file("/tmp/qlgl_no_such_file.html", CSS_PATH) == NULL);

    /* 3. CSS 语法错误 → NULL（错误信息可由 ql_ui_text_last_error 取） */
    QL_CHECK(write_file(CSS_PATH, "#root { color: }"));
    QL_CHECK(ql_ui_load_html_file(HTML_PATH, CSS_PATH) == NULL);
    QL_CHECK(ql_ui_text_last_error()[0] != '\0');

    /* 4. 纯文本入口的别名 */
    QLUIDoc *doc = ql_ui_load_html("<div id=root>x</div>", "#root { width: 10px; }");
    QL_CHECK(doc != NULL);
    if (doc != NULL) {
        QLUIRoot *ui2 = ql_ui_build(doc);
        QL_CHECK(ui2 != NULL);
        QL_CHECK_EQ(ui2->owns_doc, 0);
        ql_ui_free(ui2);
        ql_ui_doc_free(doc);
    }

    remove(HTML_PATH);
    remove(CSS_PATH);

QL_TEST_END()
