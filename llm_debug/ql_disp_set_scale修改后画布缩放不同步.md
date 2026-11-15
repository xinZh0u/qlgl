# `ql_disp_set_scale()` 不会同步已构造画布的缩放

> **核对 2026-11-15**：✅ 已修 + 本次补一处 —— 画布侧 `ql_canvas_scale_fx()` 实时跟随（`qlcanvas.c:138`、`qlcanvas.h:67`）；本次补上桌面 CPU 窗口长期持有的 `QLDraw2D` 快照同步（`src/port/qlplatform_desktop.c:285-294`）

## 问题位置

- `include/qlgl/qldisp.h`：`ql_disp_set_scale()` 的契约注释
- `src/draw/qldisp.c`：`ql_disp_set_scale()`
- `src/draw/qlcanvas.c`：`ql_canvas_from_disp_clip()`

## 问题说明

公开头文件说明 `ql_disp_set_scale()` 会自动同步到已绑定驱动的 `QLCanvas`。但设置函数只更新 `driver->scale_fx`；构造画布时，`ql_canvas_from_disp_clip()` 仅把该值复制到 `canvas->scale_fx`，画布没有保留驱动指针或注册同步关系。

因此，先构造画布、再调用 `ql_disp_set_scale()` 时，驱动和已有画布的缩放值会不同；已有画布的 CSS 尺寸与 `QLDraw2D` 后续缩放仍使用旧值。

## 影响

运行期调整显示缩放后，已创建 CPU 画布的布局尺寸/绘制缩放可能继续沿用旧 DPR，与显示驱动实际设置不一致。测试 `test_dpr` 只在构造画布前设置缩放，未覆盖该顺序。

## 检查状态

由 API 注释、画布字段复制和缩放 setter 的实现对照确认；没有修改项目代码。
