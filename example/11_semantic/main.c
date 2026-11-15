/**
 * @file    main.c
 * @brief   纯语义标签：真实 HTML 片段贴进来就能画。
 *
 *   ./build/example/example_11_semantic                    # 打开窗口
 *   ./build/example/example_11_semantic --dump /tmp/a.ppm   # 截图模式
 *
 * 这些标签**不引入任何新能力** —— UA 默认样式只有 block / inline，加它们的目的是
 * "别报错"。所以这个例程的重点是：哪些能看出区别，哪些看不出。
 *
 *   能看出区别：<dl>/<dd> 的缩进、<cite>/<var>/<dfn> 的斜体、
 *               <del> 的删除线、<ins> 的下划线、<fieldset> 的边框
 *   看不出区别：<a href>（没有导航）、<abbr title>（没有 tooltip）、
 *               <code>/<kbd>/<samp>（没有等宽字体）、<q>（没有生成引号）、
 *               <time>/<data>（属性被忽略）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#define W 320
#define H 348

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;

    QLUIRoot *ui = ql_ui_load_html_file(QL_EX_DIR "/index.html", QL_EX_DIR "/style.css");
    if (ui == NULL) {
        printf("界面编译失败：%s\n", ql_ui_text_last_error());
        qlgl_shutdown();
        return 1;
    }
    ql_ui_layout(ui, W, H);
    printf("%d 个节点，%d 个不同标签\n", (int)ui->doc->node_count,
           (int)QL_UI_TAG_COUNT);

    QLWindow *win = ql_window_create("QLGL · 11 semantic", W, H, QL_WINDOW_GL);
    if (win == NULL) {
        printf("建窗口失败（需要 EGL + GLES3 + X11/Wayland）\n");
        ql_ui_free(ui);
        qlgl_shutdown();
        return 1;
    }
    ql_window_bind_ui(win, ui);
    const int rc = ql_window_run_args(win, argc, argv);
    ql_window_destroy(win);
    ql_ui_free(ui);
    qlgl_shutdown();
    return rc;
}
