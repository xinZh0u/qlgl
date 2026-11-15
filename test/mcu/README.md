# MCU：在 QEMU 的 Cortex-M4 上跑 QLGL

PC 上的数字**不能**外推到 MCU。这里用 `qemu-system-arm` 跑一块 Cortex-M4
（`mps2-an386`），跑的是**和 PC 完全同一份** `src/` 源码，只换三样东西：

| 差别 | PC 例程 | 这里 |
|---|---|---|
| 界面来源 | 运行期解析 HTML/CSS | 构建期静态数据（`qltext cc`） |
| 画布 | EGL/GL 或内存画布 | 一块静态 `uint16_t` 帧缓冲（RGB565），纯 CPU 光栅 |
| 时钟 | `clock_gettime` | ARM 半主机 `SYS_ELAPSED`（虚拟纳秒） |

这里有两个目标程序：

| 目标 | 命令 | 内容 |
|---|---|---|
| `bench`（默认） | `./test/mcu/build.sh` | 量一帧布局/光栅，打印虚拟 ns |
| `window` | `./test/mcu/build.sh window` | **`QLWindow` 验收**：板级平台 + SysTick + 触摸 + 脏区刷屏 |

```bash
./build/qltext 需要先建好（默认就会建）
./test/mcu/build.sh                 # 基准：编译 + 在 QEMU 里跑
./test/mcu/build.sh window          # QLWindow 验收：PASS/FAIL
./test/mcu/build.sh --no-run        # 只编译（看 ROM/RAM 占用）
./test/mcu/build.sh window --no-run
```

换 CPU / 板子 / 目标主频：

```bash
CPU=cortex-m4 BOARD=mps2-an386 FREQ_MHZ=200 ./test/mcu/build.sh
```

## QLWindow 验收（`window` 目标）

这是 P2 的落地点：**同一份 `QLWindow` + `qlui`，在没有 OS、没有文件系统、
没有 GL 的 Cortex-M4 上跑起来**。板级后端在 `qlplatform_board.c`（定义强符号
`qlgl_platform_default()`，覆盖库里的桌面弱符号），应用代码只有：

```c
qlgl_init();
QLUIRoot *ui = ql_ui_build(&ui_mcu_window_doc);   /* 构建期编好的页面 */
QLWindow *win = ql_window_create("mcu", 240, 160, QL_WINDOW_CPU);
ql_window_bind_ui(win, ui);
while (ql_window_poll(win)) { /* 事件/动画/脏区都在 poll 里 */ }
```

`window.c` 断言的内容：

| 检查 | 断言 |
|---|---|
| 后端覆盖 | `qlgl_platform()->name == "mcu-board"` |
| 首帧 | `poll()` 触发一次整屏 flush（240×160 = 38 400 px） |
| 样式光栅 | 按钮中心像素 == `#0969DA` 的 RGB565 |
| 触摸 | 指针移动 → `:hover` 变色；只刷脏区（刷新像素 < 整屏） |
| 点击 | `ql_ui_set_click_cb` 触发 1 次 |
| 手动重绘 | `ql_window_render()` → 一次整屏 flush |
| 时钟 | SysTick 毫秒数在 64 次 `poll` 后前进 |
| 固定面板 | `ql_window_resize()` → `QL_ERR_UNSUPPORTED` |

两个踩过的坑（写在这里避免重犯）：

- **SysTick 是异常 15 → 向量表第 16 项（索引 15）**。少填一项会让中断跳到
  0 地址 → HardFault → 默认 handler 死循环，现象是"静默卡住、没有任何输出"。
- **`QLWindow` 的统计缓冲按需分配**：`--stats` 的 3×1024 个采样点 ≈ 12KB，
  48KB 的 MCU 堆经不起在 `ql_window_create()` 里就吃掉。

链接注意：`qlui.c` 仍保留运行期解析入口（引用 `qlui_parse.c`），MCU 构建要开
`-ffunction-sections -fdata-sections` + `-Wl,--gc-sections`，
`build.sh` 已经默认打开。

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
| 全量（`bench`，含 21 节点页面） | **138,332 B** |
| 关掉图像管线（`QL_FEAT_UI_IMAGE=0` + `QL_FEAT_IMG_*=0`） | 127,708 B（**−10,624 B，−7.7%**） |
| 关掉 Grid（`QL_FEAT_CSS_GRID=0`） | 133,768 B（**−4,564 B，−3.3%**） |
| 再关掉 border-image（`QL_FEAT_CSS_BORDER_IMAGE=0`） | 131,848 B（**−1,920 B，−1.4%**） |

带 `QLWindow` 的 `window` 目标（板级平台 + 窗口层 + 68 节点页面）：
`text = 158,940 B`，`bss = 222,492 B`（含 76.8KB 帧缓冲 + 76.8KB 模拟面板 + 48KB 堆）。

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

| 动作 | 虚拟 ns | M4@200MHz | 说明 |
|---|---|---|---|
| `ql_ui_layout()` | ≈ 93 700 | **0.47 ms** | 整棵树重排一次（改尺寸/断点切换时才跑） |
| 一帧 `ql_ui_render()` | ≈ 1 358 600 | **6.8 ms** | 含清屏，整屏 38 400 px → 约 35 周期/px |
| 1×1 脏区重绘 | ≈ 4 800 | **0.024 ms** | 静止时每帧的真实成本 |

> 绝对值会随 QEMU 版本/编译器漂（本机两次差过 ~30%）；**同一批连着跑的相对比较**才可靠。

读法：

- 240×160 / 200MHz 下"整屏重画"6.8ms → 理论上限 ~147fps，但**局部更新**只有 0.02ms 量级，
  所以 MCU 上该走"脏区重绘"，而不是每帧全画；
- 60fps 的预算是 16.7ms：6.8ms 光栅 + 0.46ms 布局 = **43%** 的预算，
  余量还够再画一块 240×240 的屏（按面积线性放大到 ~15ms，勉强 60fps）。

## 文件

| 文件 | 作用 |
|---|---|
| `bench.c` | 基准主程序（静态帧缓冲、半主机计时、打印） |
| `bench.html` / `bench.css` | 基准页面（构建期编成静态 C） |
| `window.c` | **`QLWindow` 验收**：后端覆盖、首帧、触摸、脏区、SysTick |
| `window.html` / `window.css` | 验收页面（一个按钮 + `:hover`） |
| `qlplatform_board.c` | 板级 `QLPlatform`：RGB565 面板、按键环形队列、SysTick、强符号覆盖 |
| `qlplatform_board.h` | 板级小接口（模拟触摸/看面板内容/统计） |
| `startup.c` | 向量表（含 SysTick 第 16 项）+ Reset_Handler |
| `syscalls.c` | `_sbrk`（静态堆）+ `_write`/`_exit`（半主机）+ 文件 syscall 桩 |
| `link.ld` | mps2-an386 的内存布局 |
| `build.sh` | 交叉编译 + QEMU 运行（`bench` / `window` 两个目标） |
