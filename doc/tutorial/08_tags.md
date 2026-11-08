# 标签与语义

这一章讲 HTML 标签。

**先说清楚一件事**：在这套架构里，标签**几乎不参与渲染**。布局和绘制只看编译出来的
`QLUIStyle`（`layout` / `paint` 的字段），不看标签。标签只影响三样东西：

1. **UA 默认样式**（相当于浏览器的内置样式表）；
2. **`tag` 选择器**（`header { color: ... }`）；
3. **调试与查询**（`ql_ui_tag_name(node->src->tag)` 取回 "header"）。

所以"加一个标签"的代价几乎是零，而"标签不够用"的代价很实际：真实 HTML 片段贴进来
直接报错。

## 现支持的 32 个标签

| 类 | 标签 |
|---|---|
| 容器 / 结构 | `div` `section` `header` `footer` `nav` `main` `article` `aside` `figure` `figcaption` `blockquote` |
| 标题 | `h1` `h2` `h3` `h4` `h5` `h6` |
| 文本块 | `p` |
| 行内 | `span` `b` `i` `em` `strong` `u` `s` `mark` |
| 列表 | `ul` `ol` `li` |
| 空元素 | `hr` `br` |
| 控件 | `button` |

UA 默认样式都很薄，只有"看结构就该知道"的那些：

```css
/* 大致相当于内置样式表的全部内容 */
div, section, header, footer, nav, main, article, aside,
figure, figcaption, blockquote, p, li  { display: block; }
span, b, i, em, strong, u, s           { display: inline; }
h1 { font-size: 24px; margin: 10px 0 }
h2 { font-size: 20px; margin: 9px 0 }   /* …到 h6 递减 */
p  { margin: 8px 0 }
ul, ol { margin: 8px 0; padding-left: 18px }
hr { height: 1px; background-color: #8A8A8A; margin: 8px 0 }
mark { background-color: #FFE58F; color: #000 }
```

注意这是**给这个库的观感**调的，不是"和 Chrome 截图一致"调的（比如 `p` 的
margin 是 8px 而不是 1em）。

## 一个完整的文章页

<img src="../img/08-tags.png" alt="标签示例" width="260">

```c
static const char *k_html =
    "<div id=\"root\">"
    "  <header>QLGL 标签示例</header>"
    "  <hr>"
    "  <p>普通文本，<mark>高亮</mark>，<em>强调</em>，<strong>更强</strong>。</p>"
    "  <ul><li>第一项</li><li>第二项</li></ul>"
    "  <blockquote>引用一段话</blockquote>"
    "  <figure><div class=\"box\"></div>"
    "    <figcaption>图 1：一个方块</figcaption></figure>"
    "</div>";

static const char *k_css =
    "#root { width: 260px; background-color: #0B0E14; color: #C8D2DE;"
    "        font-size: 12px; }"
    "header { font-size: 14px; color: #7CE0FF; }"
    "blockquote { margin: 8px 10px; color: #8A94A0; }"
    "figcaption { font-size: 11px; color: #6C7783; }"
    ".box { width: 140px; height: 28px; border-radius: 4px;"
    "       background-color: #2E6BE6; }";
```

完整可运行的版本见 `example/08_tags.c`。

## 三个必须知道的"不完美"

### 1. 字体变体是**合成**的，不是换了字体文件

`b`/`strong` = 加粗，`i`/`em` = 斜体，`u` = 下划线，`s` = 删除线。
它们靠三个"穷人版"实现，不加载第二个字体文件（MCU 上也不可能）：

| 效果 | 做法 | 代价 |
|---|---|---|
| 加粗 | 字形再画一遍，右移 1px | 墨迹多约 1.6 倍（面积），像素级 |
| 斜体 | 按行剪切，顶部右移 `高度 × 0.21` | 每行起点差 1px，行内像素连续写 |
| 下划线 / 删除线 | 画一条 1px 横线 | 1 个矩形 |

这意味着：**小字号下斜体看起来会比较"硬"**（每行只能挪整像素），
加粗也不是真字重的形状。要求高就自己提供粗体字重的字体文件并换 `font-family`
（本库还没有 `font-family`，所以目前就是合成这一条路）。

对应 CSS 是标准写法，可以单独用：

```css
.note { font-weight: bold; font-style: italic; text-decoration: underline; }
```

`font-weight` 也吃数字：`>= 600` 算加粗（`normal` = 400、`bold` = 700）。
`text-decoration` 支持 `underline line-through` 组合，`overline` 明确报错。

### 2. 列表不画项目符号

本库没有生成内容（`::before` / `::marker`），所以 `<ul>` 只给你缩进和块级结构，
`•` 要自己写：

```html
<ul><li>• 第一项</li><li>• 第二项</li></ul>
```

### 3. `<br>` 是一个"高度为 0 的块"

没有生成内容，也没法在一个文本节点里插换行。但**块级元素会结束当前行内行**，
所以一个高度 0 的块刚好等于"换一行"：

```html
<div><span>a</span><br><span>b</span></div>   <!-- a、b 分两行 -->
```

代价是它占一个节点（不是纯文本里的 `\n`）。

## 行内元素之间的空格会被保留

这件事看起来小，但真实 HTML 里到处都是：

```html
<p>plain <mark>mark</mark> <em>em</em> <strong>strong</strong></p>
<!-- 渲染成 "plain mark em strong"，不是 "plain markemstrong" -->
```

规则与 CSS 一致：

| 情况 | 做法 |
|---|---|
| 两个**行内**元素之间只有空白 | 保留成一个空格 |
| 两个**块级**元素之间只有空白 | 丢掉（否则每个缩进都变一行） |
| 行首 / 行尾的空白 | 丢掉 |
| flex 容器里只含空白的项 | 不渲染（CSS：匿名 flex 项只含空白时不生成） |

它是在**解析期**决定的，靠的是标签的**默认** display —— 那时级联还没跑。
所以你把 `span` 改成 `display: block`，这个判断不会跟着变（这种写法本来就该自己管空白）。

## 明确报错的三个标签

写不了就直说，并且告诉你替代做法：

```
HTML: 不支持 <img>（本库没有图片解码器）：装饰用 div + background-image / 圆角做，
      图标用字体字符或 SDF 图元做
HTML: 不支持 <input>（还没有输入控件）：文本展示用 div，点击用 :active/:hover + 事件
HTML: 不支持表格 <table>：用嵌套的 flex 行来做
```

---

上一章：[进阶](07_advanced.html)
