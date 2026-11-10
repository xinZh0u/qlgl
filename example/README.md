# 基本例程

每个例程一个文件夹，一个文件夹就是一个**完整可运行**的程序：

```
01_window/
  main.c         程序（40~160 行，只讲自己那一件事）
  index.html     界面的结构
  style.css      界面的样式
common.h         几个例程共用的样板：读文件、挑字体、跑主循环
```

## 约定：例程一律白底（浅色）主题

**所有例程都必须是白底浅色主题**，`06_theme` 是唯一的例外（它存在的意义就是
演示三套主题，浅色只是其中一套）。

理由很实际：

1. `doc/img/*.png` 的配图就是从这些例程截的。深色底和文档站的浅色页面拼在
   一起很扎眼，读者会先注意到"图怎么是黑的"，而不是图里讲的东西；
2. 浅色底下"少画了一块"、`overflow` 裁错了、阴影丢了，全都一眼能看出来；
   深色底会把这些错误藏起来（我们自己就在 `--dump` 的黑底上漏过好几回）；
3. `01`–`24` 本来就是浅色盘，新加的例程没必要再造一套。

直接抄这套色板（GitHub 浅色）：

| 用途 | 色值 |
|---|---|
| 页面底色 | `#FFFFFF` |
| 次级面（卡片、表格头、inset） | `#F6F8FA` |
| 正文 | `#1F2328` |
| 次要文字 | `#57606A` / `#6E7781` |
| 分隔线、禁用 | `#D0D7DE` / `#8C959F` |
| 主色（链接、主按钮） | `#0969DA`（按下 `#0A3069`） |
| 成功 / 警告 / 危险 | `#1A7F37` / `#9A6700` / `#B42318` |
| 紫 / 粉（点缀） | `#8250DF` / `#BF3989` |

三条硬规则：

1. **浅色文字不许上白底**：`#E6C34A`、`#BFC7D1` 这类"深色主题里的高亮色"
   在白底上对比度不到 2:1，等于没写。要黄色就用 `#9A6700`，要灰就用 `#8C959F`。
2. **深色填充块上的字必须显式写 `color: #FFFFFF`**：文字颜色是继承的，
   换浅色主题后默认继承到的是深色字，压在蓝/绿/紫块上就看不见了。
3. **截图一律用 `--dump`**，它的清除色是根元素的 `background-color`
   （见 `common.h` 的 `ex_dump_ppm()`）。自己另写一份 `clear(0x000000)`
   会在白底页面四周留一圈黑边。

## 跑起来

```bash
cmake -S . -B build && cmake --build build -j
./build/example/example_01_window                     # 打开窗口
./build/example/example_01_window --dump /tmp/01.ppm  # 截图模式：渲染一帧就退出
./build/example/example_01_window --frames 120        # 跑 120 帧
```

| 参数 | 作用 |
|---|---|
| `--dump FILE` | 把一帧存成 PPM（没有显示器也能跑） |
| `--frames N` | 跑 N 帧后退出 |
| `--size WxH` | 换窗口尺寸，看页面是不是真自适应 |
| `QLGL_EX_DIR=DIR` | 环境变量：让例程去别处读 HTML/CSS |

单个例程：`cmake --build build --target example_01_window`。

## 为什么 HTML/CSS 是单独的文件

例程是给人在 PC 上**直接跑**的，所以 `main.c` 里读的是文件路径：

```c
QLUIDoc *doc = ex_load_ui("index.html", "style.css");
```

好处有三个：

1. 改一行 CSS 重跑就能看到效果，不用重新编译；
2. 编辑器里有 HTML/CSS 高亮、格式化、跳转；
3. 同一批文件可以直接交给构建期编译工具，变成 `.rodata` 里的静态表：

```bash
python3 tools/qltext/qltext.py cc \
    --html example/01_window/index.html \
    --css  example/01_window/style.css  \
    --name demo --out build/
```

路径怎么找：`QL_EX_DIR` 由 CMake 注入成该例程目录的**绝对路径**，所以从任何
工作目录启动都能找到；相对名先看当前目录（临时改一份 CSS 试效果最顺手），
没有再落到例程目录。

## 例程清单

| 例程 | 讲什么 |
|---|---|
| `01_window` | 最小闭环：编译 → 建树 → 布局 → 显示 |
| `02_text` | 字号 / 颜色 / 对齐 / 文本规则 |
| `03_layout` | flex / 百分比 / 绝对定位 |
| `04_interaction` | 状态样式 / 事件派发 / 点击回调 |
| `05_animation` | `@keyframes` / 脏区 / 动画属性白名单 |
| `06_theme` | 同一份 HTML 四份 CSS（`index.html` + `dark/light/warm.css`） |
| `07_player` | 一个真实界面（播放器）：纵向 flex / 渐变封面 / 进度条 / 自动折行 |
| `08_tags` | HTML 标签与 UA 默认样式：一半纯默认值，一半只用 CSS 改观感 |
| `09_scroll` | 滚动容器：`overflow: auto` + 滚轮 / 拖滚动条 / PageDown |
| `10_input` | 输入控件：`<input>`（含密码框）+ `<select>` 下拉面板 + Tab 切换焦点 |
| `11_semantic` | 纯语义标签：`dl` / `del` / `ins` / `cite` / `fieldset` |
| `12_text_block` | `white-space: pre` 系列 / `<pre>` / `<sub>` `<sup>` |
| `13_form` | `<textarea>` 多行编辑 + 整张表单 |
| `14_css3` | `::before` / `:nth-child` / `[data-*]` / `:not()` / `min-max` / `!important` / `hsl()` |
| `15_backdrop` | 毛玻璃叠在动的背景上：半透明上层 + `backdrop-filter` |
| `16_css_units` | 单位与尺寸：`px` / `%` / `em` / `rem` / `vw` / `vh` / `vmin` / `vmax` / `min-max-*` / `aspect-ratio` / `margin: auto` |
| `17_css_selectors` | 选择器与伪元素：结构伪类 / 属性选择器 / `:not()` / `::before` `::after` / `+` `~` `*` / `:hover` `:active` `:focus` |
| `18_css_paint` | 绘制效果：渐变 / 圆角边框阴影 / `opacity` / `clip-path` / `transform` / 毛玻璃 / `transition` / `@keyframes` / 文本样式 |
| `19_css_text` | 文本细节：`letter-spacing` / `text-indent` / `text-transform` / `text-overflow: ellipsis` / `word-break` |
| `20_live_reload` | 热重载：改 `style.css` → 运行期重编译整份 doc（1 ms），失败保留旧界面 |
| `21_css_vars` | CSS 变量 `--x` / `var()`：构建期替换，`QLUIStyle` 不变；局部覆盖做换肤 |
| `22_media` | `@media (max-width)` 断点：同一份 HTML 在窄屏换成一列（构建期切区间，运行期不解析条件） |
| `23_calc` | `calc()` / `min()` / `max()` / `clamp()`：三等分、clamp 条、绝对定位居中（后缀字节码 + 布局期求值） |
| `24_b5` | B5 十二件：占位隐藏、装饰层不挡点击、单项/多行对齐、视觉重排、sticky 表头、分轴裁剪 |
| `25_b4` | B4 绘制细节：边框样式、四边/四角、两层与 inset 阴影、text-shadow、outline 偏移、滚动条宽度与颜色 |
| `26_css_states` | B2 状态：禁用变灰、复选框勾选、占位文字样式（`:disabled` / `:checked` / `::placeholder`） |
| `27_css_text2` | B3 文本：词间距、制表符宽度、装饰线的形态/粗细/颜色/偏移 |
| `28_bg_image` | B4 背景图：cover/contain/具体尺寸、九宫位置、平铺与圆角裁切 |
| `29_grid` | B6 Grid：具名区域搭页面骨架、12 列栅格、`span` 跨行跨列、自动放置与对齐 |
| `30_border_image` | B6 `border-image`：九宫格切片，stretch / repeat / round 三种填充 |

配套教程在 [`../doc/tutorial/`](../doc/tutorial/)，一章一个例程。

---

## 例程的循环参数（`common.h` 的 `ex_run`）

| 参数 | 含义 |
|---|---|
| （默认） | **事件驱动**：没有脏区就阻塞等事件（静止时 0 帧、0% CPU） |
| `--spin` | 每帧都渲染（基准测试用） |
| `--frames N` | 渲染 N 帧后退出（隐含 `--spin`） |
| `--seconds S` | 跑 S 秒后退出 |
| `--stats` | 退出时打帧时间 p50/p95/max + 其中等 swap 的时间 |
| `--vsync 0/1` | 强制开关垂直同步（拖动缩放期间库会自动临时关闭） |
| `--size 800x480` | 换窗口尺寸 |
| `--dump out.ppm` | 存一帧（不等着关窗口） |

测量方法与"拖动窗口掉帧"的完整案例见 [性能诊断](../doc/性能诊断.md)。
