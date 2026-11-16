# third_party

本目录只放**静态编进库**的第三方源码；不参与 MCU 构建（`QLGL_BUILD_SVG=OFF` 时
这些文件一行都不编译）。

| 目录 | 上游 | 版本 | 许可 | 用途 |
|---|---|---|---|---|
| `plutovg/` | https://github.com/sammycage/plutovg | 1.3.3 | MIT（`plutovg/LICENSE`） | 2D 路径/填充/渐变/光栅化 |
| `plutosvg/` | https://github.com/sammycage/plutosvg | 0.0.8 | MIT（`plutosvg/LICENSE`） | SVG XML → PlutoVG 绘制 |
| `thorvg/` | https://github.com/thorvg/thorvg | 1.2.0（去掉 GPU 引擎/JS expressions） | MIT（`thorvg/LICENSE`） | 可选 SVG 后端（CPU 引擎 + 全量 loader + C API） |

## 引入方式

- 只保留上游的 `source/` 与 `include/plutovg.h`，去掉示例/构建脚本；
- 由顶层 `CMakeLists.txt` 编成 `qlgl_plutosvg` 静态库，主库只调用
  `plutosvg_document_*` / `plutovg_surface_*`；
- 版本号来自上游头文件（`PLUTOVG_VERSION_*` / `PLUTOSVG_VERSION_*`）。

## ThorVG 子集说明

`third_party/thorvg/` 只保留：

```
inc/thorvg.h
src/common/  src/renderer/  src/renderer/cpu_engine/
src/modules/{raw,svg,png/static,jpg/static,webp/static,gif/load,gif/save,
             sfnt,lottie,media}
src/bindings/capi/
```

去掉了 GPU 引擎（gl/wg）、`lottie/jerryscript`（JS expressions，5MB+）以及
`*/external` 包装（它们要求系统 libpng/libjpeg/libwebp；我们用 ThorVG 自带的
static 解码器）。上游是 Meson-only 工程，这里由顶层 CMake 直接编译，并在构建
目录生成 `config.h`（全部 loader 的 `THORVG_*_SUPPORT`）。升级时覆盖这些目录，
顶层用 glob 自动纳入新增源文件。

## 升级步骤

1. 用上游新版本的 `source/` 覆盖对应目录（`plutosvg` 需要 `plutosvg-ft.h`，
   即使不开 FreeType 也保留）；
2. 更新本文件的版本号；
3. 重跑 `ctest -R "test_image_svg|test_qlsvg|test_qlsvgprobe"`；
   如果改了 ThorVG，再跑一次 `-DQLGL_BUILD_THORVG=ON` 的构建；
4. 看一眼 `example/46_svg_icon --dump` 的截图（渐变/抗锯齿变化是预期内的）。

## 注意

- `PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD=1`：不渲染 `<text>`，也不从文件/目录加载字体；
- 符号是普通的全局符号：如果宿主程序自己也静态链接了 PlutoSVG，可能重复定义。
  需要共存时把 `qlgl_plutosvg` 的符号做 hidden，或改用动态库。
