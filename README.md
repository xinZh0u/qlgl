# QLGL 轻灵图形库（QingLing Graphics Library）

用 C11 实现的轻量 GUI 图形库，目标是让界面用 HTML + CSS 描述（构建期预编译成低开销二进制），
运行期只做"按 id 取已算好的值"，可裁到 MCU，也可跑在 MPU/PC 上。

当前进度：

- **对象系统（object）已完成** —— 类型、对象生命周期、属性、事件、内存与对象树；
- **绘制系统 + 控件基类（widget）已完成** —— 显示驱动、画布原语、脏矩形渲染、QLWidget 及自定义控件；
- **字体/文本系统已完成** —— 内置 4bpp 位图字体（MCU）+ FreeType 动态字体（任意字号 / 中文）、
  UTF-8 解码、文本度量、`QLLabel` 控件；
- **交互闭环已完成** —— 命中测试、hover/pressed/focus 状态、点击合成、键盘焦点，
  事件沿控件树冒泡；HTML/CSS 的 `:hover/:active/:focus` 在构建期编译成状态样式；
- **布局表达力（M3）已完成** —— flexbox（row/column × nowrap/wrap、grow/shrink/basis、gap、
  justify-content、align-items）、`position: relative/absolute/fixed`、`z-index`、`overflow` 裁剪、
  文本与行内元素混排（生成期消解成匿名节点）；
- **HTML+CSS → C 编译已完成（v0.1）** —— `tools/qlui` 把 HTML/CSS 编译成静态 C 数据，运行时零解析；
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
  控件层的绘制函数也走 `QLDraw2D`，同一棵控件树可以直接上 GPU（`ql_render_draw2d()`）；
- **`.qlui` blob 已完成** —— 同一份 doc 可导出成二进制，运行时 `ql_ui_load_file()` 加载
  （改 UI **不用重编译**，实测热重载）；序列化只在 C 侧实现一次，与静态路径逐字节一致；
- **GPU 后端已完成（v0.1）** —— `QLDraw2D` 绘制接口 + GLES3/EGL 后端（SDF 圆角、字形图集、单 draw call），
  与 CPU 后端逐像素对齐（差分测试）；
- **Linux 窗口后端已完成** —— X11（XImage 直绘零拷贝）与 Wayland（xdg-shell + wl_shm 双缓冲），
  无桌面环境自动降级到内存显示。

## 构建与测试

```bash
cmake -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

示例程序：

```bash
./build/example_object                    # 对象系统演示
./build/example_widget /tmp/out.ppm       # 渲染一个界面并输出 PPM 图片
./build/example_text /tmp/text.ppm        # 文本样张：字号/对齐/多行/缺字
./build/example_gl_window                 # GPU 窗口：HTML/CSS 编译出的界面 + 动画
./build/example_gl_window --dpr 2         # 同上，2x 设备像素比
./build/example_anim_window               # 动效演示：@keyframes + transform（空格重播，只重绘脏区）
./build/example_widget --frames 120       # 控件示例，默认打开窗口
./build/example_text --frames 120         # 文本示例，默认打开窗口
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
| MCU | 纯 C（`QLWidget`/`QLLabel`） | 不追求 Web 兼容，追求小体积 |
| MPU / 嵌入式 Linux | HTML+CSS，构建期编译成 C（`tools/qlui`） | Web 子集，未支持特性构建期报错 |
| PC / 移动 | HTML+CSS+JS，配系统 WebView | 高兼容；详见 [Web 兼容设计](doc/Web兼容设计.md) |

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
  qlwidget.h       控件基类
  qlrender.h       脏矩形渲染器
  qlfont.h         字体（位图/动态）、字形查找、UTF-8、文本度量
  qlfont_ft.h      FreeType 后端（可选）
  qlstr.h          自管理字符串（compact / amortized 双策略）
  qllabel.h        文本控件
  qldisp_x11.h     Linux X11 窗口后端（可选）
  qldisp_wayland.h Linux Wayland 窗口后端（可选）
  qlui.h           HTML+CSS 编译产物格式与运行期布局/绘制
  qldraw2d.h       绘制接口（CPU / GPU 后端共用）
  qlgfx_gl.h       GLES3 + EGL GPU 后端（可选）
  qlanim.h         帧时钟、缓动曲线、补间动画池
  qltype_gen.h     自动生成的类型 id（勿手改）
src/core/          对象系统实现
src/draw/          几何、显示、画布、渲染器
src/widget/        控件基类与 QLLabel
src/text/          字体实现、FreeType 后端、自动生成的字形数据
src/ui/            HTML+CSS 编译结果的物化/布局/绘制
src/port/          平台后端（X11 / Wayland 窗口）
src/generated/     自动生成的类注册表（勿手改）
tools/qlgen/       扫描源码生成类型 id 表
tools/qlfontgen/   从 TTF 生成内置位图字体
tools/qlui/        把 HTML+CSS 编译成 C（相当于 Qt 的 uic）
tools/qlblob/      把生成的 doc 导出成 .qlui blob（宿主工具）
test/object/       对象系统示例与测试
test/widget/       绘制系统示例与测试
test/text/         字体/文本示例与测试
test/web/          HTML+CSS 编译示例与测试
test/input/        交互（命中/悬停/点击/焦点）示例与测试
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

- 对象系统：[QLObject 阅读指南](doc/手册/object/QLObject阅读指南.md) / [QLObject API 手册](doc/手册/object/QLObject%20API手册.md)
- 绘制与控件：[QLWidget 阅读指南](doc/手册/widget/QLWidget阅读指南.md) / [QLWidget API 手册](doc/手册/widget/QLWidget%20API手册.md)
- 交互与输入：[QLInput 阅读指南](doc/手册/input/QLInput阅读指南.md) / [QLInput API 手册](doc/手册/input/QLInput%20API手册.md)
- 字体与文本：[QLText 阅读指南](doc/手册/text/QLText阅读指南.md) / [QLText API 手册](doc/手册/text/QLText%20API手册.md)
- HTML+CSS 编译：[QLUI 阅读指南](doc/手册/ui/QLUI阅读指南.md) / [QLUI API 手册](doc/手册/ui/QLUI%20API手册.md)
- 动效：[QLAnim 阅读指南](doc/手册/anim/QLAnim阅读指南.md) / [QLAnim API 手册](doc/手册/anim/QLAnim%20API手册.md)
- 渲染后端：[QLRender 阅读指南](doc/手册/render/QLRender阅读指南.md) / [QLRender API 手册](doc/手册/render/QLRender%20API手册.md)
- Web 兼容策略：[Web 兼容设计](doc/Web兼容设计.md)
- 开发规划（里程碑与验收）：[开发规划](doc/开发规划.md)
- 性能基线：[bench.md](doc/bench.md)
- 想看整体设计：[底层实现设计](doc/底层实现设计.md)
- 编码规范：[代码规范](doc/代码规范.txt)

## 许可

见 [LICENSE](LICENSE)。
