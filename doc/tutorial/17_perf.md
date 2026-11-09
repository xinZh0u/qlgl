# 性能诊断与 MCU 基准

这一章讲**怎么量**，以及怎么判读量出来的数。

起因是一个真实的问题：拖动任何窗口（例程、基准）都会"卡一下"。肉眼看就是掉帧，
但真相是**我们一帧只花了 0.1ms，卡的那 58ms 全在等垂直同步**。
这一章把定位过程完整走一遍，顺便把工具交给读者。

```bash
# 终端 A：跑应用，退出时打帧时间
./build/example/example_22_media --seconds 6 --stats

# 终端 B：监控 CPU/GPU，同时不停缩放窗口复现
./build/qlperf watch --seconds 6 --resize "22 media" <pid>
```

---

## 1. 三条命令，三件工具

| 工具 | 回答什么问题 | 命令 |
|---|---|---|
| `--stats` | 每帧多久？**其中等 vsync 占多少**？ | `./build/example/example_22_media --seconds 5 --stats` |
| `qlperf` | CPU 满了吗？GPU 满了吗？谁在吃资源？ | `./build/qlperf run -- ./build/example/example_22_media` |
| `test/mcu/build.sh` | 同一份代码在 MCU 上要多久？ | `./test/mcu/build.sh` |

三者互补：`--stats` 看**帧**，`qlperf` 看**进程与设备**，MCU 基准看**另一种硬件**。

---

## 2. 第一步：先看帧时间，并且把它拆开

```bash
./build/example/example_22_media --seconds 5 --stats
```

```
帧时间：p50 6.93ms  p95 7.37ms  max 15.12ms  （300 帧，p50 ≈ 144 fps）
其中等 swap：p50 6.84ms  max 15.04ms
```

**第二行是重点**。`frame_us - swap_us` 才是我们自己花的（布局 + 提交 + 光栅），
`swap_us` 是 `eglSwapBuffers` 里等垂直同步 / 合成器取帧。

上面这组数的读法：

- p50 = 6.93ms，其中 6.84ms 在等 swap → **我们只花了 0.09ms**；
- 6.94ms 正好是 1/144Hz → "144fps" 是**刷新率上限**，不是我们的上限。

> 只看"跑不满 300fps"就以为有性能问题，是最常见的误判。先确认
> `p50 ≈ 1/刷新率` 且 `swap` 占大头。

### 例程的循环参数

| 参数 | 含义 |
|---|---|
| （默认） | **事件驱动**：没有脏区就阻塞等事件（静止 0 帧、0% CPU） |
| `--spin` | 每帧都渲染（基准测试用，会吃满 vsync） |
| `--frames N` | 渲染 N 帧后退出（隐含 `--spin`） |
| `--seconds S` | 跑 S 秒后退出 |
| `--stats` | 退出时打帧时间 p50/p95/max + 等 swap 时间 |
| `--vsync 0/1` | 强制开关垂直同步（拖动缩放期间库会自动临时关闭） |

---

## 3. 第二步：CPU 还是 GPU？

```bash
./build/qlperf run --interval 200 --seconds 6 -- ./build/example/example_22_media
```

```
  t(s)   CPU%  GPU进程%  GPU整机%  RSS(MB)  线程
  0.25    5.0      3.2      18.0    151.0    11
  ...
==== qlperf 汇总（6.0s，30 个采样点）====
  CPU       p50   5.0%   p95  10.0%   max  10.3%
  GPU(进程) avg   3.6%   max   5.3%
  GPU(整机) avg  21.9%   max  24.0%
  RSS       max 151.0 MB
  结论：CPU/GPU 都没满 → 时间花在等 vsync / 合成器上（…）
```

数据来源（都是内核/驱动给的，不猜）：

| 指标 | 来源 | 口径 |
|---|---|---|
| CPU% | `/proc/<pid>/stat` 的 `utime+stime` 差分 | 单核 100%（多线程可能 > 100） |
| GPU 进程% | `/proc/<pid>/fdinfo` 里 `drm-engine-*` 纳秒计数器 | 引擎时间 / 墙钟，> 100 表示多引擎并行 |
| GPU 整机% | `/sys/class/drm/cardN/device/gpu_busy_percent` | 驱动报告（amdgpu 等） |
| RSS / 线程 | `/proc/<pid>/status` | |

最后那句"结论"是工具给的建议，别跳过 —— 三个方向（CPU 满 / GPU 满 / 等 vsync）
的改法完全不同。

---

## 4. 第三步：复现"拖动窗口掉帧"

`qlperf` 能主动缩放窗口（X11 `XResizeWindow`，每 ~8ms 一步，模拟拖边框的密度）：

```bash
./build/example/example_22_media --seconds 6 --stats &      # 终端 A
./build/qlperf watch --seconds 5 --resize "22 media" $!      # 终端 B
```

修复前实测：

| 指标 | 值 |
|---|---|
| CPU p95 | 4% |
| GPU（进程） | 0.1% |
| 帧时间 | p50 6.93ms，**max 58.50ms** |
| 其中等 swap | p50 6.82ms，**max 58.40ms** |

**卡的那一帧 58.4ms 全在 `eglSwapBuffers` 里**。再用 `--vsync 0` 复测，卡顿消失
（max 4.4ms）—— 结论：不是画得慢，是"等 vblank"被合成器拖住了
（Mesa 的 Present/XCB 在合成器忙于重配窗口时收不到 frame callback，
最坏会等到它 1s 超时，实测复现过 `swap max 1000.8ms`）。

### 库里为此做的两件事

1. **事件驱动循环**：没有脏区就 `ql_gfx_gl_wait()`（`poll(2)`）阻塞等事件。
   以前每秒 swap 144 次（界面静止也一样），现在静止 0 帧、0% CPU；
   拖动时"一个 ConfigureNotify 画一帧"。
2. **拖动期间不跟 vblank**：收到 ConfigureNotify 后 200ms 内
   `eglSwapInterval = 0`，拖动结束自动恢复（GTK/Qt 在互动缩放期间也这么做）。

修复后：

| 指标 | 修复前 | 修复后 |
|---|---|---|
| 拖动时 max 帧时间 | 58.5ms（偶发 1000ms） | **≤ 7.3ms** |
| 拖动时 p50 | 6.9ms | **1.1ms** |
| 静止时 CPU / 帧数 | 3~8% / 144fps | **0% / 0fps** |

---

## 5. 第四步：MCU 上要多久（QEMU）

PC 上的数字**不能**外推到 MCU：主频、cache、内存带宽、有没有 FPU 全不一样。
所以有一套裸机基准：`test/mcu/`。

```bash
./test/mcu/build.sh              # 交叉编译 + 在 QEMU 里跑
./test/mcu/build.sh --no-run    # 只看 ROM/RAM 占用
```

它做的事：

```
bench.html/css ──qltext cc──▶ 静态 C 文档（运行期不解析）
                              │
src/ 同一份源码 ──────────────┼──▶ arm-none-eabi-gcc ──▶ bench.elf
                              │
静态 RGB565 帧缓冲 + 半主机时钟 ┘        └──▶ qemu-system-arm -M mps2-an386 -cpu cortex-m4
```

跑的是**和 PC 完全同一份** `src/` 源码，只换三样：文档是构建期静态数据、
画布是一块静态 `uint16_t` 数组（CPU 光栅）、时钟走 ARM 半主机。

### 口径（必须看，否则数字会被读成别的意思）

QEMU 用 `-icount shift=0,align=off,sleep=off`：**每条指令推进 1ns 虚拟时间**，
`SYS_ELAPSED` 读到的是虚拟纳秒，数值上 ≈ 指令数（1 IPC 参考机）。

- 实测把宿主机 CPU 打满后数值不变（±1%），所以它是**与主机无关**的确定性指标；
- 换算到 M4@200MHz：同一条指令要 5ns，所以 `时间 ≈ 虚拟 ns × 5`
  （程序里按 `ns / 200 = µs` 打印）；
- **这是乐观下界**：M4 没 cache、flash 取指有等待周期，实际 IPC 常 0.6~0.8，
  真实板子按 ×1.5~2 估。

### 当前数字

`240×160 RGB565`，21 节点 / 11 样式（flex 三列卡 + 圆角 + 边框 + 线性渐变 +
折行文本 + 裁剪容器）：

| 动作 | 虚拟 ns | M4@200MHz | 说明 |
|---|---|---|---|
| `ql_ui_layout()` | ≈ 63 500 | **0.32 ms** | 整树重排（改尺寸 / 断点切换时才跑） |
| 一帧整屏光栅 | ≈ 1 200 000 | **6.0 ms** | 含清屏，38 400 px → 约 31 周期/px |
| 1×1 脏区重绘 | ≈ 4 700 | **0.024 ms** | 静止时每帧的真实成本 |

两条结论：

1. 60fps 的 16.7ms 预算里，"光栅 6.0 + 布局 0.3" 只占 **36%**；
2. **局部更新只要 0.02ms** —— MCU 上真正该做的是脏区重绘，不是每帧全画。

### 裸机上必须自己接的两样东西

| 东西 | MCU 上的做法 |
|---|---|
| 内存 | `ql_mem_set_allocator()` 接到静态池，或像 `test/mcu/syscalls.c` 那样用 `_sbrk` 从一个静态数组切 |
| 时间 | `ql_anim_set_time_source()`（裸机没有 `clock_gettime`，**默认时间源是 NULL**，不接的话动画会停在第一帧） |

---

## 6. 判读清单（按顺序问）

1. **CPU 满了吗？** `qlperf` 的 p95 > 85% → 找热点（布局？光栅？事件循环空转？）。
2. **GPU 满了吗？** 进程 GPU% 或整机 GPU% > 85% → 填充率 / 批次问题。
3. **两个都没满，帧时间却大？** 看 `--stats` 的"等 swap"占比：
   - 等 swap 占大头 → 在等 vblank / 合成器，**不是我们的问题**；
     拖动时爆到几十上百 ms → 见 §4；
   - 我们自己占大头 → `qlperf --csv` 存曲线，配合 `--frames` 二分定位。
4. **"跑不满 xxx fps"？** 先看 p50 是不是正好 1/刷新率（如 6.94ms ≈ 144Hz）。
   是 → 就是 vsync 上限，别改代码。
5. **要在 MCU 上跑？** 用 `test/mcu`，别拿 PC 数字乘个系数就下结论。

---

## 7. 想扩展的话

| 想加什么 | 改哪里 |
|---|---|
| 新的 MCU 场景（纯文本 / 卡片网格 / 全屏渐变） | 换 `test/mcu/bench.html` + `bench.css`，或复制一套目录 |
| 另一块板子 / CPU | `CPU=cortex-m7 BOARD=mps2-an500 ./test/mcu/build.sh` |
| 采样进别的地方（Prometheus 等） | `qlperf --csv` 已经给出每次采样 |
| 更细的帧内拆分 | `ql_gfx_gl_frame_us()` / `ql_gfx_gl_swap_us()`，需要的话再加自己的计时点 |

完整的工具参数与设计取舍见 [性能诊断](../design/性能诊断.html)；
MCU 侧的文件清单见 `test/mcu/README.md`。
