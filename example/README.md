# 基本例程

每个例程一个文件夹，一个文件夹就是一个**完整可运行**的程序：

```
01_window/
  main.c         程序（40~160 行，只讲自己那一件事）
  index.html     界面的结构
  style.css      界面的样式
common.h         几个例程共用的样板：读文件、挑字体、跑主循环
```

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
| `24_b5` | `visibility` / `pointer-events` / `align-self` / `order` / `flex-flow`：占位隐藏、装饰层不挡点击、单项对齐、视觉重排 |

配套教程在 [`../doc/tutorial/`](../doc/tutorial/)，一章一个例程。
