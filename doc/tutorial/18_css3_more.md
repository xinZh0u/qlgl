# CSS3 补全收官：状态选择器、文本细节、背景图

五批计划（B1–B5）走到这一篇就收尾了。前面几篇分别讲过单位与尺寸
（[CSS 效果检阅](16_css_gallery.html)）、选择器与 `::before`
（[CSS3 补全](14_css3.html)）、绘制细节（[CSS 效果检阅](16_css_gallery.html) 与
`example/25_b4`）、布局与交互（`example/24_b5`）；这一篇补齐最后三块，
并说明**`filter` 当时为什么拆成两条路**（B9 已全部落地）。

| 批 | 这一篇补的 | 例程 |
|---|---|---|
| B2 | `:disabled` / `:enabled` / `:checked` / `::placeholder`（含复选框控件） | `26_css_states` |
| B3 | `word-spacing` / `tab-size` / `text-decoration-*`（line/style/color/thickness/underline-offset） | `27_css_text2` |
| B4 | `background-image: url()` + `background-size` / `-position` / `-repeat` | `28_bg_image` |

---

## 1. 状态选择器（B2）

三个伪类的共同点：**在构建期就编好另一份样式**，运行期只翻一个状态位。

```css
.btn:disabled { background-color: #2A3442; color: #6C7783; }
input:enabled { border-color: #2E6BE6; }
.cb:checked  { background-color: #2E6BE6; }
input::placeholder { color: #5E6B7A; font-style: italic; }
```

```html
<button disabled>禁用的按钮</button>
<input type="checkbox" checked>
<input type="text" placeholder="搜索关键字…">
```

实现要点：

- `disabled` / `checked` / `selected` 这些**属性**在建表期就变成节点标志位；
  `:disabled` 与 `:checked` 各编一份样式变体存在 `QLUINode` 里（与 `:hover` 同构）。
- 运行期状态优先级：`disabled > active > focus > checked > hover`。
  禁用元素不该再"悬停高亮"，所以它排最前。
- `:enabled` = "没有 disabled 属性"：建表期按节点属性决定要不要把这个位打开。
- **复选框**是顺带补上的最小控件：`<input type="checkbox">` 有自己的一套 UA 默认
  （16×16 方框、无内边距），点一下切换 `:checked`，勾选时画一个 √。
  它**不进输入框池**（没有文本可编辑），所以 MCU 上不占缓冲。
- `::placeholder` 不是生成内容的伪元素：构建期把 `input::placeholder` 的声明
  合并进宿主样式，单独存一份"占位样式"；输入框为空时用它画那句提示。

> 别忘了 `::placeholder` 的样式**继承宿主**（字号、字体都跟着输入框），
> 这正是我们要的行为：只改颜色/字形，不改排版。

---

## 2. 文本细节（B3）

```css
.p { word-spacing: 6px; }      /* 只加在空格之后，字母之间不变 */
pre { tab-size: 4; }           /* '\t' 折算成 4 个空格 */
a   { text-decoration: underline; text-decoration-color: #38BDF8;
      text-decoration-thickness: 2px; text-underline-offset: 3px; }
.d  { text-decoration: underline; text-decoration-style: dotted; }
.o  { text-decoration: overline; }     /* B3 新增 */
```

- `word-spacing` 加在**空格之后**（CSS 就是这么定义的），所以
  `word-spacing: -2px` 会把词挤在一起 —— 例程第 1 节能直接看出来。
- `tab-size` 在**构建期**把 `\t` 展开成 N 个空格（`white-space: pre` 系列才保留），
  运行期一个字符判断都没有。
- `text-decoration-*` 的形态（solid / double / dotted / dashed）、粗细、颜色、
  下划线偏移都是"用 `fill_rect` 拼"出来的：不需要新图元，CPU 与 GPU 逐像素一致。
  `text-decoration-line` 支持 `underline` / `line-through` / `overline` 及其组合。

---

## 3. 背景图（B4 收尾）

```css
.card { background-image: url(checker); background-size: cover; }
.icon { background-image: url(checker); background-size: 24px 24px;
        background-repeat: no-repeat; background-position: right bottom; }
```

- 资源与 `<img src>` **共用一张表**：构建期把名字记进文本池，
  运行期 `ql_ui_register_image()` 按名字填像素 —— 同一张图只解码一次。
- 几何计算复用 `object-fit` 那一套：`cover` / `contain` / `<w> <h>`；
  `background-position` 与 `object-position` 同式（百分比在"多出来的余量"里算）。
- 平铺时先把起点对齐到可见瓦片，再把裁剪设成边框盒 —— 否则会画到兄弟节点上。
- 背景图铺在**背景色之上、边框之下**，圆角由 `border-radius` 裁（走图片自己的 SDF）。

---

## 4. `filter` 后来分成两条路（B9 收尾）

这一节是历史记录：当时 B4 还留着 `filter` 一项，因为它与前面几项的成本不是一个量级。
后来的结论是**把它拆成两条路**（见 [20_filter](20_filter.md) 与 [25_clip_filter](25_clip_filter.md)）：

| 函数 | 需要什么 | 现在 |
|---|---|---|
| `brightness/contrast/saturate/grayscale/sepia/invert/hue-rotate/opacity` | 折成一个 **3×4 颜色矩阵**，CPU 逐像素、GPU 用 uniform | ✅ **不进离屏图层**（B9） |
| `blur()` / `drop-shadow()` | **离屏图层**：子树先合成到一张图，再整体处理 | ✅ 走图层（M9.3） |
| `backdrop-filter` | 读"已经画好的内容" | ✅ 独立小着色器（教程 15） |

关键洞见：绝大多数真实页面的 `filter` 只是**调色**，逐图元施加和"先合成再处理"
在观感上没有区别 —— 而代价差一整块 RGBA 缓冲 + 两遍绘制。所以只有含
`blur()` / `drop-shadow()` 时才走图层。
