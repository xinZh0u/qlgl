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
foreach(needle "fail" "block" "clipPath" "text")
    string(FIND "${out}" "${needle}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "输出里缺少 '${needle}'：\n${out}${err}")
    endif()
endforeach()

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

message(STATUS "qlsvgprobe：状态/特性/--strict 都符合预期")
