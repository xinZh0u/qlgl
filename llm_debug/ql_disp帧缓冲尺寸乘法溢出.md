# 显示帧缓冲尺寸计算可能溢出并分配不足

> **核对 2026-11-15**：✅ 本次补修 —— `src/draw/qldisp.c:149-160` 判定从 `SIZE_MAX` 改为 `UINT32_MAX`，避免 64 位平台上恒假后 `(uint32_t)` 截断；回归 `test_canvas`（65536×16384 ARGB8888 → NULL）

## 问题位置

- `src/draw/qldisp.c`：`ql_disp_buffer_size()`
- `src/draw/qldisp.c`：`ql_disp_init_fb()`
- `src/draw/qldisp.c`：`ql_disp_mem_create()`

## 问题说明

显示宽高为正的 `int32_t`，但行字节数和分配尺寸直接用 `uint32_t` 相乘，没有先用更宽类型校验：

- `ql_disp_buffer_size()` 计算 `width * bytes * height` 后直接返回，结果可能回绕为过小的缓冲区大小。
- `ql_disp_init_fb()` 的 `width * bytes` 可能回绕，使不足以容纳一行像素的 `stride` 被接受。
- `ql_disp_mem_create()` 还将行大小乘以冗余系数 4，再乘高度；这两步也可能回绕，导致像素存储不足。

之后的清屏、像素访问等逻辑仍按原始宽高迭代，可能越界访问帧缓冲区。

## 影响

使用极大宽高创建或包装显示缓冲区时，尺寸校验/分配可能低估所需内存，后续绘制可能发生越界写或越界读。普通窗口尺寸不受影响。

## 检查状态

由尺寸计算的整数宽度静态确认；没有修改项目代码。
