# 基本例程：**每个例程一个文件夹**，一个文件夹就是一个完整可运行的程序，
# 从 01 开始逐级加一点东西。
#
#   example/01_window/
#     main.c         程序（40~160 行，只讲自己那一件事）
#     index.html     界面的结构
#     style.css      界面的样式
#
# 这样拆的理由：HTML/CSS 单独成文件才好编辑（有高亮、有格式化），也才好直接喂给
# 构建期编译工具：
#
#   python3 tools/qltext/qltext.py cc --html example/01_window/index.html \
#                                     --css  example/01_window/style.css \
#                                     --name demo --out build/
#
# 例程是**给 PC 上直接跑的**，所以 main.c 里读的是文件路径而不是内嵌字符串
# （`ex_load_ui("index.html", "style.css")`，见 common.h）。
#
#   cmake -S . -B build && cmake --build build -j
#   ./build/example/example_01_window                      # 打开窗口
#   ./build/example/example_01_window --dump /tmp/a.ppm     # 无显示器也能跑
#
# 只需要其中某个：cmake --build build --target example_01_window
# 完全不要例程：cmake -S . -B build -DQLGL_BUILD_EXAMPLES=OFF

set(QL_EXAMPLES
    01_window
    02_text
    03_layout
    04_interaction
    05_animation
    06_theme
    07_player
    08_tags
    09_scroll
    10_input
    11_semantic
    12_text_block
    13_form
    14_css3
    15_backdrop
    16_css_units
    17_css_selectors
    18_css_paint
    19_css_text
    20_live_reload
    21_css_vars
    22_media
    23_calc
    24_b5
    25_b4
    26_css_states
    27_css_text2
    28_bg_image
    29_grid
    30_border_image
    31_filter
    32_transform
    33_table
    34_columns
    35_text_stack
)

foreach(e ${QL_EXAMPLES})
    add_executable(example_${e} ${CMAKE_CURRENT_SOURCE_DIR}/${e}/main.c)
    target_link_libraries(example_${e} PRIVATE qlgl)
    # 共用样板（common.h）放在 example/ 根下，不进每个例程目录
    target_include_directories(example_${e} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # 例程的 HTML/CSS 在源码目录里躺着（运行期读），把它编进程序当默认目录，
    # 这样 `./build/example/example_01_window` 从任何 cwd 启动都能找到 index.html。
    target_compile_definitions(example_${e} PRIVATE
        QL_EX_DIR="${CMAKE_CURRENT_SOURCE_DIR}/${e}"
        # common.h 用 clock_gettime 做 --seconds；宏必须在任何头文件之前生效
        _POSIX_C_SOURCE=200809L)
    set_target_properties(example_${e} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/example)   # build/example/example_NN_xxx
endforeach()
