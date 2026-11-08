cmake_minimum_required(VERSION 3.16)

project(qlgl LANGUAGES C VERSION 0.1.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)          # 严格 -std=c11，防止误用 POSIX/编译器扩展
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

file(GLOB_RECURSE QLGL_SRC CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
)

add_library(qlgl STATIC ${QLGL_SRC})
target_include_directories(qlgl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(qlgl PRIVATE -Wall -Wextra)

option(QLGL_BUILD_TESTS "构建 test/ 下的示例与单元测试" ON)
option(QLGL_BUILD_FREETYPE "构建 FreeType 矢量字体后端（中文/任意字号）" ON)

# ---- FreeType：矢量字体后端（PC/MPU；MCU 关掉即可，内置位图字体仍然可用） ----
if(QLGL_BUILD_FREETYPE)
    find_package(Freetype QUIET)
    if(FREETYPE_FOUND)
        target_sources(qlgl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/text/qlfont_ft.c)
        target_include_directories(qlgl PUBLIC ${FREETYPE_INCLUDE_DIRS})
        target_compile_definitions(qlgl PUBLIC QLGL_ENABLE_FREETYPE=1)
        target_link_libraries(qlgl PUBLIC ${FREETYPE_LIBRARIES})
        message(STATUS "QLGL: FreeType 矢量字体后端已启用")
    else()
        message(STATUS "QLGL: 未找到 FreeType，只用内置位图字体")
    endif()
endif()

if(UNIX AND NOT APPLE)
    find_package(X11)
endif()

# ---- HTML+CSS → C（相当于 Qt 的 uic）：把 test/web 下的页面编译成静态数据 ----
find_package(Python3 COMPONENTS Interpreter QUIET)
if(QLGL_BUILD_TESTS AND Python3_Interpreter_FOUND)
    set(QL_UI_IN  ${CMAKE_CURRENT_SOURCE_DIR}/test/web)
    set(QL_UI_OUT ${CMAKE_CURRENT_BINARY_DIR}/ui)
    set(QL_UI_GEN ${QL_UI_OUT}/ui_demo.c ${QL_UI_OUT}/ui_demo.h
                  ${QL_UI_OUT}/ui_demo_blob.c)
    add_custom_command(
        OUTPUT ${QL_UI_GEN}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${QL_UI_OUT}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/qlui/qlui.py
                --html ${QL_UI_IN}/ui_demo.html
                --css  ${QL_UI_IN}/ui_demo.css
                --out  ${QL_UI_OUT}
        DEPENDS ${QL_UI_IN}/ui_demo.html ${QL_UI_IN}/ui_demo.css
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/qlui/qlui.py
        COMMENT "编译 HTML+CSS 为 C")
    add_custom_target(qlui ALL DEPENDS ${QL_UI_GEN})

    # ---- flex 布局用例：每个 html/css 组合编译成一个 doc ----
    set(QL_FLEX_CASES row grow justify align wrap column
                    posabs posfix posrel zidx ovf mixed frac paint dpr trans bench100)
    set(QL_FLEX_GEN "")
    foreach(c ${QL_FLEX_CASES})
        set(_c  ${CMAKE_CURRENT_SOURCE_DIR}/test/web/flex/${c})
        set(_g  ${QL_UI_OUT}/ui_flex_${c}.c)
        add_custom_command(
            OUTPUT ${_g} ${QL_UI_OUT}/ui_flex_${c}.h
                   ${QL_UI_OUT}/ui_flex_${c}_blob.c
            COMMAND ${CMAKE_COMMAND} -E make_directory ${QL_UI_OUT}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/qlui/qlui.py
                    --html ${_c}.html --css ${_c}.css
                    --name flex_${c} --out ${QL_UI_OUT}
            DEPENDS ${_c}.html ${_c}.css ${CMAKE_CURRENT_SOURCE_DIR}/tools/qlui/qlui.py
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
        target_include_directories(qlgl PUBLIC ${QL_WL_GEN_DIR})
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
        if(test_name MATCHES "^test_web_ui$|^test_gl_vs_cpu$|^example_gl_window$|^example_bench_fps$|^test_ui_blob$|^example_ui_blob$|^test_flex$|^test_render$|^test_dpr$|^test_transition$|^example_bench_ui$" AND Python3_Interpreter_FOUND)
            set(_ql_ui_sources ${QL_UI_GEN})
            add_executable(${test_name} ${test_src} ${_ql_ui_sources})
            add_dependencies(${test_name} qlui)
            target_include_directories(${test_name} PRIVATE ${QL_UI_OUT})
            if(test_name STREQUAL "test_ui_blob")
                add_dependencies(${test_name} qlui_blob)
                target_compile_definitions(${test_name} PRIVATE
                    QL_UI_BLOB_PATH="${QL_UI_OUT}/ui_demo.qlui")
            endif()
            if(test_name STREQUAL "example_bench_ui")
                add_dependencies(${test_name} qlui_flex)
                target_sources(${test_name} PRIVATE ${QL_UI_OUT}/ui_flex_bench100.c)
            endif()
            if(test_name STREQUAL "test_flex" OR test_name STREQUAL "test_render" OR test_name STREQUAL "test_dpr" OR test_name STREQUAL "test_transition")
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
        target_link_libraries(${test_name} PRIVATE qlgl)
        target_compile_options(${test_name} PRIVATE -Wall -Wextra)
        # 窗口示例会进入事件循环，不能当单元测试跑
        if(NOT test_name MATCHES "^example_")
            add_test(NAME ${test_name} COMMAND ${test_name})
        endif()
    endforeach()
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
