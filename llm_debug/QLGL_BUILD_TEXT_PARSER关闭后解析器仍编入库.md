# QLGL_BUILD_TEXT_PARSER 关闭后解析器仍编入库

> **核对 2026-11-15**：✅ 已修 + 本次补配置期守卫 —— `CMakeLists.txt:279-285` 用 `HEADER_FILE_ONLY`；复测全新配置构建日志中 `qlui_parse` 出现 0 次；另加 `TEXT_PARSER=OFF` 与 tests/examples/benchmark 互斥的 `FATAL_ERROR`（`CMakeLists.txt:97-104`）

## 问题

`QLGL_SRC` 在 `add_library(qlgl STATIC ${QLGL_SRC})` 时已经加入目标。后续 `QLGL_BUILD_TEXT_PARSER=OFF` 分支仅从普通 CMake 列表 `QLGL_SRC` 删除 `src/ui/qlui_parse.c`，再把剩余列表传给 `target_sources()`；这不会从已创建的 `qlgl` 目标中移除此前加入的解析器源文件。

## 影响

关闭运行期 HTML/CSS 解析器的 MCU/精简构建仍会编译并链接 `qlui_parse.c`，无法达到选项承诺的零字节裁剪效果，也可能保留解析器依赖及其代码占用。

## 依据

- `CMakeLists.txt` 先通过 `add_library(qlgl STATIC ${QLGL_SRC})` 加入全部 `src/*.c`。
- `if(NOT QLGL_BUILD_TEXT_PARSER)` 分支在目标创建后执行 `list(REMOVE_ITEM QLGL_SRC ...)`，该操作只更新变量，不更新已有目标的 SOURCES 属性。

## 验证状态

根据 CMake 目标源文件添加顺序静态确认；本轮未重新配置独立的 `QLGL_BUILD_TEXT_PARSER=OFF` 构建。
