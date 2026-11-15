# 毛玻璃模糊半径过大触发整数溢出

## 问题

`ql_canvas_backdrop_blur()` / `ql_canvas_backdrop_blur_round()` 接受 `int radius`，但实现没有限制半径上界：

- `box_radius_of_sigma()` 将计算结果转换回 `int`，极大半径可能产生超出 `INT_MAX` 的浮点转整数转换；
- `backdrop_blur_impl()` 计算 `boxr * 3 + refract + 2`，可能发生有符号整数溢出；
- `box_pass()` 计算 `2 * r + 1`，并用 `for (int k = -r; k <= r; k++)` 遍历，极大半径会溢出或导致循环异常。

## 影响

使用异常大的毛玻璃半径时，可能触发未定义行为、错误的采样边界、分配失败或不受控的长时间循环。调用方即使只处理很小的画布，也无法通过有限图像尺寸阻止该路径。

## 依据

- `src/draw/qlcanvas.c` 的 `backdrop_blur_impl()`、`box_radius_of_sigma()` 和 `box_pass()` 均直接使用未经上界约束的半径参与上述运算。
- 两个公开毛玻璃入口均把 `radius` 原样传给 `backdrop_blur_impl()`。

## 验证状态

静态检查确认整数边界问题；未用极端半径做动态测试，以避免触发未定义行为或长时间循环。
