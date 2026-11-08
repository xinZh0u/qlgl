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
8. `test/widget/test_canvas.c`（像素级断言，改这些函数后必须全绿）

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

- `test/widget/test_effects.c`：28 项检查。除了渐变端点/中点/同色行、alpha=0、
  阴影衰减方向、blur=0 硬边这些**特征断言**，还有一份
  **朴素参考实现 `ref_shadow()`**——对整块外框逐像素求圆角 SDF、不做任何分解——
  用 3 种外框 × 4 种半径 × 4 种模糊 × 3 种偏移 × 3 种 alpha = **432 组**逐像素对照。
  9-slice / 快路径 / 角区镜像都是结构改写，必须逐像素等价，这份对照是唯一的证据。
  它上线时立刻抓到 3 个真 bug（见 11.2.1 的三条"容易写错"）。
- `test_gl_vs_cpu`：示例页现在含渐变与阴影，仍通过 8% 阈值。

> 唯一不参与逐像素对照的是**内实心区**（`d≤0`）：它由 `round_row_span` 的离散判定
> 负责，与连续 SDF 的边界最多差 1px；那块被元素自身背景盖住，属于既定行为。
