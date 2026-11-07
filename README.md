# QLGL 轻灵图形库（QingLing Graphics Library）

用 C11 实现的轻量 GUI 图形库，目标是让界面用 HTML + CSS 描述（构建期预编译成低开销二进制），
运行期只做"按 id 取已算好的值"，可裁到 MCU，也可跑在 MPU/PC 上。

当前进度：

- **对象系统（object）已完成** —— 类型、对象生命周期、属性、事件、内存与对象树；
- **绘制系统 + 控件基类（widget）已完成** —— 显示驱动、画布原语、脏矩形渲染、QLWidget 及自定义控件；
- **字体/文本系统已完成** —— 内置 4bpp 抗锯齿位图字体、UTF-8 解码、文本度量、`QLLabel` 控件；
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
./build/example_window                    # 打开真实窗口（自动选 Wayland 或 X11）
./build/example_window --backend=x11      # 强制 X11；--backend=wayland 强制 Wayland
./build/example_window --frames 120       # 跑 120 帧后自动退出
```

依赖：CMake ≥ 3.16、C11 编译器。

| 可选后端 | 依赖（Debian/Ubuntu） | 关闭选项 |
|---|---|---|
| X11 | `libx11-dev` | `-DQLGL_BUILD_X11=OFF` |
| Wayland | `libwayland-dev wayland-protocols`（`wayland-scanner`） | `-DQLGL_BUILD_WAYLAND=OFF` |

两者都没有时仍可正常构建，程序自动降级到内存显示（可导出 PPM）。
`xdg-shell.xml` 已随仓库提供，不需要系统安装 wayland-protocols 的头文件。

要求：CMake ≥ 3.16、支持 C11 的编译器（构建使用 `-std=c11`，不依赖 POSIX 扩展）。

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
  qlfont.h         位图字体、字形查找、UTF-8、文本度量
  qlstr.h          自管理字符串（compact / amortized 双策略）
  qllabel.h        文本控件
  qldisp_x11.h     Linux X11 窗口后端（可选）
  qldisp_wayland.h Linux Wayland 窗口后端（可选）
  qltype_gen.h     自动生成的类型 id（勿手改）
src/core/          对象系统实现
src/draw/          几何、显示、画布、渲染器
src/widget/        控件基类与 QLLabel
src/text/          字体实现与自动生成的字形数据
src/port/          平台后端（X11 / Wayland 窗口）
src/generated/     自动生成的类注册表（勿手改）
tools/qlgen/       扫描源码生成类型 id 表
tools/qlfontgen/   从 TTF 生成内置位图字体
test/object/       对象系统示例与测试
test/widget/       绘制系统示例与测试
test/text/         字体/文本示例与测试
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
- 字体与文本：[QLText 阅读指南](doc/手册/text/QLText阅读指南.md) / [QLText API 手册](doc/手册/text/QLText%20API手册.md)
- 想看整体设计：[底层实现设计](doc/底层实现设计.md)
- 编码规范：[代码规范](doc/代码规范.txt)

## 许可

见 [LICENSE](LICENSE)。
