# qlsvg（构建期 SVG → PNG）+ qltext（PNG → 目标像素格式）的端到端测试。
#
# 这条链路是 MCU 的用法：目标机不带 SVG 库，图标在构建机上预渲染。
# 断言"工具成功 + 尺寸/格式正确 + A8 不是全 0"，不做逐像素 golden。
#
#   cmake -DQLSVG=<工具> -DQLTEXT=<工具> -DSRC=<fixture> -DBIN=<生成目录>
#         -P gen_svg_tool.cmake

set(_work "${BIN}/svg_tool")
file(MAKE_DIRECTORY "${_work}")

# 1) 正常导出 + 覆盖率遮罩
execute_process(
    COMMAND "${QLSVG}" "${SRC}/svg/icon.svg" "${_work}/icon.png"
    RESULT_VARIABLE rc1 OUTPUT_VARIABLE out1 ERROR_VARIABLE err1)
if(NOT rc1 EQUAL 0)
    message(FATAL_ERROR "qlsvg（icon）失败 rc=${rc1}\n${out1}${err1}")
endif()
if(NOT EXISTS "${_work}/icon.png")
    message(FATAL_ERROR "qlsvg 没有产出 icon.png")
endif()

execute_process(
    COMMAND "${QLSVG}" "${SRC}/svg/star.svg" "${_work}/star.png"
            --alpha -w 48 -h 48
    RESULT_VARIABLE rc2 OUTPUT_VARIABLE out2 ERROR_VARIABLE err2)
if(NOT rc2 EQUAL 0)
    message(FATAL_ERROR "qlsvg（star --alpha）失败 rc=${rc2}\n${out2}${err2}")
endif()

# 2) 坏 SVG 必须失败（不能静默产出空白 PNG）
execute_process(
    COMMAND "${QLSVG}" "${SRC}/svg/bad.svg" "${_work}/bad.png"
    RESULT_VARIABLE rc3 OUTPUT_VARIABLE out3 ERROR_VARIABLE err3)
if(rc3 EQUAL 0)
    message(FATAL_ERROR "qlsvg 对畸形 SVG 仍然成功（应该失败）")
endif()

# 3) 走一遍 qltext：PNG → a8，检查尺寸/长度正确
file(WRITE "${_work}/index.html"
     "<div id=\"r\"><img src=\"icon.png\"><img src=\"star.png\"></div>")
file(WRITE "${_work}/index.css" "#r { width: 80px; height: 80px; }")

execute_process(
    COMMAND "${QLTEXT}" cc --html "${_work}/index.html" --css "${_work}/index.css"
            --name svg_tool --out "${_work}" --root "${_work}"
            --pixel-format a8
    RESULT_VARIABLE rc4 OUTPUT_VARIABLE out4 ERROR_VARIABLE err4)
if(NOT rc4 EQUAL 0)
    message(FATAL_ERROR "qltext（qlsvg 产物）失败 rc=${rc4}\n${out4}${err4}")
endif()

file(READ "${_work}/ui_svg_tool.c" gen)
foreach(needle
        ".len = 1024, .w = 32, .h = 32"
        ".len = 2304, .w = 48, .h = 48"
        "QL_IMAGE_A8")
    string(FIND "${gen}" "${needle}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "生成的 ui_svg_tool.c 里缺少 \"${needle}\"")
    endif()
endforeach()

string(REGEX MATCH "img_data\\[[0-9]+\\] = \\{([^}]*)\\}" _m "${gen}")
string(REGEX MATCH "0x[0-9A-F][1-9A-F]|0x[1-9A-F][0-9A-F]" _nz "${CMAKE_MATCH_1}")
if(_nz STREQUAL "")
    message(FATAL_ERROR "A8 遮罩全 0：qlsvg --alpha 的覆盖率没有传下去")
endif()

message(STATUS "qlsvg → qltext：尺寸/格式/覆盖率/错误路径都符合预期")
