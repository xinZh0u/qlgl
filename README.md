# QLGL 轻灵图形库（QingLing Graphics Library）

用 C11 实现的轻量 GUI 图形库，目标是让界面用 HTML + CSS 描述（构建期预编译成低开销二进制），
运行期只做"按 id 取已算好的值"，可裁到 MCU，也可跑在 MPU/PC 上。

当前进度：

- **对象系统（object）已完成** —— 类型、对象生命周期、属性、事件、内存与对象树；
- **绘制系统已完成** —— 显示驱动、画布原语、脏矩形渲染，CPU 光栅与 GPU 双后端；
  **没有控件类**：界面只由 HTML 元素 + CSS 描述（`QLWidget` 那套体系已删除）；
- **字体/文本系统已完成** —— 内置 4bpp 位图字体（MCU）+ FreeType 动态字体（任意字号 / 中文）、
  UTF-8 解码、文本度量；
- **交互闭环已完成** —— 命中测试、hover/pressed/focus 状态、点击合成、键盘焦点，
  事件沿元素树派发；HTML/CSS 的 `:hover/:active/:focus` 在构建期编译成状态样式；
- **布局表达力（M3）已完成** —— flexbox（row/column × nowrap/wrap、grow/shrink/basis、gap、
  justify-content、align-items）、`position: relative/absolute/fixed`、`z-index`、`overflow` 裁剪、
  文本与行内元素混排（生成期消解成匿名节点）；
- **HTML+CSS → C 编译已完成（v0.1）** —— `qltext` 把 HTML/CSS 编译成静态 C 数据，运行时零解析；
- **运行期 HTML/CSS 解析（M3.5 第 3 步）已完成** —— `ql_ui_load_text(html, css)` 在加载时算一次，
  产物与构建期工具**逐字节相同**（27 页用例守着）；可 `-DQLGL_BUILD_TEXT_PARSER=OFF` 整块摘掉；
- **现代 UI 效果（先行项）** —— `box-shadow` 软阴影（SDF，单趟）与 `linear-gradient`
  （垂直走行填充快路径，GPU 走顶点颜色插值、零额外 draw call）；
- **动效（M5.5）已完成 ①②③④⑥⑦⑧⑨ 与 translate/scale/分轴翻转/圆角揭示/圆形遮罩/毛玻璃** —— 帧时钟 + `transition` + `@keyframes`
  全部构建期编译成静态数据，运行期零解析、零分配；动画只改绘制期属性（颜色 / alpha / 平移），
  **零重排**，走脏区局部重绘；静态界面下 `ql_ui_anim_tick()` 返回 0，主循环整帧跳过。
  实测 5 段动画同时跑 **0.0052 ms/帧**。
  `./build/example_anim_window` 可直接看效果；
- **绘制期亚像素（M5.3）已完成** —— `translate(0.5px)` 真的落在半像素上（12px 位移有 64 个台阶，
  而不是 1 个）；边缘用面积覆盖率抗锯齿，对齐时与整数路径**逐像素相同**；
  只影响绘制不影响布局（与 CSS `transform` 语义一致），GPU 侧零成本；
- **高 DPI（M5.2）已完成** —— 布局一律 CSS px，缩放只在绘制后端入口乘一次
  （26.6 定点）；CPU 与 GL 两条路径同步缩放，`test_dpr` 断言 DPR=2 降采样后与 DPR=1 差 **0.57%**；
  X11 自动读 `Xft.dpi`，`example_gl_window --dpr 2` 可直接看效果；
- **性能与脏区（M4）** —— CPU 光栅 1080p **2.19 ms**、320×240 **0.30 ms**（比 M4 之前快 2.3~2.7×）；
  每个节点带 `paint_box`，`ql_ui_render_rect()` 只重绘脏区（局部刷新是全量的 4.5%）；
  界面层直接吃 `QLDraw2D`，同一棵树可以直接上 GPU；
- **`.qlui` blob 已完成** —— 同一份 doc 可导出成二进制，运行时 `ql_ui_load_file()` 加载
  （改 UI **不用重编译**，实测热重载）；序列化只在 C 侧实现一次，与静态路径逐字节一致；
- **GPU 后端已完成（v0.1）** —— `QLDraw2D` 绘制接口 + GLES3/EGL 后端（SDF 圆角、字形图集、单 draw call），
  与 CPU 后端逐像素对齐（差分测试）；
- **Linux 窗口后端已完成** —— X11（XImage 直绘零拷贝）与 Wayland（xdg-shell + wl_shm 双缓冲），
  无桌面环境自动降级到内存显示。

## 快速开始

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 跑第一个例程（不给 --frames 就一直跑；只给 --dump 是截图模式：一帧后退出）
./build/example/example_01_window --dump /tmp/01.ppm
```

**想系统学？读 [教程](doc/html/index.html)**（`doc/tutorial/`，一章配一个可编译运行
的完整程序）。也可以直接看 [`example/`](example/)：**一个例程一个文件夹**，
`main.c` + `index.html` + `style.css`，改一行 CSS 重跑就能看到效果。

| 例程 | 讲什么 |
|---|---|
| `01_window` | 最小闭环：编译 → 建树 → 布局 → 显示 |
| `02_text` | 字号 / 颜色 / 对齐 / 文本规则 |
| `03_layout` | flex / 百分比 / 绝对定位 |
| `04_interaction` | 状态样式 / 事件派发 / 点击回调 |
| `05_animation` | `@keyframes` / 脏区 / 动画属性白名单 |
| `06_theme` | 同一份 HTML 三份 CSS / `.qlui` 缓存 |
| `07_player` | 一个真实界面（播放器）：纵向 flex / 渐变封面 / 进度条 / **自动折行**的简介 |
| `08_tags` | HTML 标签与 UA 默认样式：一半纯默认值，一半只用 CSS 改观感 |
| `09_scroll` | 滚动容器：`overflow: auto` + 滚轮 / 拖滚动条 / PageDown |
| `10_input` | 输入控件：`<input>`（含密码框）+ `<select>` 下拉面板 + Tab 切换焦点 |
| `11_semantic` | 纯语义标签：`dl` / `del` / `ins` / `cite` / `fieldset`，整段真实 HTML 贴进来就能画 |
| `12_text_block` | `white-space: pre` 系列 / `<pre>` / `<sub>` `<sup>` |
| `13_form` | `<textarea>` 多行编辑 + 整张表单（Tab 跳、Ctrl+A/C/V、中文输入） |
| `14_css3` | `::before` / `:nth-child` / `[data-*]` / `:not()` / `min-max` / `!important` / `hsl()` |
| `15_backdrop` | 毛玻璃叠在动的背景上：半透明上层 + `backdrop-filter`，下层实时变 |
| `16_css_units` | 单位与尺寸：`em` / `rem` / `vw` / `vh` / `vmin` / `vmax` / `min-max-*` / `aspect-ratio` / `margin: auto` |
| `17_css_selectors` | 选择器与伪元素：结构伪类 / 属性选择器 / `:not()` / `::before` `::after` / `+` `~` `*` / 状态伪类 |
| `18_css_paint` | 绘制效果：渐变 / 圆角边框阴影 / `opacity` / `clip-path` / `transform` / 毛玻璃 / `transition` / `@keyframes` / 文本样式 |
| `19_css_text` | 文本细节：`letter-spacing` / `text-indent` / `text-transform` / `text-overflow: ellipsis` / `word-break` |

## 构建与测试

```bash
cmake -B build
cmake --build build -j
ctest --test-dir build --output-on-failure

cmake --build build --target doc_html      # 重新生成 doc/html/（需要 python3-markdown）
```

示例程序：

```bash
./build/example/example_01_window          # 最小窗口
./build/example/example_13_form            # 表单：input + textarea + 按钮
./build/example/example_09_scroll          # 滚动容器
./build/example/example_01_window --dump /tmp/a.ppm    # 无显示器也能出图
./build/example/example_01_window --size 900x600       # 换窗口尺寸看自适应
./build/benchmark/bench                    # 可视化性能基准（8 个维度 × CPU/GL）
./build/example/example_gl_window --dpr 2  # GPU 窗口，2x 设备像素比
./build/test_web_ui                       # HTML+CSS 编译成 C 后的布局/绘制断言
./build/test_render                       # 脏区重绘与全量重绘的逐像素一致性
./build/test_dpr                          # DPR=1 与 DPR=2 的一致性（CPU 与 GPU）
./build/test_subpx                        # 亚像素填充的覆盖率与"对齐时逐字节不变"
./build/test_effects                      # 渐变/软阴影的像素断言 + 九宫格分解等价性
./build/example_bench_ui                  # QLUI 节点级基准（布局/全量/局部/命中）
./build/test_gl_vs_cpu                    # GPU 与 CPU 后端的像素一致性（含字形几何）
./build/test_gl_dirty                     # 保留式脏区：局部重绘 == 整屏重画
./build/test_anim_stress                  # 10 元素同时动画的每帧开销（60fps 预算）
./build/test_clip                         # clip-path: circle() 子树圆形遮罩（CPU vs GPU）
./build/test_scale                        # transform: scale() 绘制期缩放（布局不受影响）
./build/test_flip                         # scaleX()/scaleY() 分轴缩放 + animation-delay（假 3D 翻转）
./build/test_blur                         # backdrop-filter: blur() 毛玻璃（CPU 与 GPU 各断言一遍性质）
./build/test_text_parse                   # 运行期解析器 vs 构建期工具：27 页 blob 逐字节相同
./build/test_flex_auto                    # flex 项的 width:auto = max-content（回归：固定宽兄弟不被压扁）
./build/test_qltext_blob                  # qltext blob 直接产出的 .qlui 与提交基准逐字节相同
./build/test_line_height                  # line-height：单行文字垂直居中（按渲染墨迹判断）
./build/test_text_wrap                    # 自动折行：断行规则 + 布局/绘制共用一份行数
./build/test_html_tags                    # 80 个标签的 UA 默认样式 + 元数据跳过 + 行内空白规则
./build/test_text_style                   # 合成加粗/斜体/下划线 + 装饰线
./build/test_scroll                       # overflow: auto 的滚动（滚轮/拖动/夹紧/命中）
./build/test_input_edit                   # <input>：值/光标/选区/UTF-8 边界/密码显示
./build/test_select                       # <select>：覆盖式面板/命中/键盘/Tab 焦点循环
./build/test_progress                     # <progress>/<meter>：value/max → 完成度，填充按比例变宽
./build/test_details                      # <details>/<summary>：折叠、点击切换、重排
./build/test_dialog                       # <dialog>：居中/遮罩/点外面与 Esc 关闭/脏区重绘也不漏遮罩
./build/test_image                        # <img>：资源注册/固有尺寸/缩放上屏/BMP·QOI 解码/有界校验
./build/test_qltext_assets                # 构建期转码：qltext cc --root 的产物与基准逐字节一致
./build/test_text_reject                  # 拒绝集合一致：35 个正负用例
./build/example_load_text --dump a.ppm    # 运行期编译 HTML+CSS 并渲染（不用重新编译程序）
./build/test_golden                       # golden 护栏：27 个 CSS 用例 × (blob 逐字节 + 基准图容差)
QLGL_GOLDEN_UPDATE=1 ./build/test_golden   # 确认画面无误后重建基准
./build/example_flip_window               # 假翻转演示：水平翻/垂直翻/圆形硬币，错开相位
./build/example_showcase_window           # M5.5 效果总览（每帧整屏重绘，60fps）：过渡/滑入/圆角揭示/圆形遮罩/径向渐变/假翻转/毛玻璃
./build/example_window                    # 打开真实窗口（自动选 Wayland 或 X11）
./build/example_window --backend=x11      # 强制 X11；--backend=wayland 强制 Wayland
./build/example_window --frames 120       # 跑 120 帧后自动退出
```

依赖：CMake ≥ 3.16、C11 编译器。

| 可选后端 | 依赖（Debian/Ubuntu） | 关闭选项 |
|---|---|---|
| X11 | `libx11-dev` | `-DQLGL_BUILD_X11=OFF` |
| Wayland | `libwayland-dev wayland-protocols`（`wayland-scanner`） | `-DQLGL_BUILD_WAYLAND=OFF` |
| GPU (GLES3) | `libegl1-mesa-dev libgles2-mesa-dev` | `-DQLGL_BUILD_GL=OFF` |

两者都没有时仍可正常构建，程序自动降级到内存显示（可导出 PPM）。
`xdg-shell.xml` 已随仓库提供，不需要系统安装 wayland-protocols 的头文件。

要求：CMake ≥ 3.16、支持 C11 的编译器（构建使用 `-std=c11`，不依赖 POSIX 扩展）。

## 平台定位

| 平台 | 界面写法 | 说明 |
|---|---|---|
| MCU | HTML+CSS（`qltext` 构建期编译成 C）+ 编译期裁能力包 | 同一套写法，裁的是能力不是语言 |
| MPU / 嵌入式 Linux | HTML+CSS，构建期编译成 C 或运行期解析 | 未支持的特性**构建期报错** |
| PC / 移动（含 Linux 手机） | HTML+CSS，同一套 | 全量能力包；**不接 WebView、不嵌浏览器、不做 JS** |

> 三条不变的红线，任何平台都一样：
> 1. **不做 JS 引擎**（没有 `script` 执行、没有 DOM API）；
> 2. **不嵌系统 WebView / 浏览器引擎** —— 即使在 PC / 手机上，绘制也是本库自己的；
> 3. 不支持的东西**构建期报错**，绝不静默降级。
>
> 详见 [Web 兼容设计](doc/Web兼容设计.md)。

## 目录

```
include/qlgl/      对外头文件（唯一安装目录）
  qlgl.h           总入口：错误码、日志、生命周期
  qlconfig.h       容量与裁剪开关
  qlvalue.h        通用值容器与 Q16.16 定点
  qlclass.h        类描述、注册、定义类的宏
  qlobject.h       根类：生命周期、对象树、私有区
  qlprop.h         属性 id 区间与派发
  qlevent.h        事件参数、传播、发射宏
  qlmem.h          分配器钩子与统计
  qlgeom.h         坐标、颜色、矩形
  qldisp.h         显示驱动接口 + 内存显示驱动
  qlcanvas.h       画布与绘制原语
  qlcanvas.h       画布与绘制原语（同上）
  qldraw2d.h       绘制接口（CPU / GPU 后端共用）
  qlfont.h         字体（位图/动态）、字形查找、UTF-8、文本度量
  qlfont_ft.h      FreeType 后端（可选）
  qlstr.h          自管理字符串（compact / amortized 双策略）
  qlinput.h        输入事件与派发
  qlimage.h        图片解码管线（BMP / QOI / PNG / GIF / JPEG 各一个开关）
  qlclipboard 由 port 层提供（`src/port/qlclipboard_x11.h`）
  qldisp_x11.h     Linux X11 窗口后端（可选）
  qldisp_wayland.h Linux Wayland 窗口后端（可选）
  qlui.h           HTML+CSS 编译产物格式与运行期布局/绘制
  qluitext.h       运行期 HTML/CSS 解析入口（`ql_ui_load_text`）
  qlgfx_gl.h       GLES3 + EGL GPU 后端（可选）
  qlanim.h         帧时钟、缓动曲线、补间动画池
  qltype_gen.h     自动生成的类型 id（勿手改）
src/core/          对象系统实现
src/draw/          几何、显示、画布、渲染器
src/text/          字体实现、FreeType 后端、自动生成的字形数据
src/ui/            运行期解析、物化、布局、绘制、blob 序列化
src/port/          平台后端（X11 / Wayland 窗口）
src/generated/     自动生成的类注册表（勿手改）
tools/qlgen/       扫描源码生成类型 id 表
tools/qlfontgen/   从 TTF 生成内置位图字体
tools/qltext/      把 HTML+CSS 编译成 C / .qlui（相当于 Qt 的 uic，链库自己的解析器）
tools/qlui/        兼容壳：只剩"找 qltext + 转发参数"（语义在 src/ui/qlui_parse.c）
tools/qlblob/      把生成的 doc 导出成 .qlui blob（宿主工具）
test/object/       对象系统示例与测试
test/draw/         绘制系统示例与测试
test/text/         字体/文本示例与测试
test/web/          HTML+CSS 编译示例与测试
test/input/        交互（命中/悬停/点击/焦点）示例与测试
test/uitext/       运行期 HTML/CSS 解析的示例
example/           基本例程（一个例程一个文件夹：main.c + index.html + style.css）
tools/doc/         文档生成：Markdown → doc/html/（静态站点）
doc/tutorial/      入门教程（Markdown 源，配 example/）
doc/img/           教程配图（由 example/ 渲染出来的真实截图）
doc/html/          生成的 HTML 文档站（可以整目录拷走/发布）
doc/
  代码规范.txt
  底层实现设计.md
  手册/object/     QLObject 阅读指南 / API 手册
```

## 加一个类之后

```bash
python3 tools/qlgen/qlgen.py          # 重新生成类型 id 表
python3 tools/qlgen/qlgen.py --check  # CI 校验是否同步
```

## 文档

**读**：[HTML 文档站](doc/html/index.html)（`cmake --build build --target doc_html` 生成，
布局参照 GTK 文档；`doc/tutorial/` 与 `doc/手册/` 是它的 Markdown 源）。

先读教程：[基础知识](doc/tutorial/01_basics.md) → [文本](doc/tutorial/02_text.md) →
[布局](doc/tutorial/03_layout.md) → [交互](doc/tutorial/04_interaction.md) →
[动画](doc/tutorial/05_animation.md) → [换皮肤](doc/tutorial/06_theme.md) →
[进阶](doc/tutorial/07_advanced.md) → [标签](doc/tutorial/08_tags.md) →
[滚动](doc/tutorial/09_scroll.md) → [与 Web 的差别](doc/tutorial/10_vs_web.md) →
[输入控件](doc/tutorial/11_input.md) → [空白与上下标](doc/tutorial/12_text.md) →
[多行输入与表单](doc/tutorial/13_form.md) → [CSS3 补全](doc/tutorial/14_css3.md) →
[毛玻璃叠在动的背景上](doc/tutorial/15_backdrop.md) →
**[CSS 效果检阅：三个例程](doc/tutorial/16_css_gallery.md)**。

模块手册：

- 对象系统：[QLObject 阅读指南](doc/手册/object/QLObject阅读指南.md) / [QLObject API 手册](doc/手册/object/QLObject%20API手册.md)
- 绘制后端：[QLRender 阅读指南](doc/手册/render/QLRender阅读指南.md) / [QLRender API 手册](doc/手册/render/QLRender%20API手册.md)
- 交互与输入：[QLInput 阅读指南](doc/手册/input/QLInput阅读指南.md) / [QLInput API 手册](doc/手册/input/QLInput%20API手册.md)
- 字体与文本：[QLText 阅读指南](doc/手册/text/QLText阅读指南.md) / [QLText API 手册](doc/手册/text/QLText%20API手册.md)
- HTML+CSS 编译：[QLUI 阅读指南](doc/手册/ui/QLUI阅读指南.md) / [QLUI API 手册](doc/手册/ui/QLUI%20API手册.md)
- HTML5 覆盖度：[HTML5 标签覆盖与裁剪](doc/手册/ui/HTML5标签覆盖与裁剪.md)（114 个元素逐条状态 + MCU 能力包裁剪）
- CSS 覆盖度：[CSS3 覆盖与裁剪](doc/手册/ui/CSS3覆盖与裁剪.md)（按模块穷举 + 原因码 + CSS 能力包）
- 图像：[图像管线设计](doc/图像管线设计.md)（解码分工 / `<img>` 布局 / 两个后端的 blit）
- 动效：[QLAnim 阅读指南](doc/手册/anim/QLAnim阅读指南.md) / [QLAnim API 手册](doc/手册/anim/QLAnim%20API手册.md)
- 运行期解析：[QLUIText 阅读指南](doc/手册/uitext/QLUIText阅读指南.md) / [QLUIText API 手册](doc/手册/uitext/QLUIText%20API手册.md)
- 渲染后端：[QLRender 阅读指南](doc/手册/render/QLRender阅读指南.md) / [QLRender API 手册](doc/手册/render/QLRender%20API手册.md)
- Web 兼容策略：[Web 兼容设计](doc/Web兼容设计.md)
- 开发规划（里程碑与验收）：[开发规划](doc/开发规划.md)
- 性能基线：[bench.md](doc/bench.md)
- 想看整体设计：[底层实现设计](doc/底层实现设计.md)
- 编码规范：[代码规范](doc/代码规范.txt)

## 许可

见 [LICENSE](LICENSE)。
