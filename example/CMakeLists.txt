# 基本例程：每个文件就是一个**完整可运行**的程序，从 01 开始逐级加一点东西。
#
#   cmake -S . -B build && cmake --build build -j
#   ./build/example/example_01_window            # 打开窗口
#   ./build/example/example_01_window --dump /tmp/a.ppm   # 无显示器也能跑
#
# 只需要其中某个：cmake --build build --target example_01_window
# 完全不要例程：cmake -S . -B build -DQLGL_BUILD_EXAMPLES=OFF
#
# 刻意不依赖 test/、也不需要先跑代码生成：界面用 `ql_ui_load_text()` 内联在
# 源文件里（HTML + CSS 就是字符串），一个 .c 文件就能编译运行。
# 构建期编译（tools/qltext）的做法见 doc/tutorial 的「进阶」一章。

set(QL_EXAMPLES
    01_window
    02_text
    03_layout
    04_interaction
    05_animation
    06_theme
    07_player
)

foreach(e ${QL_EXAMPLES})
    add_executable(example_${e} ${CMAKE_CURRENT_SOURCE_DIR}/${e}.c)
    target_link_libraries(example_${e} PRIVATE qlgl)
endforeach()
