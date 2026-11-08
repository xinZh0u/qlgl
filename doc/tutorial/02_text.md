# 文本

界面里最多的东西大概就是文字。这一章讲清 QLGL 文本的四条规则，
以及为什么它和浏览器不太一样。

<img src="../img/02-text.png" alt="文本" width="260">

完整程序：`example/02_text.c`

```c
static const char *k_html =
    "<div id=\"root\">"
    "  <div class=\"h1\">字号 = font-size</div>"
    "  <div class=\"s12\">12px 小字</div>"
    "  <div class=\"s16\">16px 正文</div>"
    "  <div class=\"s24\">24px 标题</div>"
    "  <div class=\"c\">居中的一行</div>"
    "  <div class=\"dim\">灰色、右对齐</div>"
    "</div>";

static const char *k_css =
    "#root { width: 260px; height: 220px; background-color: #0F1218; }"
    ".h1  { margin: 12px 0 8px 14px; font-size: 13px; color: #6C7783; }"
    ".s12 { margin: 2px 14px; font-size: 12px; color: #C8D2DE; }"
    ".s16 { margin: 2px 14px; font-size: 16px; color: #C8D2DE; }"
    ".s24 { margin: 2px 14px; font-size: 24px; color: #7CE0FF; }"
    ".c   { margin: 12px 14px 4px 14px; font-size: 14px; color: #E6C34A;"
    "       text-align: center; }"
    ".dim { margin: 2px 14px; font-size: 12px; color: #7A8794;"
    "       text-align: right; }";
```

## 规则一：一个元素 = 一整段文本

`<div>` 里的文字是**这个元素的文本**，不是子节点。所以：

```html
<div class="a">短文本</div>          <!-- ✅ 一个元素一段文本 -->
<p>Hello <b>world</b>!</p>            <!-- ✅ 编译期拆成三个元素 -->
```

第二种写法看着像"混合内容"，其实在**编译期**就被拆开了：

```html
<p><span>Hello </span><b>world</b><span>!</span></p>
```

文本段变成匿名的 `<span>`。这样做的好处是运行期数据模型里根本没有"文本节点"
这个东西 —— 元素树就是全部。代价是运行期不能往"文本中"插入元素（只能改整段文本）。

## 规则二：字号来自 `font-size`，颜色来自 `color`

两者都会被**继承**：

```css
.card { font-size: 14px; color: #C8D2DE; }
.card .title { font-size: 18px; }    /* 颜色自动继承 14px 那层的 */
```

`doc->font_sizes[]` 是**去重后的字号表**，每个节点的 `font_id` 是它的下标。
一个页面用几种字号就只建几套字形缓存，不会每个节点一份。

调一下程序就能看到：

```
字号表： 16px 13px 12px 24px 14px
```

（16px 排第一，因为 `@root` 没写 `font-size`，用了默认值。）

> **注意**：`font-size` 是**设备无关的 CSS 像素**。高 DPI 下由绘制后端乘缩放 ——
> 布局代码永远不需要关心 DPR。

## 规则三：`text-align` 控制水平对齐

只有三个值：`left` / `center` / `right`。对齐是在**元素的内容盒**里做的
（`content_box` = 边框盒减掉 `padding` 与 `border`）。

## 规则四：不自动折行

一段文本就是一行。超长就溢出（被父级 `overflow: hidden` 裁掉）。

想做"自动换行的段落"有两种办法：

```html
<!-- 办法一：手动切行（最可控，UI 里 99% 是这么干的） -->
<div class="line">第一行</div>
<div class="line">第二行</div>
```

```c
/* 办法二：运行期按宽度切 */
ql_ui_set_text(ui, node, "很长很长的一段话……");
/* 想要真正的多行排版用 QLText 的 ql_text_layout()，见 QLText 手册 */
```

QLGL 的选择是：**布局不猜你的意图**。文本换行是个排版问题（要处理断词、CJK
的换行规则、连字符），塞进布局算法里会让布局的复杂度失控。所以基础库给的是
"一段文本 + 对齐"，需要真段落时用 `QLText` 的排版接口。

## 运行期改文本

```c
ql_ui_set_text(ui, node, "新的内容");
```

这一条是**运行期唯一允许改的东西**（结构在编译期固定）。它会顺带把节点标脏，
主循环用 `ql_ui_consume_dirty()` 拿到区域后局部重绘。

下一章的例子里会用到它。

---

上一章：[基础知识](01_basics.html) ｜ 下一章：[布局](03_layout.html)
