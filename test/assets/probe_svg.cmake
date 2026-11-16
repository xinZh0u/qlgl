# qlsvgprobe（SVG 资产兼容性自检）的回归测试。
#
#   cmake -DQLSVGPROBE=<工具> -DSRC=<fixture> -P probe_svg.cmake
#
# 断言：
#   1. 目录里有坏文件/外部引用 → 退出码 1，输出含 fail/block 与特性名；
#   2. 单个正常文件 → 退出码 0；
#   3. 含不支持特性但能解码的文件 → 默认 0、--strict 1。

execute_process(
    COMMAND "${QLSVGPROBE}" --size 48 "${SRC}/svg"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 1)
    message(FATAL_ERROR "目录扫描期望退出码 1，实际 ${rc}\n${out}${err}")
endif()
# bad.svg：PlutoSVG 会 fail，ThorVG 可能把它当空图（warn/空白），
# 所以这里只断言"文件被报出来"，退出码由 block 保证为 1。
foreach(needle "bad.svg" "block" "text")
    string(FIND "${out}" "${needle}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "输出里缺少 '${needle}'：\n${out}${err}")
    endif()
endforeach()
# clipPath 是 PlutoSVG 不支持的；ThorVG 默认后端会把它过滤掉
if(HAVE_PLUTO)
    string(FIND "${out}" "clipPath" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "默认 PlutoSVG 后端应报出 clipPath：\n${out}${err}")
    endif()
endif()

# 正常文件：ok + 0
execute_process(
    COMMAND "${QLSVGPROBE}" "${SRC}/svg/star.svg"
    RESULT_VARIABLE rc2 OUTPUT_VARIABLE out2 ERROR_VARIABLE err2)
if(NOT rc2 EQUAL 0)
    message(FATAL_ERROR "star.svg 期望退出码 0，实际 ${rc2}\n${out2}${err2}")
endif()
string(FIND "${out2}" "ok" pos)
if(pos EQUAL -1)
    message(FATAL_ERROR "star.svg 应报告 ok：\n${out2}")
endif()

# 不支持特性：默认放行、--strict 失败
execute_process(
    COMMAND "${QLSVGPROBE}" "${SRC}/svg/unsupported.svg"
    RESULT_VARIABLE rc3 OUTPUT_VARIABLE out3 ERROR_VARIABLE err3)
if(NOT rc3 EQUAL 0)
    message(FATAL_ERROR "unsupported.svg 默认应通过，实际 ${rc3}\n${out3}${err3}")
endif()
string(FIND "${out3}" "warn" pos)
if(pos EQUAL -1)
    message(FATAL_ERROR "unsupported.svg 应报告 warn：\n${out3}")
endif()

execute_process(
    COMMAND "${QLSVGPROBE}" --strict "${SRC}/svg/unsupported.svg"
    RESULT_VARIABLE rc4 OUTPUT_VARIABLE out4 ERROR_VARIABLE err4)
if(NOT rc4 EQUAL 1)
    message(FATAL_ERROR "--strict 期望退出码 1，实际 ${rc4}\n${out4}${err4}")
endif()

# 同时编了两个后端时：--compare 必须能跑，并且对坏文件返回非 0
if(HAVE_BOTH)
    execute_process(
        COMMAND "${QLSVGPROBE}" --compare "${SRC}/svg/star.svg"
        RESULT_VARIABLE rc5 OUTPUT_VARIABLE out5 ERROR_VARIABLE err5)
    if(NOT rc5 EQUAL 0)
        message(FATAL_ERROR "--compare（star）期望 0，实际 ${rc5}\n${out5}${err5}")
    endif()
    foreach(needle "plutosvg" "thorvg")
        string(FIND "${out5}" "${needle}" pos)
        if(pos EQUAL -1)
            message(FATAL_ERROR "--compare 输出里缺少 '${needle}'：\n${out5}")
        endif()
    endforeach()

    execute_process(
        COMMAND "${QLSVGPROBE}" --compare "${SRC}/svg/bad.svg"
        RESULT_VARIABLE rc6 OUTPUT_VARIABLE out6 ERROR_VARIABLE err6)
    if(NOT rc6 EQUAL 1)
        message(FATAL_ERROR "--compare（bad）期望 1，实际 ${rc6}\n${out6}${err6}")
    endif()
endif()

message(STATUS "qlsvgprobe：状态/特性/--strict/--compare 都符合预期")
