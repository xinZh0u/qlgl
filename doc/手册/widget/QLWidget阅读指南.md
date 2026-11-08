# QLWidget / 绘制系统 阅读指南

> 本部分包含三层：**显示驱动**（qldisp）→ **画布**（qlcanvas）→ **控件与渲染器**（qlwidget / qlrender）。
> 目标读者：要读懂或修改 `src/draw/` 与 `src/widget/` 的人。
> 只想用 API，请看 [QLWidget API手册](./QLWidget%20API手册.md)。

---

## 1. 文件地图

| 文件 | 职责 | 阅读顺序 |
|---|---|---|
| `include/qlgl/qlgeom.h` | `ql_coord_t`、`ql_color_t`、`QLRect` 与矩形运算 | ① |
| `src/draw/qlgeom.c` | 圆角矩形的点包含测试 | ② |
| `include/qlgl/qldisp.h` | 显示驱动接口、像素格式、内存显示驱动 | ③ |
| `src/draw/qldisp.c` | 驱动注册、`mem` 后端、PPM 导出 | ④ |
| `include/qlgl/qlcanvas.h` | 画布结构与绘制原语 | ⑤ |
| `src/draw/qlcanvas.c` | 像素读写、填充、圆角、描边、线条、alpha 混合 | ⑥ |
| `include/qlgl/qlwidget.h` | 控件基类、属性 id、绘制函数类型 | ⑦ |
| `src/widget/qlwidget.c` | 属性实现、几何换算、默认绘制、绘制表 | ⑧ |
| `include/qlgl/qlrender.h` | 渲染器接口 | ⑨ |
| `src/draw/qlrender.c` | 脏矩形管理、对象树遍历、清屏与 flush | ⑩ |
| `include/qlgl/qldisp_x11.h` | Linux X11 后端接口 | ⑪ |
| `src/port/qldisp_x11.c` | X11 实现（XImage 直绘 + XPutImage 推脏区） | ⑫ |
| `include/qlgl/qldisp_wayland.h` | Linux Wayland 后端接口 | ⑬ |
| `src/port/qldisp_wayland.c` | Wayland 实现（xdg-shell + wl_shm 双缓冲） | ⑭ |
| `test/widget/*.c` | 可执行的说明 | ⑬ |

---

## 2. 三层关系

```
 你的代码                      渲染器                      驱动
 ────────                     ──────                     ────
 ql_widget_new_ex()  ──┐
 set 属性            ──┼─→ 脏矩形队列 ─→ ql_render() ─→ 清屏 + 遍历树 ─→ flush(area)
 ql_object_add_child()─┘                     │
                                             └─→ QLDraw2D → CPU 光栅 / GPU
```

**渲染模型：帧缓冲直绘，但出口是 `QLDraw2D`。**

- 驱动提供一块连续像素内存（`framebuffer` + `stride`）；
- 渲染器把控件直接画进去；
- 画完后调用 `flush(area)` 通知驱动"这个矩形变了"；
- 内存驱动（PC/测试）的 `flush` 只是计数；真实屏的驱动在这里把矩形搬到面板。

这样做的原因：可测试（测试直接读像素）、可移植（MCU 只需提供显存指针）。
代价是要求至少一整屏显存；需要"部分缓冲"的 MCU 后续再补行缓冲后端。

### 2.1 控件层也走 `QLDraw2D`（M4）

控件的绘制函数签名是

```c
typedef void (*ql_widget_draw_func)(QLWidget *self, QLDraw2D *d2d);
```

**不是 `QLCanvas *`**。这样同一棵控件树既能 CPU 光栅也能上 GPU，自定义控件只写一次：

```c
/* 自己管理上下文/窗口时 */
ql_gfx_gl_begin(gfx, bg);                        /* 自己清屏 */
ql_render_draw2d(ql_gfx_gl_draw(gfx), w, h);     /* 只画脏区 */
ql_gfx_gl_end(gfx);
```

`ql_render()` 内部就是把 `QLDraw2D` 包了一层 CPU 后端，再负责"清背景 + flush"。
`ql_render_draw2d()` **不清背景、不推屏、不清空脏区队列**——脏区由
`ql_render_invalidate_*` 管理，用什么颜色清屏由调用方决定（GPU 上通常是 `glClear`）。

`test_gl_vs_cpu` 里有一段专门对照：同一棵控件树分别画到 CPU 画布和 GL 离屏帧缓冲，
差异 0.29%（全在圆角边沿的 AA 上）。

### 2.1 三种现成后端

| 后端 | 场景 | 帧缓冲来源 |
|---|---|---|
| `ql_disp_mem_create` | 测试 / PC 离屏渲染 / 导出图片 | `ql_mem_calloc` |
| `ql_disp_init_fb` | 用户自己提供显存（MCU / 嵌入式 Linux） | 调用方传入 |
| `ql_disp_x11_create` | Linux 桌面窗口（X11） | **XImage 的像素内存**（零拷贝） |
| `ql_disp_wayland_create` | Linux 桌面窗口（Wayland） | 持久帧缓冲 + 2 个 shm 缓冲区（脏区拷贝） |

X11 后端没有额外拷贝：XImage 的 `data` 直接指向渲染器的帧缓冲，
`flush` 只是 `XPutImage` 一块脏矩形。因此它要求 X 的视觉与我们能写的格式匹配
（32/24bpp TrueColor 或 16bpp RGB565），不匹配会拒绝创建。

---

## 3. 关键数据结构

### 3.1 `QLDispDriver`

```c
struct QLDispDriver {
    ql_coord_t width, height;
    uint8_t    format;        /* QL_COLOR_RGB565 / RGB888 / ARGB8888 / L8 */
    uint8_t    rotation;      /* 预留 */
    void      *framebuffer;   /* 左上角第 0 字节 */
    uint32_t   stride;        /* 字节/行，可以大于 width*bytes */
    int      (*flush)(QLDispDriver *self, QLRect area);
    void      *user_data;
};
```

`stride` 故意允许大于最小行宽：内存驱动就把 `stride` 设成 `width * bytes * 4`，
这样测试顺带验证了"渲染器不假设行连续"。

### 3.2 `QLCanvas`

```c
typedef struct {
    uint8_t   *buffer;
    uint32_t   stride;
    ql_coord_t width, height;   /* 缓冲尺寸 */
    QLRect     clip;            /* 裁剪区，屏幕坐标 */
    uint8_t    format;
} QLCanvas;
```

**所有坐标都是屏幕绝对坐标**，不存在"画布局部坐标"的画布。
控件坐标是相对的，渲染器在遍历时累加成绝对坐标。

### 3.3 颜色

`ql_color_t` 是 `0xRRGGBB`，与像素格式无关。每个写入点都做一次转换：

| 格式 | 每像素 | 转换 |
|---|---|---|
| RGB565 | 2 字节 | `r>>3 / g>>2 / b>>3` |
| RGB888 | 3 字节 | 原样 |
| ARGB8888 | 4 字节 | 写入 `0xFF000000 \| rgb` |
| L8 | 1 字节 | `(77r + 150g + 29b) >> 8` |

因此 RGB565 下 `0x12 0x34 0x56` 会变成 `0x10 0x34 0x50`。
`ql_canvas_quantize()` 提前算出量化后的值，测试用它做精确断言。

---

## 4. 控件几何

```
root(0,0)
 ├── card (24,24)           ← 相对 root
 │     ├── bar  (16,16)     ← 相对 card，屏幕坐标 = (40,40)
 │     └── button(16,60)    ← 屏幕坐标 = (40,84)
 └── ...
```

- 属性 `X/Y` 是**相对父控件内容区**的偏移，父控件没有 border/padding 时内容区就是父矩形本身；
- `ql_widget_local_rect()` 返回相对坐标；
- `ql_widget_abs_rect()` 沿父链累加，返回屏幕坐标；
- 渲染器遍历时自己累加（不调用 `ql_widget_abs_rect`，省一次父链遍历）。

### 默认值

| 属性 | 默认 | 说明 |
|---|---|---|
| X / Y / WIDTH / HEIGHT | 0 | 宽高不允许负数 |
| BG_COLOR | 白 | |
| BG_ALPHA | **0** | 默认透明，与 CSS 一致；设为 255 才有底色 |
| BORDER_WIDTH | 0 | 0 表示不画边框 |
| BORDER_COLOR | 黑 | |
| BORDER_RADIUS | 0 | 0 表示直角 |
| CLIP_CHILDREN | 0 | 0 表示子控件可以画到父外面（CSS 的 overflow:visible） |
| VISIBLE | 1 | 0 时整棵子树都不画 |

---

## 5. 绘制虚函数为什么是"表"而不是"类字段"

类结构体的约定是 `struct QLXxxClass { 父类Class parent; }` —— **逐层嵌套**。
如果给 `QLWidgetClass` 加一个 `draw` 字段，那么：

```c
struct QLButtonClass { QLWidgetClass parent; };   /* 布局：QLObjectClass | draw */
```

此时把 `QLButtonClass*` 当成 `QLObjectClass*` 是安全的（首成员），
但把它当成 `QLWidgetClass*` 就**不安全**了 —— `QLWidgetClass.draw` 的偏移是
`sizeof(QLObjectClass)`，而 `QLButtonClass` 里 QLObjectClass 就在偏移 0。

所以绘制函数不放进类结构体，而是放在一张按类型 id 索引的表里：

```c
static ql_widget_draw_func s_draw_table[QL_CFG_MAX_CLASS];   /* src/widget/qlwidget.c */

ql_widget_class_set_draw(type, fn);   /* class_init 里调用 */
ql_widget_class_get_draw(type);       /* 按 id 查 */
```

查找时沿 `klass->parent` 向上，返回**第一个非 NULL** 的绘制函数 —— 天然实现了"子类覆盖父类"。

```c
/* 子类写法 */
static void ql_box_draw(QLWidget *self, QLDraw2D *d2d)
{
    ql_widget_draw_default(self, d2d);          /* 背景 + 边框 */
    QLRect r = ql_widget_abs_rect((QLObject *)self);
    d2d->draw_line(d2d, r.x, r.y, ql_rect_right(r) - 1, ql_rect_bottom(r) - 1, 1,
                   QL_COLOR_RED);
}

static void ql_box_class_init(QLBoxClass *klass)
{
    ql_widget_class_set_draw(((QLObjectClass *)klass)->type, ql_box_draw);
}
```

代价是每个类多一个指针（≤ `QL_CFG_MAX_CLASS` × 8 字节），换来的是类型安全、无 UB、无需 `void*` 强转函数指针。

---

## 6. 渲染流程

```c
int ql_render(void);
```

1. 取活动显示与根节点，缺一个就返回 `QL_ERR_NOT_INIT`；
2. 收集区域：首帧 / `ql_render_invalidate_all()` 之后是整屏，否则是脏矩形队列；
3. 包一层 CPU `QLDraw2D`，对每个区域：
   1. 用显示背景色清空（`ql_disp_bg_color()`）；
   2. `render_node(root, 0, 0, area, d2d)` 遍历对象树；
   3. `disp->flush(area)`；
4. 清空脏矩形队列。

换后端只需要换成 `ql_render_draw2d(d2d, w, h)`，其余完全不变。

### `render_node` 的规则

```
render_node(node, ox, oy, clip):
    if node 有 QL_OBJ_FLAG_HIDDEN: return
    if node 不是 QLWidget: 直接以 (ox, oy, clip) 递归子节点
    if 控件的 VISIBLE == 0: return                    ← 整棵子树都不画
    ox += node.x; oy += node.y
    box = (ox, oy, w, h)
    draw = 查 type 到根的绘制表
    if draw: d2d->set_clip(clip ∩ box) 后调用 draw
    if node.clip_children: clip = clip ∩ box
    以 (ox, oy, clip) 递归子节点
```

要点：

- **父子顺序**：先画父再画子，所以后挂的子控件覆盖先挂的（用 `add_child` 的顺序决定层叠）；
- **自身裁剪**：绘制函数只能在"自身矩形 ∩ 脏区"里画，越界自动丢弃；
- **子控件裁剪**：默认不裁剪，加 `CLIP_CHILDREN` 后子控件被限制在父矩形内；
- **隐藏即剪枝**：不可见的控件连同子树直接跳过。

### 脏矩形

- 数组容量 `QL_CFG_DIRTY_RECT_MAX`（默认 16）；
- 新区域与已有区域**相交就合并**，合并后继续检查是否能与其他区域再合并；
- 数组满时全部并成一块（正确性优先）；
- 区域先与屏幕求交，越界部分被裁掉；
- 无脏区时 `ql_render()` 返回 0，不做任何事。

### 自动打脏的时机

| 触发 | 位置 |
|---|---|
| 任何控件属性被 set | `ql_widget_set_property` 末尾 |
| 控件挂到父上 / 从父上摘除 | 对象层的 `ql_object_tree_hook`（由 `ql_render_set_root` 注册） |
| 显式调用 | `ql_render_invalidate(obj)` / `ql_render_invalidate_rect(r)` |

还没挂进渲染树的对象（无父且不是根）不会打脏 —— 它不在屏幕上。

---

## 7. 绘制原语实现细节

| 原语 | 算法 | 复杂度 |
|---|---|---|
| `fill_rect` | 逐像素写入（已裁剪） | O(面积) |
| `fill_round_rect` | 逐像素做"点到圆心的距离 ≤ r"判定 | O(面积)，未优化 |
| `stroke_rect`（radius=0） | 四条矩形，快速路径 | O(周长×线宽) |
| `stroke_rect`（radius>0） | 环 = 外圆角矩形 − 内圆角矩形，逐像素判定 | O(面积) |
| `draw_line` | Bresenham，粗线用方形笔刷 | O(长度) |
| alpha 混合 | 读旧像素 → 按 255 分母线性插值 → 写回 | 每次多一次读 |

**已知取舍**：圆角与圆角描边是逐像素判定，没有做"按行扫描线"优化。
320×240 的整屏重绘在 PC 上约 1~2 ms，在 168 MHz Cortex-M4 上偏慢。
后续优化方向是"每行只算一次内缩量"，接口不变。

抗锯齿、字体渲染、图片 blit 尚未实现（`ql_canvas_blit_mask` 留待文本系统）。

---

## 8. 常见坑

| 坑 | 现象 | 说明 |
|---|---|---|
| 忘记设 `BG_ALPHA` | 控件"看不见" | 默认 alpha=0 是透明的，只设 `BG_COLOR` 不会显示 |
| 忘了 `ql_disp_set` | `ql_render()` 返回 `QL_ERR_NOT_INIT` | `ql_disp_mem_create` 会自动设为活动显示 |
| 忘了 `ql_render_set_root` | 同上 | 根节点必须显式设置 |
| 期望子控件被裁剪 | 子控件画到父外面 | 需要把 `CLIP_CHILDREN` 设为 1 |
| 直接改私有结构 | 编译不过 | 私有结构在 `.c` 里，外部用属性/便捷函数 |
| 在 `draw` 里用局部坐标 | 画错位置 | 画布是屏幕坐标，用 `ql_widget_abs_rect(self)` |
| 半透明背景看不清 | 混合后颜色偏暗 | 半透明是与**已有像素**混合，底层颜色会影响结果 |
| 脏区不合并 → 重绘变多 | 性能下降 | 同一帧内连续 set 会累积脏区，`ql_render` 一次处理完 |

---

## 9. 调试手段

| 需求 | 手段 |
|---|---|
| 看渲染结果 | `ql_disp_mem_save_ppm(disp, "out.ppm")` |
| 查某个像素 | `ql_disp_mem_pixel(disp, x, y)` |
| 查脏区 | `ql_render_dirty_count()` + `ql_render_dirty_rect(i, &r)` |
| 查 flush 次数 | `ql_disp_mem_flush_count(disp)` |
| 查控件绝对位置 | `ql_widget_abs_rect(obj)` |
| 查命中 | `ql_widget_contains(obj, x, y)` |
| 确认绘制函数归属 | `ql_widget_class_get_draw(type)` |

---

## 10. 建议的阅读 / 验证顺序

1. `ctest --test-dir build --output-on-failure` 全绿；
2. `./build/example_widget` 并打开输出的 PPM，先建立"结果长什么样"的直觉；
3. 读 `test/widget/test_canvas.c` —— 每个原语都有像素级断言；
4. 读 `test/widget/test_widget_render.c` —— 层叠、裁剪、脏矩形的可执行说明；
5. 读 `src/draw/qlcanvas.c` 与 `src/draw/qlrender.c`；
6. 最后读 `src/widget/qlwidget.c`。
