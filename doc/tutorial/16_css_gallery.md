# CSS 效果检阅：三个例程

<img src="../img/16-css-units.png" alt="单位与尺寸：em / rem / vw / vh / vmin / vmax / aspect-ratio / margin:auto" width="480">

完整程序：`example/16_css_units/`、`example/17_css_selectors/`、`example/18_css_paint/`

前几章是按"补了什么"讲的（[CSS3 补全](14_css3.md)、[毛玻璃](15_backdrop.md)）。
这一章反过来：**按"能看出效果的最小样本"讲** —— 每一个已支持的 CSS 效果都摆成
一格，旁边写着它该是什么值。跑起来一眼就能看出哪一格不对。

```bash
cmake --build build -j
./build/example/example_16_css_units        # 单位与尺寸
./build/example/example_17_css_selectors    # 选择器与伪元素
./build/example/example_18_css_paint        # 绘制效果
```

三张图、三份 `index.html` + `style.css`，都可以直接改、直接重跑 —— 例程在运行期
读文件，不重新编译。

---

## 1. `16_css_units`：单位与尺寸

<img src="../img/16-css-units.png" alt="单位与尺寸" width="520">

| 格 | 验证什么 | 该看到什么 |
|---|---|---|
| `2em × 1.5em` | `em` 相对**元素自己**的 `font-size: 16px` | 32 × 24 |
| `6rem × 3rem` | `rem` 相对**根**字号 `12px` | 72 × 36 |
| `aspect-ratio: 16/9` | 只给宽 140，高由比例推出来 | 140 × 78 |
| `12vw` | 视口宽的 12% | 随窗口宽度变 |
| `15vh` | 视口高的 15% | 随窗口高度变 |
| `10vmin` / `20vmax` | 短边 / 长边（不跟横竖方向走） | 52 / 128 |
| `width: 50%` + `max-width: 70px` | 上限夹住 | 70 |
| `width: 20%` + `min-width: 110px` | 下限撑开 | 110 |
| `margin: 0 auto` | 块级元素水平居中 | 卡片在父盒里居中 |
| `padding: 1em`（两种 `box-sizing`） | `content-box` 外框 42，`border-box` 仍 42 | 两个方块一样大 |
| `hsl()` / `#rrggbbaa` | 颜色写法 | 蓝 / 半透明粉 |

**这几个单位什么时候折成像素**，是理解本库的关键：

| 单位 | 什么时候算 | 为什么 |
|---|---|---|
| `px` / `%` | 布局期 | `%` 要包含块宽度 |
| `em` | **构建期**（编译样式那一刻就折成 px） | 字号那时已确定 |
| `rem` | 构建期末尾（整棵树走完再折） | 根字号自身也可能是 `em` |
| `vw` / `vh` / `vmin` / `vmax` | 布局期 | 只有布局时才知道视口多大 |

## 2. `17_css_selectors`：选择器与伪元素

<img src="../img/17-css-selectors.png" alt="选择器与伪元素" width="520">

左列是**单条件命中**，右列是**组合与生成**。例程自己会数节点：

```
节点 60 个，其中 ::before 生成的圆点 5 个
```

| 格 | 验证什么 |
|---|---|
| `:first-child` / `:last-child` / `:nth-child(2n)` | 五行里第一行蓝、最后一行红、偶数行斑马 |
| `[data-state]` / `[data-state=warn]` / `[data-tag^=fe]` | 属性存在 / 精确 / 前缀三种 |
| `:not(.on)` | 两个 `off` 变灰，`on` 保持原样 |
| `p:nth-of-type(2)` | 只加粗第二个 `<p>`（中间夹着的 `<div>` 不干扰计数） |
| `::before` / `::after` + `content` | 三条竖线是**圆点**，引用前后有引号 |
| `.s + .s` | 第二、三项变色（相邻兄弟） |
| `.gh ~ .after-h3` | 小标题之后的所有兄弟都命中 |
| `*` | 统一 `box-sizing` |
| `:hover` / `:active` / `:focus` | 鼠标移上去变蓝、按住变红、点一下拿焦点出描边 |

**伪元素在这里是"真节点"**：构建期 `::before` 被展开成一个内容为 `content` 的
节点，运行期完全不知道有伪元素这回事。宿主自己有文字时（`<div>正文</div>`），
那段文字也会被挪进一个匿名 `<span>` —— 否则它和圆点都从内容盒原点开始排，
两样东西会叠在一起。这条路径和"文本 + 行内元素混排"是同一份实现
（见 [QLUI API 手册](../手册/ui/QLUI%20API手册.md) §3.4）。

## 3. `18_css_paint`：绘制效果

<img src="../img/18-css-paint.png" alt="绘制效果" width="560">

| 格 | 验证什么 |
|---|---|
| `linear-gradient` 垂直 / 水平 | 行填充快路径与逐像素插值 |
| `radial-gradient` | 圆心在中心，按到最远角归一化 |
| `border-radius` + `border` + `box-shadow` | 圆角、描边、外阴影一起画 |
| `opacity: 1 / 0.5 / 0.2` | 逐图元降低不透明度 |
| `clip-path: circle(50%)` | 圆形裁剪 |
| `transform: scale()` / `scaleX(-1)` | 缩放与水平镜像 |
| `backdrop-filter: blur(8px)` | **糊的是下面已经画好的像素**，那个蓝色方块一直在动，糊影跟着动 |
| `transition` | 鼠标移上去 200ms 变色 |
| `@keyframes` | 平移 + 缩放循环 |
| 文本样式 | `bold` / `italic` / `underline` / `line-through` / `sub` / `sup` / `smaller` |
| `text-align` / `line-height` | 左 / 中 / 右与行高 |
| `white-space` | `normal` 折行、`nowrap` 不折（溢出裁掉） |

毛玻璃那一格是这个例程的重点：**跑道先画、玻璃后画**，玻璃里看到的是跑道
被糊开的样子，而且是每帧重新糊的。

## 4. `19_css_text`：文本细节

<img src="../img/19-css-text.png" alt="字距 / 缩进 / 大小写 / 省略号 / 断词" width="520">

完整程序：`example/19_css_text/`

| 格 | 验证什么 | 该看到什么 |
|---|---|---|
| `letter-spacing: 0 / 2px / 6px` | 字距加在**每个字符之后** | 三行同一串字，一行比一行松 |
| `text-indent: 2em` | 首行缩进（只第一行） | 第一行缩进两个字，第二行顶格 |
| `text-transform` 三种 | `uppercase` / `lowercase` / `capitalize` | `HELLO WORLD` / `hello world` / `Hello World` |
| `text-overflow: ellipsis` | 列表项经典写法：左名字自动缩、右数字固定宽 | 超长的名字变 `…`，右边的 `12 KB` 不动 |
| 对照：`text-overflow: clip` | 不省略会怎样 | 红字溢出到盒子外面 |
| `word-break: normal` / `break-all` | 折行时允许断在哪里 | 同一句英文，normal 在空格断（右边参差），break-all 每行塞满（右边齐） |

两条容易踩的：

- **本库默认 `white-space: nowrap`**（与浏览器不同），要折行必须显式写 `white-space: normal`；
- 写例程时别把类名起成 `.cap` —— `text-transform` 会被同名标题类一起套上（我第一版就把所有标题变成了大写）。

## 5. `20_live_reload`：改 CSS 窗口自己变

<img src="../img/20-live-reload.png" alt="热重载：改 style.css 窗口自己更新" width="480">

完整程序：`example/20_live_reload/`

```bash
./build/example/example_20_live_reload
# 另开编辑器改 example/20_live_reload/style.css，保存
```

终端会打印 `重载：41 个节点，17 个样式，1 ms` —— **应用二进制没动过**。

原理只有一句：界面是可以在运行期编译的（`ql_ui_load_text`），所以热重载 = 重新读文件 +
重新建树。新文件写坏了会**保留旧界面**并打印原因（"明确失败优于半个正确"）。
这件事和"设计工具"是同一套东西，见 [对比与取舍 §12](../design/对比与取舍.html)。

## 6. 这几个例程查出了什么

写完这三个例程当场暴露了一个真 bug：

- `::before` 配宿主文本时，两边都从内容盒原点开始排 —— 圆点被正文盖掉，
  `::after` 的引号同理。修法是把宿主文本挪进匿名 `span`（与混排同一份实现），
  并让这个匿名节点**不参与** `:first-child` / `+` / `~` 的匹配与计数。
  回归测试：`test/web/test_pseudo_text.c`。
- 顺带修掉 `expand_pseudo` 里两处隐患：一条会漏内存的占位语句，
  以及规则表 `realloc` 之后继续用旧 `Rule *` 的悬垂指针。

这就是"把每个效果都摆出来"的价值 —— 单看每个特性的单元测试都绿，
放到同一张图里才看得出**它们之间的相互作用**。
