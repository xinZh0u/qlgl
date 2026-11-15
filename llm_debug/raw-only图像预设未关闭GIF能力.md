# `raw-only` 图像预设仍启用 GIF 解码

> **核对 2026-11-15**：✅ 已修 —— `CMakeLists.txt:55-60` raw-only 分支置 `_ql_preset_gif=0`；复测 `flags.make` 里 `QL_FEAT_IMG_GIF=0`

## 问题

`CMakeLists.txt` 将 `_ql_preset_gif` 默认设为 `1`。`raw-only` 分支关闭 BMP、QOI、PNG 和平台图像后端，却没有关闭 GIF；后面的另一个 `elseif(QLGL_IMG_PRESET STREQUAL "raw-only")` 本想设 `_ql_preset_gif=0`，但条件重复，永远不可达。

因此使用 `QLGL_IMG_PRESET=raw-only` 且未单独覆盖 `QL_FEAT_IMG_GIF` 时，构建仍会启用 GIF 解码，与“只吃构建期 RAW”的预设说明不符。

## 影响

选择该预设的精简构建仍包含 GIF 解码能力及其代码/资源开销；使用者也可能误以为 GIF 已被禁用。

## 依据

- `CMakeLists.txt` 的图像预设初始化将 `_ql_preset_gif` 设为 `1`。
- `raw-only` 第一个分支未更改该值，之后重复的 `raw-only` 分支不可达。
- 只有当 `QL_FEAT_IMG_GIF` 未显式定义时，后续才使用 `_ql_preset_gif` 作为 GIF 能力值。

## 验证状态

由 CMake 配置条件静态确认；本次未单独执行 `raw-only` 配置并检查编译定义。
