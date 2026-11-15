# benchmark CPU 脏区模式仍将整屏 flush

> **核对 2026-11-15**：✅ 已修 —— `benchmark/bench.c:143-179` 记录本帧脏区并集，`end()` 只 flush 该范围（DPR≠1 退整屏）

## 问题

CPU 后端的 `cpu_begin_rects()` 只清理传入的脏矩形，但 `cpu_end()` 无论当前模式如何，都调用显示驱动的 `flush()` 并传入完整画布矩形。

## 影响

窗口模式下，X11 会对整屏执行 `XPutImage`，Wayland 会将整屏拷入 shm 缓冲并提交，即使本帧只绘制一个小脏区。因此 `--mode dirty` 仍承担全屏传输/拷贝成本，不能准确衡量 CPU 后端端到端的增量重绘收益；离屏内存后端的结果也不能代表窗口呈现开销。

## 依据

- `benchmark/bench.c` 的 `cpu_begin_rects()` 按矩形清理脏区。
- 同文件 `cpu_end()` 固定 flush `(0, 0, disp->width, disp->height)`，不保留或提交脏区范围。
- X11 与 Wayland 显示驱动的 `flush()` 都按传入区域传输像素。

## 验证状态

静态追踪确认所有 CPU 模式都执行整屏 flush；未使用窗口驱动进行性能对比。
