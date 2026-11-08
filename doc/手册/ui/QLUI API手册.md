# QLUI / HTML+CSS 编译 API 手册（v0.1）

配套阅读：[QLUI 阅读指南](./QLUI阅读指南.md) · 定位：[Web 兼容设计](../../Web兼容设计.md) §7

> **先明确适用场景**
> - **MCU**：不要用本模块，直接用 `QLWidget`/`QLLabel` 的 C API（见 [QLWidget API手册](../widget/QLWidget%20API手册.md)）。
> - **MPU / 嵌入式 Linux**：用本模块，把 HTML+CSS 编译成 C。
> - **PC 要高兼容**：用系统 WebView（WebView2 / WKWebView / WebKitGTK），不用本模块。

---

## 1. 工作流

```
index.html ┐
index.css  ├─ qltext cc ─▶ ui_index.c + ui_index.h ─▶ 编译进固件
tools/qltext/qltext.c ┘
```

### 1.1 命令行

```bash
qltext cc \
    --html test/web/ui_demo.html \
    --css  test/web/ui_demo.css \
    --out  build/ui
# 输出：build/ui/ui_demo.c  build/ui/ui_demo.h
```

| 参数 | 说明 |
|---|---|
| `--html` | 输入 HTML（必填） |
| `--css` | 输入 CSS（必填） |
| `--out` | 输出目录（必填，不存在会创建） |
| `--name` | 输出标识（默认取 HTML 文件名） |

退出码：`0` 成功；`1` 有错误（错误信息带 `文件:行号`）。

### 1.2 CMake 集成

```cmake
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(UI_OUT ${CMAKE_CURRENT_BINARY_DIR}/ui)
add_custom_command(
    OUTPUT  ${UI_OUT}/ui_index.c ${UI_OUT}/ui_index.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${UI_OUT}
    COMMAND qltext cc
            --html ui/index.html --css ui/index.css --out ${UI_OUT}
    DEPENDS ui/index.html ui/index.css qltext
    COMMENT "编译 HTML+CSS 为 C")

add_executable(app main.c ${UI_OUT}/ui_index.c)
target_link_libraries(app qlgl)
target_include_directories(app PRIVATE ${UI_OUT})
```

改 HTML/CSS 后 `cmake --build` 会自动重新生成并重编译。

---

## 2. 支持的 HTML

| 标签 | UA 默认样式 |
|---|---|
| `div` / `section` | `display:block` |
| `p` | `display:block`，`margin: 8px 0` |
| `span` / `b` / `i` | `display:inline` |
| `button` | `display:inline-block`，带内边距/边框/背景 |
| `h1` / `h2` | `display:block`，`font-size:16px` |
| `h3` | `display:block`，`font-size:12px` |
| `input` | `display:inline-block`，带内边距/边框/圆角；`type` 只支持 `text` / `password` |
| `select` / `option` | 与 `input` 同形 / **`display:none`**（只活在面板里） |

目前认 **80 个**（HTML5 共 114 个）。全量清单、每个标签的状态与能力包见
[HTML5 标签覆盖与裁剪](HTML5标签覆盖与裁剪.html)；UA 默认样式细节见
[阅读指南 §5.4.7](QLUI阅读指南.html)。

**限制**：
- 文档必须**有且只有一个根元素**；
- 元素不能同时包含文本和子元素（混合内容）→ 报错，请把文本包进 `<span>`；
- `img` / `table` / `textarea` / 任意未实现的 `type` → 报错，错误信息给出替代做法；

---

## 3. 支持的 CSS

### 3.1 选择器

| 选择器 | 支持 |
|---|---|
| `tag` / `.class` / `#id` | ✅ |
| `A B`（后代） | ✅ |
| `A > B`（子代） | ✅ |
| `A + B` / `A ~ B`（兄弟） | ❌ 报错 |
| `[attr]` / `::before` | ❌ 报错 |
| `:hover` / `:active` / `:focus` | ✅ 只允许单个，编译成状态变体 |

优先级按规范三元组 `(id, class, tag)` 计算，同级按出现顺序。

### 3.2 属性

| 属性 | 值 | 说明 |
|---|---|---|
| `display` | `block` `inline` `inline-block` `none` `flex` `inline-flex` | `flex` 见 §3.8 |
| `width` / `height` | `auto` `Npx` `N%` | `%` 相对父**内容宽度**；`height` 的 `%` 不支持 |
| `margin` / `padding` | `Npx`（1/2/4 值） | 也支持 `margin-top` 等长写 |
| `border-width` | `Npx` | |
| `border-color` | 颜色 | |
| `border` | `1px solid #333` | 只支持 `solid` |
| `border-radius` | `Npx` | |
| `background` / `background-color` | 颜色，或 `linear-gradient(...)`（见 §3.5） | |
| `box-shadow` | `<dx> <dy> [blur] <color>` | 软阴影，SDF 实现，单趟。`none` 关闭 |
| `color` | 颜色 | |
| `font-size` | `Npx` | 任意整数；生成期汇总成 `font_sizes` 表，运行期按字号取 `ql_font_default(N)`（配置了矢量字体就是该字号的动态字体，支持中文；否则回落到内置 12/16 位图字体） |
| `text-align` | `left` `center` `right` | |
| `box-sizing` | `content-box` `border-box` | 默认 `content-box`（与浏览器一致） |
| `flex` / `flex-grow` / `flex-shrink` / `flex-basis` | `1` / `1 0 auto` / `0 0 100px` | 见 §3.8 |
| `flex-direction` | `row` `row-reverse` `column` `column-reverse` | |
| `flex-wrap` | `nowrap` `wrap` | |
| `justify-content` | `flex-start` `center` `flex-end` `space-between` `space-around` `space-evenly` | |
| `align-items` | `stretch` `flex-start` `center` `flex-end` | |
| `gap` / `row-gap` / `column-gap` | `Npx`（1~2 值） | 不支持百分比 |
| `position` | `static` `relative` `absolute` `fixed` | 见 §3.8 |
| `top` / `right` / `bottom` / `left` | `Npx` `N%` `auto` | 仅非 static 时生效 |
| `z-index` | 整数（int16） | 同级兄弟间排序 |
| `overflow` | `visible` `hidden` `auto`（`scroll` 视同 `auto`） | `hidden`/`auto` 都裁掉溢出 |

### 3.3 颜色写法

`#rgb`、`#rrggbb`、`rgb(r,g,b)`、`rgba(r,g,b,a)`、命名色（`black`/`white`/`red`/`green`/`lime`/`blue`/`gray`/`silver`/`yellow`/`orange`/`purple`/`navy`/`teal`/`transparent`）。

### 3.4 混合内容

支持 `<p>`/`<div>` 里文本与行内元素混排：

```html
<p class="m">Hello <b>bold</b> and <i>it</i>!</p>
```

**在生成期消解**：每个文本段被包成一个匿名 `<span>` 子节点，
运行期只认识"元素树"，因此布局与绘制代码完全不需要额外分支。

- 文本段的空白按 HTML 规则折叠（连续空白变单个空格）；
- 元素之间的**纯空白**不产生盒子（否则每个换行缩进都会变成一个 4px 宽的 span）；
- 匿名 span 继承父元素的 `color` / `font-size` / `text-align`；
- 相邻文本段不会自动合并，写 CSS 时按"每个文本段是一个 inline 盒子"来理解。

### 3.5 渐变

```css
background: linear-gradient(to right, #3399ff, #1a4f8a);
background: linear-gradient(to bottom, #4ad08a, #2a9a5c);
```

| 项 | 支持 |
|---|---|
| 方向 | `to bottom` / `to top` / `180deg`、`to right` / `to left` / `90deg` |
| 颜色停靠点 | 只支持 **2 个**（起始 + 终止） |
| 多停靠点 / 角度其他值 / `radial-gradient` | ❌ 构建期报错 |

实现：垂直渐变走**行填充快路径**（整行同色，成本≈纯色填充）；水平渐变逐像素插值，慢一个量级。
GPU 侧用**顶点颜色插值**实现，不增加顶点、不增加 draw call。

> ⚠️ **状态样式与渐变**：`:hover` / `:active` 等变体样式是"整套重算"的，
> 如果变体规则只写了 `background-color`（没有 `background`），渐变会**退回纯色**。
> 想让渐变按钮在 hover/active 下保持渐变，要显式写：
>
> ```css
> .btn:hover { background: linear-gradient(to bottom, #58e09a, #35a869); }
> ```

> **径向渐变**：`background: radial-gradient(circle, <起色>, <止色>)`。
> 圆心固定为元素中心，按到**最远角**的距离归一化（等价 CSS `farthest-corner`）。
> 只支持 `circle` / `circle at center` / `ellipse`；写 `at 偏移`、自定义半径、
> color stop 位置一律报错。UI 里的光晕、暗角就是它。

### 3.6 阴影

```css
box-shadow: 0 4px 12px rgba(0, 0, 0, 0.55);
box-shadow: 0 1px 0 #000;          /* 硬阴影（无模糊） */
box-shadow: none;
```

| 项 | 支持 |
|---|---|
| `dx dy` | 必须（`px`） |
| `blur` | 可选，`0` = 硬边 |
| 颜色 | 必须（不支持浏览器默认色） |
| `spread`（第 4 个长度）、`inset`、多重阴影 | ❌ 构建期报错 |

实现是 **SDF + smoothstep 衰减**，与 GPU 着色器同一公式，因此两条后端视觉一致。
只有"距轮廓 blur 以内"的环需要逐像素算距离，内部实心区走行填充——
所以在 PC 上几乎无感，在 MCU 上也可用（见开发规划 M5.5 的能力矩阵）。

### 3.7 Flexbox

```css
.row { display: flex; flex-direction: row; gap: 8px; align-items: center; }
.card { flex: 1 1 0px; }             /* 等分 */
.btn  { flex: 1 1 auto; }            /* 按内容比例分 */
```

**支持的语义**

| 项 | 说明 |
|---|---|
| 主轴方向 | row / row-reverse / column / column-reverse |
| 换行 | nowrap / wrap（按主轴尺寸断行） |
| 剩余空间 | 正：按 `flex-grow` 比例分配；负：按 `flex-shrink × basis` 加权收缩 |
| 主轴对齐 | justify-content 六种（`grow` 生效时 justify 不再参与分配） |
| 交叉轴对齐 | `stretch`（默认，子项无显式交叉尺寸时拉满）或 start/center/end |
| gap | `gap` / `row-gap` / `column-gap`，px |
| 子项上限 | 单个 flex 容器 ≤ `QL_CFG_UI_FLEX_MAX_ITEMS`（默认 256），超了降级为 block 流并告警 |

**实现要点（影响你写 CSS 时的预期）**

1. **`flex-direction: column` + `height: auto` 时没有剩余空间可分**，
   `flex-grow` 不生效 —— 与浏览器一致（grow 需要确定的容器尺寸）；
2. 布局是"**先测量、再分配、最后定位**"：容器先让每个子项按自然尺寸排一遍，
   再根据 grow/shrink 改写尺寸重排。所以**子项的自然尺寸必须能算出来**
   （`width: auto` 的块级子项会按容器宽度算，这是 CSS 的行为）；
3. `flex-basis` 支持 px 与 `%`（相对容器主轴尺寸）；
4. 百分比 `width` 在 flex 子项上是相对**容器内容宽**，不是 flex-basis。

**不支持的**：`align-self`、`align-content`、`order`、`flex-flow` 简写、
`flex-basis: auto` 的完整语义（等同"用 width"）、多行 `align-content` 对齐。

### 3.8 定位与 z-index

```css
.card  { position: relative; }                    /* 成为绝对子项的包含块 */
.badge { position: absolute; top: -6px; right: 8px; }
.toast { position: fixed; bottom: 5px; left: 5px; }
```

| `position` | 行为 |
|---|---|
| `static` | 常规流 |
| `relative` | 常规流占位不变，**绘制位置**按 `top/left`（或 `right/bottom` 取负）偏移，不影响兄弟 |
| `absolute` | 脱离文档流；相对**最近的非 static 祖先的 padding box** 定位；没有则相对根 |
| `fixed` | 脱离文档流；相对**视口**定位 |

**绝对定位的尺寸规则**

| 给了什么 | 宽度 |
|---|---|
| 显式 `width` | 用它 |
| `left` + `right`（无 width） | 拉伸填满 |
| 只有 `right`（无 width） | 从右边界往左放 |
| 都没有 | **shrink-to-fit**：只支持"内容是本节点自己的文本"（badge/标签场景）；有块级子项时撑满包含块 |

高度同理（`top` + `bottom` 拉伸，`bottom` 反推 y）。

> ⚠ 已知简化：`position: absolute` 在 CSS 里还有 "static position"（没给 inset 时
> 停在原来该在的位置）与 shrink-to-fit 的完整算法。这里都没实现，
> 没给 inset 时贴包含块左上角。涉及这些的布局请显式写 inset。

**z-index**：布局结束后对所有兄弟按 `z-index` **稳定排序**子链表，
之后**绘制顺序与命中测试顺序自动一致**（链表顺序即绘制顺序）。
z-index 相同则保持文档顺序（稳定）。

**overflow**

| 值 | 行为 |
|---|---|
| `visible`（默认） | 子项可以画到父盒子外 |
| `hidden` / `auto` / `scroll` | 子树被裁到本节点边框盒；**被裁掉的部分不可命中**（命中测试同样裁剪） |

> `auto`/`scroll` 目前**不产生滚动条，也不能滚动** —— 只是裁剪。
> 滚动容器排在 M3 之后的滚动里程碑。

### 3.9 过渡（`transition`）

```css
.btn       { transition: 180ms ease-out; }                 /* 简写：全部可过渡属性 */
.btn       { background-color: #204060; }
.btn:hover { background-color: #ff8000; }                  /* 180ms 内渐变过去 */
.card      { transition: background-color 300ms ease-in-out, opacity 120ms; }
```

| 部分 | 允许值 | 默认 |
|---|---|---|
| `transition-property` | `background-color` / `opacity` / `color` / `border-color` / `border-radius` / `transform` / `clip-path` / `all` | `all` |
| `transition-duration` | `Nms` 或 `Ns` | `0s`（等于不写） |
| `transition-timing-function` | `linear` `ease` `ease-in` `ease-out` `ease-in-out` `steps(n)` `cubic-bezier(a,b,c,d)` | `ease` |
| `transition-delay` | ❌ 报错 | — |

行为与浏览器一致的三条：

1. **打断从当前插值重基**，不回跳、不从头开始。
2. 过渡只改**绘制期属性**，不进布局 → 过渡期间**零重排**。
3. 只有写进 CSS 的规则才产生动画；`transition` 可以写在 base 规则上（对所有状态生效）。

**支持**：`background-color`（渐变时连同终止色）、`opacity`、`color`、`border-color`、
`border-radius`、`transform`、`clip-path`。`transition: all` 七个一起开。

`transform` 的四个分量（`tf_x` / `tf_y` / `sc_x` / `sc_y`）**永远一起插值**——
只动 translate 不动 scale 会出现"边缩边移"的错位。所以放大 + 位移写一条就够了：

```css
.card        { transition: transform 200ms ease-out; }
.card:hover  { transform: translate(4px, 2px) scale(1.5); }
.mask        { clip-path: circle(20%); transition: clip-path 300ms ease-out; }
.mask:hover  { clip-path: circle(100%); }
```

**不支持**：`transition-delay`、`box-shadow`（构建期报错，不静默降级）。

> **`opacity` 的真实含义**：本实现没有 CSS 的"组不透明度"（那需要离屏合成），
> `transition: opacity` 插值的是**背景 alpha**（来自 `background-color: rgba(...)` 的 a 通道）。
> 所以 `opacity: 0` 在这里等价于"背景变全透明"，子元素不受影响——与 Web 不同，
> 写过渡时请用 `background-color: rgba(...)` 表达淡入淡出。

运行期推进（主循环里）：

```c
uint32_t dirty_count = ql_ui_anim_tick(ui, ql_anim_now_ms());
if (dirty_count > 0) {
    QLRect areas[8];
    int n = ql_ui_consume_dirty(ui, areas, 8);
    for (int i = 0; i < n; i++)
        ql_ui_render_rect(ui, &dw, areas[i]);   /* 只重绘变化区域 */
}
```

> `ql_ui_node_style()` 返回的是**过渡中的样式**（可能是中间色）。
> 需要"最终值"时请等动画结束，或读 CSS 源。

---

### 3.9.1 圆形揭示（`clip-path: circle()`）

```css
.reveal { animation: iris 400ms ease-out 1 forwards; }
@keyframes iris {
  from { clip-path: circle(0%); }
  to   { clip-path: circle(100%); }
}
```

| 写法 | 支持 |
|---|---|
| `circle(<百分比>)` | ✅ 0%~100% |
| `none` | ✅ |
| `circle(<N>px)` / `inset()` / `polygon()` / `path()` / `url()` | ❌ 构建期报错 |

语义：

- 圆心固定在**元素中心**（与 `radial-gradient` 同一个决定）。
- 百分比是**最远角距离**的千分比：`0%` 什么都不显示，`100%` 完全覆盖。
  用百分比而不是 px，是因为揭示动画要"全程都在动"——100px 的半径在 200px 的盒子上
  会在动画前半段就把元素盖满。
- 它裁的是**整棵子树**（等价 CSS 的语义）：伸出父盒子的子元素也会被切掉。
  这跟 `border-radius` 动画的区别就在这里——后者只切父元素自己的背景/边框。
- **只支持一层**。嵌套 `clip-path` 的 CSS 语义是取交集，而两个圆盘的交集不是圆盘，
  所以运行期保留外层并打一条警告。

### 3.10 变换（`transform`）

```css
.slide-in { transform: translate(-40px, 0); }   /* 静态平移 */
.icon     { transform: translateX(6px); }
```

| 写法 | 支持 |
|---|---|
| `translate(Xpx, Ypx)` / `translate(Xpx)` | ✅ |
| `translateX(Xpx)` / `translateY(Ypx)` | ✅ |
| `scale(<倍数>)` | ✅ 绕元素中心 |
| `scale(sx)` / `scale(sx, sy)` | ✅ 轴对齐的非等比（见下） |
| `scaleX(s)` / `scaleY(s)` | ✅ **假 3D 翻转**就靠这个 |
| `none` | ✅ |
| `rotate()` / `skew()` / `matrix()` / 3d | ❌ **构建期报错** |

可以组合：`transform: translate(4px, 6px) scale(2)`。

**`scale()` 的三条行为**：

1. **不进布局**：`flex` / `border_box` 看不到它，邻居不会被挤开（与 CSS 一致）。
2. **文字一起放大**：内部按"CSS 字号 × scale × DPR"重新取字号，所以是清晰的放大，
   不是把小图拉大。
3. **命中测试跟着走**：视觉上占多大就能点多大（压扁时被压掉的部分点不中）。

**非等比缩放（`scaleX`/`scaleY`）的近似**：圆角、描边、阴影、字号都按
`min(sx, sy)` 缩放，**字形不做横向挤压**。所以 `scaleX(0.7)` 的文字高度也会变成 0.7。
`0` 是合法值（`scaleX(0)` = 零宽），此时一个字都不画、也点不中。做翻转就用它：

```css
@keyframes flip-x {
  0%   { transform: scaleX(1); }
  50%  { transform: scaleX(0); }
  100% { transform: scaleX(1); }
}
.coin { animation: flip-x 900ms ease-in-out infinite; }
```

`backface-visibility` **报错**：假翻转没有背面，没有"背面朝后就不画"这回事。
要翻面换成另一张脸，用两个元素错开 `animation-delay`。

三条语义（与 CSS 一致）：

1. **不影响布局**：`transform` 是绘制期属性，`flex` 计算里看不到它，所以它是
   "移动元素但不触发重排"的唯一手段。
2. **作用于整个子树**：父元素平移，子元素跟着走。
3. **命中测试跟着变**：视觉位置就是可点位置，动画过程中也点得中。

`transform` 也可以在 `@keyframes` 里插值（见 §3.11），这是做"滑入"最省事的办法：

```css
@keyframes slidein {
  from { transform: translate(-70px, 0); opacity: 0; }
  to   { transform: translate(0, 0);     opacity: 1; }
}
.card { animation: slidein 400ms ease-out 1 forwards; }
```

> **代价（诚实说明）**：因为不进布局，被平移的元素需要把它"可能出现的所有位置"算进
> 脏区上界——动画的位移区间越大，脏区就越大（最多大出整个位移量）。
> 这是"零重排"换来的，正常入场动效（几十像素）完全无感。

---

### 3.11 关键帧动画（`@keyframes` + `animation`）

```css
.pulse { animation: pulse 1s linear infinite alternate; }
.card  { animation-name: fade; animation-duration: 400ms;
         animation-fill-mode: forwards; }

@keyframes pulse {
  from { background-color: #204060; }
  to   { background-color: #ff8000; }
}
@keyframes fade {
  from { opacity: 0; }
  to   { opacity: 1; }
}
```

`@keyframes` 在**构建期**编译成静态帧表（`QLUIKeyframe`）+ 段表（`QLUIAnimation`），
运行期零解析。所以 `animation-name` 必须是同一份 CSS 里定义过的名字。

| 部分 | 允许值 | 默认 |
|---|---|---|
| `animation-name` | 本文件里定义过的 `@keyframes` 名 / `none` | `none` |
| `animation-duration` | `Nms` / `Ns`，**必须 > 0** | 必填 |
| `animation-timing-function` | `linear` `ease` `ease-in` `ease-out` `ease-in-out` | `ease` |
| `animation-iteration-count` | 整数 1~255 / `infinite` | `1` |
| `animation-direction` | `normal` `reverse` `alternate` `alternate-reverse` | `normal` |
| `animation-fill-mode` | `none` `forwards` `backwards` `both` | `none` |
| `animation-delay` | `Nms` / `Ns`（0~65535） | `0ms` |
| `animation-play-state: paused` | ❌ 报错 | — |

**关键帧里能写什么**（只允许能插值的绘制期属性）：

| 属性 | 含义 |
|---|---|
| `background-color` / `background` | 背景色（含 `rgba()` 的 alpha） |
| `background-image: gradient(...)` | 渐变的首末色，两个颜色一起插值 |
| `opacity` | **背景 alpha**（与 `transition` 同义，见 §3.9 的说明） |
| `transform: translate(...)` | 平移（见 §3.10），和颜色一起插值 |
| `color` | 文本颜色 |
| `border-color` | 边框颜色 |
| `border-radius` | 圆角半径（只切元素自身，做圆角/圆形揭示） |
| `clip-path: circle(<pct>%)` | 圆形遮罩，**裁整棵子树**（见 §3.9.1） |
| `animation-timing-function` | 这一帧到下一帧的缓动 |

其它属性（`box-shadow` / `font-size` / `filter` …）在构建期**报错**。

三条必须知道的行为：

1. **各帧的属性集合必须一致**。`0% {background-color} 50% {opacity}` 会被拒绝——
   否则运行期要额外记录"隐式值"，容易做出半正确的效果。
2. **只写一半的关键帧是合法的**：`@keyframes x { 50% { opacity: 0 } }` 的首末帧
   自动取**元素自己的基础样式**（与 CSS 的隐式关键帧一致），所以同一个 `@keyframes`
   可以被不同底色的元素复用。
3. **`animation-delay` 期间**：`fill-mode` 带 `backwards`/`both` 时显示 0% 帧，
   否则显示元素的基础样式（即"还没开始动"）。`animation` 简写里的第二个时长就是
   delay（`animation: pulse 1s 300ms`），写第三个时长报错。

**动画与 `transition` 同时存在时，动画赢**：动画已经占用了颜色通道，
状态过渡在这个节点上直接跳过（不会互相打架）。

**`animation` 只能写在基础规则上**：动画在 `ql_ui_build` 时按基础样式起播，
写在 `:hover` 之类里永远不会触发，所以构建期直接报错（不静默失效）。

运行期接口：

```c
/* 从头重播（点击反馈、出错抖动）；节点没有动画时返回 QL_ERR_INVAL */
ql_err_t ql_ui_anim_restart(QLUIRoot *ui, QLUINodeRT *node);

/* 正在跑的动画数（过渡 + 未播完的关键帧动画） */
uint32_t ql_ui_anim_running(const QLUIRoot *ui);
```

### `animationend` / `transitionend`

```c
typedef enum {
    QL_UI_ANIM_END_KEYFRAMES = 0,   /* @keyframes 播完 */
    QL_UI_ANIM_END_TRANSITION,      /* transition 走到终点 */
} QLUIAnimEndKind;

typedef void (*ql_ui_anim_end_cb)(QLUIRoot *ui, QLUINodeRT *node,
                                  QLUIAnimEndKind kind, void *user_data);

void ql_ui_set_anim_end_cb(QLUIRoot *ui, ql_ui_anim_end_cb cb, void *user_data);
```

典型用法——淡出后删节点：

```c
static void on_anim_end(QLUIRoot *ui, QLUINodeRT *node, QLUIAnimEndKind kind, void *user)
{
    if (kind != QL_UI_ANIM_END_TRANSITION) return;
    if (node == (QLUINodeRT *)user) {
        /* 这里已经安全：事件是 tick 末尾派发的，槽位早就释放了 */
        ql_ui_set_text(ui, node, "");
    }
}
```

| 会触发 | 不会触发 |
|---|---|
| `@keyframes` 播完（非 infinite） | `animation-iteration-count: infinite` 的每一轮之间 |
| `transition` 走到终点 | 被 `ql_ui_anim_restart()` 打断（那是新的开始） |
| | 状态变化后目标样式没有过渡而直接撤销 |
| | 过渡槽满 / 时长为 0 导致"瞬间到位"（没真的播过） |

事件队列有界（`QL_CFG_UI_ANIM_EVENT_MAX`，默认 16）。一帧内超过这个数会丢事件并
告警一次——**不做无界排队**（那会在慢机器上滚雪球）。

推进与重绘仍然只用 `ql_ui_anim_tick` / `ql_ui_consume_dirty`（见 §3.9）。
`animation-iteration-count: infinite` 的动画永远让 `ql_ui_anim_tick` 返回非 0，
所以**别用"tick 返回 0"来判断能不能退出**，那不是它的语义。

---

### 3.11.1 毛玻璃（`backdrop-filter: blur()`）

```css
.glass {
  background-color: rgba(255, 255, 255, 0.10);   /* 想看见效果就得带 alpha */
  border-radius: 14px;
  backdrop-filter: blur(12px);
}
```

| 允许值 | 说明 |
|---|---|
| `blur(Npx)`，N = 1~64 | 高斯标准差（CSS 语义）；N 越大越糊 |
| `none` | 关闭 |

它模糊的是**这层元素背后已经画好的内容**，所以：

1. **必须写在"上面那一层"**：靠 z 序/文档顺序决定谁是"背后"；
2. **自己的 `background-color` 要带 alpha**，否则糊完又被不透明底色盖住，看不出效果；
3. **只影响自己的矩形**（含 `border-radius` 的圆角），不会把外面的颜色吸进来；
4. 它跟着 `transform` 一起走——毛玻璃卡片滑动时，糊的是它当前盖住的那块内容；
5. 每帧多一次 draw call；CPU 后端代价约等于"半次整屏重绘"（见 `bench.md`）。

**构建期报错**：`blur()` 以外的函数（`blur` 以外的 `backdrop-filter` 值）、半径 0 或 > 64。

---

### 3.12 不支持的写法一律报错

```
float / grid / align-self / align-content / order / flex-flow
rotate / skew / matrix / 3d transform（只支持 translate 与轴对齐 scale）
filter / clip-path / var()
min-width / max-width / min-height / max-height
@media / em / rem / vh / vw / calc()
transition-delay
animation-play-state: paused
backface-visibility（假翻转没有背面）
backdrop-filter 的 blur() 以外的函数
```

报错示例：

```
qlui: 错误：index.css:27: 不支持的属性 'position'
qlui: 错误：index.css:12: 不支持的长度 '1.5em'（只支持 px 与 %）
qlui: 错误：index.html:5: <p> 同时包含文本和子元素（混合内容）——v0 不支持，请把文本放进独立的 <span>/<p>
```

---

## 4. 生成的数据结构

```c
typedef enum {
    QL_UI_TAG_DIV, QL_UI_TAG_SECTION, QL_UI_TAG_P, QL_UI_TAG_SPAN,
    QL_UI_TAG_B, QL_UI_TAG_I, QL_UI_TAG_BUTTON,
    QL_UI_TAG_H1, QL_UI_TAG_H2, QL_UI_TAG_H3, QL_UI_TAG_COUNT
} QLUITag;

typedef enum { QL_UI_DISPLAY_BLOCK, QL_UI_DISPLAY_INLINE,
               QL_UI_DISPLAY_INLINE_BLOCK, QL_UI_DISPLAY_NONE } QLUIDisplay;

typedef enum { QL_UI_UNIT_AUTO, QL_UI_UNIT_PX, QL_UI_UNIT_PERCENT } QLUIUnit;

typedef struct { int16_t top, right, bottom, left; } QLUIEdges;
typedef struct { uint16_t value; uint8_t unit; uint8_t reserved; } QLUILength;

typedef struct {
    uint32_t bg_color, text_color, border_color;
    QLUILength width, height;
    QLUIEdges margin, padding;
    uint8_t display, bg_alpha, border_width, border_radius;
    uint8_t text_align, font_id, box_sizing, reserved;
    /* 省略：渐变/阴影/flex/定位/过渡/动画/transform 的字段，完整定义见 qlui.h
       （transform 用 int16_t tf_x/tf_y + int16_t sc_x/sc_y，千分比）
       line-height 是 int16_t line_h（0 = normal），
       white-space 是 uint8_t white_space（0 = nowrap 默认，1 = normal 折行），
       font_flags 是 uint8_t，见 QL_UI_FONT_*（合成加粗/斜体/下划线/删除线） */
} QLUIStyle;  /* sizeof == 120（blob v12） */

typedef struct {
    uint16_t tag, parent, first_child, next_sibling, style, text;
    uint8_t  flags, reserved;      /* QL_UI_NODE_PASSWORD 等 */
} QLUINode;  /* sizeof == 20（blob v12） */

typedef struct {
    const char *name;
    uint16_t node_count, style_count, text_count, font_size_count;
    const QLUINode   *nodes;
    const QLUIStyle  *styles;
    const char       *text_pool;      /* 以 '\0' 分隔的 UTF-8 池 */
    const uint16_t   *text_offsets;
    const uint16_t   *font_sizes;     /* font_id 索引它，值是像素字号 */
    void             *arena;          /* NULL = 静态数据，不归 doc 管 */
} QLUIDoc;
```

`0xFFFF` 表示"无"（无父/无子/无兄弟/无文本）。

`arena` 是**唯一的所有权判据**：

- `arena == NULL` → 静态 doc（`.rodata` 或调用方自己管内存），`ql_ui_doc_free` 是空操作；
- `arena != NULL` → `ql_ui_doc_load*` 分配，`ql_ui_doc_free` 释放。

因为这条判据，静态 doc 误进 free 路径不可能发生，`ql_ui_doc_free` 可以无脑调用。

> 所有内部引用都是 `uint16` **下标**而不是指针 —— 这既让 blob 的加载变成
> "设 6 个基址指针"（零重定位），也让 doc 之间可以各自独立（不能跨 doc 引用）。

---

## 5. 运行期 API

```c
QLUIRoot *ql_ui_build(const QLUIDoc *doc);
void      ql_ui_free(QLUIRoot *ui);

void      ql_ui_layout(QLUIRoot *ui, ql_coord_t width, ql_coord_t height);
void      ql_ui_render(QLUIRoot *ui, QLDraw2D *d2d);          /* CPU 或 GPU */
void      ql_ui_render_canvas(QLUIRoot *ui, QLCanvas *canvas); /* 便捷：包一层 CPU */
void      ql_ui_render_rect(QLUIRoot *ui, QLDraw2D *d2d, QLRect dirty);
QLRect    ql_ui_paint_bounds(const QLUIRoot *ui);
```

| 函数 | 说明 |
|---|---|
| `ql_ui_build` | 把静态表物化成节点树；一次分配，失败返回 `NULL` |
| `ql_ui_free` | 释放（不负责显示驱动） |
| `ql_ui_layout` | 按视口尺寸做完整布局 + 算 `paint_box` |
| `ql_ui_render` | 按树顺序绘制：阴影 → 背景 → 边框 → 文本 → 子节点 |
| `ql_ui_render_rect` | 只重绘 `dirty`：裁剪设成 `dirty∩视口`，靠 `paint_box` 剪掉不相交的子树。**dirty 内的结果与全量重绘逐像素相同** |
| `ql_ui_paint_bounds` | 整棵树的绘制范围（= 根节点的 `paint_box`，可能超出视口，例如带阴影的贴边元素） |

### 查询

```c
QLUINodeRT *ql_ui_root_node(QLUIRoot *ui);
QLUINodeRT *ql_ui_find_by_tag(QLUIRoot *ui, QLUITag tag, uint16_t nth);
QLUINodeRT *ql_ui_hit_test(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
const char *ql_ui_node_text(const QLUINodeRT *node);
const QLFont *ql_ui_node_font(const QLUINodeRT *node);
QLRect      ql_ui_node_rect(const QLUINodeRT *node);
const char *ql_ui_tag_name(QLUITag tag);
```

`ql_ui_hit_test` 从树末尾向前找第一个包含该点的节点（后面的节点视觉上更靠上）。

### 运行期改内容

```c
ql_err_t ql_ui_set_text(QLUIRoot *ui, QLUINodeRT *node, const char *text);
```

只允许改"没有子节点的元素"的文本。改完需要重新 `ql_ui_layout` + `ql_ui_render`。

> **不支持**运行期改样式类、增删节点。页面结构在构建期固定。

### 交互

```c
typedef void (*ql_ui_click_cb)(QLUIRoot *ui, QLUINodeRT *node, void *user_data);

void ql_ui_set_click_cb(QLUIRoot *ui, ql_ui_click_cb cb, void *user_data);
int  ql_ui_pointer_down(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
int  ql_ui_pointer_up(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
int  ql_ui_pointer_move(QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
```

| 函数 | 返回 | 说明 |
|---|---|---|
| `pointer_down` | 1 = 命中 | 给命中节点置 `QL_UI_RT_ACTIVE` |
| `pointer_up` | 1 = 触发点击 | 清 `ACTIVE`；若按下时命中同一节点则调 `click_cb` |
| `pointer_move` | 1 = 悬停状态有变化 | 更新 `QL_UI_RT_HOVER` |

节点状态位：`QL_UI_RT_HOVER` / `QL_UI_RT_ACTIVE` / `QL_UI_RT_DIRTY`。

> 注意：状态位**会影响绘制**（`:hover`/`:active`/`:focus` 有独立样式），但**不影响布局**（M1 限制），
> 需要高亮请在 `click` 回调或渲染后自行处理。

### 输入框

```c
typedef void (*ql_ui_input_cb)(QLUIRoot *ui, QLUINodeRT *node,
                               const char *value, void *user);

void        ql_ui_set_input_cb(QLUIRoot *ui, ql_ui_input_cb cb, void *user);
const char *ql_ui_input_value(const QLUIRoot *ui, const QLUINodeRT *node);
ql_err_t    ql_ui_input_set_value(QLUIRoot *ui, QLUINodeRT *node, const char *value);
```

| 函数 | 说明 |
|---|---|
| `set_input_cb` | 值变化 / 按回车时回调；`cb == NULL` 取消 |
| `input_value` | 当前值；不是 `<input>` 返回 `NULL`。指针在下次编辑前有效 |
| `input_set_value` | 改值（夹到 `QL_CFG_UI_INPUT_LEN−1`），光标移到末尾 |

编辑能力：点击定位、拖动选字、`Backspace`/`Delete`、`←`/`→`/`Home`/`End`、
`Shift+方向` 扩展选区、`QL_INPUT_TEXT` 插入、`Esc` 收焦点。
**没有剪贴板、没有 IME、光标不闪。**

### 进度条 / 度量条

```c
int      ql_ui_progress_permille(const QLUIRoot *ui, const QLUINodeRT *node);
ql_err_t ql_ui_progress_set(QLUIRoot *ui, QLUINodeRT *node, int value, int max);
```

| 函数 | 说明 |
|---|---|
| `progress_permille` | 0..1000；`-1` = 不是 `progress`/`meter`，`-2` = 没有 `value`（不确定） |
| `progress_set` | 按 `value`/`max` 设置（夹到 `[0,max]`；`max<=0` 视为 100） |

轨道 = `background-color`，填充 = `color`。不确定进度画固定 1/3（不做动画）。

### 图片（`<img>`）

```c
typedef struct {
    const char    *name;         /* 对应 <img src> */
    const uint8_t *data;
    size_t         len;
    uint16_t       w, h;         /* RAW 必填；其他格式填 0 */
    uint8_t        format;       /* QL_IMAGE_SRC_RAW/BMP/QOI/PNG/AUTO */
    uint8_t        pixel_format; /* RAW 时必填（QL_IMAGE_RGB565 等） */
} QLUIImageInput;

ql_err_t       ql_ui_register_image(QLUIRoot *ui, const QLUIImageInput *in);
const QLImage *ql_ui_node_image(const QLUIRoot *ui, const QLUINodeRT *node);
```

| 情况 | 返回 |
|---|---|
| 名字对不上任何 `<img src>` / 界面里没有 `<img>` / 参数非法 | `QL_ERR_INVAL` |
| 格式这个构建没编进来（PNG/BMP/QOI 或平台后端） | `QL_ERR_UNSUPPORTED` |
| 解码成功 | `QL_OK`，并且**内部已重排**（固有尺寸可能变了） |

`ql_image_validate()` / `ql_image_decode()`（`<qlgl/qlimage.h>`）是通用入口：
任何外来字节进绘制层之前都要过 `validate`（"先除后乘"挡 `w*h*bpp` 溢出）。

| 输入格式 | 现在支持 | 输出 |
|---|---|---|
| `QL_IMAGE_SRC_RAW` | ✅（不复制，`owned = 0`，零 RAM） | 直通 |
| `QL_IMAGE_SRC_BMP` | ✅ 24/32 位、8 位调色板、`BITFIELDS`、上下翻转（不解 RLE） | 按 `out_format` 转换 |
| `QL_IMAGE_SRC_QOI` | ✅ 六种 op | 同上 |
| `QL_IMAGE_SRC_PNG` | ⏳ 待做（现在返回 `QL_ERR_UNSUPPORTED`） | — |

`out_format` 默认 **RGBA8888**（它的枚举值就是 0，所以 `memset` 清零的请求即默认）。
`src_format = QL_IMAGE_SRC_AUTO` 时按魔数探测（PNG/BMP/QOI；RAW 无法探测，必须显式给）。

**构建期**用同一份解码器：`qltext cc --root DIR --pixel-format rgb565` 会把
`<img src>` 指向的文件解成目标格式嵌入 doc，运行时**零解码**。

**CPU 与 GPU 都有 `blit_image`**，两者逐像素一致（`test_gl_vs_cpu` 里有一页专门比对，
差异 0.00%）。GPU 侧是"图片单独成批 + 纹理缓存"，滤波默认 `GL_NEAREST` 以对齐 CPU 的
最近邻；想要双线性就把 `QL_CFG_IMAGE_BILINEAR` 设成 1（两条后端一起变）。

### 图像热区（`<map>` / `<area>`）

```c
int              ql_ui_area_at(const QLUIRoot *ui, ql_coord_t x, ql_coord_t y);
int              ql_ui_area_count(const QLUIRoot *ui);
const QLUIArea  *ql_ui_area_get(const QLUIRoot *ui, int index);
const QLUINodeRT *ql_ui_area_image(const QLUIRoot *ui, int index);
QLRect           ql_ui_area_rect(const QLUIRoot *ui, int index);
const char      *ql_ui_area_href(const QLUIRoot *ui, int index);
const char      *ql_ui_area_alt(const QLUIRoot *ui, int index);
void             ql_ui_set_area_cb(QLUIRoot *ui, ql_ui_area_cb cb, void *user);
```

| 函数 | 说明 |
|---|---|
| `area_at` | 命中的热区下标；没命中 / 没有热区返回 `-1` |
| `area_count` | 这份界面里的热区条数（doc 里没有 `<area>` 时为 0） |
| `area_get` | 只读描述（`img` / `shape` / `ncoord` / `href` / `alt` 的池下标）；越界返回 `NULL` |
| `area_image` | 该热区所属的 `<img>` 节点 |
| `area_rect` | 当前布局下热区在屏幕上的包围盒（已与内容盒求交） |
| `area_href` / `area_alt` | 字符串；没写返回 `NULL` |
| `set_area_cb` | 点击回调 `(ui, index, user)`；注册后**接管**图片上的点击 |

```html
<div id="root">
  <img src="map.png" usemap="#board">
  <map name="board">
    <area shape="rect"   coords="0 0 63 31"   href="#cpu"  alt="CPU">
    <area shape="circle" coords="96 64 20"     href="#ram"  alt="RAM">
    <area shape="poly"   coords="10 90 50 90 30 120" href="#io">
    <area shape="default"                      href="#bg">
  </map>
</div>
```

要点：

- **坐标是图片像素**（原点 = 图片左上角），与 HTML 一致；图片被缩放到别的尺寸时
  热区跟着缩放（一次整数换算），所以 `img { width: 200px }` 不会让热区错位。
- `shape` 缺省是 `rect`；`rect` 只要 4 个坐标、`circle` 3 个、`poly` 偶数个且 ≥ 6、
  `default` 不许带坐标。**数量不对就是编译错误**，不会"猜一半"。
- `<map>` / `<area>` / `<source>` 是 `display:none`，**不占空间**；`<picture>` 是透明容器。
- 命中时派发 `area_cb`，**不再**派发 `click_cb`（与浏览器一致：点击的 target 是 `<area>`）。
- 不做导航：`href` 只是字符串，跳转/换页由回调里的应用逻辑决定。
- 上限 `QL_CFG_UI_AREA_MAX`（默认 16 条，每条 ≤ 128 个坐标）；设成 0 则整套裁掉
  （doc 里再多 `<area>` 也解析成 0 条，`area_at` 恒返回 `-1`）。

### 浮层（`<dialog>`）

```c
int      ql_ui_dialog_open(const QLUIRoot *ui, const QLUINodeRT *node);
ql_err_t ql_ui_dialog_set_open(QLUIRoot *ui, QLUINodeRT *node, int open);
int      ql_ui_dialog_close_top(QLUIRoot *ui);
```

打开时：居中、视口压暗、点遮罩或 `Esc` 关闭。同一时刻只认**最后打开**的那个。
`ql_ui_paint_bounds` 在浮层打开时返回整个视口（遮罩要盖满，脏区重绘才不会漏）。

### 折叠面板

```c
int      ql_ui_details_open(const QLUIRoot *ui, const QLUINodeRT *node);
ql_err_t ql_ui_details_set_open(QLUIRoot *ui, QLUINodeRT *node, int open);
```

`<details>` 收起时把非 `<summary>` 的子节点指向共享隐藏样式（`display:none`）。
`set_open` 会重排（高度变了）；状态没变时是空操作。点 `<summary>` 也会切换。

### 下拉框

```c
void     ql_ui_set_select_cb(QLUIRoot *ui, ql_ui_input_cb cb, void *user);
int      ql_ui_select_index(const QLUIRoot *ui, const QLUINodeRT *node);
ql_err_t ql_ui_select_set_index(QLUIRoot *ui, QLUINodeRT *node, int index);
```

| 函数 | 说明 |
|---|---|
| `set_select_cb` | 用户改了选中项或程序设值时回调（`value` = 选项文字） |
| `select_index` | 当前第几项；不是 `<select>` 返回 `-1` |
| `select_set_index` | 设置选中项（**越界夹紧**，不报错） |

面板是**覆盖层**：不占布局、不受父级 `overflow` 裁剪、同一时刻只开一个。
键盘：`↑`/`↓` 改选中，`空格`/`回车` 展开收起。

### 滚动

```c
ql_err_t ql_ui_scroll_to(QLUIRoot *ui, QLUINodeRT *node, ql_coord_t x, ql_coord_t y);
ql_err_t ql_ui_scroll_by(QLUIRoot *ui, QLUINodeRT *node, ql_coord_t dx, ql_coord_t dy);
int      ql_ui_scrollable(const QLUINodeRT *node);
```

`scroll_to` 只设状态（**需要自己再调一次 `ql_ui_layout`**），`scroll_by` 内部重排并标脏。

### 焦点

```c
void        ql_ui_set_focus_node(QLUIRoot *ui, QLUINodeRT *node);
QLUINodeRT *ql_ui_focus_node(const QLUIRoot *ui);
```

`Tab` / `Shift+Tab` 在 `input` / `select` / `button` 之间按树序循环。
焦点只点亮 `QL_UI_RT_FOCUS`（`:focus` 样式靠它），**不影响布局**。

---

## 6. 完整示例

```c
#include <qlgl/qlgl.h>
#include "ui_index.h"          /* extern const QLUIDoc ui_index_doc; */

static void on_click(QLUIRoot *ui, QLUINodeRT *node, void *user)
{
    printf("clicked: %s\n", ql_ui_node_text(node));
}

int main(void)
{
    qlgl_init();
    QLDispDriver *d = ql_disp_x11_create("QLGL", 320, 240, QL_COLOR_FORMAT_AUTO);

    QLUIRoot *ui = ql_ui_build(&ui_index_doc);
    ql_ui_layout(ui, 320, 240);
    ql_ui_set_click_cb(ui, on_click, NULL);

    QLCanvas canvas;
    ql_canvas_from_disp(&canvas, d);

    int running = 1;
    while (running) {
        ql_ui_pointer_move(ui, x, y);      /* 你的输入层把坐标传进来 */
        ql_ui_render(ui, &canvas);
        disp_flush_all();                  /* 或接渲染器的脏矩形 */
        running = ui_poll_events();
    }

    ql_ui_free(ui);
    ql_disp_x11_destroy(d);
    qlgl_shutdown();
}
```

---

## 6.5 `.qlui` blob API（换界面不用重编译）

头文件 `<qlgl/qlui.h>`。想省字节量的目标机可以不编译 `src/ui/qlui_blob.c`，
下面的函数就不存在（声明不会产生引用，链接器不会拖入）。

### 6.5.1 最常用：一行加载

```c
QLUIRoot *ui = ql_ui_load_file("ui/index.qlui");
if (ui == NULL) {
    fprintf(stderr, "load failed: %s\n", ql_ui_doc_last_error());
    return 1;
}
ql_ui_layout(ui, 320, 240);
/* ... 渲染 / 交互 ... */
ql_ui_free(ui);          /* doc 由 root 持有，会一起释放 */
```

`ql_ui_load_file` = `ql_ui_doc_load()` + `ql_ui_build()`，并让 root **持有** doc。

### 6.5.2 想自己管 doc

```c
QLUIDoc *doc = ql_ui_doc_load("ui/index.qlui");
QLUIRoot *ui = ql_ui_build(doc);
...
ql_ui_free(ui);          /* 不释放 doc（root 不持有它） */
ql_ui_doc_free(doc);
```

### 6.5.3 API 一览

```c
/* 构建期（宿主工具用） */
size_t   ql_ui_doc_encode(const QLUIDoc *doc, void *buf, size_t cap);
ql_err_t ql_ui_doc_save(const QLUIDoc *doc, const char *path);

/* 运行期 */
QLUIDoc *ql_ui_doc_load_mem(const void *data, size_t size);
QLUIDoc *ql_ui_doc_load(const char *path);
void     ql_ui_doc_free(QLUIDoc *doc);
QLUIRoot *ql_ui_load_file(const char *path);

/* 辅助 */
int         ql_ui_doc_is_owned(const QLUIDoc *doc);   /* arena != NULL */
size_t      ql_ui_doc_text_pool_size(const QLUIDoc *doc);
const char *ql_ui_doc_last_error(void);
```

| 函数 | 说明 |
|---|---|
| `ql_ui_doc_encode(doc, NULL, 0)` | 只算需要多少字节（返回 0 = doc 非法） |
| `ql_ui_doc_encode(doc, buf, cap)` | 写入；`cap` 不足时不写、只返回需求 |
| `ql_ui_doc_load_mem` | **拷贝**一份数据进来，调用方可以立刻释放自己的缓冲 |
| `ql_ui_doc_free` | 对**静态 doc（`arena == NULL`）是空操作**，可以无脑调用 |
| `ql_ui_load_file` | 失败返回 NULL 并打错误日志，原因也可用 `ql_ui_doc_last_error()` |

### 6.5.4 失败原因

加载失败**一律返回 NULL**，绝不"跳过坏数据继续画"。原因用
`ql_ui_doc_last_error()` 取（静态缓冲，非线程安全）：

| 信息 | 原因 |
|---|---|
| `bad magic (not a .qlui blob)` | 不是 blob 文件 |
| `version mismatch: blob built by a different qlgl` | 库版本变了，重新生成 |
| `QLUINode layout mismatch (blob built on another ABI)` | 结构体布局变了（换编译器/架构/改了结构体） |
| `size mismatch (file truncated or corrupted)` | 文件被截断 |
| `checksum mismatch (file corrupted)` | 内容被改过 |
| `section offsets inconsistent with counts` | 头部被手工篡改 |
| `too many nodes` / `text pool too large` | 超过 `QL_CFG_UI_MAX_*` 上限 |

### 6.5.5 容量上限

```c
#define QL_CFG_UI_MAX_NODES         4096
#define QL_CFG_UI_MAX_STYLES        4096
#define QL_CFG_UI_MAX_TEXT_BYTES    (64u * 1024u)
#define QL_CFG_UI_MAX_FONT_SIZES    256
```

只作用于 blob（外来数据）。静态编译的 doc 是编译器保证的常量，不受限制。
MCU 上按实际页面调小，例如 256 / 256 / 8K。

> ⚠ `text_offsets` 是 `uint16`，所以**单个 doc 的文本池天然上限 64KB**
> （中文约 2 万字）。超了会加载失败，不会静默截断。

### 6.5.6 与静态 C 数据混用

```c
/* 主壳：构建期固化，零加载成本 */
QLUIRoot *shell = ql_ui_build(&ui_shell_doc);

/* 侧栏/插件：运行时加载，可换皮 */
QLUIRoot *panel = ql_ui_load_file("themes/dark/sidebar.qlui");

ql_ui_layout(shell, 320, 480);
ql_ui_layout(panel, 120, 480);
/* 各自 render 到自己的区域 */
```

混用单位是**面板**。`parent/first_child/next_sibling` 是同一数组内的 `uint16` 下标，
**doc 之间不能互相引用合成一棵树**；跨面板的 CSS 继承也不存在（每个面板独立闭合）。
需要"某块热改"就重建那个 `QLUIRoot`（几十微秒），不要尝试做元素级混合。

---

## 7. 限制一览

| 项 | 现状 |
|---|---|
| 布局 | block / inline / inline-block；**无 flex / grid / position** |
| 高度 | 不支持 `%`，不支持 `min/max-height` |
| 文本 | 不支持混合内容、不自动换行、无字距/行距调整 |
| 伪类 | `:hover`/`:active`/`:focus` 编译成变体样式；布局仍按基础样式 |
| 运行期 | 只能改文本（含 `<input>` 的值），不能改结构/样式类（要改结构就换一份 doc） |
| 换 UI | 支持 `.qlui` blob，**不用重编译**；但不支持目标机上解析 HTML/CSS 文本（M3.5） |
| JS | 不支持（交互用 C 回调） |
| 表单 | 有 `<input>` / `<select>`，但**没有 `<form>`**、没有提交、没有校验、没有剪贴板/IME |
| 性能 | 一次 `layout` 遍历整棵树；节点数 ≤ 数百时足够 |

需要更高兼容度 → 用 PC + WebView（[设计文档 §7.3](../../Web兼容设计.md)）。
