# `qltext check --platform` 的回归测试（M7）。
#
# 两个目标平台各跑一次，断言：
#   mcu → 退出码 3（有降级项），并报出表格/滤镜/Grid/双向；
#   pc  → 退出码 0，并说"不需要裁任何特性"。
# 用 cmake -P 而不是 add_test 直接跑，是因为"退出码 3"本身是契约的一部分，
# 而 ctest 会把任何非零退出码当失败。

execute_process(COMMAND ${QLTEXT} check --html ${SRC}/platform_report.html
                        --css ${SRC}/platform_report.css --platform mcu
                RESULT_VARIABLE rc_out OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc_out EQUAL 3)
    message(FATAL_ERROR "mcu 档期望退出码 3，实际 ${rc_out}\n${out}${err}")
endif()
# 注意 Grid **不在** mcu 的裁剪清单里（实测它反而降低 MCU 布局时间，
# 见《CSS3 覆盖与裁剪》§18.9），所以这里只断言那三样会被报出来。
foreach(needle "裁掉" "表格" "滤镜" "双向")
    string(FIND "${out}" "${needle}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "mcu 报告里缺少 '${needle}'：\n${out}")
    endif()
endforeach()

execute_process(COMMAND ${QLTEXT} check --html ${SRC}/platform_report.html
                        --css ${SRC}/platform_report.css --platform pc
                RESULT_VARIABLE rc_pc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc_pc EQUAL 0)
    message(FATAL_ERROR "pc 档期望退出码 0，实际 ${rc_pc}\n${out}${err}")
endif()
string(FIND "${out}" "不需要裁任何特性" pos)
if(pos EQUAL -1)
    message(FATAL_ERROR "pc 报告没有给出 '不需要裁任何特性'：\n${out}")
endif()
message(STATUS "platform 报告：mcu/pc 两侧都符合预期")
