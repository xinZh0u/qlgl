cmake_minimum_required(VERSION 3.16)

project(qlgl LANGUAGES C VERSION 0.1.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)          # 严格 -std=c11，防止误用 POSIX/编译器扩展

# 给编辑器用的编译数据库（.vscode/c_cpp_properties.json / clangd 都读它）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

file(GLOB_RECURSE QLGL_SRC CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
)

add_library(qlgl STATIC ${QLGL_SRC})
target_include_directories(qlgl PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_options(qlgl PRIVATE -Wall -Wextra)
# libm 是自己用的（缓动曲线、SDF、颜色插值），不借 FreeType 的传递依赖 ——
# 之前关掉 FreeType 之后例程集体链接失败，就是这个"顺手用别人的 -lm"
find_library(QL_LIBM m)
if(QL_LIBM)
    target_link_libraries(qlgl PUBLIC ${QL_LIBM})
endif()

# 图像能力预设：full（全开）/ lean（砍 PNG 与平台后端）/ raw-only（只吃构建期 RAW）
set(QLGL_IMG_PRESET "full" CACHE STRING "图像能力预设：full / lean / raw-only")
set_property(CACHE QLGL_IMG_PRESET PROPERTY STRINGS full lean raw-only)
# 预设只给"默认值"；命令行 -DQL_FEAT_IMG_xxx=0/1 永远优先（单品覆盖）
set(_ql_preset_bmp 1)
set(_ql_preset_qoi 1)
set(_ql_preset_png 1)
set(_ql_preset_platform 1)
set(_ql_preset_gif 1)
if(QLGL_IMG_PRESET STREQUAL "raw-only")
    set(_ql_preset_bmp 0)
    set(_ql_preset_qoi 0)
    set(_ql_preset_png 0)
    set(_ql_preset_platform 0)
elseif(QLGL_IMG_PRESET STREQUAL "lean")
    set(_ql_preset_png 0)
    set(_ql_preset_platform 0)
    set(_ql_preset_gif 0)
elseif(QLGL_IMG_PRESET STREQUAL "raw-only")
    set(_ql_preset_gif 0)
endif()
if(NOT DEFINED QL_FEAT_IMG_BMP)
    set(QL_FEAT_IMG_BMP ${_ql_preset_bmp})
endif()
if(NOT DEFINED QL_FEAT_IMG_QOI)
    set(QL_FEAT_IMG_QOI ${_ql_preset_qoi})
endif()
if(NOT DEFINED QL_FEAT_IMG_PNG)
    set(QL_FEAT_IMG_PNG ${_ql_preset_png})
endif()
if(NOT DEFINED QL_FEAT_IMG_PLATFORM)
    set(QL_FEAT_IMG_PLATFORM ${_ql_preset_platform})
endif()
if(NOT DEFINED QL_FEAT_IMG_GIF)
    set(QL_FEAT_IMG_GIF ${_ql_preset_gif})
endif()
add_compile_definitions(QL_FEAT_IMG_BMP=${QL_FEAT_IMG_BMP}
                        QL_FEAT_IMG_QOI=${QL_FEAT_IMG_QOI}
                        QL_FEAT_IMG_PNG=${QL_FEAT_IMG_PNG}
                        QL_FEAT_IMG_GIF=${QL_FEAT_IMG_GIF}
                        QL_FEAT_IMG_PLATFORM=${QL_FEAT_IMG_PLATFORM})

option(QLGL_BUILD_TESTS "构建 test/ 下的示例与单元测试" ON)
option(QLGL_BUILD_EXAMPLES "构建 example/ 下的基本例程" ON)
option(QLGL_BUILD_BENCHMARK "构建 benchmark/ 下的可视化性能基准（bench_cpu / bench_gl）" ON)
# 运行期 HTML/CSS 解析器（M3.5 第 3 步）。MCU 侧关掉它即零字节成本
option(QLGL_BUILD_TEXT_PARSER "构建 ql_ui_load_text()（运行期 HTML/CSS 解析）" ON)
option(QLGL_BUILD_FREETYPE "构建 FreeType 矢量字体后端（中文/任意字号）" ON)
option(QLGL_BUILD_JPEG "构建 libjpeg-turbo 平台后端（JPEG；找不到就跳过）" ON)

# ---- libjpeg-turbo：平台后端的参考实现（JPEG）----
# 平台后端**接口**永远在（几十字节），这里加的是"自带一个 libjpeg 实现"。
if(QLGL_BUILD_TESTS OR QLGL_BUILD_EXAMPLES)
    set(_ql_need_jpeg_hint ON)
endif()
if(QLGL_BUILD_JPEG AND QL_FEAT_IMG_PLATFORM)
    find_package(JPEG QUIET)
    if(JPEG_FOUND)
        target_sources(qlgl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/port/qlimage_jpeg.c)
        target_include_directories(qlgl PUBLIC ${JPEG_INCLUDE_DIRS})
        target_compile_definitions(qlgl PUBLIC QL_FEAT_IMG_LIBJPEG=1)
        target_link_libraries(qlgl PUBLIC ${JPEG_LIBRARIES})
        message(STATUS "QLGL: libjpeg-turbo 平台后端已启用（JPEG）")
    else()
        target_compile_definitions(qlgl PUBLIC QL_FEAT_IMG_LIBJPEG=0)
        message(STATUS "QLGL: 未找到 libjpeg，JPEG 只能靠外部注册的后端")
    endif()
else()
    target_compile_definitions(qlgl PUBLIC QL_FEAT_IMG_LIBJPEG=0)
endif()

# ---- FreeType：矢量字体后端（PC/MPU；MCU 关掉即可，内置位图字体仍然可用） ----
if(QLGL_BUILD_FREETYPE)
    find_package(Freetype QUIET)
    if(FREETYPE_FOUND)
        target_sources(qlgl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/text/qlfont_ft.c)
        target_include_directories(qlgl PUBLIC ${FREETYPE_INCLUDE_DIRS})
        target_compile_definitions(qlgl PUBLIC QLGL_ENABLE_FREETYPE=1)
        target_link_libraries(qlgl PUBLIC ${FREETYPE_LIBRARIES})
        set(QLGL_FREETYPE_ENABLED 1)
        message(STATUS "QLGL: FreeType 矢量字体后端已启用")
    else()
        message(STATUS "QLGL: 未找到 FreeType，只用内置位图字体")
    endif()
endif()

# 文本栈（B8 的 @font-face / font-family 家族列表）**必须有 FreeType**：
# 没有它的构建要自动把它关掉，否则库自己就编不过（qlfont_ft.h 里的 #error 是给
# 直接 include 头文件的用户看的，不该被 CMake 造出来）。
if(NOT QLGL_FREETYPE_ENABLED)
    if(DEFINED QL_FEAT_CSS_FONT_STACK AND QL_FEAT_CSS_FONT_STACK)
        message(FATAL_ERROR
            "QL_FEAT_CSS_FONT_STACK=1 需要 FreeType：请打开 QLGL_BUILD_FREETYPE，"
            "或同时定义 -DQL_FEAT_CSS_FONT_STACK=0")
    endif()
    target_compile_definitions(qlgl PUBLIC QL_FEAT_CSS_FONT_STACK=0)
    message(STATUS "QLGL: 没有 FreeType → 文本栈（@font-face / 家族列表）已强制关闭")
endif()

# ---- B8 的第三方库：构建期探测 HarfBuzz / fribidi ----
#
# 两条路都能用（这是"平台选择第三方库"的落点）：
#   1. **有开发头文件** → 直连（`QLGL_HAVE_HARFBUZZ` / `QLGL_HAVE_FRIBIDI`），不依赖 dlopen；
#   2. **只有运行库**（很多发行版就是这样）→ 运行期 `dlopen("libharfbuzz.so.0")`；
#   3. 都没有 → 退回内建整形 / 内建 UAX#9 子集。
#
# 三种情况下**用户代码一行都不用改**。
option(QLGL_USE_HARFBUZZ "构建期直连 HarfBuzz（找不到头文件时自动退回运行期 dlopen）" ON)
option(QLGL_USE_FRIBIDI "构建期直连 fribidi（找不到头文件时自动退回运行期 dlopen）" ON)

if(QLGL_USE_HARFBUZZ)
    find_path(HARFBUZZ_INCLUDE_DIR hb.h PATH_SUFFIXES harfbuzz)
    find_library(HARFBUZZ_LIBRARY NAMES harfbuzz)
    if(HARFBUZZ_INCLUDE_DIR AND HARFBUZZ_LIBRARY)
        target_include_directories(qlgl PRIVATE ${HARFBUZZ_INCLUDE_DIR})
        target_link_libraries(qlgl PUBLIC ${HARFBUZZ_LIBRARY})
        target_compile_definitions(qlgl PUBLIC QLGL_HAVE_HARFBUZZ=1)
        message(STATUS "QLGL: HarfBuzz 直连（整形）")
    else()
        message(STATUS "QLGL: 未找到 HarfBuzz 开发头文件，整形将在运行期 dlopen（没有就退回内建）")
    endif()
endif()

if(QLGL_USE_FRIBIDI)
    find_path(FRIBIDI_INCLUDE_DIR fribidi/fribidi.h)
    find_library(FRIBIDI_LIBRARY NAMES fribidi)
    if(FRIBIDI_INCLUDE_DIR AND FRIBIDI_LIBRARY)
        target_include_directories(qlgl PRIVATE ${FRIBIDI_INCLUDE_DIR})
        target_link_libraries(qlgl PUBLIC ${FRIBIDI_LIBRARY})
        target_compile_definitions(qlgl PUBLIC QLGL_HAVE_FRIBIDI=1)
        message(STATUS "QLGL: fribidi 直连（双向）")
    else()
        message(STATUS "QLGL: 未找到 fribidi 开发头文件，双向将在运行期 dlopen（没有就退回内建）")
    endif()
endif()

# dlopen 本体（走的是 libdl；glibc 2.34+ 已经在 libc 里，链接空串也没事）
target_link_libraries(qlgl PUBLIC ${CMAKE_DL_LIBS})

if(UNIX AND NOT APPLE)
    find_package(X11)
endif()

# ---- HTML+CSS → C（相当于 Qt 的 uic）：把 test/web 下的页面编译成静态数据 ----
find_package(Python3 COMPONENTS Interpreter QUIET)

# 宿主工具 qltext：HTML/CSS → 静态 C 表 / .qlui。
# 它链的是库自己的解析器（src/ui/qlui_parse.c），所以 **语义只有一份**：
# 构建期、运行期、blob 三条路走的是同一段代码，不存在两份实现漂移。
if(QLGL_BUILD_TEXT_PARSER AND TARGET qlgl)
    add_executable(qltext ${CMAKE_CURRENT_SOURCE_DIR}/tools/qltext/qltext.c)
    target_link_libraries(qltext PRIVATE qlgl)
    message(STATUS "QLGL: 宿主工具 qltext 已启用（构建期 HTML/CSS 编译）")
endif()

# 宿主工具 qlperf：进程级 CPU/GPU/内存探针（调"拖动窗口掉帧"这类问题用）。
# 它不认识 qlgl 的任何类型 —— 只读 /proc 与 /sys，所以不算运行时依赖。
if(UNIX)
    add_executable(qlperf ${CMAKE_CURRENT_SOURCE_DIR}/tools/qlperf/qlperf.c)
    if(X11_FOUND)
        target_compile_definitions(qlperf PRIVATE HAVE_X11=1)
        if(TARGET X11::X11)
            target_link_libraries(qlperf PRIVATE X11::X11)
        else()
            target_include_directories(qlperf PRIVATE ${X11_INCLUDE_DIR})
            target_link_libraries(qlperf PRIVATE ${X11_LIBRARIES})
        endif()
    endif()
    message(STATUS "QLGL: 宿主工具 qlperf 已启用（CPU/GPU/内存探针）")
endif()

if(QLGL_BUILD_TESTS AND TARGET qltext)
    set(QL_UI_IN  ${CMAKE_CURRENT_SOURCE_DIR}/test/web)
    set(QL_UI_OUT ${CMAKE_CURRENT_BINARY_DIR}/ui)
    set(QL_UI_GEN ${QL_UI_OUT}/ui_demo.c ${QL_UI_OUT}/ui_demo.h
                  ${QL_UI_OUT}/ui_demo_blob.c)
    add_custom_command(
        OUTPUT ${QL_UI_GEN}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${QL_UI_OUT}
        COMMAND qltext cc
                --html ${QL_UI_IN}/ui_demo.html
                --css  ${QL_UI_IN}/ui_demo.css
                --out  ${QL_UI_OUT}
        DEPENDS ${QL_UI_IN}/ui_demo.html ${QL_UI_IN}/ui_demo.css qltext
        COMMENT "编译 HTML+CSS 为 C")
    add_custom_target(qlui ALL DEPENDS ${QL_UI_GEN})

    # ---- flex 布局用例：每个 html/css 组合编译成一个 doc ----
    set(QL_FLEX_CASES row grow justify align wrap column
                    posabs posfix posrel zidx ovf mixed frac paint dpr trans kf grad clip scale flip flipdemo showcase blur anim anim10 bench100 tags b4)
    set(QL_FLEX_GEN "")
    foreach(c ${QL_FLEX_CASES})
        set(_c  ${CMAKE_CURRENT_SOURCE_DIR}/test/web/flex/${c})
        set(_g  ${QL_UI_OUT}/ui_flex_${c}.c)
        add_custom_command(
            OUTPUT ${_g} ${QL_UI_OUT}/ui_flex_${c}.h
                   ${QL_UI_OUT}/ui_flex_${c}_blob.c
            COMMAND ${CMAKE_COMMAND} -E make_directory ${QL_UI_OUT}
            COMMAND qltext cc
                    --html ${_c}.html --css ${_c}.css
                    --name flex_${c} --out ${QL_UI_OUT}
            DEPENDS ${_c}.html ${_c}.css qltext
            COMMENT "编译 flex 用例 ${c}")
        list(APPEND QL_FLEX_GEN ${_g} ${QL_UI_OUT}/ui_flex_${c}.h
                                 ${QL_UI_OUT}/ui_flex_${c}_blob.c)
    endforeach()
    add_custom_target(qlui_flex ALL DEPENDS ${QL_FLEX_GEN})

    # ---- 构建期顺便产出 .qlui blob ----
    # 序列化只在 C 侧实现一次（src/ui/qlui_blob.c），因此这里链一个宿主工具，
    # 而不是让 Python 再实现一遍二进制格式（避免两份实现漂移）。
    if(TARGET qlgl)
        add_executable(qlblob
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/qlblob/qlblob.c
            ${QL_UI_GEN})
        add_dependencies(qlblob qlui)
        target_include_directories(qlblob PRIVATE ${QL_UI_OUT})
        target_link_libraries(qlblob PRIVATE qlgl)

        set(QL_UI_BLOB ${QL_UI_OUT}/ui_demo.qlui)
        add_custom_command(
            OUTPUT ${QL_UI_BLOB}
            COMMAND qlblob ${QL_UI_BLOB}
            DEPENDS qlblob ${QL_UI_GEN}
            COMMENT "生成 ui_demo.qlui")
        add_custom_target(qlui_blob ALL DEPENDS ${QL_UI_BLOB})
    endif()
endif()
if(NOT QLGL_BUILD_TEXT_PARSER)
    # 全库唯一吃外部文本的地方；不编它就没有任何运行期解析
    list(REMOVE_ITEM QLGL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/qlui_parse.c)
    target_sources(qlgl PRIVATE ${QLGL_SRC})
endif()

option(QLGL_BUILD_WAYLAND "构建 Linux Wayland 窗口后端" ON)
option(QLGL_BUILD_GL "构建 OpenGL ES 3.0 / EGL GPU 后端" ON)

if(QLGL_BUILD_GL AND UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(QL_EGL IMPORTED_TARGET egl)
        pkg_check_modules(QL_GLES IMPORTED_TARGET glesv2)
    endif()
    if(QL_EGL_FOUND AND QL_GLES_FOUND AND X11_FOUND)
        target_sources(qlgl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/port/qlgfx_gl.c)
        target_compile_definitions(qlgl PUBLIC QLGL_ENABLE_GL=1)
        set(QLGL_ENABLE_GL_FLAG 1)
        target_link_libraries(qlgl PUBLIC PkgConfig::QL_EGL PkgConfig::QL_GLES)
        message(STATUS "QLGL: GL(ES3/EGL) GPU 后端已启用")
    else()
        message(STATUS "QLGL: 未找到 EGL/GLESv2，跳过 GPU 后端")
    endif()
endif()

if(QLGL_BUILD_WAYLAND AND UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner)
    if(PkgConfig_FOUND AND WAYLAND_SCANNER_EXECUTABLE)
        pkg_check_modules(WAYLAND_CLIENT IMPORTED_TARGET wayland-client)
    endif()
    if(WAYLAND_CLIENT_FOUND AND WAYLAND_SCANNER_EXECUTABLE)
        set(QL_WL_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/port/protocols)
        set(QL_WL_GEN_DIR   ${CMAKE_CURRENT_BINARY_DIR}/wayland)
        set(QL_WL_HDR ${QL_WL_GEN_DIR}/xdg-shell-client-protocol.h)
        set(QL_WL_SRC ${QL_WL_GEN_DIR}/xdg-shell-protocol.c)
        add_custom_command(
            OUTPUT ${QL_WL_HDR} ${QL_WL_SRC}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${QL_WL_GEN_DIR}
            COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header
                    ${QL_WL_PROTO_DIR}/xdg-shell.xml ${QL_WL_HDR}
            COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code
                    ${QL_WL_PROTO_DIR}/xdg-shell.xml ${QL_WL_SRC}
            DEPENDS ${QL_WL_PROTO_DIR}/xdg-shell.xml
            COMMENT "生成 xdg-shell 协议代码")
        target_sources(qlgl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/port/qldisp_wayland.c
                                    ${QL_WL_SRC})
        target_include_directories(qlgl PUBLIC $<BUILD_INTERFACE:${QL_WL_GEN_DIR}>)
        target_compile_definitions(qlgl PUBLIC QLGL_ENABLE_WAYLAND=1)
        target_link_libraries(qlgl PUBLIC PkgConfig::WAYLAND_CLIENT)
        message(STATUS "QLGL: Wayland 窗口后端已启用")
    else()
        message(STATUS "QLGL: 未找到 wayland-client / wayland-scanner，跳过 Wayland 后端")
    endif()
endif()
option(QLGL_BUILD_X11 "构建 Linux X11 窗口后端" ON)

if(QLGL_BUILD_X11 AND UNIX AND NOT APPLE)
    if(X11_FOUND)
        target_sources(qlgl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/port/qldisp_x11.c)
        target_compile_definitions(qlgl PUBLIC QLGL_ENABLE_X11=1)
        if(TARGET X11::X11)
            target_link_libraries(qlgl PUBLIC X11::X11)
        else()
            target_include_directories(qlgl PUBLIC ${X11_INCLUDE_DIR})
            target_link_libraries(qlgl PUBLIC ${X11_LIBRARIES})
        endif()
        # _NET_WM_SYNC_REQUEST（缩放时和窗口管理器对齐帧）要 XSync，位于 libXext
        if(X11_Xext_FOUND)
            target_compile_definitions(qlgl PRIVATE QLGL_HAVE_XSYNC=1)
            if(TARGET X11::Xext)
                target_link_libraries(qlgl PUBLIC X11::Xext)
            else()
                target_link_libraries(qlgl PUBLIC ${X11_Xext_LIB})
            endif()
        endif()
        message(STATUS "QLGL: X11 窗口后端已启用")
    else()
        message(STATUS "QLGL: 未找到 X11，跳过窗口后端")
    endif()
endif()

if(QLGL_BUILD_TESTS)
    enable_testing()
    file(GLOB QLGL_TEST_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/*/*.c)
    foreach(test_src ${QLGL_TEST_SRC})
        get_filename_component(test_name ${test_src} NAME_WE)
        # test/mcu 是裸机基准（arm-none-eabi + QEMU），不走主机的测试构建；
        # 它有自己的入口 test/mcu/build.sh。
        if(test_src MATCHES "/test/mcu/")
            continue()
        endif()
        if(test_name MATCHES "^test_web_ui$|^test_gl_vs_cpu$|^example_gl_window$|^example_bench_fps$|^test_ui_blob$|^example_ui_blob$|^test_flex$|^test_flex_auto$|^test_render$|^test_dpr$|^test_transition$|^test_keyframes$|^test_clip$|^test_scale$|^test_flip$|^test_blur$|^test_line_height$|^test_text_wrap$|^test_html_tags$|^test_text_style$|^test_scroll$|^test_input_edit$|^test_select$|^test_progress$|^test_details$|^test_dialog$|^test_image$|^test_golden$|^test_text_parse$|^test_text_reject$|^test_anim_stress$|^test_gl_dirty$|^test_pct_height$|^test_blur_dirty$|^test_opacity$|^test_css_units$|^test_pseudo_text$|^test_transition_shorthand$|^test_text_adv$|^test_css_vars$|^test_media$|^test_calc$|^test_b4$|^test_b5$|^test_m8$|^test_grid$|^test_layer$|^test_transform$|^test_table$|^test_columns$|^test_font_stack$|^test_text_shape$|^test_b9$|^example_anim_window$|^example_flip_window$|^example_showcase_window$|^example_bench_ui$")
            set(_ql_ui_sources ${QL_UI_GEN})
            add_executable(${test_name} ${test_src} ${_ql_ui_sources})
            add_dependencies(${test_name} qlui)
            target_include_directories(${test_name} PRIVATE ${QL_UI_OUT})
            if(test_name STREQUAL "test_text_parse")
                add_dependencies(${test_name} qlui_flex)
                target_compile_definitions(${test_name} PRIVATE
                    QL_TEXT_CASE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/web/flex")
            endif()
            if(test_name STREQUAL "test_golden")
                add_dependencies(${test_name} qlui_flex)
                target_compile_definitions(${test_name} PRIVATE
                    QL_GL_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/golden")
            endif()
            if(test_name STREQUAL "test_ui_blob")
                add_dependencies(${test_name} qlui_blob)
                target_compile_definitions(${test_name} PRIVATE
                    QL_UI_BLOB_PATH="${QL_UI_OUT}/ui_demo.qlui")
            endif()
            if(test_name STREQUAL "example_bench_ui")
                add_dependencies(${test_name} qlui_flex)
                target_sources(${test_name} PRIVATE ${QL_UI_OUT}/ui_flex_bench100.c)
            endif()
            if(test_name STREQUAL "test_flex" OR test_name STREQUAL "test_render" OR test_name STREQUAL "test_dpr" OR test_name STREQUAL "test_transition" OR test_name STREQUAL "test_keyframes" OR test_name STREQUAL "example_anim_window"
               OR test_name STREQUAL "test_gl_vs_cpu"
               OR test_name STREQUAL "test_gl_dirty"
               OR test_name STREQUAL "test_anim_stress"
               OR test_name STREQUAL "test_clip"
               OR test_name STREQUAL "test_scale"
               OR test_name STREQUAL "test_flip"
               OR test_name STREQUAL "test_blur"
               OR test_name STREQUAL "test_golden"
               OR test_name STREQUAL "test_text_parse"
               OR test_name STREQUAL "example_flip_window"
               OR test_name STREQUAL "example_showcase_window")
                add_dependencies(${test_name} qlui_flex)
                # 只要 doc 本体；_blob.c 会重复定义 qlui_blob_doc()
                foreach(c ${QL_FLEX_CASES})
                    target_sources(${test_name} PRIVATE ${QL_UI_OUT}/ui_flex_${c}.c)
                endforeach()
            endif()
            if(test_name STREQUAL "example_ui_blob")
                add_dependencies(${test_name} qlui_blob)
                target_compile_definitions(${test_name} PRIVATE
                    QL_UI_BLOB_DEFAULT="${QL_UI_OUT}/ui_demo.qlui")
            endif()
        else()
            add_executable(${test_name} ${test_src})
        endif()
        # 对拍语料目录：放在分支外面，因为它们不需要生成好的 doc
        if(test_name STREQUAL "test_png_assets" OR test_name STREQUAL "test_png_fuzz"
           OR test_name STREQUAL "test_image_backend")
            target_compile_definitions(${test_name} PRIVATE
                QL_PNG_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/assets/png")
        endif()
        if(test_name STREQUAL "test_gif_assets" OR test_name STREQUAL "test_jpeg")
            target_compile_definitions(${test_name} PRIVATE
                QL_IMG_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/assets/img")
        endif()
        target_link_libraries(${test_name} PRIVATE qlgl)
        target_compile_options(${test_name} PRIVATE -Wall -Wextra)
        # 窗口示例会进入事件循环，不能当单元测试跑
        if(NOT test_name MATCHES "^example_")
            add_test(NAME ${test_name} COMMAND ${test_name})
        endif()
    endforeach()

    # 生成器的错误路径（"明确失败"红线）现在由 test_text_reject 覆盖：
    # qlui.py 退化成宿主工具的壳之后，接受/拒绝集合只剩 C 侧那一份。

    # qltext blob（不经过编译器直接出 .qlui）必须与提交的基准逐字节相同。
    # 这条顺带证明"宿主工具 + 运行期"用的是同一段解析代码。
    set(QL_DIRECT_BLOB ${QL_UI_OUT}/direct_row.qlui)
    add_custom_command(
        OUTPUT ${QL_DIRECT_BLOB}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${QL_UI_OUT}
        COMMAND qltext blob --html ${QL_UI_IN}/flex/row.html
                --css ${QL_UI_IN}/flex/row.css --name flex_row --out ${QL_DIRECT_BLOB}
        DEPENDS qltext ${QL_UI_IN}/flex/row.html ${QL_UI_IN}/flex/row.css
        COMMENT "qltext blob 直接产出 .qlui")
    add_custom_target(qltext_blob ALL DEPENDS ${QL_DIRECT_BLOB})
    # `qltext check --platform` 的降级报告（退出码 3 = 有降级项）
    add_test(NAME test_qltext_platform
             COMMAND ${CMAKE_COMMAND}
                     -DQLTEXT=$<TARGET_FILE:qltext>
                     -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/test/assets
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/test/assets/platform_report.cmake)
    add_test(NAME test_qltext_blob
             COMMAND ${CMAKE_COMMAND} -E compare_files ${QL_DIRECT_BLOB}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test/golden/row.qlui)

    # 构建期转码（qltext cc --root）：生成物与基准逐字节比对。
    # 样例是 BMP，raw-only 预设把 BMP 解码器砍掉了 → 这条测不了，就不注册。
    if(NOT DEFINED QL_FEAT_IMG_BMP OR QL_FEAT_IMG_BMP)
        add_test(NAME test_qltext_assets
                 COMMAND ${CMAKE_COMMAND}
                         -DQLTEXT=$<TARGET_FILE:qltext>
                         -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/test/assets
                         -DBIN=${CMAKE_CURRENT_BINARY_DIR}
                         -DQL_FEAT_IMG_PLATFORM=${QL_FEAT_IMG_PLATFORM}
                         -DQL_FEAT_IMG_LIBJPEG=${QL_FEAT_IMG_LIBJPEG}
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/test/assets/gen_assets.cmake)
    endif()
endif()

    # ---- benchmark 回归 ----
    #
    # 每个维度只跑 0.05 s（跑完 8 个维度约 0.4 s），然后自检"最后一帧不是空白"。
    # 这条不是性能门槛（CI 机器抖动会让 FPS 断言天天红），而是**功能门槛**：
    # 场景被改坏、渲染管线静默画不出东西时，它会立刻失败。
    #
    # 想在本机看性能门槛：bench --backend cpu --offscreen（人工读表）。
    if(QLGL_BUILD_BENCHMARK)
        add_test(NAME test_bench_check
                 COMMAND bench --backend cpu --offscreen --seconds 0.05 --check)
        set_tests_properties(test_bench_check PROPERTIES TIMEOUT 120)
    endif()

# 重新生成类型 id 表：cmake --build build --target qlgen
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(qlgen
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/qlgen/qlgen.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "扫描 src/**/*.c 重新生成类型 id 表"
    )
endif()

# ---- example/：基本例程（每个文件夹都是一个完整可运行的程序） ----
if(QLGL_BUILD_EXAMPLES)
    add_subdirectory(example)
endif()

# ---- benchmark/：可视化性能基准（CPU / GL 两个后端，同一场景） ----
if(QLGL_BUILD_BENCHMARK)
    add_subdirectory(benchmark)
endif()

# ---- 文档站：doc/**/*.md → doc/html/*.html（需要 python3-markdown） ----
if(Python3_Interpreter_FOUND)
    add_custom_target(doc_html
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/doc/gen_html.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "生成 HTML 文档到 doc/html/"
        VERBATIM)
endif()

# ---- 安装与打包（M7）：cmake --install + find_package(qlgl) + pkg-config ----
#
# 目标：别人的工程里三行就能用上 ——
#     find_package(qlgl REQUIRED)
#     target_link_libraries(app PRIVATE qlgl::qlgl)
# 或者非 CMake 的工程：
#     pkg-config --cflags --libs qlgl
#
# 说明：能力开关（`QL_FEAT_*`）**不随包导出** —— 它们是"这一份构建的属性"，
# 静态库的调用方如果编译期看到的宏与库里不一致，结构体布局就会错位。
# 所以 `qlgl.pc` 与 `qlglConfig.cmake` 只导出 include 目录与链接依赖。
include(GNUInstallDirs)

install(TARGETS qlgl
        EXPORT qlglTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/qlgl
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h")

# 宿主工具：构建期把 HTML/CSS 编成 C（目标机不需要它）
if(TARGET qltext)
    install(TARGETS qltext RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT qlglTargets
        FILE qlglTargets.cmake
        NAMESPACE qlgl::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qlgl)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/qlglConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

# 导出的 target 里有 imported target（PkgConfig::QL_EGL / X11::X11 …），
# 消费方的 CMake **必须**先把它们找出来，否则 include(qlglTargets.cmake) 会报
# "target was not found"。所以 config 文件里按"这份构建到底链了谁"补上依赖。
set(QLGL_CONFIG_DEPS "include(CMakeFindDependencyMacro)\n")
if(QLGL_ENABLE_GL_FLAG OR WAYLAND_CLIENT_FOUND)
    string(APPEND QLGL_CONFIG_DEPS "find_dependency(PkgConfig)\n")
endif()
if(QLGL_ENABLE_GL_FLAG)
    string(APPEND QLGL_CONFIG_DEPS "pkg_check_modules(QL_EGL IMPORTED_TARGET egl)\n")
    string(APPEND QLGL_CONFIG_DEPS "pkg_check_modules(QL_GLES IMPORTED_TARGET glesv2)\n")
endif()
if(WAYLAND_CLIENT_FOUND)
    string(APPEND QLGL_CONFIG_DEPS
        "pkg_check_modules(WAYLAND_CLIENT IMPORTED_TARGET wayland-client)\n")
endif()
if(QLGL_FREETYPE_ENABLED)
    string(APPEND QLGL_CONFIG_DEPS "find_dependency(Freetype)\n")
endif()
if(QLGL_BUILD_JPEG AND JPEG_FOUND)
    string(APPEND QLGL_CONFIG_DEPS "find_dependency(JPEG)\n")
endif()
if(QLGL_BUILD_X11 AND X11_FOUND)
    string(APPEND QLGL_CONFIG_DEPS "find_dependency(X11)\n")
endif()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/qlglConfig.cmake
"${QLGL_CONFIG_DEPS}include(\"\${CMAKE_CURRENT_LIST_DIR}/qlglTargets.cmake\")\n")
install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/qlglConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/qlglConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qlgl)

# pkg-config：链接依赖要跟着"这份构建到底链了谁"走
set(QL_PC_LIBS "-lm")
if(QLGL_FREETYPE_ENABLED)
    set(QL_PC_LIBS "${QL_PC_LIBS} -lfreetype")
endif()
if(QLGL_BUILD_JPEG AND JPEG_FOUND)
    set(QL_PC_LIBS "${QL_PC_LIBS} -ljpeg")
endif()
if(QLGL_ENABLE_GL_FLAG)
    set(QL_PC_LIBS "${QL_PC_LIBS} -lEGL -lGLESv2")
endif()
# 窗口后端：静态库按需拉取对象文件，但 pkg-config 的调用方通常一次链完，
# 所以把它们的库名也写上（没装这些 -dev 包的机器会在链接期明确报错）。
if(QLGL_BUILD_X11 AND X11_FOUND)
    set(QL_PC_LIBS "${QL_PC_LIBS} -lX11")
    if(X11_Xext_FOUND)
        set(QL_PC_LIBS "${QL_PC_LIBS} -lXext")
    endif()
endif()
if(WAYLAND_CLIENT_FOUND)
    set(QL_PC_LIBS "${QL_PC_LIBS} -lwayland-client")
endif()
if(QL_CFG_HAVE_DLOPEN_PC)
    set(QL_PC_LIBS "${QL_PC_LIBS}")
elseif(UNIX)
    set(QL_PC_LIBS "${QL_PC_LIBS} -ldl")
endif()
# 用 `${pcfiledir}` 相对定位，这样 `cmake --install --prefix <任意目录>` 之后
# 不用重新生成 .pc（否则里面写死的 prefix 会指向 configure 时的默认路径）。
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/qlgl.pc
"prefix=\${pcfiledir}/../..
exec_prefix=\${prefix}
libdir=\${pcfiledir}/..
includedir=\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}

Name: qlgl
Description: CSS+HTML 的 GUI 库（HTML/CSS 编译成静态数据、CPU/GPU 双后端、可裁到 MCU）
Version: ${PROJECT_VERSION}
Cflags: -I\${includedir}
Libs: -L\${libdir} -lqlgl ${QL_PC_LIBS}
")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/qlgl.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

message(STATUS "QLGL: 安装目标已就绪（cmake --install <build> --prefix /usr/local）")
