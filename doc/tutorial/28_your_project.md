# 在自己的工程里用 QLGL

前面 27 章都在讲"怎么写界面"。这一章讲**怎么把它接到你的工程里**：

> qlgl 装出来只有三样东西 —— **一个静态库、一组头文件、一份 CMake 包配置**。
> 没有守护进程、没有构建系统侵入、没有"必须用我们的工程骨架"。

---

## 1. 先装进系统

```bash
cd qlgl
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build --prefix /usr/local
```

不给 `--prefix` 的话默认也是 `/usr/local`。**装到别处完全合法**（不需要 sudo）：

```bash
cmake --install build --prefix $HOME/opt/qlgl
```

装完之后是这样：

| 路径 | 内容 |
|---|---|
| `<prefix>/include/qlgl/*.h` | 全部公开头文件 |
| `<prefix>/lib/libqlgl.a` | 静态库 |
| `<prefix>/lib/cmake/qlgl/*.cmake` | `find_package(qlgl)` 用的包配置 |
| `<prefix>/lib/pkgconfig/qlgl.pc` | `pkg-config` 用的元数据 |
| `<prefix>/bin/qltext` | 构建期工具（HTML/CSS → C，**目标机不需要**） |

> `qltext` 只在你自己构建界面时用得到。发行一个已经用构建期编译的产品时，
> 这个东西可以不装。

## 2. 三种接法，任选一种

### 2.1 CMake（推荐）

```cmake
cmake_minimum_required(VERSION 3.16)
project(helloworld C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(qlgl REQUIRED)

add_executable(helloworld main.c)
target_link_libraries(helloworld PRIVATE qlgl::qlgl)
```

就这么三行。`qlgl::qlgl` 这个 target 已经带上了：

- include 目录；
- **能力开关**（`QL_FEAT_*` / `QLGL_ENABLE_*`）—— 这点很关键，见 §6.1；
- 传递依赖（FreeType / JPEG / EGL / GLESv2 / X11 / Wayland）。

**装到了非默认前缀**时，配置时告诉 CMake 去哪找：

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/opt/qlgl
# 或者一次性写进环境：export CMAKE_PREFIX_PATH=$HOME/opt/qlgl
```

### 2.2 pkg-config（非 CMake 工程）

```bash
cc main.c $(pkg-config --cflags --libs qlgl) -o helloworld
```

装到非默认前缀时：

```bash
export PKG_CONFIG_PATH=$HOME/opt/qlgl/lib/pkgconfig
```

`qlgl.pc` 里的 `Libs` 按"这一份构建到底链了谁"生成，所以能力开关决定的依赖
（比如 Wayland GPU 路径的 `-lwayland-egl`）会自动出现。

### 2.3 手工链接（最不推荐，但可行）

```bash
cc -std=c11 main.c \
   -I<prefix>/include \
   -L<prefix>/lib -lqlgl \
   -lfreetype -ljpeg -lEGL -lGLESv2 -lX11 -lXext \
   -lwayland-client -lwayland-egl -lm -ldl
```

⚠️ 这一种**最容易出错**：你必须自己写对与本份构建一致的 `-D` 能力开关。
`qlui.h` 里的结构体布局受这些宏影响，任何不一致都会在运行期变成"字段读错位"。
能走 2.1 / 2.2 就不要走这条。

## 3. 最小可用程序

一个 QLGL 程序永远是四步：

```text
qlgl_init() → 拿到 doc（编译好的界面）→ ql_ui_build() → ql_ui_layout()
                                              ↓
                                    建窗口 → 帧循环 → 释放
```

完整代码（就是 `test/helloworld/main.c` 的骨架，这里省掉读文件/找字体的细节）：

```c
#include <qlgl/qlgl.h>       /* 伞头文件：包含下面所有 */
#include <qlgl/qldisp.h>     /* 离屏渲染用 */
#include <qlgl/qluitext.h>   /* ql_ui_load_text：运行期编译 */

#include <locale.h>

QLUIDoc *doc = ql_ui_load_text(html, css);      /* 运行期编译一次 */
if (doc == NULL) {
    fprintf(stderr, "界面编译失败：%s\n", ql_ui_text_last_error());
    return 1;
}
QLUIRoot *ui = ql_ui_build(doc);

QLGfxGL *gfx = ql_gfx_gl_create("Hello", W, H, QL_GFX_GL_WINDOW);
ql_ui_layout(ui, W, H);
ql_ui_attach_input(ui);              /* 少这一句：能滚、点不动、打不了字 */
ql_gfx_gl_attach_clipboard(gfx, ui); /* Ctrl+C / Ctrl+V */
ql_gfx_gl_attach_ime(gfx, ui);       /* 中文输入法组字 */
(void)ql_gfx_gl_set_retained(gfx, 1);/* 保留式画布：静止时一个像素都不重画 */

QLDraw2D *d2d = ql_gfx_gl_draw(gfx);
while (ql_gfx_gl_poll(gfx)) {
    if (ql_gfx_gl_resize_pending(gfx)) {        /* 窗口被拖大：重排 */
        ql_coord_t cw = W, ch = H;
        ql_gfx_gl_size(gfx, &cw, &ch);
        ql_ui_layout(ui, cw, ch);
    }
    ql_input_dispatch();                        /* 队列 → 界面层 */

    const uint32_t moving = ql_ui_anim_tick(ui, ql_anim_now_ms());
    QLRect areas[16];
    const int n = ql_ui_consume_dirty(ui, areas, 16);
    if (n == 0 && moving == 0) {                /* 静止：阻塞等事件，不空转 */
        if (!ql_gfx_gl_wait(gfx, 32))
            break;
        continue;
    }
    if (ql_gfx_gl_begin_rects(gfx, clear, areas, n))
        ql_ui_render(ui, d2d);                  /* 必须整屏 */
    else
        for (int i = 0; i < n; i++)
            ql_ui_render_rect(ui, d2d, areas[i]);
    ql_gfx_gl_end(gfx);
}

ql_ui_detach_input();
ql_gfx_gl_destroy(gfx);
ql_ui_free(ui);
ql_ui_doc_free(doc);        /* 顺序：先 ui，后 doc */
qlgl_shutdown();
```

### 每一句为什么不能省

| 语句 | 漏掉的后果 |
|---|---|
| `qlgl_init()` | 内置类/属性表没注册，后续调用行为未定义 |
| `setlocale(LC_ALL, "")` | XIM 挑不到输入法 → **中文打不进来**（这是应用的事，库代劳不了） |
| `ql_ui_attach_input(ui)` | 窗口能开、能滚，**点击和键盘全无反应** |
| `ql_gfx_gl_attach_ime(gfx, ui)` | 输入框里看不到组字中的拼音/汉字 |
| `ql_ui_anim_tick()` | `transition` / `@keyframes` 不动 |
| 静止时 `ql_gfx_gl_wait()` | 主循环空转，CPU 100% |
| `ql_ui_free()` 在 `ql_ui_doc_free()` 之前 | 悬垂引用（`ql_ui_build` 不接管 doc） |

> 想直接要一份能跑的完整代码：`test/helloworld/`（CMakeLists + main.c + index.html
> + style.css，四个文件，全部公开 API）。

## 4. 界面从哪来：两选一

| 方式 | 代码 | 适用 |
|---|---|---|
| **运行期编译** | `ql_ui_load_text(html, css)` | 界面会变、插件式界面、开发期热改 |
| **构建期编译** | `qltext` 生成 `.c`，`ql_ui_build(&ui_x_doc)` | 正式产品：数据进 `.rodata`，加载成本为 0 |

两者产出的数据**逐字节相同**，渲染开销也一样。构建期那条在 CMake 里长这样：

```cmake
add_custom_command(
    OUTPUT ${OUT}/ui_app.c ${OUT}/ui_app.h
    COMMAND qltext cc --html ui/app.html --css ui/app.css --name app --out ${OUT}
    DEPENDS ui/app.html ui/app.css)
```

也可以两者兼得：把运行期编译的结果存成 `.qlui`，下次 `ql_ui_load_file(path)` 直接映射
（改界面不用重编译）。

### 界面文件怎么随程序走

qlgl **不知道**你的 `.html/.css` 在哪 —— 它只接收字符串。所以"资源路径"是应用的策略。
两种常见做法：

```cmake
# ① 把源码目录编进去（开发期最省事；从任何 cwd 启动都找得到）
target_compile_definitions(app PRIVATE UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui")

# ② 装到标准位置（发行版友好）
install(FILES ui/app.html ui/app.css DESTINATION share/app)
```

## 5. 没有显示器 / 在 CI 里怎么验

**离屏渲染**不需要窗口，也不需要 GL：

```c
ql_ui_layout(ui, W, H);
QLDispDriver *disp = ql_disp_mem_create(W, H, QL_COLOR_RGB888);
QLCanvas canvas;
ql_canvas_from_disp(&canvas, disp);
ql_canvas_clear(&canvas, clear_color(ui));   /* = 根元素背景色，否则露黑边 */
ql_ui_render_canvas(ui, &canvas);
ql_disp_mem_save_ppm(disp, "out.ppm");
```

值得做成程序的一个开关（`--dump out.ppm`），CI 里就能直接跑像素比对。
要 PNG 的话，`ql_disp` 只有 PPM，转一下即可（`convert` / PIL）。

## 6. 三个容易踩的坑（都是"外部工程才会遇到"）

### 6.1 能力开关必须和库一致

`QLUIStyle` 等结构体的**布局**受 `QL_FEAT_*` 影响。库按"开 `QL_FEAT_CSS_BLUR=1`"
编译，而你的程序按"关"编译 → 同一份头文件在两个翻译单元里尺寸不同 → 运行期读到
错位的字段，症状是"颜色/圆角/文字莫名奇妙"。

**所以：能力开关不随 `.pc` / `Config.cmake` 导出成字符串，而是通过
`target_compile_definitions(qlgl PUBLIC ...)` 传递到链 `qlgl::qlgl` 的目标上。**
用 2.1 / 2.2 就自动正确；只有 2.3 手工链接需要你自己抄。

### 6.2 头文件要按需包含

伞头文件 `<qlgl/qlgl.h>` 一次包含全部，最省事。单独包含某个头文件也可以
（每个头文件都是自包含的），但**不要在伞头文件之后重复 include 子头文件**去"补定义"，
那只会掩盖真实的顺序问题。

### 6.3 构建期报错是设计，不是 bug

界面里写了库不支持的 CSS，会在 `ql_ui_load_text()` 处返回 NULL，
`ql_ui_text_last_error()` 给出行号与原因：

```text
界面编译失败：<div #1>: 不支持的渐变方向 '160deg'（只支持水平/垂直：
to bottom/top/right/left、180deg/90deg）
```

这是"不静默降级"原则。别去猜为什么没生效 —— 错误信息就是答案。
边界清单见 [与 Web 的差别](10_vs_web.html)、[CSS3 覆盖与裁剪](../manual/ui/CSS3覆盖与裁剪.html)。

## 7. 上手检查清单

- [ ] `cmake --build build -j` 通过，`sudo cmake --install build --prefix /usr/local`
- [ ] 自己的工程里 `find_package(qlgl REQUIRED)` 能找到（装到非默认前缀记得 `CMAKE_PREFIX_PATH`）
- [ ] `target_link_libraries(app PRIVATE qlgl::qlgl)`，**没有**手写任何 `QL_FEAT_*`
- [ ] `main()` 里有 `qlgl_init()` + `setlocale`，退出前 `qlgl_shutdown()`
- [ ] 窗口后立刻 `ql_ui_attach_input(ui)`、`ql_gfx_gl_attach_ime(gfx, ui)`
- [ ] 静止时走 `ql_gfx_gl_wait()`（别空转）
- [ ] `ql_ui_free(ui)` 在 `ql_ui_doc_free(doc)` 之前
- [ ] 有 `--dump` 之类的离屏开关，便于 CI 与无显示器环境验证

---

下一章：[与 Web 的差别](10_vs_web.html) · 相关：[性能](17_perf.html)、[基础知识](01_basics.html)
