# `QLRect` 边界运算未防护有符号溢出

> **核对 2026-11-15**：✅ 已修 —— `include/qlgl/qlgeom.h:53-74` 新增 `ql_coord_add_sat/sub_sat`，right/bottom/intersect/union 均改用饱和运算

## 问题位置

- `include/qlgl/qlgeom.h`：`ql_rect_right()`、`ql_rect_bottom()`
- `include/qlgl/qlgeom.h`：`ql_rect_intersect()`、`ql_rect_union()`

## 问题说明

`QLRect` 的坐标和尺寸均为有符号 32 位整数。右/下边界直接计算 `x + w`、`y + h`；交集和并集还会执行边界相减。若调用方传入仍可表示的坐标与尺寸、但它们的和超出 `int32_t` 范围，表达式会发生有符号溢出，属于未定义行为。

例如 `ql_rect_right(ql_rect_make(INT32_MAX, 0, 1, 1))` 就会溢出。后续相交、合并及使用这些结果的裁剪逻辑都可能受到影响。

## 影响

接近坐标类型边界的矩形计算结果不可靠，可能错误裁剪或命中。常见屏幕尺寸及坐标范围不受影响。

## 检查状态

由公开内联函数的算术表达式静态确认；没有修改项目代码。
