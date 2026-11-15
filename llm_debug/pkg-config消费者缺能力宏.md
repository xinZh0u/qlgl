# pkg-config 消费者缺能力宏：QL_FEAT_CSS_FONT_STACK 默认 1 撞上 QLGL_ENABLE_FREETYPE 未导出

> **核对 2026-11-15**：✅ 已修 —— `CMakeLists.txt:643-665` 从 `INTERFACE_COMPILE_DEFINITIONS` 生成 `Cflags`

## 现象

库安装后，非 CMake 工程按 README 承诺的 `pkg-config --cflags --libs qlgl` 方式使用，
只要 include 了伞头文件 `qlgl/qlgl.h` 就编译失败：

```
include/qlgl/qlfont_ft.h:26:2: error: #error "QL_FEAT_CSS_FONT_STACK=1 需要 QLGL_ENABLE_FREETYPE=1（文本栈要用 FreeType 读字体文件）"
```

## 复现步骤

```bash
cmake -S qlgl -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/tmp/prefix
cmake --build build -j && cmake --install build

export PKG_CONFIG_PATH=/tmp/prefix/lib/pkgconfig
printf '#include <qlgl/qlgl.h>\nint main(void){return 0;}\n' > main.c
gcc -std=c11 $(pkg-config --cflags qlgl) main.c $(pkg-config --libs qlgl) -o hello
# ← 失败：#error QL_FEAT_CSS_FONT_STACK=1 需要 QLGL_ENABLE_FREETYPE=1
```

## 根因

三方因素叠加：

1. **`QL_FEAT_CSS_FONT_STACK` 默认是 1**（`include/qlgl/qlconfig.h:130`）：
   ```c
   #ifndef QL_FEAT_CSS_FONT_STACK
   #define QL_FEAT_CSS_FONT_STACK      1
   ```
   消费者不定义它时，头文件里按 1 处理。

2. **`qlfont_ft.h:25` 的编译期守卫**：
   ```c
   #if QL_FEAT_CSS_FONT_STACK && !defined(QLGL_ENABLE_FREETYPE)
   #error "QL_FEAT_CSS_FONT_STACK=1 需要 QLGL_ENABLE_FREETYPE=1（...）"
   #endif
   ```
   这个守卫的本意是"库没编 FreeType 时，消费者也别用文本栈"。

3. **`qlgl.pc` 的 `Cflags:` 只有 `-I${includedir}`，没有任何 `-D`**
   （`CMakeLists.txt` 第 619 行附近生成）。库构建时 `QLGL_ENABLE_FREETYPE=1`
   是 `target_compile_definitions(qlgl PUBLIC ...)`（第 108 行），
   CMake 导出路径会把它写进 `INTERFACE_COMPILE_DEFINITIONS`，
   但 `.pc` 文件**完全没有**把这类 PUBLIC 宏带出去。

于是：库是带 FreeType 编的（宏=1），`.pc` 却不告诉消费者这个宏，
消费者侧 `QL_FEAT_CSS_FONT_STACK` 落到默认 1 + `QLGL_ENABLE_FREETYPE` 未定义
→ 守卫触发 `#error`。

## 验证

- `pkg-config --cflags qlgl` 输出里 `-D` 计数为 0。
- 手动补 `-DQLGL_ENABLE_FREETYPE=1` 后同一命令编译链接全部通过
  → 证明唯一缺口就是宏，头文件/链接都没问题。
- 对照：CMake 导出路径（`qlglTargets.cmake`）的
  `INTERFACE_COMPILE_DEFINITIONS` 里**有** `QLGL_ENABLE_FREETYPE=1`，
  所以 find_package 消费者不会踩这个 `#error`（但会踩 include 目录丢失，
  见《安装后find_package找不到头文件.md》）。两条消费路径各坏一半。

## 影响面

- 所有用 pkg-config 消费**带 FreeType 构建**的第三方工程（README 明确推荐的
  非 CMake 用法）。
- 反向组合（库关 FreeType、消费者默认宏=1）同样触发 `#error`——
  这个至少是"构建期报错"符合设计红线，但报错信息对消费者不友好：
  消费者无法从 `.pc` 得知该定义 `QL_FEAT_CSS_FONT_STACK=0`。

## 修复建议

二选一（或都做）：

1. **`.pc` 导出能力宏**：生成 `qlgl.pc` 时把 `QLGL_ENABLE_FREETYPE`、
   `QLGL_ENABLE_GL`、`QLGL_ENABLE_X11`、`QLGL_ENABLE_WAYLAND`、
   `QL_FEAT_IMG_*` 等 PUBLIC 宏写进 `Cflags:`（与 `INTERFACE_COMPILE_DEFINITIONS`
   保持同一来源，避免两处漂移）。
2. **守卫改软**：`qlfont_ft.h` 的 `#error` 只在
   `QL_FEAT_CSS_FONT_STACK` 被**显式**定义为 1 时触发，
   默认值场景改为 `#warning` 或直接按 0 处理——
   因为"默认 1"是头文件侧的猜测，不是消费者的承诺。

方案 1 与 CMake 导出路径语义一致，推荐优先。
