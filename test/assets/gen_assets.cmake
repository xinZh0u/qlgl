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

message(STATUS "qltext assets: 生成物与基准逐字节一致")
