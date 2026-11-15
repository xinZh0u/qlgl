# qlperf 进程 GPU 累计计数器下降时差分下溢

> **核对 2026-11-15**：✅ 已修 —— `tools/qlperf/…:309` 先比较 `gpu >= last_gpu`，回退按 0 计

## 问题

`proc_gpu_ns()` 每次采样都把进程当前仍打开的 DRM fdinfo 引擎累计值求和。进程关闭 DRM 文件描述符或重建 GPU 上下文时，这个总和可能小于上一采样值。`sample_once()` 直接用无符号表达式 `gpu - *last_gpu` 计算增量，下降时会下溢为接近 `UINT64_MAX` 的巨大值。

## 影响

一次正常的 GPU 句柄关闭/重建可能令 qlperf 报告极大的进程 GPU 百分比，并误判 GPU 瓶颈。

## 依据

- `tools/qlperf/qlperf.c` 的 `proc_gpu_ns()` 对当前 `/proc/<pid>/fdinfo` 中仍存在的引擎计数求和。
- `sample_once()` 将 `gpu` 与 `last_gpu` 都存为 `unsigned long long`，未检查新值是否小于旧值便相减。

## 验证状态

静态追踪确认无符号差分在计数器回退时下溢；未构造真实 DRM fdinfo 计数器变化进行动态复现。
