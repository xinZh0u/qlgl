# 进阶

前六章走完了"能写界面"。这一章讲清剩下的三件事：绘制后端怎么选、
数据是怎么流到屏幕上的、以及接下来该读什么。

## 两个绘制后端

同一份界面数据可以喂给两个后端，产物应当**逐像素接近**：

| 后端 | 入口 | 适合 |
|---|---|---|
| CPU | `ql_ui_render_canvas(ui, &canvas)` | 无 GPU 环境、MCU、离屏截图、单元测试 |
| GPU | `ql_ui_render(ui, ql_gfx_gl_draw(gfx))` | 桌面/移动的正常路径 |

CPU 后端是**行跨度扫描**（每行算一个区间，内层 SIMD），
GPU 后端是**单批次**（整帧一次 `glDrawArrays`，所有矩形变成带 SDF 参数的四边形）。

一致性由测试守着：

```
test_gl_vs_cpu      GPU vs CPU 逐像素（含字形几何），27 个页面 ≤ 2%
test_gl_dirty       保留式脏区：局部重绘 == 整屏重画
test_golden         27 个 CSS 用例 × (blob 逐字节 + 基准图容差)
```

性能基线（1080p，Release）：

| | 耗时 |
|---|---|
| CPU 光栅全屏 | 2.19 ms |
| 320×240 全屏 | 0.30 ms |
| 211 节点布局 | 0.037 ms |
| 211 节点全量绘制 | 0.348 ms |
| 80×60 局部重绘 | 0.014 ms |

## 一帧的数据流

```
  HTML/CSS 文本
       │  ql_ui_load_text()  ← 构建期由 qltext 走同一段代码
       ▼
   QLUIDoc          扁平数组 + uint16 下标（nodes/styles/text/fonts/keyframes/anims）
       │  ql_ui_build()
       ▼
   QLUIRoot         运行期节点树（带 border_box / paint_box / 状态样式指针）
       │  ql_ui_layout(w, h)
       ▼
   QLDraw2D         后端无关的绘制接口（fill_rect / draw_text / backdrop_blur ...）
       │
       ├─ CPU → QLCanvas → 帧缓冲内存
       └─ GPU → QLGfxGL  → 顶点缓冲 → 一次 draw call
```

每一层都能单独用：

```c
/* 只要绘制，不要界面？直接用画布 */
QLCanvas c;
ql_canvas_from_disp(&c, disp);
ql_canvas_fill_round_rect(&c, ql_rect_make(10, 10, 80, 40), 8, 0x2E6BE6, 255);

/* 只要界面树，自己画到任意目标？用 QLDraw2D */
QLDraw2D d2d;
ql_draw2d_canvas_init(&d2d, &c);
ql_ui_render(ui, &d2d);
```

## 脏区与保留模式

窗口是双缓冲的，`eglSwapBuffers` 之后**后缓冲内容没有保证**。所以"只重绘脏区"
必须先把上一帧存在自己的 FBO 里：

```c
ql_gfx_gl_set_retained(gfx, 1);              /* 打开 */

int full = ql_gfx_gl_begin_rects(gfx, bg, areas, n);
if (full) ql_ui_render(ui, d2d);             /* 首帧 / 改尺寸 → 整屏 */
else      for (int i = 0; i < n; i++)
              ql_ui_render_rect(ui, d2d, areas[i]);
ql_gfx_gl_end(gfx);                          /* 帧末 blit 到窗口 */
```

`begin_rects` 返回 1 表示"这一帧必须整屏重画"（首帧、FBO 刚建、被要求清屏）。
这条如果搞错，症状是"动画一停所有元素消失"—— 很值得记一下。

## 组件分层

QLGL 不是"只有一套 API"，而是四层，看你需要多底层：

| 层 | 头文件 | 是什么 |
|---|---|---|
| 对象系统 | `qlobject.h` / `qlprop.h` / `qlclass.h` | C 里的类、属性、信号（GTK 那套思路） |
| 控件 | `qlwidget.h` / `qllabel.h` | 用 C 拼界面的传统路子 |
| 界面（推荐） | `qlui.h` / `qluitext.h` | HTML+CSS → 静态数据 |
| 绘制 | `qldraw2d.h` / `qlcanvas.h` / `qlgfx_gl.h` | 直接画 |
| 文本 | `qlfont.h` / `qlfont_ft.h` | 字体与字形 |

日常写界面用第三层。第一、二层在需要"用 C 组装"或者做自定义控件时用。
第四、五层在写自定义绘制时用。

## 明确不做的事

这些不是"还没做"，是**已经决定不做**：

| 不做 | 原因 |
|---|---|
| 保留式场景图 / render tree | 会变成 GTK4 的 GSK，每帧重建节点，性能优势全丢 |
| 运行期 CSS 匹配 | 样式在编译期消解，这是性能与体积的根基 |
| DOM API / JS 引擎 | 需要高兼容就嵌系统 WebView，不在本项目范围 |
| 可访问性 / IME / 多媒体 | 只做 UI |
| 静默降级 | 不支持的 CSS/HTML 一律报错 |
| 热路径 malloc | 所有热路径预分配 |

## 接下来读什么

| 你想做的事 | 读 |
|---|---|
| 弄清 CSS 到底支持哪些写法 | [QLUI API 手册](../manual/ui/QLUI%20API手册.html) |
| 搞清楚"为什么这么设计、坑在哪" | [QLUI 阅读指南](../manual/ui/QLUI阅读指南.html) |
| 运行期解析的边界与上限 | [QLUIText API 手册](../manual/uitext/QLUIText%20API手册.html) |
| 渲染管线、字形、脏区细节 | [QLRender 阅读指南](../manual/render/QLRender阅读指南.html) |
| 动效的求值模型 | [QLAnim 阅读指南](../manual/anim/QLAnim阅读指南.html) |
| 自己写控件 | [QLWidget 阅读指南](../manual/widget/QLWidget阅读指南.html) |
| 用 C 拼界面（不用 CSS） | [QLObject 阅读指南](../manual/object/QLObject阅读指南.html) |

## 例程一览

所有例程在 `example/`，每个都是独立的 `.c`：

| 程序 | 讲什么 |
|---|---|
| `01_window` | 最小闭环：编译 → 建树 → 布局 → 显示 |
| `02_text` | 字号 / 颜色 / 对齐 / 文本规则 |
| `03_layout` | flex / 百分比 / 绝对定位 / 读布局结果 |
| `04_interaction` | 状态样式 / 事件派发 / 点击回调 / 合成事件 |
| `05_animation` | `@keyframes` / 脏区 / 动画属性白名单 |
| `06_theme` | 同一份 HTML 三份 CSS / `.qlui` 缓存 |

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
for e in 01_window 02_text 03_layout 04_interaction 05_animation 06_theme; do
    ./build/example/example_$e --frames 3 --dump /tmp/$e.ppm
done
```

`--frames N` 跑 N 帧后退出，`--dump path.ppm` 存最后一帧。
没有窗口时会自动退回离屏渲染，所以 CI 里也能跑。

---

上一章：[换皮肤](06_theme.html)
