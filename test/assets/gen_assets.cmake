# 构建期转码的回归测试：跑一遍 qltext，把生成的 .c 与提交的基准逐字节比对。
#
#   cmake -DQLTEXT=<工具> -DSRC=<fixture 目录> -DBIN=<生成目录> -P gen_assets.cmake
#
file(MAKE_DIRECTORY "${BIN}/assets_out")

execute_process(
    COMMAND "${QLTEXT}" cc --html "${SRC}/app.html" --css "${SRC}/app.css"
            --name app --out "${BIN}/assets_out" --root "${SRC}"
            --pixel-format rgb565
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "qltext cc --root 失败（returncode=${rc}）")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${BIN}/assets_out/ui_app.c"
            "${SRC}/ui_app_expected.txt"
    RESULT_VARIABLE diff)
if(NOT diff EQUAL 0)
    message(FATAL_ERROR "生成的 ui_app.c 与基准不一致：改了生成器就要更新 ui_app_expected.txt")
endif()

# 第二份 fixture：**JPEG 与 GIF** 走构建期转码（JPEG 依赖 libjpeg 后端，
# GIF 走自有 LZW）。这个构建把两者都裁了时跳过 —— 那是另一条预设的合法组合。
if(EXISTS "${SRC}/img_ui/index.html")
    set(TRY_JPEG ON)
    if(DEFINED QL_FEAT_IMG_PLATFORM AND NOT QL_FEAT_IMG_PLATFORM)
        set(TRY_JPEG OFF)
    endif()
    if(DEFINED QL_FEAT_IMG_LIBJPEG AND NOT QL_FEAT_IMG_LIBJPEG)
        set(TRY_JPEG OFF)
    endif()
    if(TRY_JPEG)
        execute_process(
            COMMAND "${QLTEXT}" cc --html "${SRC}/img_ui/index.html"
                    --css "${SRC}/img_ui/index.css" --name img_ui
                    --out "${BIN}/assets_out" --root "${SRC}/img_ui"
                    --pixel-format rgb565
            RESULT_VARIABLE rc2)
        if(NOT rc2 EQUAL 0)
            message(FATAL_ERROR "qltext cc --root（JPEG/GIF）失败（returncode=${rc2}）")
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E compare_files
                    "${BIN}/assets_out/ui_img_ui.c"
                    "${SRC}/img_ui/ui_img_ui_expected.txt"
            RESULT_VARIABLE diff2)
        if(NOT diff2 EQUAL 0)
            message(FATAL_ERROR "ui_img_ui.c 与基准不一致（改了生成器就要更新基准）")
        endif()
    endif()
endif()

message(STATUS "qltext assets: 生成物与基准逐字节一致")
