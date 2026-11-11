# CSS 开发日志

## 章节分布（三份文档）

这份文档是**开发日志**：每一批落地时怎么做的、踩了什么坑、代价多大、怎么验证的。
逐条状态见 [CSS 覆盖清单](CSS3覆盖与裁剪.html)，排期见 [CSS 进度规划](CSS3进度规划.html)。
章节编号沿用原文档（§18.4–§18.17，外部引用按 § 号定位）。

---

### 18.4 B5 小件：十二个"只加开关"的属性

这一批的共同点是**不动布局大框架**，各自挂到一个已有机制上：

| 属性 | 挂在哪 | 实现要点 |
|---|---|---|
| `visibility: hidden` | 绘制 + 命中 | 只是"不画自己"，布局照常 → 与 `display: none` 的区别就在这里。它是**继承**属性，子级写 `visible` 能显示回来 |
| `pointer-events: none` | 命中测试 | 自己不作为命中目标，**子节点照常**（子级写 `auto` 能重新接住）——盖在按钮上的装饰层用它 |
| `align-self` | flex 交叉轴 | `auto` = 跟容器的 `align-items`；`stretch` 只在交叉轴尺寸是 `auto` 时撑满 |
| `order` | flex 子链表 | 布局开头按 order **稳定** 排一遍子链表，之后测量/断行/摆放/绘制/命中全都自动跟着走（比"每次遍历按 order 挑下一个"少六七处改动） |
| `flex-flow` | 简写展开 | `<flex-direction> \|\| <flex-wrap>`，两个 token 任意顺序；展开进 `flex-direction` / `flex-wrap` |
| `overflow-x` / `overflow-y` | 裁剪 + 滚动 | 分轴只改裁剪矩形的一个方向（另一轴沿用外层裁剪）；只有 `auto` 的轴算可滚范围、才画滚动条 |
| `align-content` | flex 交叉轴 | 多行时按 stretch / start / center / end / space-* 分配行间空间；间距同样用"累计推进"（余数不丢） |
| `position: sticky` | 布局末尾补偿平移 | 在流里占位；滚动平移之后再把自己推回可视边缘，**约束在包含块内**（这就是"跟到头就一起滚走"）。改的是 `border_box`，所以绘制/命中/脏区/`paint_box` 全都自动正确 |
| `inset` | 简写展开 | `inset: <t> <r> <b> <l>` → 四条长写，复用 margin/padding 的 1/2/4 值解析 |
| `cursor` | 命中测试 | 只算**意图**（`ql_ui_cursor_at()`）：命中节点往上找第一个写了 `cursor` 的祖先，都没写时输入框给 `text`、其余 `default` |
| `user-select` | 输入框选字 | `none` → 点还是能点（光标落点不变），但拖拽不再连选 |
| `scroll-behavior: smooth` | `ql_ui_anim_tick` | 只记目标偏移，由每帧推进（160ms ease-out）；"跳"和"滑"共用同一份 `scroll_x/y` 状态 |

**顺带修掉一个真缺陷（这是本轮最重要的收获）**：`align-items: center / flex-end`
在"容器有确定高度、只有一行"这个最常见的情况下**完全没有效果** —— 因为单行的
交叉尺寸取的是"各项最大值"，`room` 恒为 0。CSS 的默认 `align-content: stretch`
要求单行撑满容器的交叉尺寸，补上之后 `align-items` 才真的能用（golden 的
`align` 页因此变化，肉眼核对过：两个方块现在在 120px 高的容器里居中）。

同一轮把 `align-content` 也补上了（多行的行间分配），所以现在单行、多行两种
情况的交叉轴都按 CSS 语义走。

---

### 18.4.1 `position: sticky` 与分轴 `overflow` 为什么便宜

这两件看起来都要"动滚动子系统"，实际都只挂到已有机制上：

```
滚动      apply_scroll：把子树整体平移 -scroll         （早就有）
sticky    平移之后再把自己推回可视边缘，改的是 border_box
          → 绘制/命中/脏区/paint_box 读的都是 border_box，一行都不用改
overflow-x 裁剪矩形只改一个方向（另一轴沿用外层裁剪）
          → QLDraw2D 的裁剪本来就是矩形，逐轴覆盖一个边就够
```

sticky 的三条约束（都写在 `sticky_adjust` 里）：

1. **只贴给了 inset 的方向**（CSS 也是这样：不给 `top` 的 sticky 等于 relative）；
2. 贴边位置按**最近的滚动容器**的内容盒算；没有滚动容器就按视口；
3. 不能推出**包含块**（父内容盒）—— 这就是"跟着列表滚到头就一起走"，
   而不是永远钉在屏幕上。

`cursor` 则刻意只做一半：CSS 值 + 命中测试算出**意图**（`ql_ui_cursor_at()`），
真正设光标要窗口系统配合（X11 `XDefineCursor` / Wayland `wl_cursor`），
后端目前没有接 —— 写进 API 手册 §3.13，不让它悄悄失效。

### 18.5 B4 第一批：`border-style: dashed/dotted` 与 `outline`（blob v25）

这两样都**只改绘制、不动布局**，所以实现上都不碰布局求解器：

```
border-style: dashed/dotted
  → 绘制期把四条边切成小段，每段一个 fill_rect（点线宽 >= 4 时用 fill_round_rect 画圆点）
  → 图案尺寸跟线宽缩放：dashed = 段 3w + 缝 2w，dotted = 点 1w + 缝 1w
  → CPU 与 GL 走的是同一条 fill_rect，天然像素一致（golden 的 b4 页差异 0.5%）

border-style: none
  → 解析期把 border-width 按 0 存（CSS 的 computed border-width = 0）
  → 不改布局代码，但内容盒不会再莫名缩水 2×width

outline: W style color
  → 绘制期用 stroke_round_rect：外沿 = 边框盒外扩 (offset + width)，厚度 = width
    → 这圈线正好落在 [offset, offset + width] 区间；offset 为负就压进边框里
  → 不参与布局：盒子位置/尺寸、兄弟位置、命中测试都不受影响
  → CSS 语义照抄：outline-style 初值是 none，只写 outline-width **不画**
```

为什么 dashed 不走 SDF：SDF 里只有"到形状边界的距离"，没有"沿周长走了多远"，
要画虚线段得在着色器里反解周长参数，成本远高于"切段填矩形"。代价是与
`border-radius` 同用时圆角按直角画 —— 这条写在覆盖表里，不藏着。

验收：`test/web/test_b4.c`（64 项：解析 + 逐点取色）、
golden 新页 `b4`（CPU vs GPU 0.52%）、例程 `example/25_b4`。

### 18.6 B4 收尾：四边四角 / 两层阴影 / 细节属性（blob v26）

这一批把 `QLUIStyle` 里的"单值边框"换成了"四边 + 四角"：

```
uint8_t  border_w[4]        /* 上 右 下 左 */
uint8_t  border_style4[4]   /* 每边自己的样式 */
uint32_t border_color4[4]   /* 每边自己的颜色 */
int16_t  radius[4]          /* 左上 右上 右下 左下 */
QLUIShadow shadow[2]        /* 两层，先写的画在下面（视觉上在最上） */
QLUIShadow text_shadow
```

布局侧的改动很机械：`2 * border_width` 变成了 `ql_ui_border_h(s)`（左右之和）
与 `ql_ui_border_v(s)`（上下之和），内容盒原点按 `border_w[3]` / `border_w[0]` 内缩 ——
CSS 的"四边宽度不同"就是这样影响内容盒的。

绘制侧分三条路（`paint_border`）：

```
四边全同        → 一次整圈描边（最常见，也是最快的一条）
有 dashed/dotted → 逐边切段（角上按直角）
各边不同        → 整圈描边被"按边裁四次"，每次换一种颜色
```

第三条里的裁剪**必须原样恢复**：一开始忘了恢复，结果 `.sides` 之后的所有兄弟节点
都被裁进那一个小盒子里 —— 画面表现为"某一行之后整页空白"。写进注释，别再踩。

四角圆角则靠两个新入口：

- `QLDraw2D::fill_round_rect4` / `stroke_round_rect4` / `fill_round_rect_gradient4`
  （四角不全同时才走；全同仍走单半径的快路径）
- CPU：`round_row_span4()` —— 每行左右两侧各算一次圆心到边的 `dx`
- GPU：新增顶点属性 `a_radii`（vec4），片元里按象限挑半径，SDF 不变

投影与文字：

- `box-shadow` 最多**两层**，`inset` 用反向 SDF（GPU `mode=7`，CPU 逐像素但只算"环"）
- `text-shadow` 一层，`blur` 用 5 次采样近似（不需要离屏缓冲，MCU 也能跑）

细节属性都很便宜：`image-rendering`（GL 切纹理滤波）、
`scrollbar-width/color`（内置滚动条的宽度与颜色）、
`object-position`（`object-fit` 之后在余量里摆位，`cover` 时改的是源矩形）。

验收：`test/web/test_b4.c`（104 项）、golden 的 `b4` 页（CPU vs GPU 0.05%）、
例程 `example/25_b4`（九节，含滚动条与 focus 环）。

### 18.7 B2 / B3 尾巴与背景图（blob v27）

这一节收掉五批里最后三项。它们共同的思路是：**能在构建期算清的一律不算在运行期**。

**状态选择器**（`QLUINode` 20 → 28 字节）：

```
style_disabled / style_checked / style_placeholder   /* 各一份变体样式下标 */
placeholder                                          /* 文案在文本池里的下标 */
flags: QL_UI_NODE_CHECKBOX / _CHECKED / _DISABLED
```

- 与 `:hover` 完全同构：多带一个状态位再跑一遍级联，与基础样式不同才建一份新样式；
- 运行期优先级 `disabled > active > focus > checked > hover`；
- `:enabled` 是"没有 disabled"，所以**基础样式**按节点属性决定要不要打开这个位；
- 复选框有自己的 UA 默认（内部标签名 `#cb` → 16×16、无内边距），
  并且**不进输入框池**（它没有文本）；
- `::placeholder` 在构建期把 `input::placeholder` 的声明**合并进宿主样式**，
  单独存一份"占位样式"，输入框为空时用它画。

**文本细节**（`QLUIStyle` 240 → 272 字节）：

- `word-spacing` 只加在空格之后（量宽与绘制走同一个 `adv_of`）；
- `tab-size` 在**建表期**把 `\t` 展开成 N 个空格 —— 运行期一个字符判断都没有；
- `text-decoration-*` 的形态用 `fill_rect` 拼（double 两条、dotted 点、dashed 段+缝），
  不需要新图元，CPU / GPU 天然一致；`overline` 占 `deco_style` 的高位。

**背景图**：与 `<img src>` 共用资源表。

```
background-image: url(checker)  → 名字进文本池，样式里存 QL_UI_NO_IMAGE 之外的下标
运行期 ql_ui_register_image("checker", ...) 按名字填像素（同一张图只解码一次）
绘制：object-fit 的同一套几何（cover / contain / <w> <h>）+ position + repeat
```

平铺时先把起点对齐到可见瓦片、再把裁剪设成边框盒 —— 不设裁剪会画到兄弟节点上
（与"四边不同色"那次裁剪没恢复是同一类错误，这次提前防住了）。

**当时没做的：`filter`**。颜色矩阵类（brightness/grayscale/...）技术上可行，
但 `blur()` / `drop-shadow()` 需要**离屏图层**（子树先合成到一张图再整体模糊），
当时留作单独立项 —— 后来 M9.3 落地（§18.10），`transform: rotate` 也在这层上
长出来（§18.11）。

### 18.8 B6/B7 的代价是怎么估出来的

新批次（Grid / 表格 / 多栏 / `border-image`）都要在"进 MCU 能力包"之前先说清代价。
这一节说明**怎么量**，数字见 §2.1。

**① 布局的边际成本**（`test/mcu` 的同一份 CSS，只改卡片数量）：

```
12 节点 0.23 ms → 39 节点 0.82 ms → 75 节点 1.94 ms
拟合斜率 ≈ 4.4k 指令/节点 ≈ 22 µs/节点 @200 MHz
```

**② wrapper 容器的成本**（同一视觉，两种结构）：

```
3×3 卡片用嵌套 flex（3 个 .row，15 节点）   0.427 ms
同一视觉用扁平 flex-wrap（12 节点）         0.229 ms
```

差 0.198 ms 只换来 3 个"什么都不画"的 `.row` —— **一层 wrapper ≈ 3 个叶子节点**。
这条读数直接推翻了旧文档"Grid 太贵所以不做"的判断：

- Grid 的求解是"轨道定尺 → 放置 → 对齐"三段，都是整数加法，**没有逐像素**；
- 而"用 flex 拼网格"要付 wrapper 的钱，**每次重排都付**；
- 所以 Grid 落地后，网格类页面的 MCU 布局时间是**下降**的（省 46%，见上）。

**③ 表格为什么比 Grid 贵**：`table-layout: auto` 的列宽分配要**两遍**
（第一遍量每列 `min-content` / `max-content`，第二遍按可用宽度分配），
即"布局边际成本 ×2"；`border-collapse` 还要"半边边框相加"。所以它估 6–10 KB。

**④ 多栏最贵**：`column-count` 要求列高尽量相等，只能"先排一遍量总高、
算出目标列高、再排一遍断列"。同样是 ×2 遍，但还要维护"一个节点跨两列"的中间态 ——
这是**唯一真正与"一个节点一个盒"冲突**的一项，所以估 8–15 KB。

**⑤ `border-image` 最便宜**：九宫格 = 把源图切 9 块，四角原样、四边单向拉伸、
中间双向拉伸。绘制走的是现成的 `ql_canvas_blit_image()`（只是采样区间不同），
解析期切一次就存进样式，**运行期零额外状态**。估 2–3 KB。

**⑥ 第三方库为什么必须按 `G` 处理**（`@font-face` / 整形 / 双向）：

```
libharfbuzz.so   1,178 KB   = 当前整个 MCU 固件（131.5 KB）的 8.9 倍
libfreetype.so     813 KB   = 6.2 倍
libfribidi.so      106 KB   = 0.8 倍（fribidi 本身还勉强，但单独它不解连字）
一份全量 CJK TTF    21 MB   = 160 倍
```

这几样在 PC/Mobile 档是**必须的**（否则中文以外的语言、真字重、连字全都没有），
在 MCU 档是**必须裁的**（裁掉后仍是内建 ASCII 位图字体 + 可选子集 TTF）。
这就是 `G` 这个原因码存在的意义：**它标的是"依赖关系"，不是"不做"**。

---

### 18.9 B6 落地：Grid 与 border-image 是怎么做的

#### ① 数据放哪：模板进池，摆放进样式

`QLUIStyle` 只长了 40 字节（272 → **312**），因为**轨道列表不进样式**：

```c
/* QLUIStyle：容器 3 个字段、子项 4 个字段 */
uint16_t grid;          /* doc->grids[] 下标；QL_UI_NO_GRID = 不是 grid 容器 */
int16_t  grid_col, grid_row;        /* 起始线（1-based）；0 = 自动 */
uint8_t  grid_col_span, grid_row_span;
uint8_t  grid_flow, justify_items, justify_self;
int16_t  grid_auto_row, grid_auto_col;

/* doc 里三段池（所有容器共享；同一模板经样式去重后只存一份） */
QLUIGrid      grids[];       /* col/row 轨道的起止下标 + 具名区域起止 */
QLUIGridTrack grid_tracks[]; /* { min_kind, max_kind, min_val, max_val } */
QLUIGridArea  grid_areas[];  /* { 名字, 列, 行, 列跨, 行跨 } */
```

一条轨道是 `[min, max]` 区间：`100px` → 两边都定长；`1fr` → `min=auto, max=fr`；
`minmax(60px, auto)` → `min=60px, max=auto`。定尺时先满足 min，再把剩余按 max 侧的
"拉伸能力"分配（`fr` 权重 / `auto` 取内容）。

#### ② 摆放与模板搜索为什么在构建期

`grid-area: main` 要查**父容器**的具名区域，`grid-column: 1 / -2` 要查父容器有几列 ——
这些在样式编译时都还不知道（编译是逐节点、按声明集去重的）。所以分两趟：

1. `compile_style()` 只把 Grid 属性的**原文**挂到"按样式下标"的侧表上；
2. 整棵树都有了样式之后，`grid_resolve_all()` 统一解析：模板 → 池；
   每个子项的线号/跨度 → 4 个整数写进样式。

去重带来一个坑：同一个 `.cell` 可能被两个不同模板的容器使用，摆放值就不一样。
这里用**写时复制**——样式第一次被占用时直接写，第二次要不同的值就复制一份样式
（`sig = NULL`，不再参与去重）。运行期因此永远只读整数，没有任何字符串比较。

#### ③ 布局：三段，全是整数

```
① 放置   ：显式位置 → 只给列 → 只给行 → 全自动（游标扫描，dense 时回原点）
② 列定尺 ：min → 定长上限 → auto 取内容 max-content → fr 分剩余（累计推进）
③ 行定尺 ：先按"列宽已定"把每项真排一遍拿高度 → auto 行取最大值 →
           span>1 按差额均摊 → fr 行分剩余（容器高度确定时）
④ 定位   ：按 justify-self / align-self 在格子里摆，默认双向 stretch
```

`fr` 的份额用**累计分子**（`num += 总额×权重; 份额 = num/权重和; num %= 权重和`），
与 flex 的 grow 同一套 —— 三列 `1fr 1fr 1fr` 在 100px 里是 33+34+33，右边界严丝合缝。

#### ④ 嵌套 grid 的临时区：按栈的纪律切

轨道数组、列/行起点、每项的放置结果都是临时数据。**不能**用一份全局数组：
测量阶段会递归进入内层 grid，把外层刚算好的列宽冲掉（实测：内层列宽对、外层整体
缩小一半）。所以 `QLUIRoot` 里放一个只增不减的缓冲 + bump 指针：

```c
const uint16_t saved_place = ui->grid_place_used;
const uint32_t saved_coord = ui->grid_size_used;
... 用 ...
ui->grid_place_used = saved_place;   /* 函数出口（含降级路径）都还原 */
ui->grid_size_used  = saved_coord;
```

`ql_ui_layout()` 每次把两个指针归零，所以稳态下**零分配**（不违反"热路径不 malloc"）。

#### ⑤ `border-image`：九宫格 = 9 次 blit

后端早就有 `blit_image(dst, src)`（子矩形采样），所以九宫格不需要新图元：

```
四角：src 原样 → dst 角块（缩放）
四边：src 单条 → dst 单条（stretch / repeat / round / space）
中间：src 中心 → dst 中心（stretch，或裁剪后平铺）
```

切片值 `border-image-slice` 是**源图**上的像素（或百分比）；
画出来的宽度用 `border-image-width`，默认等于切片值。
写了 `border-image` 就不再画 `border-style`（CSS 语义）。

资源走的是 `<img src>` / `background-image` 的**同一张表**：
`ql_ui_register_image(ui, "frame", ...)` 填一次像素，三种用法共享。

### 18.10 M9.3 落地：离屏图层（`filter` / `mix-blend-mode` / `mask` / 真 group opacity）

四样特性共用一句前提：**先把子树合成到一张临时图上，再整体处理**。

#### 触发条件（`style_needs_layer()`）

| 样式 | 开图层？ |
|---|---|
| `filter_n > 0` | 是 |
| `mix_blend != normal` | 是 |
| `mask_image != none` | 是 |
| `opacity < 1` **且**有子节点 | 是 |
| 其余 | 否 —— 走原来的单批次路径 |

#### 绘制范围

```
bounds = 边框盒 ∪ paint_box（子树可能画到的范围）
       ∪ blur·3 的外扩
       ∪ drop-shadow 的偏移与 blur·3
       ∩ 当前裁剪（不裁剪就会为屏幕外的东西分配缓冲）
```

**裁剪仍然按元素的边框盒 + 圆角**：所以"圆角卡片 + 模糊光晕"的四个角是干净的，
光晕不会漏到圆角外面（`QLDraw2DLayerFx.box` + `.corner` 就是为这一条存在的）。

#### 后端协议（`qldraw2d.h`）

```c
int  (*layer_begin)(QLDraw2D *, QLRect bounds, const QLDraw2DLayerFx *);
int  (*layer_end)(QLDraw2D *);      /* 返回 1 = 请把同一棵子树再画一遍 */
```

`layer_end` 返回 1 是 CPU 侧的协议：它要两遍。

#### CPU：两遍哑光反解

CPU 光栅有二十多条混合路径（整行快路径、SSE2 块、字形位图、图片搬运…），
它们全都写成"只在 RGB 上混、alpha 恒 0xFF"。要让它们各自累积 alpha，
改动面大且极易漏。于是换个方向：**同一棵子树画两次**，一次在黑底、一次在白底。

```
黑底：B = C·a          白底：W = C·a + 255·(1-a)
⇒  a = 255-(W-B)，而**预乘色就是 B**
```

既不用除 alpha（除法的量化误差很大），也不用知道 C。代价是子树画两遍
（实测：一块 200×120 的卡片多花 ~0.2 ms），换来的是**零风险**——
所有既有图元的混合语义一个字都没改。

> 三个通道各自给出的 alpha 会差 1~2（8 位量化），实现里取平均把误差摊掉。

#### GPU：真 RGBA FBO + 一个合成着色器

GL 有 RGBA 帧缓冲，只画一遍。合成走 `LAYER_FS`：

- 颜色矩阵族（6 个 uniform 对，按顺序作用在**直通色**上，再乘回 alpha）；
- `opacity` / 圆角裁剪（SDF + smoothstep，与 CPU 同一公式）/ `mask`（第二张纹理）；
- 混合模式：把目标表面那一块先 `glBlitFramebuffer` 成一张纹理当背景，
  片元里按 CSS 公式算（和毛玻璃同一个套路）；
- `drop-shadow`：额外的两趟模糊 + 一次"只画影子"的合成（先画影子，再画本体）。

**踩过的坑**（都在代码注释里留了记号）：

1. `layer_ensure()` 建纹理时临时切了 FBO，结尾一句 `glBindFramebuffer(0)`
   把**外层图层**的输出丢到了默认帧缓冲 —— 嵌套时外层整个不见。必须存/还原绑定。
2. 模糊那几趟把 viewport 改成了模糊纹理的尺寸，不切回来合成四边形就会按错误投影
   画出去。抽了 `gl_bind_target()` 统一负责"绑定 + viewport + scissor"。
3. 毛玻璃那套 `BLUR_FS` 的片元固定输出 `alpha = 1`（它背后永远是不透明的页面）。
   拿它糊图层会把透明区域糊成**不透明黑**，图层边缘立刻出现黑边。
   所以另开了一个 4 通道版本 `BLUR_RGBA_FS`。
4. GLSL 的 `mat3(a,b,c, …)` 是**按列**填的，照抄 CPU 的行主序色相矩阵会得到转置
   （色相旋转不对称 → 颜色明显跑偏）。改成逐行写死 `dot()`。
5. CPU 侧 `ql_layer_drop_shadow()` 里写反了合成顺序
   （`shadow + layer*(1-shadow_a)` 是"影子盖住元素"），现在固定为
   `layer + shadow*(1-layer_a)`。
6. **`glBlitFramebuffer` 也受 scissor 影响**（它是片元流水线之外的少数例外之一）。
   混合模式要把"图层背后那一块"先 `blit` 进一张纹理，那张纹理的目标矩形在 `(0,0)`；
   脏区重绘时 scissor 却是"屏幕上那一小块"，两块不重叠 → 整次拷贝被**静默丢弃**，
   没有 GL 错误。表现：`multiply`/`overlay`/`darken` 全黑、`screen`/`difference`
   退化成原色（首帧背景纹理刚建 = 黑；之后的帧会读到**上一次**抓的背景）。
   整屏重绘时 scissor 恰好覆盖 `(0,0)`，所以只测整屏的用例永远是绿的 ——
   回归用例必须让"背后的颜色变了、而只重画这一小块"（`test_gl_dirty` 第 5 节）。

#### 一致性

`test_gl_vs_cpu` 里新增六个图层用例（blur / drop-shadow / 颜色矩阵链 /
multiply / 组 opacity 重叠 / 嵌套两层），两条后端的差异：

| 用例 | 差异 | 最大单通道差 |
|---|---|---|
| `filter: blur` | 0.00% | 1 |
| `filter: drop-shadow` | 0.00% | 1 |
| 颜色矩阵链 | 0.00% | 3 |
| `mix-blend-mode: multiply` | 0.00% | 1 |
| 真 group opacity（重叠） | 0.00% | 0 |
| 嵌套两层 | 0.95% | 152（全在抗锯齿边上） |

模糊为了两端一致，统一改成"9 抽头、抽头间距 = 半径/2、边缘钳位"——
CPU 原来那套"两趟盒式 ≈ 钟形"与 GL 的高斯形状不同，同一份 CSS 会糊出不同宽度。

#### 容量与裁剪

| 项 | 上限 |
|---|---|
| 每层缓冲 | 宽 × 高 × 4 字节（预乘 RGBA） |
| 嵌套深度 | `QL_CFG_UI_LAYER_MAX_DEPTH`（默认 4） |
| 单层尺寸兜底 | 4096×1024（防误用） |
| `filter` 链长 | `QL_CFG_UI_FILTER_MAX_OPS`（默认 6） |
| 编译开关 | `QL_FEAT_CSS_FILTER_LAYER`（MCU 默认关，整个 `qllayer.c` 编译成空） |

**MCU 实测**（`test/mcu/build.sh`，Cortex-M4 / mps2-an386，只算静态量）：

| 配置 | `.text` | `.bss` |
|---|---|---|
| `QL_FEAT_CSS_FILTER_LAYER=0`（MCU 默认） | 139,740 B | 140,264 B |
| `QL_FEAT_CSS_FILTER_LAYER=1` | 155,084 B | 140,940 B |
| **差额** | **+15,344 B（+11.0%）** | **+676 B**（图层栈本身） |

那 676 B 是 `QLDraw2DCanvas` 里的图层栈（4 层 × 约 170 B），编译成空之后 RAM
和 Flash 一起还回去。**运行期**的 RAM 峰值另算：一层 = 元素尺寸 × 4 字节，
所以 MCU 上这个包是"默认关 + 真要玻璃面板时按页面调"。

对比一下：`CSS_GRID` 是 **4,528 B**、`CSS_BORDER_IMAGE` 是 **2,176 B** ——
离屏图层是这两者之和的**两倍多**，也解释了为什么它必须是一个独立的能力包
（而不是塞进 BASE）。

### 18.11 M9.4 落地：旋转光栅（`transform: rotate/skew/matrix` + `transform-origin`）

CSS 的 `transform` 作用于**元素的渲染结果**，不是"把每个图元按矩阵画一遍"。
这条语义决定了实现方式：

```
子树先按原样画进离屏图层  →  再按 2×3 矩阵做一次仿射重采样  →  贴回目标
```

逐图元转（另一种做法）会把文字转成马赛克、把圆角转成方角、把 `filter` 的结果
转丢，所以从一开始就否掉了。好处是它**完全复用 M9.3 的图层基建**：
`style_needs_layer()` 里只要多一条"有 xform 就要图层"。

#### 数据：矩阵进 doc 池，样式只留下标

| 项 | 做法 | 理由 |
|---|---|---|
| 矩阵存哪 | `QLUIDoc.xforms[]`（`QLUIXform` = 6 × int32，26.6 定点） | 24 B/样式在 MCU 上不可接受；同一份 CSS 里重复的 `rotate(15deg)` 只存一份（按值去重，池上限 32） |
| 样式里存什么 | `int16_t xform`（`QL_UI_NO_XFORM = -1`）+ `QLUILength origin[2]` | 共 +8 B/样式（336 → **344**），blob **v30**、头 144 → 152 |
| 坐标系 | 矩阵在 **CSS px 空间**定义 | 设备空间 = 同一份线性部分；平移与原点乘 dpr（等比缩放与线性变换可交换） |
| `transform-origin` 的百分比 | **千分比**（0..1000） | 与宽度类的 `/100` 不同：这里用 1000 才能表达 1‰ 的微调，且与 `sc_x` 同一口径 |

轴对齐的 `translate()` / `scale()` **不进**矩阵池（零重采样，走老的绘制期变换）。
一条声明里只要出现 `rotate/skew/matrix`，整串（含 `translate`/`scale`）乘成一个矩阵
—— 这是 CSS 的语义，也避免了"一半在布局期、一半在合成期"。

#### CPU：逆向映射 + 双线性

正向映射（把源像素搬到变换后的位置）会在放大时留空洞，所以按目标像素反查：

```
q（目标像素中心） → u = q - origin - t → p = origin + M⁻¹·u → 在图层缓冲里采样
```

圆角 / 遮罩用**局部（未旋转）坐标**判定，也就是"先裁再转"，与浏览器一致。
轴对齐矩阵（含 `matrix(1,0,0,1,0,0)`）仍走原来的整块贴回快路。

#### GPU：变换四角 + 一个 `u_local` 开关

四边形还是 4 个顶点，只是把四角按矩阵摆过去。平行四边形上的 `v_uv` 线性插值
**恰好等于**局部坐标，于是片元里一句就能换来正确的圆角判定点：

```glsl
lp = (u_local != 0) ? u_lbounds.xy + vec2(uv.x, 1.0 - uv.y) * u_lbounds.zw : dp;
```

#### 踩过的坑（最重要的两个）

1. **图层合成整体上下翻转**（M9.3 就存在，M9.4 才暴露）。
   内容是按"CSS y 向下"画进 FBO 的（主着色器里 `ndc.y = -ndc.y`），
   所以纹理的 **v=1 才是元素顶边**；而合成四边形的 uv 一直按 v=0 在顶边取。
   纯色、居中的规则方块上下对称，六个老用例全是绿的 —— 一旦图层里有"上红下蓝"
   或者转过的文字，立刻原形毕露。现在四边形的 uv 顶边给 v=1，同时把依赖
   `v_uv` 的三处一起翻正：遮罩图（`1-uv.y`）、背景纹理（直接按 `uv`）、
   `drop-shadow` 的 y 偏移（改号）。
2. **嵌套图层的位置**：内层的 `L->dev` 已经是"相对外层 FBO"的坐标（进图层时
   offset 已经减过外层原点），再减一次外层 `dev` 就会整体上移。合成四边形的
   归一化分母取当前目标的 `g->width/height` 即可，别再减原点。

#### 命中测试

把屏幕点按矩阵**逆变换**回局部空间再判盒，逐层下降正好是仿射连乘：

```c
det = a*d - b*c;                          /* =0：压成一条线，点不中 */
lx  = (d*ux - c*uy) / det;   ly = (a*uy - b*ux) / det;   /* 出来就是 px */
```

（`ux/uy` 先乘 `QL_FX_ONE`，`det` 是 `FX²`，所以结果不用再除 —— 第一次写成了
再除一次，表现是"原位置也能点中"。）

#### 一致性（`test_gl_vs_cpu` 新增 7 个用例）

| 用例 | 差异 | 备注 |
|---|---|---|
| 图层方向（上下不对称） | 0.00% | 专门守上面第 1 条 |
| 图层内文字（混合状态） | 0.23% | 守 M9.3 的"图层结束后没还原混合状态" |
| `rotate(90deg)` / `rotate(30deg)` | 0.00% / 0.00% | 轴对齐完全一致；30° 只有边缘 11 px 不同 |
| `skewX` / `matrix` | 0.00% / 0.15% | 差异全在斜边那一个像素上 |
| `rotate` + 文字 | 0.06% | 双线性 vs 逐像素覆盖率 |

CPU 侧的硬数字在 `test_transform`（63 项）：`rotate(90°)` 把 40×10 的横条变成
10×40 的竖条且像素数不变、`rotate(45°)` 面积 364 px、`skewX(45°)` 包围盒 40+10、
`transform-origin: left top` 让顶边留在轴上、圆角 + 45° 仍是圆的边缘。

> 重采样对越界点做**边缘钳位**，所以旋转/平移后的元素外沿会多出约 1 px
> 的"边缘色"。两个后端一致，肉眼不可见，但写断言时要算上这一圈。

#### 容量与裁剪

| 项 | 值 |
|---|---|
| 矩阵池 | 32 个（超出报错，不静默丢） |
| 每样式额外 | +8 B（`int16` 下标 + 两个 `QLUILength`） |
| 编译开关 | 与 M9.3 同一个：`QL_FEAT_CSS_FILTER_LAYER` |
| 裁掉之后的行为 | `rotate/skew/matrix` **构建期报错**（不做"悄悄不转"的降级） |

**MCU 实测**（同一套 mps2-an386 基准，与 §18.10 同一把尺子）：

| 配置 | `.text` | `.bss` |
|---|---|---|
| `QL_FEAT_CSS_FILTER_LAYER=0`（MCU 默认） | 139,740 → **140,544 B**（+804） | 140,264 B（不变） |
| `QL_FEAT_CSS_FILTER_LAYER=1` | 155,084 → **158,572 B**（+3,488） | 140,940 → **141,148 B**（+208） |

也就是说：**矩阵本身只要 804 B**（解析 + 定点运算），真正贵的是它能用的那张
图层（3,488 B 的合成慢路）。原规划"比离屏图层便宜"的判断成立。

---

---

### 18.12 B7 落地：表格（`display: table` 全家）

浏览器里表格是**两套 display 交叉**的结构，本库照搬：

```
table（有盒子） ← caption（有盒子，排在上方）
                ← row-group（**没有盒子**）← row（**没有内容盒**）← cell（真正的盒子）
```

所以样式要写在 `td`/`th` 上 —— 写在 `tr`/`thead` 上不会有边框和背景，这一点与
浏览器一致（不是偷懒）。

#### 布局：五遍扫描，全部用"计数器"而非矩阵

| 遍 | 做什么 |
|---|---|
| 1 | 收集行（展开 `thead`/`tbody`/`tfoot`），按 rowspan 顺延算出**真实列号**，得列数 |
| 2 | 定列宽：首行显式 `width` 的单列单元格优先，其余列等分剩余宽度 |
| 3 | 量行高：`rowspan == 1` 的单元格各排一次，取最大值 |
| 4 | 跨行单元格：不够高就把差额补给覆盖范围内**最后一行** |
| 5 | 摆放：按列宽/行高定位，再把单元格高度撑到整行、内容按 `vertical-align` 摆 |

**关键数据结构是 24 字节**：`uint8_t live[QL_CFG_UI_TABLE_MAX_COLS]`
—— "每列还被上面几行的 rowspan 占着"。行是按顺序处理的，行末把大于 0 的减一即可。
不用 `rows × cols` 的占用矩阵（64×24 = 1.5 KB 栈，MCU 上不可接受）。

**踩过的坑（都留了注释）**：

1. **rowspan 之后的列号**。只看"本行第几个格子"会把第二行的第一个单元格排到第 0 列，
   压住上一行跨行的那个格子（第一版就是这样，截图里 `Q1` 和 `二月` 叠在一起）。
   必须按"跳过被占用的列"来分配起始列 —— 五遍扫描每一遍都要做同样的记账。
2. **单元格不能用文字撑宽**。`border_width_of()` 里有一条"行内盒宽度由文本决定"的
   快路，`display: table-cell` 不是 block，一开始就落进了那条快路 → 每个单元格只有
   文字那么宽（34px）。现在把**所有表格角色**排除在"文本撑宽"之外。
3. **`vertical-align` 的余量要用"自然内容高"算**。单元格自己写了 `height: 60px`
   时 `border_box.h` 已经是 60，拿它当内容高就永远算不出余量（middle 和 top 一样）。
   所以另写 `table_cell_content_h()`：有子节点取子树底边，纯文字按"折行后的行数 × 行高"。
4. **文字直接挂在单元格上**时（没有子节点），平移子节点是空操作 —— 要挪的是
   `content_box.y`，绘制时文字才会跟着走。
5. **`collapse` 的相邻边框**：把每个单元格朝左上挪"自己那半条边框"、同时把尺寸补回
   同样的量，两个边框就落在同一像素上；右/下边缘仍落在网格线上（不挪则外框会跑出去）。

#### `table-layout: auto`：min/max-content 之间插值

第 2 遍（定列宽）有两条路，`s->table_layout == 0` 走 auto：

1. 单列单元格各量一次 **max-content**（`max_content_main`）与 **min-content**
   （最长连续非空白段，中文按整句算），取该列所有单元格的最大值 → `pref[]` / `floorw[]`；
2. `colspan > 1` 的单元格：它覆盖的列宽之和不够就**均摊补差额**（还不开循环，只做一遍）；
3. 三种情形落定：
   - `avail >= Σpref` → 余量**按 `pref` 比例**摊给各列（表格式块级，盒子仍撑满容器，
     所以余量只能摊到列上 —— 这一点与浏览器"表格盒子跟着内容走"不同，见 §19.1 的受限表）；
   - `avail <= Σfloor` → 直接取 `floorw`（宁可溢出也不把字压碎）；
   - 中间 → 按 `[floor, pref]` 线性插值。

#### `<colgroup>` / `<col>`

`display: table-column(-group)` 是**真角色**（不再是 `display: none`）：不产生盒子，
只在第二遍里提供列宽。`<col span>` 用 `span` 属性（不是 `colspan`），构建期就展开成多列。
`fixed` 下 `<col>` 的宽度**优先于首行**；`auto` 下它同时充当 `pref`/`floor` 的**初值**
（内容更宽时取内容）。

两个坑：

1. **单元格宽度必须由列宽决定**。`td{width:250px}` 在 fixed 下只是"这一列要 250"的申请，
   列宽算完之后单元格必须填满整列。第一版没有这一步，`td{width}` 会把自己的 border-box
   撑到 250，把列宽算出来的结果覆盖掉（`<col>` 明明写了 40，渲染出来还是 250）。
   现在摆放前用 `table_force_cell_w()` 走 `style_copy` 把宽度改写成列宽。
2. **`<col>` 是空元素**。不把 `col` 加进"空元素表"，`<col>` 会一路吞掉后面的兄弟节点，
   报"结束标签不匹配"。

#### 明确不做的

`caption-side: bottom`、`empty-cells`、跨行内容超高时"按比例分配差额"。
都在 §16 的表格里如实标着。

#### 验证

`test_table`（78 项）：标签 → 角色映射、等分/固定列宽、`auto` 的内容列宽
（长文本列 >3 倍于短文本列、同样内容下 fixed 仍是等分）、`colspan`/`rowspan` 的宽高、
`<col>`/`<col span>` 在 fixed 与 auto 下的列宽、`border-spacing`、`collapse` 的像素级单线、
`vertical-align` 三档（量第一行墨迹）、`caption` 宽度、5 条报错。

#### 代价与裁剪

| 项 | 值 |
|---|---|
| `QLUINode` | 28 → **30 B**（`colspan` / `rowspan`） |
| `QLUIStyle` | 344 → **352 B**（`table_layout` / `border_collapse` / `valign` / `border-spacing`） |
| 编译开关 | `QL_FEAT_CSS_TABLE`（**MCU 默认关**） |
| 裁掉之后 | `<table>` 与 `display: table*` **构建期报错**；布局退回普通块流 |
| 容量上限 | `QL_CFG_UI_TABLE_MAX_ROWS`(64) / `QL_CFG_UI_TABLE_MAX_COLS`(24) |

**MCU 实测**（mps2-an386，与 §18.10 / §18.11 同一把尺子）：

| 配置 | `.text` | `.bss` |
|---|---|---|
| `QL_FEAT_CSS_TABLE=0`（MCU 默认） | 141,624 B | 140,268 B |
| `QL_FEAT_CSS_TABLE=1`（多栏仍关） | 146,956 B | 140,268 B |
| **差额** | **+5,332 B（+3.8%）** | **0**（布局用 24 字节栈，不占静态 RAM） |

（`table-layout: auto` 与 `<col>` 落地前是 +3,156 B；auto 的两遍 min/max-content 测量
与 `<col>` 的列宽收集又加了约 2.2 KB。）

比 Grid（4,528 B）贵一些、比 border-image（2,176 B）贵一倍多 —— 主要成本是
"每列还被占几行"的记账、五遍扫描的分支，以及 auto 的 min/max-content 测量。

### 18.13 B7 后半落地：多栏（`column-*`）

多栏**不是**"把一份内容排两遍"。本库的做法是三步，且只有第一步真的排版：

| 步 | 做什么 |
|---|---|
| 1 | 按栏宽把每个子节点在 `(0,0)` 排一遍（这一次就是正常的 `layout_node`），顺带记下高度 |
| 2 | 定栏高：容器有 `height` 就用它；否则**二分**找"最小能让内容塞进 N 栏"的栏高（纯算术，不重排） |
| 3 | 按顺序贪心分配 + `translate_subtree` 平移到最终位置（**不重排**） |

**栏数**怎么来（`n`）：

```
n = column_count                      /* 只给了栏数 */
n = floor((宽 + 缝) / (栏宽 + 缝))     /* 只给了栏宽 */
n = min(column_count, 上一个式子)     /* 两个都给：取小的 */
```

然后 `栏宽 = (容器内容宽 - 缝×(n-1)) / n`，上限 `QL_CFG_UI_COLUMN_MAX`（8）。
`column-gap` 与 flex 共用 `gap_col` 字段；没写时按 CSS 的 `normal`≈16px 算。

**为什么"不劈子节点"这条要写死**：真分栏的断点可能落在文字行中间，需要把同一个节点的
绘制切成两段 —— 而本库的绘制、命中测试、滚动、`paint_box` 剪枝全都建立在"一个节点
一个盒"上。要支持就得引入"盒内分裂"整套机制，与 MCU 目标直接冲突。
所以：**一个子节点整体落到某一栏**，塞不下就换栏（空栏至少放一个，避免死循环）；
最后一栏放不下就**溢出**（宁可溢出也不丢内容）。

**踩过的坑**：

1. **`cursor` 与 `target` 单位不同**。`cursor` 是绝对坐标（`content_y + 已用高度`），
   `target` 是"栏高"。第一版直接写 `cursor + adv > target`，于是从第二个子节点起
   永远判定为"放不下"，每栏只放一个、其余全堆在最后一栏（例程 34 第一节一跑就现形）。
   正确的是 `(cursor - col_top) + adv > target`。
2. **均分栏高不等于"等高"**。`ceil(总高/n)` 在子节点高度不齐时会让第一栏连一个高个子
   都塞不下（同上现象）。所以改成二分求"最小可容纳栏高"——栏高越大需要的栏数越少，
   这个谓词是单调的，二分 12 次足够；代价是纯算术重扫，不打乱任何盒子。
3. **`column-rule` 不是 `border`**。`paint_dash_run()` 会把"solid"也切成
   `3w + 2w` 的虚线，直接复用会画成一串短横线。solid 要走一次 `fill_rect`。

#### 绘制

栏间分隔线画在**背景/边框之上、内容之下**（`render_node_inner` 里），
x 取缝的中点，高度取内容盒高；`column_rule_width == 0` 或 `style == none` 时整条不画，
颜色 0 表示 `currentColor`。

#### 验证

`test_columns`（34 项）：栏数三种来源、栏宽公式、等高平衡（4 项 2 栏 → 每栏 2 项、
容器高 40）、确定高度下"每栏 1 项 + 剩余溢出"、`column-rule` 的像素级检查
（缝中点 1px 红、`width:0` 不画）、5 条报错。

#### 代价与裁剪

| 项 | 值 |
|---|---|
| `QLUIStyle` | 352 → **364 B**（`column_count` / `column_rule_*` / `column_width`） |
| `QLUINodeRT` | +4 B（`col_n` / `col_w`，绘制分隔线要用；运行期结构，不进 blob） |
| 编译开关 | `QL_FEAT_CSS_COLUMNS`（**MCU 默认关**） |
| 裁掉之后 | `column-*` **构建期报错**；布局退回普通块流 |
| 容量上限 | `QL_CFG_UI_COLUMN_MAX`(8) |

**MCU 实测**（mps2-an386，同一把尺子，见 §21.2 的汇总表）：

| 配置 | `.text` | `.bss` |
|---|---|---|
| `QL_FEAT_CSS_COLUMNS=0`（MCU 默认） | 141,624 B | 140,268 B |
| `QL_FEAT_CSS_COLUMNS=1`（表格仍关） | 142,264 B | 140,268 B |
| **差额** | **+640 B（+0.5%）** | **0** |

两个都开：147,596 B（+5,972 B）。多栏比表格（+5,332 B）便宜一个数量级：
没有列宽/行高那五遍扫描，只有"排一遍 + 二分 + 平移"。

### 18.14 B8 落地：文本栈（`@font-face` / 家族列表 / 整形 / 双向）

三段是**一件事的三个层次**，所以一起落地：

```
@font-face          → 注册表：家族 →（字重, 斜体）→ 字体文件
font-family: A, B   → 回退链：逐字找"谁有这个字形"
HarfBuzz / fribidi  → 一行怎么排：连字、字距、连接形、视觉顺序
```

#### 1. `@font-face`：构建期只存声明，运行期真读文件

CSS 里写：

```css
@font-face { font-family: "My Sans"; src: url(/usr/share/fonts/.../DejaVuSans.ttf);
             font-weight: 400 }
@font-face { font-family: "My Sans"; src: url(...-Bold.ttf); font-weight: 700 }
@font-face { font-family: "My Sans"; src: url(...-Oblique.ttf); font-style: italic }
```

构建期把它变成 doc 里的一张 8 字节的小表（家族名 / 资源名都是文本池下标），
**不读字体文件、也不内嵌字体字节**。运行期 `ql_ui_build()` 时按表注册进
字体注册表（`ql_font_family_register()`），注册表按 (文件, 字号) 缓存已加载的字面。

只认四样：`font-family` / `src: url()` / `font-weight` / `font-style`。
`unicode-range`（子集化）、`src: local(...)`（要 fontconfig）、`font-display`
**一律构建期报错** —— 静默忽略会让"这段中文为什么用了别的字体"这类问题无迹可寻。

#### 2. `font-family` 列表与逐字回退

样式里的 `font_family` 现在是"家族列表"的下标（blob v33）。运行期：

1. 按顺序取列表里的名字：`monospace` / `serif` / `sans-serif` / `system-ui` 落到
   本库的正文/等宽字体；其余名字查注册表；
2. 第一个"装得上"的家族当**主字体**，后面的接成 `QLFont.fallback_font` 链；
3. 缺字时 `ql_font_find_glyph()` 沿链往下问 —— **逐字回退**，
   所以"DejaVu + Noto CJK"这种混排不需要拆 `span`。

**回退链为什么要包装体**：注册表里的字体按 (字面, 字号) 共享，同一张字面可能被
两条不同的链用到（`A, monospace` 与 `A, B`）。直接改共享字体的 `fallback_font`
会让两条链互相污染，所以链上每一节都是一个**只借后端与 impl 的包装体**
（字形缓存是自己的，销毁时只放自己的部分；`ql_font_family_reset()` 负责真正的字体）。

链的缓存按 (文档, 列表, 字号, 字重, 斜体) 记 16 槽，DPR 放大时按**同一个列表**重取
（`ql_font_set_scale_hook`），否则放大后字族会悄悄变回默认。

#### 3. 整形：HarfBuzz（直连或 `dlopen`）+ 内建兜底

`ql_text_shape()` 是唯一的入口：**量宽度**与**画字形**都走它，所以"量出来 3 个字符、
画出来 1 个连字"这种错位在原理上不会发生。

- **有 HarfBuzz 时**：按"字体 + 方向"切段 → 每段 `hb_shape` → 取字形下标 →
  用 `QL_GLYPH_ID_BASE + gid` 这个合成码位复用原来那套有序字形缓存
  （真实码位 ≤ 0x10FFFF，撞不上）；
- **没有 HarfBuzz 时**：逐码位查字形 + 逐字回退，行为与 B8 之前**逐像素一致**
  （没有连字与字距）。

**第三方库怎么选**（`CMakeLists.txt` 里的 `QLGL_USE_HARFBUZZ` / `QLGL_USE_FRIBIDI`）：

| 机器上有什么 | 走哪条路 |
|---|---|
| 开发头文件 + 库 | **构建期直连**（自动 `find_path` / `find_library`，定义 `QLGL_HAVE_HARFBUZZ`） |
| 只有运行库（很多发行版的默认状态） | 运行期 `dlopen("libharfbuzz.so.0")` |
| 都没有 | 退回内建整形 / 内建 UAX#9 子集 |

三条路填的是**同一张函数指针表**，所以上层一行都不用改。`dlopen` 白名单只列
真正用到的 11 个符号；直连路径把 `hb_glyph_info_t` / `hb_glyph_position_t` /
`hb_feature_t` 的布局用 `_Static_assert` 钉在编译期 —— 这套检查真的抓到过一个
bug：`hb_shape` 是 **4 个参数**（`font, buffer, features, count`），多写一个在
x86-64 上"碰巧"没事，但那是未定义行为。

#### 4. 双向：先整形后重排，**不要先重排再整形**

这是最容易写错的地方：

```
错：bidi 重排文本 → 交给 HarfBuzz 整形 → 阿拉伯字母变成一串孤立形
对：按"字体 + 方向"切段 → 每段整形（HarfBuzz 按方向给出视觉顺序）→ 段落 RTL 时倒段
```

`direction: rtl` 的段落里，"阿拉伯文 + 数字"会切成两段：阿拉伯段按 RTL 整形
（HarfBuzz 输出就是视觉顺序），数字段按 **LTR** 整形（UAX#9 里数字是"二级"，
整体保持 LTR），最后把段顺序整体倒过来。

内建路径（没有 HarfBuzz）没有连写能力，只能先把整行按 UAX#9 子集排成视觉顺序
再逐字画 —— 顺序能读对，但没有连写。

`unicode-bidi`：`normal` / `embed` / `isolate` 按同一层处理，`bidi-override` 记成
"强制按段落方向排"。嵌套隔离与括号镜像**没做**（见 §9.2 表格）。

#### 5. 真字重 / 真斜体

`@font-face` 声明了 700 或 italic 字面时，`font_of_style()` 按"字重距离 + 斜体是否
匹配"挑最接近的字面；挑到的字面如果本身就是粗/斜体（`QLFont.is_bold/is_italic`），
绘制层就不再合成（`ts->bold && !font->is_bold` 才抹粗）。挑不到真字面时退回原来的
合成加粗/倾斜 —— 这条兜底必须留着，否则"只装了一份 regular"的机器上粗体会消失。

#### 6. 验证

| 测试 | 项数 | 覆盖 |
|---|---|---|
| `test_font_stack` | 38 | 注册表按字重/斜体挑字面、家族列表顺序、逐字回退、`@font-face` 三条报错 |
| `test_text_shape` | 27 | 字形数与字距、连字开/关、cap 分块、阿拉伯文连写、视觉顺序、RTL+数字、字形下标缓存 |

两条后端都跑：`QL_TEXT_SHAPE=builtin` / `QL_TEXT_BIDI=builtin` 两个环境变量强制走内建路径
（CI 里各跑一遍）。

#### 7. 代价与裁剪

| 项 | 值 |
|---|---|
| `QLUIStyle` | 364 → **368 B**（`direction` / `unicode_bidi` / `ligatures` / `kerning`） |
| `QLUIDoc` | + 三段小表（`font_faces` / `font_lists` / `font_names`），没有 `@font-face` 时**一个字都不占** |
| blob | **v33**，头 152 → **172 B** |
| 编译开关 | `QL_FEAT_CSS_FONT_STACK` / `QL_FEAT_CSS_TEXT_SHAPE` / `QL_FEAT_CSS_BIDI`（**MCU 默认全关**） |
| 关掉之后 | `@font-face`、家族名、`direction: rtl`、关连字 **构建期报错**；绘制退回逐码位路径 |

**MCU 实测**（mps2-an386，与 §18.12 / §18.13 同一把尺子）：

| 配置 | `.text` | `.bss` |
|---|---|---|
| B8 之前（B7 收尾） | 141,624 B | 140,268 B |
| B8 之后，三个开关**全关**（MCU 默认） | **144,808 B** | 140,268 B |
| 再开 `QL_FEAT_CSS_TEXT_SHAPE=1` | 144,808 B（**+0**） | 140,268 B |
| 再开 `QL_FEAT_CSS_BIDI=1` | 144,808 B（**+0**） | 140,268 B |
| `QL_FEAT_CSS_FONT_STACK=1` | **编译期报错**：它需要 FreeType，MCU 档根本没编 FT |

也就是说：**三个能力开关在 MCU 上不额外花一个字节** —— 它们的实现要么要 FreeType、
要么要 `dlopen`，MCU 上一条都不存在。真正涨的 3,184 B 是**共享**成本：
内建整形循环（替换掉原来的逐码位加法）、内建 UAX#9 子集、样式变大 4 B、
以及 blob v33 的读写路径。这份成本换来的是"PC/Mobile 上一行代码就能开整形与双向"。

**为什么 MCU 不能开**：HarfBuzz 1,178 KB、fribidi 106 KB、一份全量 CJK TTF 20 MB 级；
而整块 MCU 固件现在是 141 KB。这不是"再优化一下"，是数量级。

### 18.15 B9 落地：`clip-path` 四种形状 / 纯颜色 `filter` 单批次 / 字体变体（blob v34）

这一批解决的是"真实页面天天写、写了以前会报错"的几件：`clip-path` 不只圆、
`filter` 不必每次走离屏图层、`small-caps` / `tabular-nums`。

#### 18.15.1 `clip-path`：四种形状，三种代价

| 形状 | 实现 | 额外内存 | 额外绘制遍数 |
|---|---|---|---|
| `inset(t r b l)` | **与裁剪矩形求交**，然后照原样画 | 0 | 0 |
| `circle(p%)` | 解析椭圆 SDF（`rx == ry`），GPU 走顶点属性 `a_mask=(cx,cy,rx,ry)`，CPU 走 `mask_cov_at` + `mask_row_span` 快路径 | 0 | 0 |
| `ellipse(rx ry)` | 与圆同一条路，两个轴分开 | 0 | 0 |
| `polygon(...)` | 顶点进 doc 池 → 绘制时换算成绝对坐标 → **解析覆盖率**（奇偶内点 + 到最近边距离 + ±0.5px AA）；CPU 逐像素 `poly_cov`，GPU 用 uniform `u_pn`/`u_p[N]` | 0 | 0 |

覆盖率是**解析**算的，不是"画进一张小蒙版再拉伸"：早先版本用 64×64 A8 蒙版图，
按边框盒拉到大元素上会糊成 3~4px 的斜坡（而且每个元素多一遍离屏合成）。
现在边的过渡带恒为 ~1px，CPU 与 GPU 同一条公式（`test_gl_vs_cpu` 的 polygon
用例最大单通道差 1）。

顶点池写在 doc 里（`clip_pts` / `clip_pt_count`，blob 新增一段），同值去重：
一排同款徽标共用一个顶点表。

明确报错的写法（不做静默降级）：`circle(… at x y)`（圆心恒居中）、`inset(… round R)`
（圆角矩形裁剪要另一套扫线）、`polygon(… evenodd)`（形状只有一层，两种环绕结果相同）、
`path()` / `url()`（要 SVG 路径解析器）。

#### 18.15.2 纯颜色 `filter` 折成 3×4 矩阵

八个纯颜色函数（brightness / contrast / saturate / grayscale / sepia / invert /
hue-rotate / opacity）在构建期**链式相乘**成一个 3×4 仿射矩阵：

```
brightness(k) : diag(k,k,k)
contrast(k)   : diag(k,k,k) + 127.5*(1-k)
saturate(s)   : Filter Effects 的 3×3 系数矩阵
grayscale(k)  : (1-k)*I + k*lum   （lum = .213/.715/.072）
sepia(k)      : (1-k)*I + k*sepia9
invert(k)     : (1-2k)*I + 255k
hue-rotate(θ) : Filter Effects 的固定 3×3
opacity(k)    : 不动颜色，乘进 d2d->alpha
```

矩阵用 16.16：线性部分无量纲、第 4 列是"色值 × 65536"，所以组合与施加都不丢精度。

- **CPU**：`QLCanvas` 加 `cm[12]` + `cm_on`，`put_px` / 行填充 / `blit_image` 三个入口
  各过一遍；开启时绕开 SIMD 行填充（它不经过 `put_px`）。
- **GPU**：`u_cm[3]`（每行一个 vec4）+ `u_cm_on`。片元被重构成
  `vec4 shade()` + 一个统一出口 `main()`：**所有分支只 return 颜色，矩阵在唯一出口施加** ——
  这样不会重演"圆遮罩漏掉某个 return"的老坑。单批次渲染下矩阵只能当 uniform 传，
  所以 `gl_set_cm()` 在矩阵变化时先 flush（一个带 filter 的元素多一次 draw call，
  换来不走图层）。

`style_needs_layer()` 只在**含 blur / drop-shadow** 时才返回 1。

> **语义差别**：CSS 的 filter 作用于"元素合成后的结果"。逐图元施加时，子树内部
> **互相重叠**处略有不同。常见用法（整体调色）看不出差别；要严格等价就加
> `blur()`（哪怕 `blur(0.001px)`）把它推回图层。

#### 18.15.3 字体变体

`font-variant-caps: small-caps` / `font-variant-numeric: tabular-nums` →
`variant_flags` 一个字节 → `QLShapeOpt.small_caps/tabular_nums` → HarfBuzz 的
`smcp` / `tnum` 两个 `hb_feature_t`（`feats[6]`，与 `liga`/`clig`/`kern` 共用同一个数组）。
没有整形能力时**构建期报错**。

#### 18.15.4 顺手补的 L1 小件

| 属性 | 实现要点 |
|---|---|
| `text-align: justify` | 剩余空间 / (字形数-1) 摊进字间；**最后一行不拉伸**（`li+1 == count` 时关掉），`ellipsis` 行不拉伸；跨整形块时按整行字形数近似（误差 ≤1px 级） |
| `box-shadow` 的 `spread` | 复用 `QLUIShadow.reserved`（零结构体膨胀）；矩形与圆角一起按 spread 调整 |
| `border-radius: a / b` | `radius_v[4]`（8 字节）；绘制侧取两者平均作为等价单半径 |
| `font` 简写 | `[style] [variant] [weight] size[/line-height] family`；数字字重要求**整段都是数字**（否则 `20px` 会被当成字重，踩过） |
| `word-wrap` | `overflow-wrap` 的老名字，同一套解析 |
| `:empty` | 有子元素 → 不匹配；文本全空白 → 匹配（`items` 里的混合内容也扫） |
| `revert` | 与 `unset` 同路径 |
| `::first-line` / `::first-letter` | **明确报错**并给替代写法（依赖折行结果，构建期无法确定） |

#### 18.15.5 结构体与 blob

`QLUIStyle` 368 → **400**（`clip_box[4]` 32 + `clip_pts`/`clip_pt_n`/`clip_reserved` 4 +
`variant_flags`/`radius_reserved`/`radius_v[4]` 12 + 对齐），blob **v34**、
头 172 → **180**（`clip_pt_count` + `off_clip_pts`），doc 新增一段 `clip_pts`。

#### 18.15.6 这一轮抓到的三个老 bug

1. **GL 片元的零半径遮罩**：半径 0 时 `min(rx,ry)` 把距离乘成 0 → `smoothstep` 得 0.5 →
   整块半透明。`clip()` 的 golden 用例 CPU/GPU 差 **47%**。现在 `rr <= 0` 明确判全裁。
2. **column flex 里折行文本的高度**：折行基准取了**容器**宽度而不是**子项自己**的
   `width`，于是"窄列 + 长段落"按 1 行算高度（20px），绘制却按 440px 折成 2 行 →
   第二行压到下一个元素上。
3. **行内子节点的测量高度**：column 布局把 `inline` 子节点高度**逐个相加**
   （`<span>a</span><span>b</span>` 得 84px 而不是 23px），后面的兄弟被顶下去一大截。

前两个只在"column flex + 文本"这种组合下出现，第三个只要"inline 子节点有 padding"就能看到 ——
例程 `36_clip_filter` 同时踩中了三个。

#### 18.16 解析层收尾：真实页面写法（blob v36）

这一批不是新特性，而是**解析器太窄**——真实 CSS 里的常见写法被一律打回。全部修完，
`test_css_syntax`（100 项）逐条钉住，每条都验到具体数值（样式字段 / 布局尺寸 / 颜色），
不只验"能解析"。

| 写法 | 以前 | 现在 |
|---|---|---|
| `.5` / `.5em` / `.5px`（前导小数） | 报"不是数字"（词法只认 `\d+`） | 与 `0.5` 完全等价；`1.` 仍非法 |
| `hsl(120 50% 50%)` / `rgb(255 0 0 / .5)` / `rgb(100% 0% 0%)` | 只认逗号 | 逗号/空白两种都认，`/` 分隔 alpha，分量可写百分比 |
| `hwb()` / `lab()` / `lch()` / `oklab()` / `oklch()` / `color(display-p3 …)` | 报"不支持的颜色写法" | 按标准矩阵换算到 sRGB 再夹取（与浏览器降采样观感一致） |
| `box-shadow: inset 0 0 2px red` / `0 1px 2px black`（命名色） | 报"无法识别" | 颜色按 token 整段命中色表；`inset` 不再被当成颜色 |
| `linear-gradient(red, blue)`（省略方向） | 报"需要 3 个参数" | 默认 `to bottom`（CSS 默认） |
| `linear-gradient(180deg, red 0%, blue 100%)` | 报"不支持的颜色写法 'red 0%'" | 两端位置与不写等价，接受；**中间位置/多色标**明确报错（会改变画面，不做静默近似） |
| `gap: 1rem` / `1em` | 只吃 px（`padding: 1rem` 却行） | 新增 `QLUIStyle.gap_units`，em/rem 在解析末尾折成 px；`%` / `vw` 仍明确报错 |
| `line-height: 1.5em` / `150%` | 报"不支持" | 按自己的字号折成 px |
| `border-width: 1em` | 报"必须 0~64px" | em 折成 px（rem 给明确提示） |
| `transition-delay` / `transition: … .2s` | 报"暂不支持 delay" | 支持；过渡期样式停在起点值 |
| `steps(n)` / `cubic-bezier(a,b,c,d)`（过渡与动画） | 报"暂不支持" | 都支持：过渡在客户端套缓动，动画在 `anim_apply` 里按"帧 > 动画"优先级选 |
| `padding-inline` / `margin-block` / `inset-inline-start` … | 报"不支持的属性" | 解析期改写成物理四边（`direction: rtl` 时 inline 左右互换） |

**`margin` / `padding` 的相对单位（同一批收尾）**

`%` / `vw` / `vh` / `vmin` / `vmax` 现在都支持：

* 每边的单位从 2 位扩到 **4 位**（`margin_unit` / `padding_unit`），编码
  `0=px 1=em 2=rem 3=auto 4=% 5=vw 6=vh 7=vmin 8=vmax`；
* `QLUIStyle` 里存的是**原始数值**（`padding: 5%` → 5），布局期由
  `margin_of()` / `padding_of()` 按**包含块宽度**（`%`）或视口（`vw` 族）折成 px；
* 布局代码一律通过 `margin_h/margin_v/padding_h/padding_v/margin_of/padding_of`
  读，**不再直接读 `style->margin.left`** —— 直接读会拿到原始数值（5 当成 5px）；
* 固有尺寸（`max-content` / `min-content` / 表格定宽）里百分比按 **0** 算，
  与 CSS "auto 宽度下百分比基准不确定" 的处理一致。

**还剩一个限制（诚实记录）**：逻辑属性只在**元素自己写了** `direction: rtl` 时交换
左右；从祖先继承来的 RTL 看不到（解析期拿不到继承值），这类界面请直接写物理属性。

**结构体与 blob**：`QLUIStyle` 400 → **416**（`margin_unit`/`padding_unit` 各 8→16 位）（`gap_units` + `trans_delay_ms` +
`trans_steps` + `trans_bez_on` + `trans_bez[4]`），`QLUIKeyframe` 36 → **44**（帧级
`bez[4]`/`bez_on`），`QLUIAnimation` 12 → **22**（`steps`/`bez[4]`/`bez_on`），
blob **v36**；`qltext cc` 的 C 生成器与全部 golden 基准同步重建。

---

---

---

#### 18.17 伪元素的多条规则：一个伪元素一个节点（2026-11-11）

**问题**：伪元素是构建期展开的**实体节点**（内部 tag `#pe`），最早"一条源规则 → 一个节点"。
于是浏览器里最常见的写法直接崩：

```css
.x::before { content: ""; position: absolute; width: 20px; height: 20px; }
.x::before { background: rgba(255,255,255,.5); }   /* 只想补一点样式 */
```

第一条建出一个节点，第二条又建一个（两条都写 `content` 时页面上会长出**两个**装饰）；
反过来，`.x::after { content: ""; … }` + `.x::after { transform: … }` 会被判成
"::after 规则缺少 content"。

**做法**（只动 `src/ui/qlui_parse.c` 的展开段，运行期零改动）：

1. 先按"**选择器完全相同**"分组（`selector_same`：tag / id / class / attr / 伪类状态 /
   `::` 名逐项相等）。合并选择器 `.a::before, .b::before {}` 在解析期已经拆成三条规则，
   所以它们各自入各自的组。
2. 每组只生成**一个** `#pe` 节点，组里**每一条**源规则各生成**一条内部规则**
   （`.__pe_N { … }`，`allow_pseudo = 1`、`spec_cls = 源 spec_cls + 1`、`order`/`media`
   随源规则、跳过 `content`）—— 级联语义完全不动，"后写的赢 / 高优先级赢"照旧。
3. 组里任一条写了 `content` 就不报错；**整组都没有**才报"缺少 content"。
   `content` 的取值取组内**最后**一条写了它的规则。

**踩过的坑**：组员必须在 `members[]` 里**连续**存放（展开函数只拿到 `members + first`），
而规则表里的顺序是按书写顺序交叉的（合并选择器先来、单条后补）。第一版按"发现顺序"
追加，`first` 一算就错位，某组会拿到**别人**的规则 —— 表现是 `.b::before` 的第二条规则
完全不生效、`.a::before` 丢了自己的 `left`。改成两趟扫描（先数条数算偏移，再按组紧凑填充）。

**验证**：新增例程 `example/39_pseudo_multi`（四格：单规则基线 / 拆两条 / 两条抢同一属性 /
`::before` 与 `::after` 各拆两条），`--dump` 逐节点读 `bg_color` + `border_box`；
`ctest` 88/88。

**顺手记下的既有缺陷（未修，与伪元素无关）**：绝对定位子节点放在 `display: flex` 的容器里，
`width: 24px` 会被算成 20px（普通 `div` 可复现）。例程 39 因此刻意不用 flex 做卡片。

---

#### 18.18 `outline` 的宽度关键字与 `0`（2026-11-11）

**问题**：`outline` 简写的 token 判别只有三条路 —— 带 `px` 的当宽度、几个样式关键字当
`outline-style`、**剩下的全当颜色**。于是两种最常见的写法都过不去：

```css
button { outline: 0; }              /* 去掉 focus 环 —— 被当颜色，报"不支持的颜色" */
button:focus { outline: thin solid #2563EB; }  /* thin 同样被当颜色 */
```

**做法**：新增 `outline_width_kw()`（`thin`=1px、`medium`=3px、`thick`=5px，浏览器实测值），
简写与 `outline-width` 长写共用：

* 简写里命中关键字就折成 `"<n>px"` 存回 `outline-width`，后续走同一条路；
* 简写里出现无单位 `0` 直接当 `0px`（CSS 的 unitless zero）；
* 长写在 `parse_length()` 之前先试关键字，认到了就跳过长度解析。

`outline: 0` 的最终结果是 `outline_w = 0` → 与 `outline: none` 一样完全不画（不占布局，无副作用）。

**验证**：新增例程 `example/40_outline`（六格：`0` / thin / medium / thick / `offset:-6px` /
长写 `thick`），`--dump` 自检输出 `1/3/3/5/5` 与 offset、颜色都对；
`ctest` 88/88。

---

#### 18.19 `border-radius` 的百分比（2026-11-11）

**问题**：边框圆角在构建期就折成 px 存进 `QLUIStyle.radius[4]`，所以写
`border-radius: 50%` 直接报"此处不支持百分比"。而 `50%` 恰恰是最常见的写法 ——
圆形头像、胶囊按钮、玻璃按钮全靠它。

**关键点**：百分比相对的是**盒子自身**的宽（横向半径）与高（纵向半径），
构建期没有盒子尺寸，算不出来。

**做法（零结构体改动、零 blob 版本变更）**：

1. 解析：新增 `parse_radius_token()`，px / em / rem / calc() 照旧折 px；
   百分比存**负数**当标记（`50%` → `-50`）。`radius[]` 本来就只可能是 ≥ 0 的 px，
   负数是一个空出来的取值域，所以不用加字段、不必升 blob 版本。
2. 布局：`resolve_radius_pct()` 在 `border_box` 算好之后调用，
   把 `-pct` 折成 `w*pct/100`（横向）/ `h*pct/100`（纵向），写进 `style_copy`
   （`ensure_style_copy()` 会把 `node->style` 指过去）。
3. 于是**绘制、命中、圆角裁剪、动画**读到的仍然全是 px —— 那 9 个
   `ql_ui_radius_max()/radius_max4()` 调用点一个都没改。
4. 防御：`ql_ui_radius_max4()` / `ql_ui_radius_max()` 里负值按 0 处理，
   免得 `display: none` 的节点（从未布局）把标记漏进绘制。

**语义**：`50%` 正方形 = 内切圆；长方形上横向 50% 被短边夹住 → 胶囊。
`border-radius: 50% / 25%` 走同一条路，横向 50%、纵向 25%（仍是单半径近似，见 §18.15）。

**验证**：新增例程 `example/41_radius_pct`（六格：正圆 / 胶囊 / 对照 px / 椭圆角 /
小百分比 / 只圆上面两角），`--dump` 自检输出 32、70、24、60+20、21、90/90/0/0；
`ctest` 88/88。

---

#### 18.20 `transform: translate()` 的百分比（2026-11-11）

**问题**：`translate()` 只认 `px` 与 `0`，`transform: translate(-50%,-50%)` 这种
"把自己居中"的写法直接报错。

**关键点**：百分比相对元素**自身**的边框盒（x → 宽、y → 高），而平移是**绘制期**的
偏移、不进布局 —— 尺寸要等布局结束才有。

**做法（同样不动结构体、不升 blob 版本）**：

1. `QLUIStyle.clip_reserved` 这个对齐填充字节改名成 **`tf_pct`**（bit0 = x 是百分比、
   bit1 = y），占用不变，所以 blob 布局与版本都不用动。
2. 解析：新增 `tf_len()`（px / 0 / `%`），`translate` / `translateX` / `translateY`
   都走它；百分比把**百分数**存进 `tf_x/tf_y` 并置标记位。
3. 布局：`resolve_tf_pct()` 在 `resolve_transform()`（自上而下累加绝对位移那一步）
   开头调用，按 `border_box.w/h` 折成 px 写回样式副本并清掉标记位。
   于是绘制偏移、命中逆变换、`paint_box` 膨胀读到的全是 px。
4. 明确报错的两处（不静默算错）：
   * `translate(50%) rotate(10deg)` —— 矩阵在构建期就把 translate 折进去了；
   * `@keyframes` 里的百分比 translate —— 帧表存的是整数 px。

**验证**：新增例程 `example/42_translate_pct`（六格，每格里用 `::before` 画一块灰色
"原位置"参照物），`--dump` 自检输出 `(-45,-17) / (90,0) / (0,34) / (45,0) /
(-22,0) / (9,-25)` —— 最后一格证明 y 用的是**高度**（50 × -50% = -25）。

**连带修掉的真 bug（GL 后端的几何裁剪）**：`clip_quad()` 拿**没加绘制偏移**的矩形
去和**屏幕坐标**的 `g->clip` 求交，而偏移是在裁完之后才加到顶点上的。于是带
`transform: translate()` 的元素在**脏区重绘**时会被误裁掉：脏区正好落在位移后的
位置、而未偏移的盒子与它不相交 → 求交为空 → 整块消失；全帧重绘时裁剪框是整个
窗口，所以看不出来（现象就是"鼠标一放上去内容就没了"）。
修法：`clip_quad()` 里先把裁剪框按当前偏移搬到元素自己的坐标系
（`clip - offset`）再求交。同一段还有两个兄弟坑，一并记在这里：
`gl_set_mask()` 关闭时没清 `poly_n`、`ql_canvas_set_mask()` 关闭时没清 `mask_pn`
（都用"关闭统一走 set_mask(-1)"这个入口，必须两个都清）。

---

#### 18.21 伪元素：不同选择器补样式的已知差异（未修）

`.x::before{content:""}` + `.x.y::before{top:12px}` 这种"不同选择器补样式"的写法，
本库要求**两条都写 `content`**（分组按"选择器完全相同"）。

浏览器按"元素匹配到哪些规则"合并，只写一次即可。要做的话得把伪元素展开从
"先按选择器分组、再遍历节点"改成"逐节点收集命中的伪元素规则"，属于展开算法的重构，
本次没动。

---

#### 18.22 `backdrop-filter: refract()` —— 玻璃边缘的折射（2026-11-11）

**问题**：只有 `blur()` 的玻璃是一块**均匀的毛面**，边缘没有任何"厚度"，怎么调都不像
Apple 那套液态玻璃。参考稿（CodePen）用的是内联 SVG 滤镜：
`feGaussianBlur` 模糊 + `feDisplacementMap` 按法线图位移。

**做法**：把它拆成两个 CSS 函数，语义一一对应。

1. `blur(<N>px)` —— 毛面（对应 `feGaussianBlur`）。
2. `refract(<N>px)` —— **轮廓附近把采样点沿法线方向往外推**，最多 N px；圆心不推。
   等价于 `feDisplacementMap`，但不需要内联滤镜图，参数就是一个像素半径。
3. `saturate(<N>%)` —— 模糊会把颜色洗淡，这里推回去。

**实现要点（CPU 与 GL 同一套顺序与参数）**：

- **采样范围外扩**：从 `rect` 扩出 `blur 半径 + refract`。模糊核本来就会读盒子外面，
  只糊盒内再靠钳位补边，得到的是"这块方形的均值色"，半径一大就发灰、四边留方边。
- **只写轮廓之内**：结果按圆角覆盖率与模糊前的原像素混合，轮廓外一个像素都不动
  （否则四个圆角缺口里渗出方的模糊块）。
- **三段盒式卷积**近似高斯：两段是三角核，边缘有一道可见折线；三段才接近钟形。
- 接口上是 `backdrop_blur_round(..., corner, sat, refract)`；`refract == 0` 时行为与
  上一版逐像素一致（旧例程的截图不受影响）。

**为什么 `blur()` 要小**：半径一大，背后就被糊成均值色 → 灰塑料。例程取 6px，
厚度全部由 `refract(14px)` 提供；悬停时折射加倍到 26px。

**验证**：`example/38_liquid_glass` 整版重写成参考稿那套（水彩背景 + 玻璃圆盘 + 白色加号），
`QL_EX_WINDOW_DUMP` 抓真窗口帧确认折射可见、圆角缺口无方形渗出；
`ctest` 88/88（`blur.qlg` / `showcase.qlg` 基准随三段卷积一起重建）。

**顺带的版本变更**：`QLUIStyle` 多一个 `backdrop_refract` 字节 →
`QL_UI_BLOB_VERSION` **37 → 38**，所有 golden 重建。
