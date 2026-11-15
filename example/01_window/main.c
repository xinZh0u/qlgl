/**
 * @file    main.c
 * @brief   最小的 QLGL 程序：编译一份界面，显示出来。
 *
 * 编译运行：
 *   cmake -S . -B build && cmake --build build -j
 *   ./build/example/example_01_window --dump /tmp/01.ppm
 *
 * 目录里的两个文件就是界面本身：
 *   index.html   结构（一行 div 加一段文字）
 *   style.css    样式（铺满窗口、加一点内边距和颜色）
 *
 * 这个程序做了四件事，多一件也没有：
 *   1. `qlgl_init()`      初始化库
 *   2. `ex_load_ui()`      读 index.html + style.css，编译成界面数据（doc）
 *   3. `ql_ui_build()`    用 doc 建出运行期节点树，然后布局一次
 *   4. 交给窗口 / 或者离屏渲染成图片
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

int main(int argc, char **argv)
{
    if (qlgl_init() != QL_OK)
        return 1;

    /* ① 编译界面。失败时 last_error 里有 "css:行号: 原因" */
    QLUIRoot *ui = ql_ui_load_html_file(QL_EX_DIR "/index.html", QL_EX_DIR "/style.css");
    if (ui == NULL) {
        printf("界面编译失败：%s\n", ql_ui_text_last_error());
        qlgl_shutdown();
        return 1;
    }

    /* ② 建运行期树并布局（视口 240x120，与 #root 一致） */

    printf("%u 个节点 / %u 条样式\n", (unsigned)ui->doc->node_count,
           (unsigned)ui->doc->style_count);

    /* ③ 跑起来 */
    QLWindow *win = ql_window_create("QLGL · 01 window", 240, 120, QL_WINDOW_GL);
    if (win == NULL) {
        printf("建窗口失败（需要 EGL + GLES3 + X11/Wayland）\n");
        ql_ui_free(ui);
        qlgl_shutdown();
        return 1;
    }
    ql_window_bind_ui(win, ui);
    const int rc = ql_window_run_args(win, argc, argv);
    ql_window_destroy(win);

    /* ④ 收尾：先释放运行期树，再释放 doc（doc 是独立的所有权） */
    ql_ui_free(ui);
    qlgl_shutdown();
    return rc;
}
