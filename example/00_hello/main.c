/**
 * @file    main.c
 * @brief   最短的 QLGL 程序：载入 HTML/CSS，开一个窗口，跑起来。
 *
 *   ./build/example/example_00_hello
 *
 * 全部代码就是下面这些 —— 窗口里的输入法、剪贴板、高 DPI、脏区重绘都在
 * `ql_window_*` 里，应用层不需要知道 X11 / Wayland / EGL 的存在。
 *
 * 注意所有权：`ql_ui_load_html_file()` 返回的 root **持有** doc，
 * `ql_ui_free(ui)` 会一并释放；所以这里没有单独的 `ql_ui_doc_free(doc)`。
 */
#include <stdio.h>

#include <qlgl/qlgl.h>

/* CMake 注入的例程目录（从任何 cwd 启动都能找到 index.html/style.css） */
#ifndef QL_EX_DIR
#define QL_EX_DIR "example/00_hello"
#endif

int main(void)
{
    if (qlgl_init() != QL_OK) {
        fprintf(stderr, "qlgl_init 失败\n");
        return 1;
    }

    /* ① 编译界面 + 建运行期树（doc 归 ui 所有） */
    QLUIRoot *ui = ql_ui_load_html_file(QL_EX_DIR "/index.html",
                                       QL_EX_DIR "/style.css");
    if (ui == NULL) {
        fprintf(stderr, "界面载入失败：%s\n", ql_ui_text_last_error());
        qlgl_shutdown();
        return 1;
    }

    /* ② 开窗口：输入法、剪贴板、高 DPI、脏区重绘都由它接好 */
    QLWindow *win = ql_window_create("Hello, QLGL", 360, 200, QL_WINDOW_GL);
    if (win == NULL) {
        fprintf(stderr, "建窗口失败（需要 EGL + GLES3 + X11/Wayland）\n");
        ql_ui_free(ui);
        qlgl_shutdown();
        return 1;
    }

    ql_window_bind_ui(win, ui);
    ql_window_run(win);            /* 阻塞到关窗 */

    /* ③ 收尾：窗口不拥有 ui，顺序是先窗口后界面 */
    ql_window_destroy(win);
    ql_ui_free(ui);
    qlgl_shutdown();
    return 0;
}
