# QLRender / GPU 后端 阅读指南

> 本部分回答一个问题：**同一份界面，怎么既能用 CPU 画（MCU），又能用 GPU 画（PC/移动）？**
> 答案是一层极薄的绘制接口 `QLDraw2D` + 两个后端实现。
> 目标读者：要加后端（Metal/D3D11/Vulkan）或改渲染性能的人。

---

## 1. 分层

```
qlui / 控件          ← 只认识 QLDraw2D
      │
   QLDraw2D          ← 6 个函数：set_clip / fill_rect / fill_round_rect
      │                        / stroke_round_rect / draw_text / flush
      ├── qldraw2d.c   → CPU 后端（包一层 QLCanvas，逐像素光栅）
      └── qlgfx_gl.c   → GPU 后端（EGL + GLES3，SDF + 字形图集）
```

**唯一分界点就是 `QLDraw2D`。** 上层（布局、控件）一行都不需要知道用的是 CPU 还是 GPU。

---

## 2. 为什么选 GLES3 + EGL（而不是 Vulkan）

| 项 | 结论 |
|---|---|
| UI 负载特征 | 一帧几百~几千个 quad，瓶颈在**批处理与上传**，不在 API 开销 |
| Vulkan 代价 | 画第一个矩形要 ~2500 行样板（设备/队列/交换链/管线/同步） |
| GLES3 优势 | 跨 Linux/Android 全覆盖，代码量 1/10，Mesa 质量好 |
| Vulkan 何时再考虑 | 需要 bindless 纹理、多线程命令录制、大量纹理图集时 |

**所以：v0 只做 GLES3，但抽象层已经预留**——加 Vulkan 是新增一个 `qlgfx_vk.c`，不动上层。

---

## 3. GPU 后端的三条核心设计

### 3.1 单管线、单 draw call

一个 shader 同时处理两类图元，用顶点属性 `use_tex` 分支：

| use_tex | 图形 | 覆盖率来源 |
|---|---|---|
| 0 | 圆角矩形 / 边框 / 实心矩形 | **SDF 解析计算** |
| 1 | 字形 | 采样字形图集（R8） |

好处：整帧只在最后 `glDrawArrays` 一次（实测 **1 个 draw call**），
状态切换为零，不需要"先画完所有矩形再画所有文字"这种破坏 z 序的排序。

> 代价：顶点较大（15 个 float）。后续可压缩到 8 个 float（用 half/uint 打包）。

### 3.2 SDF 圆角矩形

```glsl
float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}
```

- `p` 是相对矩形中心的局部坐标，`b` 是半宽高，`r` 是圆角半径；
- **填充**：`cov = 1 - smoothstep(-0.5, 0.5, d)`
- **描边**：`cov = (1-smoothstep(-0.5,0.5,d)) - (1-smoothstep(-0.5,0.5,d+w))`
- 抗锯齿是免费的吗？不是"免费"，而是**解析的**：不需要 MSAA，也不需要 4x 超采样。

这就是为什么圆角/边框在 GPU 上比 CPU 光栅更快：CPU 要逐像素判断"是否在圆角内"，
GPU 每个像素只算一次距离场。

### 3.3 字形图集

- 启动时把所有内置字体的字形从 4bpp 展开成 8bpp，shelf packing 排进一张 512×512 的 R8 纹理；
- 每个字形记录 UV 矩形（`QLGLUV`），绘制时按 `(font, glyph index)` 查；
- 采样用 `GL_LINEAR`，将来支持 DPR 缩放时放大也不糊。

> 注意：v0 只支持两个内置位图字体。接入 FreeType 后需要**动态图集**（LRU 淘汰 + 纹理扩容），
> 那是 P4 的工作。

---

## 4. 一次 GPU 帧的完整流程

```c
ql_gfx_gl_begin(gfx, clear_color);       // glClear + 重置顶点计数
ql_ui_render(ui, ql_gfx_gl_draw(gfx));   // 只往顶点数组里加 quad，不碰 GL
d2d->fill_round_rect(...);               // 也可以直接画
ql_gfx_gl_end(gfx);                      // 上传顶点 + 一次 draw + eglSwapBuffers
```

关键点：**绘制阶段完全不调用 GL 函数**，只往 CPU 侧顶点数组里追加。
这样：
- 一次 `glBufferData` 上传，避免每图元一次 API 调用；
- 顶点数组预分配（`QL_GL_MAX_VERTS = 65536`），稳态零 malloc；
- 顶点数超限时**丢弃**而不是崩溃（宁可少画一帧）。

---

## 5. 踩过的坑（都是真实发生的）

| 坑 | 现象 | 根因 | 修复 |
|---|---|---|---|
| **EGL 平台自动选择** | 段错误，栈在 `wl_proxy_create_wrapper` | Wayland 会话里 `eglGetDisplay()` 自动选 Wayland 平台，把 X11 的 `Display*` 当 `wl_display*` | 显式 `eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_KHR, dpy, NULL)` |
| **顶点字段顺序** | 部分文字变成黑色实心方块 | `u,v` 夹在 `border` 与 `use_tex` 之间，`a_params`（vec4）读到的是 `(radius,border,u,v)`，导致 `use_tex = u` | 把 `radius/border/use_tex/pad` 排在一起，`u,v` 单独一段 |
| `clock_gettime` 未声明 | `-std=c11` 下编译失败 | 需要 `_POSIX_C_SOURCE` | 文件顶部定义（只在 port 层） |
| 竖坐标 | 画面上下颠倒 | GL 原点在左下 | 顶点着色器里 `ndc.y = -ndc.y`；`glReadPixels` 后按行翻转 |
| **XIM 少了 `XSetLocaleModifiers("")`** | ibus/fcitx 在跑，输入框里只有字母 | 没调这一句时 `XOpenIM` 仍成功、IC 也建得出、`XNFilterEvents` 也报 0x3，但 `XFilterEvent()` **永远返回 False** —— 输入法根本没接管按键（手工对照 `test/` 外的探针程序才发现） | 建 IM 前 `XSetLocaleModifiers("")`；拿不到再依次试 `@im=ibus` / `@im=fcitx` |
| **XIM 少了 `FocusChangeMask`** | 同上，且输入法完全不响应 | 输入法只给"有焦点的输入上下文"转事件，而 `XSetICFocus()` 是在 `FocusIn` 里调的；窗口不选这个事件掩码就永远收不到 `FocusIn` | 窗口事件掩码加 `FocusChangeMask`，`FocusOut` 时 `XUnsetICFocus` |
| **EGL 窗口表面不会跟着窗口变** | 刚开窗时顶部黑一条、内容贴底；**拖一下窗口就正常** | `eglCreateWindowSurface` 按创建时的窗口几何定尺寸，WM 之后改窗口大小不会改表面；我们把 viewport 设成新尺寸却画进老尺寸的缓冲 | `surface_sync()`：尺寸一变就 `eglDestroySurface` + `eglCreateWindowSurface` + 重新 `eglMakeCurrent`（见 §8） |

**教训**：GPU 后端最容易错的是"数据布局"和"坐标/平台约定"，而不是图形学本身。
所以每一个都要有测试（见 §6）。

---

## 6. 怎么保证 CPU 与 GPU 画得一样

`test/web/test_gl_vs_cpu.c`：

```
同一份 HTML/CSS
  ├─ CPU  → ql_disp_mem_create(RGB888) + ql_ui_render_canvas  → 参考图
  └─ GPU  → ql_gfx_gl_create(OFFSCREEN) + ql_ui_render        → glReadPixels
        ↓
   逐像素比较，单通道差 ≤ 40，异常像素占比 ≤ 6%
```

当前实测：**3.66% 像素不同，最大单通道差 239**（差异集中在文字与圆角边缘的抗锯齿上）。
这个测试是"多后端不漂移"的唯一保障——加 Metal/D3D 后同样要跑。

**离屏模式**（`QL_GFX_GL_OFFSCREEN`）用 pbuffer，不弹窗，所以能进 CI。

---

## 7. 性能现状（PC，本机实测）

| 指标 | 实测 |
|---|---|
| 320×240 界面帧时间（含 swap） | **~220 µs** |
| 顶点数 | ~430 |
| draw call | **1** |
| 首次建上下文 | ~1000 µs（一次性） |

对比思路：一帧 220 µs ≈ 4500 fps 的 CPU 侧提交能力，
实际帧率被 `eglSwapBuffers` 的 vsync 限到 60。也就是说**目前完全不是 GPU 瓶颈**。

---

## 7.1 窗口缩放为什么不再闪黑

三件事凑在一起，缺一个都会在拖边框时看到"黑一下再出内容"：

| 措施 | 位置 | 解决什么 |
|---|---|---|
| 窗口背景像素 = 本帧清除色 | `sync_window_background()`（`begin_common` 里） | 窗口变大时新露出的那块是 **X 服务器**填的，背景像素没设过就是黑的 |
| 清除色传页面底色 | 调用方（例程里 `ex_clear_color()`） | 万一还漏一帧，露出来的是页面底色，视觉上无感 |
| FBO 按容量复用、只增不减（256 对齐） | `fbo_ensure()` | 保留模式下窗口每变一次尺寸就重建全屏纹理 = 每步一次 `glTexImage2D`，拖起来一顿一顿 |
| 默认 `eglSwapInterval(1)` | `ql_gfx_gl_create()` | 不跟 vblank 主循环会几百 Hz 空转，每帧都塞一个新尺寸给 compositor |

注意 FBO 的**分配尺寸**可以大于窗口：`glViewport` 与帧末 blit 都只取
`(0, 0, width, height)` 那块子矩形，所以"分配得大一点"对绘制结果没有影响。

---

## 8. 加一个新后端要做什么

1. 实现 `QLDraw2D` 的 5 个绘制函数（`flush` 可选）；
2. 把 quad → 图形 API 的顶点/命令；
3. 保证 `stroke_round_rect` 的描边**向内**、圆角半径裁到短边一半（与 CPU 一致）；
4. 跑 `test_gl_vs_cpu`（把它参数化成"后端 vs CPU"）。

预期工作量：Metal/D3D11 各 ~600 行（如果复用"单管线 + SDF"的设计）。

---

## 9. 建议阅读顺序

1. `include/qlgl/qldraw2d.h`（30 行，先看接口）
2. `src/draw/qldraw2d.c` 的 `ql_draw2d_text_layout`（CPU/GPU 共用的排版逻辑）
3. `src/port/qlgfx_gl.c` 的 `VS_SRC` / `FS_SRC`（着色器就是全部图形学）
4. `emit_quad`（顶点怎么发）
5. `test/web/test_gl_vs_cpu.c`（怎么验收）

---

## 10. CPU 光栅快路径（M4-G1）

早期 `qlcanvas.c` 是最朴素的写法：`for y` → `for x` → `put_pixel()`。
每次写像素都要走一遍"边界检查 + 格式 switch + alpha 混合"，1080p 全屏填充要 44 ms。

现在拆成三个层次，把不变量提到循环外：

### 10.1 一次算裁剪

`fill_rect_alpha` 先把目标矩形与 `canvas->clip` 求交，得到 `x0/x1/y0/y1`。
循环内部不再做任何裁剪判断——这是最大的一笔收益，因为裁剪比较和分支预测
在逐像素粒度上非常昂贵。

### 10.2 按格式特化（`fill_row`）

```c
static void fill_row(QLCanvas *c, int y, int x, int n, uint32_t color);
```

按 `c->format` 分派**一次**，然后用该格式的最优指令序列填满一整行：

| 格式 | 不透明（`alpha == 255`） | 半透明 |
|---|---|---|
| RGB888 | 4 像素 = 3 次 32 位写（构造 12 字节模式串，尾部再补） | 逐像素 3 通道混合 |
| ARGB8888 | 每像素 1 次 32 位写 | **SSE2 4 像素/批** |
| RGB565 | 每像素 1 次 16 位写，颜色预打包 | 逐像素（先扩成 888 再压回） |
| LUM8 | `memset` | 逐像素 |

**半透明路径曾经是最大的漏点**：旧实现落到逐像素 `put_px()`，每像素重算一次
`buffer + y*stride` 并再跳一次格式 switch。而软阴影、渐变、圆角覆盖全是
"整行同 alpha"，代价全砸在这上面。现在行指针与格式 switch 都在循环外。

SSE2 版本（ARGB8888）每批 4 像素：

```
unpacklo/hi_epi8(v, 0)      → 8+8 个 16 位车道 [b,g,r,a]×2
mullo_epi16(·, 255-a) + 源预乘常量
mulhi_epu16(·, 32897) >> 7  → 精确的 /255
packus_epi16 → 4 像素
or 0xFF000000               → A 通道统一置不透明
```

两个**逐值恒等**的 `/255`（`0..65025` 全域验证过与 `x/255` 相同，
这是能上 SIMD 的前提——中间和 `src*a + dst*(255-a)` 恒定 ≤ 65025，正好装进 16 位）：

| 形态 | 式子 |
|---|---|
| 标量 | `(x + (x>>8) + 1) >> 8` |
| SSE2 | `mulhi_epu16(x, 32897) >> 7` |

> 写这个常量时有两个坑，都踩过：**源项是 16 位预乘值，不能塞进 8 位字槽**
> （`(sb<<16)|(sg<<8)|sr` 会串道），必须用 `setr_epi16` 逐车道写；
> **A 通道也不要用 `255*alpha` 塞进高字节**（超 8 位会被截断），
> 最后 `or 0xFF000000` 更简单也更对。

### 10.3 行跨度扫描（圆角 / 描边）

圆角矩形逐行只有**两段**连续区间：`[x0, xl)` 和 `[xr, x1)`。
`round_row_span` 用圆心 + 半径一次解出该行的 `xl/xr`（含 1px 抗锯齿覆盖率），
再把两段交给 `fill_row`。描边环同理：一行是"左环段 + 右环段"。

```
       ┌─────────────────┐
y=top  │  █████████████  │   中间行： [x0, xl) + [xr, x1)
       │ ██           ██ │   上下行： 单段
       │ ██           ██ │
       └─────────────────┘
```

### 10.4 字形

`draw_glyph` 按行处理：先算本行可见的像素范围（裁剪），把**行指针提到行外**
（原来每像素重算一次 `buffer + y*stride`），行内只做无检查写。

### 10.5 结果（M4 第一批之后，Release，示例页 780 顶点）

| 分辨率 | 最初 | 行循环+格式特化 | 半透明 SIMD + 阴影 9-slice |
|---|---|---|---|
| 320×240 | 3.68 ms | 0.696 ms | **0.296 ms** |
| 800×480 | 11.7 ms | 1.854 ms | **0.697 ms** |
| 1280×720 | 23.1 ms | 3.169 ms | **1.370 ms** |
| 1920×1080 | 44.4 ms | 4.929 ms | **2.191 ms** |

1080p CPU 光栅 2.19 ms ≈ 456 fps。剩下的大头是阴影（54%）与文本；
继续优化的方向见 `doc/bench.md` 的"进一步优化的方向"。

### 10.6 阅读顺序补充

6. `qlcanvas.c` 的 `put_px` / `fill_row` / `round_row_span`（三个基础件）
7. `ql_canvas_fill_round_rect_alpha`（怎么用它们拼出圆角）
8. `test/draw/test_canvas.c`（像素级断言，改这些函数后必须全绿）

---

## 10.7 高 DPI（DPR）：缩放只在后端入口发生

三层单位，各管一段：

| 层 | 单位 | 谁负责换算 |
|---|---|---|
| 布局 / qlui / 控件 / 自定义绘制 | **CSS px** | 不换算 |
| `QLDraw2D` 后端入口（CPU 与 GL） | 设备像素 | **唯一**的换算点 |
| 驱动 `flush(area)` / 清屏 | 设备像素 | 调用方各自换一次 |

缩放系数是 26.6 定点（`QL_SCALE_ONE` = 1.0，96 = 1.5），挂在"表面"上逐层继承：

```
QLDispDriver.scale_fx ──▶ QLCanvas.scale_fx ──▶ QLDraw2D.scale_fx ──▶ 后端入口
QLGfxGL.scale_fx     ────────────────────────▶ QLDraw2D.scale_fx
```

所以换 DPR 时**上层一行都不用改**。后端入口只有一个判断：

```c
static inline QLRect sx(QLDraw2D *self, QLRect r)
{
    return (self->scale_fx == QL_SCALE_ONE) ? r : ql_scale_rect(r, self->scale_fx);
}
```

### 两个必须知道的坑

1. **矩形要按"边"缩放**，不能 `x*scale + w*scale`：

   ```c
   x1 = ql_scale_len(r.x);                  /* 同一条边界 */
   x2 = ql_scale_len(ql_rect_right(r));
   w  = x2 - x1;
   ```

   否则相邻矩形各自算宽度，会在 1.5× 这种非整数倍下裂开或重叠 1px。
   （和 flex 的"累计分配"是同一个道理。）

2. **文本要按设备字号重新取字体**（`ql_draw2d_device_font`）：DPR>1 时用
   `ql_font_default(css_px * scale)` 重新光栅化，否则字形被拉伸变糊。
   只有动态字体（有 `backend`）能重取；位图字体在 MCU 上不缩放——那是固有限制。

### GPU 侧：单批次的裁剪是"几何裁剪"

本后端整帧只有一次 `glDrawArrays`，**不能用 scissor 逐图元裁剪**，
只能在发顶点时把四边形范围与 `g->clip` 求交：

```c
quad = intersect(形状外扩 1px 的范围, g->clip);
```

关键：**形状参数（`hx/hy`、`radius`、`border`、渐变跨度）必须继续用未裁剪的矩形**，
否则片元里的 SDF 会算错。被裁掉的只是光栅化范围。

> 这里原来有个真 bug：`emit_quad` 从来没做这个求交，`g->clip` 只存不用，
> 导致 **`overflow:hidden` 在 GPU 上完全没生效**（DPR 测试里 CPU vs GL 差 28%）。
> 修好后仍是单 draw call，差异降到 0.4%。纹理四边形（字形）裁剪后按顶点位置
> 线性插值重算 UV——图集 UV 是轴对齐的，插值精确。

---


## 10.7.1 Wayland 后端：scale / 触摸 / 帧回调 / 剪贴板

纯 CPU 的 Wayland 后端（`src/port/qldisp_wayland.c`）在这一轮补齐了四件事。
都是**协议层**的事，不影响上面那套单位换算。

### 高 DPI（`wl_output.scale`）

合成器的 `wl_surface.set_buffer_scale(N)` 只负责"把逻辑尺寸缩回去"，
**缓冲区本身必须是 N 倍物理像素**。所以缩放一变：

1. 帧缓冲按 `css × scale` 重新分配（`buffers_rebuild()`）；
2. `base.width/height/stride` 换成物理值，`scale_fx` 同步（画布/`QLDraw2D` 自动跟上）；
3. **画布必须重建** —— `ql_disp_wayland_resize_pending()` 读一次即清，
   调用方看到就 `ql_canvas_from_disp()` + `ql_ui_layout(css_w, css_h)`。

`ql_disp_css_width()` 始终是创建时给的逻辑尺寸，所以**布局代码一行都不用改**。
输入坐标（pointer / touch）本来就是 surface 的逻辑坐标，也不需要换算。

> 调试：`QLGL_WAYLAND_SCALE=2` 可以在 1x 屏上强制按 2x 分配缓冲区 ——
> 这是本机（`output scale=1`）唯一能自动验证 2x 路径的办法，见 `test_wayland`。

### 帧回调（`wl_surface.frame`）

同一时刻只挂一个回调。上一帧还没被呈现（`frame_pending`）时**不提交**，
脏区继续攒在同一个缓冲区里，等回调到了在 `poll()` 里补提交 ——
这就是"提交节奏压在刷新率上"，也是 roadmap 里说的省电关键。

兜底：连续被节流 3 次（`QL_WL_MAX_STALL`）就强提交一次，
免得某个合成器不回帧回调时界面僵住。

### 触摸（`wl_touch`）

映射成"主键按下 / 移动 / 抬起"，界面层只认指针事件，所以点按、拖动滚动、
输入框聚焦全都自动可用。多指手势没做（本机没法验证，写了也是猜）。

### 剪贴板（`wl_data_device`）

`ql_disp_wayland_attach_clipboard(driver, ui)` 装上 `QLUIClipboard` 后端：

* 复制：建 `wl_data_source`，offer `text/plain;charset=utf-8` 等四种 MIME；
  `set_selection` 必须带**最近一次输入事件的 serial**，所以得等用户先点一下窗口；
* 粘贴：`wl_data_offer_receive()` + 匿名管道**非阻塞**读，读不到 EOF 就下一帧再问
  （与 X11 那套"异步等 SelectionNotify"同一个接口形状）。

三个踩过的坑：

1. **监听器槽位一个都不能漏**。libwayland 收到事件而对应槽位是 `NULL` 时
   **直接 `abort()`**（打印 `listener function for opcode N of wl_pointer is NULL`）。
   我们绑 `wl_seat` v5，指针就会多出 `frame` / `axis_source` / `axis_stop` /
   `axis_discrete` 四个事件 —— 少一个，**鼠标一进窗口进程就崩**（现象是
   "窗口一闪就没了 / 根本没显示"）。规则：**升级 `bind` 的版本时，同步补槽位**；
   `test_wayland` 第 0 节把每个槽位都断言了一遍（不需要合成器就能跑）。
2. `wl_data_offer_add_listener` 只能加一次（`data_offer` 与
   `selection` 两条事件都会带同一个 offer，重复加会报 `proxy already has listener`）；
3. `xdg-shell` 第一帧必须先 `wl_display_roundtrip()` 拿到 `configure`，
   否则前几帧 `flush` 会因为 `!configured` 被静默丢掉。

### 10.7.2 Wayland GPU 后端（`wl_egl_window`）

`qlgfx_gl.c` 支持把 GL 画到原生 Wayland 表面（`wl_egl_window`），与 CPU 后端
共用同一套窗口外壳 `src/port/qlwindow_wayland.h`（xdg-shell / 输入 / 高 DPI /
帧回调 / 剪贴板都在里面，两个后端只各自管"像素怎么变成 wl_buffer"）。

```
QLGL_GL_BACKEND=wayland ./build/example/example_02_text --frames 30 --stats
```

**默认仍然是 X11**。原因不是技术问题而是功能问题：原生 Wayland 的中文输入法走
`zwp_text_input_v3`，我们还没接；切过去会让组字、候选窗全失效。所以只有
`QLGL_GL_BACKEND=wayland` 显式指定时才走 Wayland；`DISPLAY` 为空（纯 Wayland
会话）时自动回落。

几个和 X11 路径**不一样**的地方：

| 项目 | X11（GLX/EGL-X11） | Wayland（EGL-Wayland） |
|---|---|---|
| 原生窗口句柄 | `Window`（XID） | `wl_egl_window`（由 `wl_surface` 包出来的） |
| EGL 平台 | `EGL_PLATFORM_X11_KHR` | `EGL_PLATFORM_WAYLAND_KHR` |
| 改尺寸 | 销毁 `EGLSurface` 重建（X11 表面尺寸是创建时定死的） | `wl_egl_window_resize()`，表面不重建 |
| 尺寸单位 | 设备像素 | 设备像素（= CSS × scale，缓冲缩放交给 `set_buffer_scale`） |
| 输入 / 输入法 | Xlib 事件 + XIM | 共用外壳（键盘/指针/触摸）；**输入法暂缺** |
| 帧节奏 | `eglSwapInterval` + 自算 pace | 合成器在缓冲排队满时阻塞 `eglSwapBuffers`（天然 vsync） |

`ql_gfx_gl_poll()` 在 Wayland 下泵 `qlwl_pump()`：事件全由外壳推给 `ql_input`，
自己只处理 `wl.resize_pending` —— 缩放变了就重算设备像素尺寸、`wl_egl_window_resize()`
并置 `ql_gfx_gl_resize_pending()`，调用方据此重建画布 + 重排。
`ql_gfx_gl_wait()` 直接 `poll()` 协议 fd，不用 `usleep` 空转。

> **抓图的两个坑**（`QL_EX_WINDOW_DUMP` 就是这么实现的）：`eglSwapBuffers`
> 之后后缓冲内容**未定义**（Wayland 上读出来全黑），而 `ql_gfx_gl_end()` 之前
> 顶点批次还没上传（只读到清除色）。正确的时机是新增的
> `ql_gfx_gl_flush_frame()`：落盘但**不交换**，这时 `ql_gfx_gl_read_pixels()`
> 拿到的才是真的这一帧。设备像素尺寸用 `ql_gfx_gl_device_size()`
> （`ql_gfx_gl_size()` 给的是 CSS px，高 DPI 下两者不等）。

### 10.7.3 应用级缩放（`ql_gfx_gl_set_zoom`）：双指捏合的落点

DPR（`scale_fx`）管的是"帧缓冲多大"；**应用缩放**（`zoom_fx`）管的是"同样的缓冲里
内容画多大"。两者相乘才是 CSS px → 设备像素的系数（内部叫 `dscale()`）。

```c
ql_gfx_gl_set_zoom(gfx, QL_SCALE_ONE * 3 / 2);   /* 150% */
ql_ui_layout(ui, win_w * QL_SCALE_ONE / z, win_h * QL_SCALE_ONE / z);
```

约定三条：

1. **布局基准 = 窗口 / zoom**（`ql_gfx_gl_window_size()` 拿窗口逻辑尺寸，
   `ql_gfx_gl_size()` 给的已经是除过 zoom 的值）；
2. **输入坐标后端负责换算**：X11 按 `设备像素 / (DPR×zoom)`，Wayland 外壳按
   `逻辑坐标 / zoom` —— 界面层只会看到自己那套 CSS 坐标；
3. 改完 zoom 内部会把保留模式画布作废（`fbo_fresh`），调用方整屏重绘一帧即可。

为什么这么做而不是"改字号/改宽度"：那要重解析 CSS、重算 em/rem，而且文字会重排；
按缩放系数画是纯几何的，只重排一次布局基准，字形还按新尺寸重新取字体（放大后是清晰的）。

| 抓图相关 | 用途 |
|---|---|
| `ql_gfx_gl_device_size()` | 帧缓冲**设备像素**尺寸（抓图/stride 用它） |
| `ql_gfx_gl_window_size()` | 窗口**逻辑**尺寸（不含 zoom，算布局基准用它） |
| `ql_gfx_gl_size()` | 当前布局基准（已经是 `窗口 / zoom`） |
| `ql_gfx_gl_flush_frame()` | 落盘但**不交换**：抓图必须在这一步之后、`end()` 之前 |

验证：`example/37_touch --zoom 150` 抓窗口帧，X11 与 Wayland 两个 GPU 后端逐字节相同。

验证：`test_wayland`（`QLGL_TEST_WAYLAND=1`）在 1x/2x 两种缩放下断言
"设备像素 = CSS × scale"以及"10 CSS px 的红方块正好覆盖 10×scale 个设备像素"；
同一界面在 X11 与 Wayland 两个 GPU 后端抓窗口帧**逐字节相同**，
2x 帧按 2×2 平均降到 1x 后与 1x 帧一致（差异只剩字形微调）。

## 10.8 绘制期亚像素与边缘覆盖率（M5.3）

整像素矩形只能对齐到整格：`translate(0.5px)` 会变成 1px 一跳，60fps×300ms 的动画
只有 18 个台阶，肉眼可辨。这一步给光栅化加了"边落在小数上也能画"的能力。

### 三样东西

```c
typedef struct { ql_coord_t x, y, w, h; } QLRectFx;   /* 设备像素 26.6，低 6 位是亚像素 */

void ql_canvas_fill_rect_fx(QLCanvas *c, QLRectFx r, ql_color_t color, uint8_t alpha);
void ql_canvas_fill_round_rect_fx(QLCanvas *c, QLRectFx r, ql_coord_t radius_fx,
                                  ql_color_t color, uint8_t alpha);
void ql_canvas_stroke_rect_fx(QLCanvas *c, QLRectFx r, ql_coord_t radius_fx,
                              ql_coord_t thickness_fx, ql_color_t color);

/* 动画的落点：CSS px 26.6，translate(0.5px) 就是 32 */
void ql_draw2d_set_offset(QLDraw2D *d2d, ql_coord_t x_fx, ql_coord_t y_fx);
```

CPU 后端把矩形转成 `QLRectFx` 走覆盖率填充；GPU 后端**直接加进顶点浮点**
（本来就免费，而且精确）。`offset` **不影响布局**，与 CSS `transform` 语义一致，
所以滑动/缩放动画零重排。

### 覆盖率怎么算

一个像素的 alpha = alpha × (横向重叠比例) × (纵向重叠比例)，都是 64 分制整数：

```
像素 [x, x+1) 与形状 [fx0, fx1) 的重叠长度 / 64
```

性能上只多两笔：**每行只在两端各多一次 `put_px`**，中间整像素仍走 `fill_row`
的 SIMD 快路径。所以亚像素不是"全部逐像素"的代价。

### 三个刻意的实现选择（都是为了不引入跳变）

1. **圆角跨度复用整数版 `round_row_span`**，再把整像素跨度平移亚像素余量。
   一开始用中心采样（直接算 SDF），圆角比整数版**大将近 2px**——
   动画跨过 0 时拐角会"突然胀一圈"。改用整数跨度后，对齐时逐字节相同。
2. **整数矩形 = 低 6 位全 0 的 fx 矩形**，覆盖率恰好全满 →
   与 `ql_canvas_fill_rect_alpha` **逐像素相同**。这是安全阀：
   `test_subpx` 用 5 种矩形 × 4 种半径 × 3 种 alpha 对照，任何偏差都会被抓住。
3. **文本与阴影只吃偏移的整数部分**：亚像素平移会让字形发糊；阴影本来就软，
   为了省事没必要走 fx 路径。

### 效果

| 实现 | 12px 矩形从 0 挪到 1px，边缘能看到几个不同灰度 |
|---|---|
| 整像素 | **1**（直接跳过去） |
| 26.6 亚像素 | **64** |

`test_subpx` 抽查 6 个偏移，得到 6 个不同的边缘灰度值；
亚像素偏移下 CPU vs GPU 差 **1.62%**（全在圆角 AA 上）。

---

## 11. 渐变与软阴影（M5.5 先行项）

两个效果都**不需要离屏缓冲、不需要多趟渲染**，因此不打破"单批次 1 draw call"。

### 11.1 渐变：CPU 走行填充，GPU 走顶点颜色插值

| 后端 | 做法 | 成本 |
|---|---|---|
| CPU 垂直 | 每行算一个颜色，交给 `fill_row` | **≈ 纯色填充** |
| CPU 水平 | 每像素 `lerp_color`，逐像素写 | 慢一个量级 |
| GPU | 上下（或左右）顶点给不同颜色，让插值器算 | **零额外顶点、零额外 draw call** |

关键点：GPU 侧**没有改着色器**。`v_color` 本来就是 varying，插值器自动完成线性渐变；
只有 `cov`（SDF 覆盖率）参与 alpha 相乘，所以圆角抗锯齿依然正确。

顶点颜色按**顶点自身位置**求值（而不是按矩形边），这样扩出去做 AA 的 1px 也和
CPU 的逐行 lerp 对齐。

### 11.2 软阴影：SDF + smoothstep，两端同一公式

```
d   = sdRoundBox(p - 中心, 半径半尺寸, radius)      // 负值在内部
cov = 1 - smoothstep(0, blur, d)
```

GPU 直接在片元里算；CPU 用等价的展开式（`qx = |px| - hx + rad`，`d = sqrt(max(qx,0)² + max(qy,0)²) + min(max(qx,qy),0) - rad`），
**公式逐字相同**，所以 `test_gl_vs_cpu` 的差异仍在 4% 量级（只差边缘 AA）。

### 11.2.1 CPU 侧：九宫格分解（性能的关键）

把"轮廓外扩 blur"的外框按 `{qx>0}` / `{qy>0}` 两个判据切成 9 块：

| 区域 | 判据 | 距离依赖 | 填充方式 |
|---|---|---|---|
| 内实心 | `qx≤0, qy≤0` | 恒 `d<0` | `round_row_span` + 整行 |
| 上 / 下边带 | `qx≤0, qy>0` | **只取决于 y** | 整行同 alpha（`fill_row`） |
| 左 / 右边带 | `qx>0, qy≤0` | **只取决于 x** | 整列同 alpha（`fill_col`） |
| 四个角块 | `qx>0, qy>0` | 取决于两者 | 逐像素 `sqrtf` |

各块的边界（**容易写错，改这里前先看 `ref_shadow()` 的对照测试**）：

- 上/下边带的 x 范围 = `[sx0+rad, sx1-rad)`；
- 左/右边带的 y 范围 = `[sy0+rad, sy1-rad)`；
- 角块横向范围 = `[sx0-pad, sx0+rad)` 与 **`[outer右-pad-rad, outer右)`**，
  右侧必须以 **outer 的右边界**为锚——写成 `sx1-(rad+pad)` 会往内偏 `pad`，
  既与直边带重叠（同一点混两次、偏黑）又漏掉最外侧 `pad` 条带；
- `rad == 0` 时角块退化成 `pad×pad` 的方角，**不能跳过**，否则那片留白。

四个角互为镜像（覆盖率只取决于相对偏移 `(qx,qy)`），所以每对 `(qx,qy)` 只算一次
`s=sqrt(qx²+qy²)`，然后写入四个镜像位置——角区工作量降到 1/4。

角内 `qx,qy` 恒为正，`sd_round_box` 的 `inside` 项在那里恒为 0；三个 `continue`
（`d≤0` / `t≥1` / `cov==0`）合成一次 `a != 0` 判定，避免角带边界处的分支误预测。

一条 143×68、blur=10 的卡片：逐像素开方次数从 ~4200 降到 ~400。

### 11.3 顶点复用的技巧

阴影的 quad 覆盖"轮廓外扩 blur"，但 `lx/ly` 相对**轮廓中心**、`hx/hy` 是**轮廓半尺寸**——
于是片元里的 `sdRoundBox` 算出来的就是到轮廓的距离。
**不需要新增任何顶点属性**，顶点结构仍是 64 字节。

`v_params.z` 从 `use_tex` 布尔升级成 `mode`：

| mode | 含义 | `v_params.y` |
|---|---|---|
| 0 | 形状（填充/描边） | 描边宽度（<0 表示填充） |
| 1 | 字形（采样图集） | 未用 |
| 2 | 软阴影 | blur 半径 |

### 11.4 测试

- `test/draw/test_effects.c`：28 项检查。除了渐变端点/中点/同色行、alpha=0、
  阴影衰减方向、blur=0 硬边这些**特征断言**，还有一份
  **朴素参考实现 `ref_shadow()`**——对整块外框逐像素求圆角 SDF、不做任何分解——
  用 3 种外框 × 4 种半径 × 4 种模糊 × 3 种偏移 × 3 种 alpha = **432 组**逐像素对照。
  9-slice / 快路径 / 角区镜像都是结构改写，必须逐像素等价，这份对照是唯一的证据。
  它上线时立刻抓到 3 个真 bug（见 11.2.1 的三条"容易写错"）。
- `test_gl_vs_cpu`：示例页含渐变、阴影、文本；像素差异 **0.73%**（阈值 2%）。

> 唯一不参与逐像素对照的是**内实心区**（`d≤0`）：它由 `round_row_span` 的离散判定
> 负责，与连续 SDF 的边界最多差 1px；那块被元素自身背景盖住，属于既定行为。

---

## 12. GPU 字形路径：三个曾经踩过的坑（M5.5）

窗口里的字曾经**又糊又乱**（笔画连成一片、字距被吃掉、被 transform 平移的节点上
直接显示成不相干的字）。根因都在 `emit_quad()` 这一个函数里，值得单独记一节。

### 12.1 字形 quad 绝对不能外扩 1px

形状（填充/描边）会外扩 1px 给 SDF 抗锯齿留余量，字形**不能**跟着外扩：

```
外扩后的 quad 宽 = w + 2，而 uv 仍然从 u0 走到 u1（= 图集里 w 宽的那块）
→ 字形被横向拉宽 (w+2)/w。8px 的字就是 +25%。
```

后果不是"稍微糊"，而是**字距被吃掉、笔画相连**（`gradient pulse` 看起来像
`gradientpulse`），而且图集里字形只隔 1px，外扩还会采到邻字的像素。

正确做法：`pad = (use_tex == 1) ? 0.0f : 1.0f`。字形位图本身已经带抗锯齿
（覆盖率烘在 alpha 里），不需要第二层。

### 12.2 uv 的插值基准必须含绘制期平移

`emit_quad()` 里 uv 是按"顶点在形状矩形里的相对位置"线性插值算的，
这样被裁剪后 uv 仍然对得上图集：

```c
v.u = u0 + (xs[k] - ux0) / (ux1 - ux0) * (u1 - u0);
```

`xs[]` 已经加上了 `ox`（绘制期平移，来自 `transform: translate`），
而 `bx0/bx1` 是**平移前**的值。少加这一层，`ox` 越大 uv 偏得越多——
`translate(-70px)` 时直接采到完全不相干的字形。所以要先 `ux0 = bx0 + ox`。

### 12.3 片段着色器要 highp

图集是 1024 宽，uv 分辨率需求是 1/1024 ≈ 2⁻¹⁰；`precision mediump float` 的尾数
刚好也是 10 位，纹素会落到两个纹素之间 → 整片文字发虚。
改成 `GL_FRAGMENT_PRECISION_HIGH` 时用 `highp`（不支持再回落 mediump）。

### 12.4 怎么守住

`test_gl_vs_cpu` 加了两道闸：

| 断言 | 修好前 | 修好后 | 阈值 |
|---|---|---|---|
| 示例页 CPU vs GPU 像素差异 | 5.72% | **0.73%** | ≤ 2% |
| `.hint` 一行字的墨迹宽度（CPU 104 px） | GPU 106 px | **GPU 105 px** | 差 ≤ 1 px |
| 墨迹左边界 | 差 2 px | 差 1 px | 差 ≤ 1 px |

墨迹宽度那条是**直接针对 12.1** 的：外扩会让左右各长 1px，一行十几个字就整体宽 2px。

---

## 13. 保留式脏区：窗口里"局部重绘"为什么需要一块 FBO

`ql_ui_render_rect()` 在 CPU 上直接画进同一块 framebuffer，天然"保留"。
**窗口 + 双缓冲**不是这样：`eglSwapBuffers` 之后后缓冲内容没有任何保证，
只重画脏区就等于把其余部分清成底色。

现象很好认：动画一停（`fill: forwards` 停在末帧，不再产生脏区），
卡片、标题、提示文字**全都不见了**，只剩还在动的那一块。

`ql_gfx_gl_set_retained(gfx, 1)` 的解法：

```
每帧：bind FBO → 只 scissor-clear 脏区 → 只画脏区 → blit FBO → 窗口 → swap
```

三个实现要点：

1. **FBO 刚建（或窗口改尺寸）那一帧必须整屏重画**——新 FBO 内容是未定义的。
   靠 `fbo_fresh` 标记，`ql_gfx_gl_begin_rects()` 在这一帧**返回 1**，
   调用方据此改成 `ql_ui_render()` 整屏画。
2. **清屏也用 scissor**，而且 rect 要乘 `scale_fx`（与 `gl_set_clip` 同一套换算）。
3. **绘制时关 scissor**：顶点已经按 `g->clip` 做过几何裁剪，这里再 scissor 一遍
   会把 SDF 的 1px 抗锯齿余量切掉。

`test_gl_dirty`（75 项）守这条：FBO 刚建必须要求整屏；之后连续 12 帧"只画脏区"
与"整屏重画"**逐像素相同**；跨过动画终点后仍相同。

---

## 14. 圆遮罩：为什么改"像素汇点"而不是每个图元

圆形揭示需要**非矩形裁剪**。`clip` 是矩形，表达不了圆盘，于是有两条路：

1. 每个图元（填充 / 圆角 / 渐变 / 阴影 / 字形）各加一个圆判断 → 8 处，容易漏；
2. **在像素汇点上做** → CPU 只要改 `fill_row` + `put_px`（+ 阴影/字形的两个逐像素循环），
   GPU 只要改 `emit_vertex`（所有 `emit_*` 的唯一出口）。

选 2。具体：

| 位置 | 做法 |
|---|---|
| `fill_row` | 每行与圆的水平范围求交（**每行一次 `sqrtf`**），内层仍走 SIMD 快路径，两端各留 1 个像素给覆盖率 |
| `put_px` | alpha 乘上该像素的圆覆盖率（`1 - smoothstep(-0.5, 0.5, d)`，与 GPU 同一公式） |
| 阴影角区 / 字形 | 这两处为了性能自己拿行指针逐像素写，所以额外用 `blend_px_masked()` |
| GPU | 顶点属性 `a_mask = (cx, cy, r, -)`，`emit_vertex` 统一填；片元里乘一次覆盖率 |

**"关闭"的哨兵必须是 -1，不能是 0**：`0` 的语义是"遮罩存在但半径 0 = 全裁"，
那是揭示动画的第一帧。这个约定在三天内踩了三次（`memset` 默认 0、GPU 只重置了半边状态、
顶点属性忘绑 `glVertexAttribPointer`），每次都表现为**整屏全黑或只剩第一个节点**。

### 14.1 片元着色器里"唯一出口"必须真的是唯一的

"在像素汇点上做"这条设计在 **顶点** 侧落实得很好（`emit_vertex` 是唯一出口），
但在 **片元** 侧曾经破了个洞：着色器按 `mode` 分派形状，径向渐变那一档是

```glsl
if (mode > 2.5) {
    ...
    frag = vec4(mix(...), v_color.a * cov);
    return;            /* ← 提前返回，后面的遮罩块根本没跑到 */
}
if (v_mask.z >= 0.0) { cov *= ...; }
frag = vec4(v_color.rgb, v_color.a * cov);
```

后果：`clip-path` + `radial-gradient` 在 **GPU 上静默丢掉遮罩**（CPU 是对的）。
这正是"半个正确"，而且**比例断言抓不住**——整块丢掉遮罩只让 CPU/GPU 差异从
1.47% 变成 3.00%，刚好卡在 3% 阈值上通过。

修法：把遮罩算成 `float m` **挪到分派之前**，所有出口都乘 `m`
（`frag = vec4(rgb, alpha * cov * m)`），径向那一档也不例外。

守住它的两条断言（`test_gl_vs_cpu`）：

1. 比例阈值从 3% **收紧到 2%**（原 bug 是 3.00%，会被抓）；
2. 更重要的**结构性断言**：遮罩半径 75% 盖不到 `.rad2` 的角落，
   所以 GPU 读回的角落像素必须是底色；同时中心必须真的画出了渐变
   （防止"为了过断言整块不画"）。

> 教训：**"覆盖率类"效果宁可让每条出口都乘一遍，也不要放在某个分支之后**。
> 比例断言只能发现"少了一点"，发现不了"整类图元都没效果"。

### 14.2 B9：遮罩从"圆"升级成"椭圆"，并给多边形留了一条后路

`clip-path` 现在有四种形状，它们落在三条不同的路上：

| 形状 | 后端怎么做 | 为什么 |
|---|---|---|
| `inset()` | **不碰遮罩**：与裁剪矩形求交 | 纯矩形，两个后端本来就精确 |
| `circle()` / `ellipse()` | 遮罩参数从 `(cx, cy, r)` 扩成 `(cx, cy, rx, ry)`，公式改成归一化椭圆距离 × `min(rx, ry)` | 一条 SDF 就能表达，零缓冲；`rx == ry` 时退化成原来的圆 |
| `polygon()` | 顶点表走 uniform（`u_pn` / `u_p[N]`），片元里算"内点判定 + 到最近边距离" | 边数 ≤ 16，逐片元的代价可忽略；换来**解析**抗锯齿，不再糊 |

零半径那条坑又多了一个：椭圆版的片元里 `min(rr.x, rr.y) == 0` 会把距离乘成 0，
`smoothstep(-0.5,0.5,0) = 0.5` ⇒ **整块半透明**。所以现在显式判 `rr.x <= 0 || rr.y <= 0 → m = 0`
（CPU 的 `mask_cov_at` 早就是这么写的，两边现在一致）。这一条让 golden 的 `clip`
用例从 CPU/GPU 差 **47%** 回到 0。

### 14.3 颜色矩阵：单批次下怎么带"每元素不同"的 uniform

纯颜色 `filter` 折成 3×4 矩阵后，**GPU 只能在 uniform 里传**（整帧一个批次）。
做法：

- `gl_set_cm()` 发现矩阵变化就 **先 flush 一次**，再更新 `g->cm[]`；
- `flush_batch()` 里 `glUniform4fv(u_cm, 3, …)` + `glUniform1i(u_cm_on, …)`。

代价是"一个带 filter 的元素多一次 draw call"，换来不走近屏图层（省一块 RGBA 缓冲 +
两遍绘制）。矩阵没变时直接返回，所以一帧内同一份 filter 只 flush 一次。

片元这边顺手做了个结构性改进：整个着色器重构成

```glsl
vec4 shade(){ ... 每个分支只 return 颜色 ... }
void main(){
    vec4 c = shade();
    if (u_cm_on != 0) { c.rgb = clamp(mat3x4 * c.rgb, 0.0, 1.0); }
    frag = c;
}
```

—— **矩阵施加在唯一出口**，这样 §14.1 那个"某个分支提前 return 就漏掉"的坑
在这类"全图元共享的后处理"上不会再出现第二遍。

## 15. 毛玻璃：为什么必须"中途提交一次"

`backdrop-filter: blur(Npx)` 模糊的是**元素背后已经画好的内容**，这在本架构里
是唯一一处"必须回头读自己画过的东西"的效果。

### 15.1 单批次是性能核心，但这里必须破例

前面（§13）说过：整个后端是**单批次**——整帧只在 `ql_gfx_gl_end()` 里发一次
`glDrawArrays`。毛玻璃不能用"整帧结束后再处理"，因为它要读的颜色还没落盘。

所以把提交动作抽成 `flush_batch(g)`：正常帧只被 `end()` 调一次（行为与原来完全相同），
毛玻璃在需要时**自己先 flush 一次**，之后继续往顶点缓冲里摞。代价是：

| | draw call | 顶点数 |
|---|---|---|
| 没有毛玻璃 | 1 | 不变 |
| 有 1 个毛玻璃 | **2** | 不变（多 6 个顶点画回贴图） |

### 15.2 半分辨率两趟模糊

```
目标帧缓冲 ──blit(GL_LINEAR)──▶ tex[0]（半分辨率）
                                  │  H 趟（9 抽头）
                                  ▼
                                tex[1]
                                  │  V 趟（9 抽头）
                                  ▼
                                tex[0] ──▶ mode 4 的四边形画回元素矩形
```

* **半分辨率**：模糊本来就要丢高频，降采样顺带把 4 个像素的工作变成 1 个；
* **两趟可分离**：二维卷积拆成一维两次，代价从 O(r²) 降到 O(2r)；
* **9 抽头固定权重**（`0.227, 0.195, 0.122, 0.054, 0.016`）：
  抽头间距按半径缩放（`半径/4` 个纹素），所以小半径和大半径共用一个着色器；
* `glBlitFramebuffer` 的 `GL_LINEAR` 同时也是**降采样滤波器**，省掉一趟全屏 pass。

### 15.3 三个必须注意的地方

| 坑 | 后果 | 做法 |
|---|---|---|
| `u_bg` 采样器没绑东西 | 采样结果是**未定义值**（实测整块变白或变黑） | 初始化时建一张 1×1 占位纹理，`flush_batch` 里始终绑上 |
| 忘了把视口/FBO 还原 | 后续所有顶点都画进了半分辨率的 blur FBO | 模糊结束显式 `glBindFramebuffer(target)` + `glViewport(全屏)` |
| 纹理上下颠倒 | 玻璃里出现"倒影" | blit 的源 y 用 `height - bottom`，最终四边形的 `v` 用 `1 - (y-y0)/h` 翻回来 |

### 15.4 CPU 侧的对应实现

CPU 没有"读回"问题——画布本身就是已经画好的东西，所以直接在内存里就地模糊
（`ql_canvas_backdrop_blur`）：取出区域 → 两趟滑动窗口盒式卷积（每像素 O(1)）→ 写回。
GPU 侧走的是"blit 一小块 → 半分辨率两趟模糊 → 画回"，中间纹理**只增不减地复用**：
一块大玻璃用完后，更小的玻璃复用同一张更大的纹理，有效内容只在左上角一块。
所以回画的四边形必须按有效区域缩 `uv`，两趟模糊的抽头也要夹在区域内 ——
漏了任一处，小玻璃会显示出"上一块大玻璃被压扁"的内容（模糊位置和背后物体对不上）。

带 `border-radius` 的元素走 `ql_canvas_backdrop_blur_round()`：模糊前先存下四个
`corner×corner` 角落，糊完按圆角覆盖率混回去，于是圆角缺口里不会渗出方形模糊块。

和 GPU 的差异是**故意的**：CPU 走全分辨率 + 盒式，GPU 走半分辨率 + 9 抽头高斯近似。
所以 `test_blur` **不做逐像素比较**，而是断言同一组性质：

* 玻璃矩形内出现一长串"中间色"像素（硬边被摊开）；
* 玻璃矩形外**一个中间色都没有**（模糊不外溢）；
* 半透明的玻璃自己叠在模糊结果之上。

实测（120×80 的用例页）：CPU 有 13 个中间色像素，GPU 7 个（半分辨率 → 斜坡更窄）。
