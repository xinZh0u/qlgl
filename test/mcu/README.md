# MCU 基准：在 QEMU 的 Cortex-M4 上量一帧

PC 上的数字**不能**外推到 MCU。这里用 `qemu-system-arm` 跑一块 Cortex-M4
（`mps2-an386`），跑的是**和 PC 完全同一份** `src/` 源码，只换三样东西：

| 差别 | PC 例程 | 这里 |
|---|---|---|
| 界面来源 | 运行期解析 HTML/CSS | 构建期静态数据（`qltext cc`） |
| 画布 | EGL/GL 或内存画布 | 一块静态 `uint16_t` 帧缓冲（RGB565），纯 CPU 光栅 |
| 时钟 | `clock_gettime` | ARM 半主机 `SYS_ELAPSED`（虚拟纳秒） |

## 跑

```bash
./build/qltext 需要先建好（默认就会建）
./test/mcu/build.sh                 # 编译 + 在 QEMU 里跑
./test/mcu/build.sh --no-run        # 只编译（看 ROM/RAM 占用）
```

换 CPU / 板子 / 目标主频：

```bash
CPU=cortex-m4 BOARD=mps2-an386 FREQ_MHZ=200 ./test/mcu/build.sh
```

## 口径（重要）

- QEMU 用 `-icount shift=0,align=off,sleep=off`：**每条指令推进 1ns 虚拟时间**，
  所以 `SYS_ELAPSED` 的虚拟纳秒数 ≈ 指令数（1 IPC 参考机）。
  实测把宿主机 CPU 打满后数值不变（±1%），所以它可以当"与主机无关"的确定性指标用。
- 换算到 M4@200MHz：同一条指令在 200MHz 上要 **5ns**，所以
  `时间 ≈ 虚拟 ns × 5`（程序里按 `ns / 200 = µs` 打印）。
- **这是乐观下界**：M4 没有 cache，从 flash 取指有等待周期，实际 IPC 常低到 0.6~0.8；
  另外 QEMU 不计内存总线争用、DMA 抢占。真实板子上把这里的数字 ×1.5~2 更接近。

## 能力包裁剪实测

同一份源码、同一个场景，只改 `-DQL_FEAT_*`：

| 配置 | `text` |
|---|---|
| 全量 | **139,260 B** |
| 关掉图像管线（`QL_FEAT_UI_IMAGE=0` + `QL_FEAT_IMG_*=0`） | 131,144 B（**−8,116 B，−6.2%**） |
| 关掉 Grid（`QL_FEAT_CSS_GRID=0`） | 134,732 B（**−4,528 B，−3.3%**） |
| 再关掉 border-image（`QL_FEAT_CSS_BORDER_IMAGE=0`） | 132,556 B（**−2,176 B，−1.6%**） |

内建 ASCII 位图字体（12px + 16px 的位图与字形表）≈ 9 KB。
参考量级：`libharfbuzz.so` 的 `text` 是 1,178 KB（8.9 倍），`libfreetype.so` 813 KB，
一份全量 CJK TTF 21 MB —— 所以"真字体栈"在 MCU 档是恒裁的。

## 布局的边际成本（同场景只改卡片数量）

| 节点数 | 布局 | 备注 |
|---|---|---|
| 12 | 0.23 ms | 扁平 `flex-wrap` |
| 39 | 0.82 ms | 同上 |
| 75 | 1.94 ms | 同上 |
| **15（嵌套 3 列 flex）** | **0.427 ms** | 同一视觉，多 3 个 wrapper → +86% |

拟合斜率 ≈ 4.4k 指令/节点 ≈ 22 µs/节点 @200 MHz；**一层 wrapper ≈ 3 个叶子节点**。
这条读数就是"Grid 在 MCU 上反而更快"的依据（见
[CSS3 覆盖与裁剪 §11](../../doc/手册/ui/CSS3覆盖与裁剪.md)）。

## 当前数字

`240x160 RGB565`，21 个节点 / 11 条样式（`bench.html`：flex 三列卡片 +
圆角 + 边框 + 线性渐变 + 文本折行 + 裁剪容器）：

| 动作 | 虚拟 ns | M4@200MHz |
|---|---|---|
| `ql_ui_layout()` | ≈ 84 000 | **0.42 ms** |
| 一帧整屏光栅（含 clear） | ≈ 1 600 000 | **8.0 ms** |
| 1×1 脏区重绘 | ≈ 6 600 | **0.033 ms** |

> 绝对值会随 QEMU 版本漂（本机两次差过 ~30%）；**同一批连着跑的相对比较**才可靠。

| 动作 | 虚拟 ns | M4@200MHz | 说明 |
|---|---|---|---|
| `ql_ui_layout()` | ≈ 63 500 | **0.32 ms** | 整棵树重排一次（改尺寸/断点切换时才跑） |
| 一帧 `ql_ui_render()` | ≈ 1 200 000 | **6.0 ms** | 含清屏，整屏 38 400 px → 约 31 周期/px |
| 1×1 脏区重绘 | ≈ 4 700 | **0.024 ms** | 静止时每帧的真实成本 |

读法：

- 240×160 / 200MHz 下"整屏重画"6ms → 理论上限 ~160fps，但**局部更新**只有 0.02ms 量级，
  所以 MCU 上该走"脏区重绘"，而不是每帧全画；
- 60fps 的预算是 16.7ms：6ms 光栅 + 0.3ms 布局 = **36%** 的预算，
  余量足够再去画一块 240×240 的屏（按面积线性放大到 ~13.5ms，仍能上 60fps）。

## 文件

| 文件 | 作用 |
|---|---|
| `bench.c` | 基准主程序（静态帧缓冲、半主机计时、打印） |
| `bench.html` / `bench.css` | 基准页面（构建期编成静态 C） |
| `startup.c` | 向量表 + Reset_Handler |
| `syscalls.c` | `_sbrk`（静态堆）+ `_write`/`_exit`（半主机） |
| `link.ld` | mps2-an386 的内存布局 |
| `build.sh` | 交叉编译 + QEMU 运行（一条命令搞定） |
