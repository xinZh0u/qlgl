# SVG 支持（默认运行期 ThorVG + 备用 PlutoSVG + 构建期 qlsvg）

> 状态：**已实现（2026-11-16）**。目标读者：要接 SVG 图标、改 `qltext` 资源管线、
> 或评估"哪些 SVG 画不出来"的人。
> 相关：[图像管线设计](./图像管线设计.md)、[第三方依赖](./第三方依赖.md)、
> [HTML5 标签覆盖与裁剪](./手册/ui/HTML5标签覆盖与裁剪.md)。

核心判断：

> **图标是"资源"，不是"文档"。** PC/Mobile 默认在运行期用 ThorVG 解析
> （PlutoSVG 是可选的纯 C 备用后端）；MCU 不链 SVG 库，用 PlutoSVG 写成的
> 宿主工具 `qlsvg` 在构建期转成 PNG/A8。

---

## 0. 结论摘要

| 问题 | 决定 |
|---|---|
| PC / Mobile 怎么解 SVG | **运行期**：`ql_image_decode()` 识别 `.svg`；默认 **ThorVG** 渲染，PlutoSVG 作为备用后端 |
| MCU 怎么用 SVG | **构建期**：`tools/qlsvg` 转 PNG（`--alpha` 出覆盖率遮罩），再交给 `qltext --root` |
| 渲染器有几份 | **默认 ThorVG**（C++，全量 loader）；**PlutoSVG** 是纯 C 备用后端（`-DQLGL_BUILD_SVG=ON`）。`qlsvg` 构建期工具固定用 PlutoSVG |
| 关掉 SVG 会怎样 | `QLGL_BUILD_SVG=OFF` → `.svg` 明确返回 `QL_ERR_UNSUPPORTED`，其余格式不受影响 |
| 单色图标染色 | 运行期 `mask-image: url(x.svg)`（取 alpha）；MCU 用 `qlsvg --alpha` + `qltext -a8` |
| 文本 / 内嵌图片 | PlutoSVG：内嵌 png/jpeg；ThorVG 全量：内嵌 png/jpeg/webp/svg+xml，`<text>` 需 `ql_image_svg_load_font()` |
| 内联 `<svg>` | **不做**，继续"占位盒 + 内容跳过"；要图标请用 `<img src="x.svg">` |
| SVG 动画 | **不做**（PlutoSVG 不支持 SMIL/CSS 动画）；动效做在 QLUI 层（对位图做 transform/opacity） |
| `clip-path: path()` | **已接**（PlutoVG 解析 + 0.75px 展平 + 现有覆盖率）；单条子路径、≤64 顶点 |
| 怎么对比后端 | 默认就是 ThorVG；`-DQLGL_BUILD_SVG=ON` 加编 PlutoSVG，运行期 `ql_image_svg_set_backend("plutosvg")`；`qlsvgprobe --compare` 逐文件并排 |

---

## 1. 能力边界（很重要）

> 下表是 **PlutoSVG 备用后端**的边界。默认 ThorVG 还支持
> `clipPath` / `mask` / `pattern` / 基础 `filter` / `<style>`、内嵌
> png/jpeg/webp、以及加载字体后的 `<text>`（见 §4.1）。两者都**不做** SMIL。

PlutoSVG 是"图标级静态 SVG"渲染器，不是完整 SVG 1.1：

| 支持 | 说明 |
|---|---|
| `svg` / `g` / `defs` / `use` / `symbol` | 结构、引用、复用 |
| `path` / `rect` / `circle` / `ellipse` / `line` / `polyline` / `polygon` | 基本形状 |
| `linearGradient` / `radialGradient` / `solidColor` | 渐变与纯色 |
| `transform` / `viewBox` / `preserveAspectRatio` / `fill-rule` / `opacity` | 常用属性 |
| 内联 `style=""` | 常用项可用 |

| 不支持 | 说明 |
|---|---|
| `clipPath` / `mask` / `pattern` / `marker` | PlutoSVG 当前版本没有实现（`TAG_CLIP_PATH` 仍是 TODO） |
| 滤镜（`feGaussianBlur` 等） | 不做 |
| `<style>` 样式表 / class 选择器 / CSS 变量（可传 palette 回调，但我们没接） | 不保证 |
| `<text>` / `<tspan>` | 没开 FreeType，不渲染（图标请把文字转曲） |
| `<image>` 的外链、`@font-face`、外部 `<use>` | 拒绝 |
| SMIL / CSS 动画 | **完全不支持** |

**结论**：PlutoSVG 覆盖单色图标、logo、渐变、`<use>`；ThorVG 额外覆盖
遮罩/裁剪/pattern/部分滤镜/内嵌图片，复杂插画仍然建议构建期导出 PNG。

---

## 2. 运行期解码（PC / Mobile）

### 2.1 入口

不新增公开解码 API：走现有 `ql_image_decode()`。

```c
QLImageDecodeReq req;
memset(&req, 0, sizeof(req));
req.data = svg_bytes;
req.len  = svg_len;
req.src_format = QL_IMAGE_SRC_AUTO;   /* 按 `<svg` / `<?xml ... <svg` 探测 */
req.out_format = QL_IMAGE_RGBA8888;   /* 或 RGB565 / A8 */
req.target_w = 64;                    /* 0 = 用 SVG 固有尺寸 */
req.target_h = 64;                    /* 只给一边按比例补另一边 */

QLImage img;
if (ql_image_decode(&req, &img) == QL_OK) {
    /* img.px 是非预乘像素；owned = 1，用完 ql_mem_free(img.px) */
}
```

- `QL_IMAGE_SRC_SVG` 也可以显式指定；
- 显式给 `target_w/target_h` 时按精确尺寸渲染；两个都 0 时用
  `width/height`，没有就 `viewBox`，再没有按规范兜底 300×150；
- 输出尺寸单边夹到 `QL_CFG_IMAGE_SVG_MAX_DIM`（默认 2048；超限时**按比例**缩，
  不改变长宽比），surface 按解码预算检查；
- 峰值内存 ≈ `surface（4 B/px）+ 输出像素`：A8 输出很小，RGBA 会多一份。
  大图请自觉给 `target_w/h` 或调 `ql_image_set_decode_budget()`。

### 2.2 与 QLUI 的配合

`<img src="logo.svg">` 的 `src` 只是**资源名**。应用读文件后按名字注册：

```c
QLUIImageInput in;
memset(&in, 0, sizeof(in));
in.name   = "logo.svg";
in.data   = bytes;
in.len    = len;
in.format = QL_IMAGE_SRC_AUTO;
ql_ui_register_image(ui, &in);      /* 解码一次；之后每帧只搬像素 */
```

`background-image: url(x.svg)` / `mask-image: url(x.svg)` 与 `<img>` 共用同一张
资源表：`ql_ui_build()` 会为样式里引用的名字也分配资源槽（`node == NULL`），
所以**只有 CSS 引用、没有 `<img>`** 时同样可以 `ql_ui_register_image(ui, "x.svg")`。
`example/46_svg_icon` 里 `star.svg` 就是只通过 `mask-image` 引用的。

### 2.3 A8 与染色

SVG → `QL_IMAGE_A8` 取的是**覆盖率（alpha）**，不是黑色图形的亮度：

- `fill="#000"` + 透明背景的图标 → 形状处 255、外部 0，正好当遮罩；
- 运行期 `mask-image: url(star.svg)` 读图片 alpha，颜色由 `background-color` 决定；
- 不需要为每种主题色重新导出资源。

SVG 里直接写 `fill="currentColor"` / `stroke="currentColor"` 也支持：
PlutoSVG 把元素自己的 `color` 传给 `plutosvg_document_render_to_surface` 的
`current_color` 参数；ThorVG 则在加载前把 `currentColor` 替换成具体颜色。`<img>` 用自己的 `color`；`background-image` / `mask-image`
这种按名字共享的资源用第一个使用者的 `color`。颜色变化会触发一次重栅格化
（同一个资源名被多个 `<img>` 使用时，每个 `<img>` 有独立光栅，互不影响）。

### 2.4 尺寸与 DPR：自动重栅格化

运行期注册的 SVG 会**保留原文**（≤ `QL_CFG_IMAGE_SVG_MAX_SOURCE`，默认 1 MiB），
第一次真正绘制前按"元素盒子 × DPR"栅格化，而不是按固有 24×24 直接放大：

- `<img>` 按 `object-fit` 之后的实际绘制尺寸算（`contain` 不会为宽盒子白占像素）；
- `background-image` / `mask-image` 按元素的边框盒（CSS 里它们本来就拉伸到盒子）；
- 目标尺寸保持 SVG 固有宽高比，单边上限 `QL_CFG_IMAGE_SVG_MAX_DIM`；
- 尺寸变化超过 **±25%** 才重新栅格化，避免动画/拖动窗口时每帧重来；
- DPR 从画布（`ql_canvas_scale_fx`）取，所以 1x/2x/3x 屏都会自动变清晰；
- **布局与 `object-fit` 仍用固有 CSS 尺寸**（`QLImage` 的光栅尺寸只影响采样）。

限制：构建期嵌入的 RAW 位图没有源，无法重栅格化；`border-image` 的切片依赖
源像素，名字被 `border-image` 引用时不会自动重栅格化（避免切片错位）。

---

## 3. 构建期工具 `qlsvg`（MCU / 预渲染）

```bash
# 普通 PNG（保留颜色/透明度）
qlsvg icons/logo.svg icons/logo.png

# 指定尺寸（只给一边按比例）
qlsvg icons/logo.svg icons/logo.png -w 64 -h 64

# 单色遮罩：白 = 不透明，黑 = 透明
qlsvg icons/star.svg icons/star.png --alpha
```

然后走现有资源管线：

```bash
qltext cc --html app.html --css app.css --root icons \
          --pixel-format a8          # 或 rgb565 / rgba8888
```

- `--alpha` 输出的是**灰度图**（RGB=覆盖率），这样 `qltext -a8` 按亮度取也能得到遮罩；
  直接把透明背景 + 黑色图形交给 `qltext -a8` 会得到全 0（黑色亮度为 0）。
- qlsvg 固定用 PlutoSVG/PlutoVG；与**备用后端**渲染一致，默认 ThorVG 可能有
  细微抗锯齿/特性差异（可用 `qlsvgprobe --compare` 看）。
- 产物是普通 PNG，可以提交进版本库；目标机不需要 `qlsvg`。
- 顺带：如果**宿主**的 `qltext` 是在带 SVG 后端的构建里编出来的（桌面默认
  ThorVG），`qltext cc --root` 也能直接把 `.svg` 解成目标像素格式嵌入；
  两边后端都关掉时才必须先用 `qlsvg` 预渲染。

---

## 4. CMake 与裁剪

```cmake
option(QLGL_BUILD_THORVG "默认 SVG 后端（ThorVG，C++）" ON)   # 桌面默认
option(QLGL_BUILD_SVG    "备用 SVG 后端（PlutoSVG，纯 C）" OFF)
option(QLGL_BUILD_CLIP_PATH "clip-path: path()（PlutoVG）" ON)
```

| 能力 | 宏 | 默认 | 关掉的后果 |
|---|---|---|---|
| 运行期 SVG | `QL_FEAT_IMG_SVG` | 任一后端打开即为 1（桌面默认 ThorVG） | `.svg` → `QL_ERR_UNSUPPORTED` |
| 宿主工具 | `QLGL_BUILD_QLSVG` | 非交叉编译时 ON（**与后端开关独立**） | 不产出 `qlsvg`；MCU 仍可手写/提交预渲染 PNG |
| ThorVG 后端 | `QLGL_BUILD_THORVG` | **ON**（桌面默认；raw-only/交叉编译 OFF） | 退回 PlutoSVG 或 UNSUPPORTED |
| PlutoSVG 备用后端 | `QLGL_BUILD_SVG` | OFF（需要时显式打开） | 只有 ThorVG 一个后端 |
| `clip-path: path()` | `QLGL_BUILD_CLIP_PATH` | ON（只依赖 PlutoVG） | `path()` 不可用 |
| 后端宏 | `QL_FEAT_IMG_SVG_PLUTOSVG` / `QL_FEAT_IMG_SVG_THORVG` | 由上面开关决定 | 关掉的后端调用 `ql_image_svg_set_backend()` 返回 `UNSUPPORTED` |
| `clip-path` 路径解析 | `QL_FEAT_CSS_CLIP_PATH_FILE` | 由 `QLGL_BUILD_CLIP_PATH` 决定（复用 PlutoVG） | `path()` 明确报错 |

容量上限（`qlconfig.h`）：

| 宏 | 默认 | 含义 |
|---|---|---|
| `QL_CFG_IMAGE_SVG_MAX_DIM` | 2048 | 输出单边像素上限 |
| `QL_CFG_IMAGE_SVG_MAX_DEPTH` | 64 | 元素嵌套深度上限 |
| `QL_CFG_IMAGE_SVG_MAX_ELEMENTS` | 4096 | 元素总数上限 |
| `QL_CFG_IMAGE_MAX_ENCODED_BYTES` | 32 MiB | SVG 文本字节上限（与其它编码共用） |

两个后端都关掉时 `src/draw/qlimage_svg.c` 编译成空文件，ThorVG/PlutoSVG
不参与 `libqlgl` 编译；`QLGL_BUILD_CLIP_PATH=OFF` 再把 PlutoVG 也裁掉。
开发机上的 `qlsvg` **仍然默认构建**，所以"MCU 库裁掉 SVG"与"构建期还能把
SVG 转 PNG"不冲突。

### 4.1 后端选择与对比

```c
/* 默认 = ThorVG；下面切到 PlutoSVG 备用后端（需要 -DQLGL_BUILD_SVG=ON） */
ql_image_svg_set_backend("plutosvg");
printf("%s\n", ql_image_svg_backend_name());

/* 恢复默认（ThorVG） */
ql_image_svg_set_backend(NULL);
```

- 后端是**进程级**全局状态；切换后新的解码/重栅格化走新后端，已经解好的图不会自动重来；
- 两个后端都参与 `ql_image_decode()` 的同一套预检查/尺寸/预算/`out_format` 转换；
- ThorVG 构建编 **CPU 引擎 + 全量 loader**（SVG / Lottie / PNG / JPEG / WebP /
  GIF / SFNT(ttf,otf) / media）+ C API：
  `clipPath` / `mask` / `pattern` / 基础 `filter` / `<style>` 比 PlutoSVG 完整；
  `<image>` 支持内嵌 **png / jpeg / webp / svg+xml**（GIF 被 ThorVG 的 SVG
  mime 白名单拒绝，GIF loader 只用于直接加载图片）；
  `<text>` 需要先调 `ql_image_svg_load_font("DejaVuSans.ttf")` 之类的字体加载；
  Lottie loader 已编入，但本库目前的图片 API 仍只解 SVG；
- `qlsvg` 构建期工具始终用 PlutoSVG；与**备用后端**一致，和默认 ThorVG
  可能有抗锯齿/特性差异（用 `qlsvgprobe --compare` 可量化）。

逐文件对比：

```bash
./build/qlsvgprobe assets/ --compare
```

```
  icon.svg
    plutosvg  ok    64x64   0.12ms cov 97%
    thorvg    ok    64x64   0.17ms cov 97%
  complex.svg
    plutosvg  warn  64x64   0.10ms cov 100%
    thorvg    warn  64x64   0.27ms cov  45%
    plutosvg  不支持/风险: clipPath text clip-path 属性
    thorvg    不支持/风险: text
```

（ThorVG 应用了 clipPath，所以覆盖率从 100% 变成 45% —— 这才是资产原本的形状。）

---

## 5. 安全与有界

PlutoSVG 把 SVG 当 XML 元素树解析，不执行脚本、不展开 DTD 实体；
但我们仍然在外层加了一道预扫描：

| 风险 | 应对 |
|---|---|
| 脚本 / 事件 | 文本里出现 `<script`、`javascript:` 直接拒绝 |
| 外部引用 | `href` / `xlink:href` 只允许 `#fragment` 与 `data:`；`http`、`file://`、**相对路径**、`url(http…)`、`url(font.ttf)`、`@import` 一律拒绝（ThorVG 开了 FILE_IO，会真的读路径） |
| 实体膨胀（billion laughs） | PlutoSVG 只跳过 `DOCTYPE`，不展开自定义实体 |
| 深递归 / 元素炸弹 | 预扫描限制深度 ≤ 64、元素数 ≤ 4096 |
| 超大输出 | 目标尺寸夹到 2048；surface 按解码预算检查（4 B/px） |
| 文件本身过大 | `QL_CFG_IMAGE_MAX_ENCODED_BYTES` 先拒绝 |
| 字体 / 文件系统 | 不启用 FreeType；PlutoVG 的字体缓存加载被 `PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD` 关掉 |

> 运行期只接受**调用方给的字节**；本库不读路径、不访问网络。

---

## 6. 测试

| 用例 | 文件 | 断言 |
|---|---|---|
| 解码 / 尺寸 / A8 | `test/web/test_image_svg.c` | AUTO 探测、固有尺寸、target 只给一边、A8 覆盖率非零且不是全白、外链/畸形拒绝、预算超限 |
| 关掉 SVG | 同上（`QL_FEAT_IMG_SVG=0` 分支） | 明确 `QL_ERR_UNSUPPORTED`，`ql_image_last_error()` 有原因 |
| 工具链 | `test/assets/gen_svg_tool.cmake` | `qlsvg` 正常/`--alpha`/坏 SVG 失败；PNG 经 `qltext -a8` 后尺寸与覆盖率正确 |
| 例程 | `example/46_svg_icon` | 多色 `<img>` + 单色 `mask-image`，CPU/GL 截图一致 |
| 资产自检 | `test_qlsvgprobe` | 目录扫描：坏文件、外部引用 block、不支持特性 warn、`--strict`、`--compare` |
| ThorVG 后端 | `test_image_svg_thorvg` | 颜色/尺寸/A8/currentColor、内嵌 PNG/JPEG/WebP、字体 `<text>`；没有 ThorVG 的构建打印 SKIP |

不做逐像素 golden：抗锯齿细节由 PlutoVG 版本决定，跨版本可能微调。

### 6.1 资产兼容性自检：`qlsvgprobe`

PlutoSVG 对不支持的标签是**静默忽略**的（解码"成功"但画面缺一块），只看
`ql_image_decode()` 的返回值不够。`tools/qlsvgprobe` 同时做两件事：

1. **批量解码**（默认目标宽 64px、按固有宽高比）→ 记录成功/失败、尺寸、耗时、
   非透明覆盖率；全透明的成功结果标为 `warn`（大概是资源本身有问题）。
2. **静态特性扫描**：`clipPath` / `mask` / `pattern` / `marker` / `filter` / `fe*` /
   `text` / `tspan` / `<style>` / SMIL（`animate` / `set`）/ `foreignObject` /
   `switch` / `clip-path=` / `filter=` / `mask=` / 非内嵌 `<image>` / 外部 `href` /
   `javascript:` / `@import` / `url(http…)`。

```bash
./build/qlsvgprobe assets/                  # 递归扫描
./build/qlsvgprobe assets/ --size 96
./build/qlsvgprobe assets/ --backend thorvg # 指定后端（默认 auto）
./build/qlsvgprobe assets/ --compare        # 两个后端并排（需同时编入）
./build/qlsvgprobe assets/ --strict         # 有 warn 也返回 1（CI 门槛）
./build/qlsvgprobe assets/ --quiet          # 只打汇总与失败项
```

状态：`ok` / `warn`（能画但有已知不支持特性，或解码后全透明）/ `fail`（解码失败）/
`block`（外部引用或脚本，运行期会直接拒绝）。特性告警会按**当前后端**过滤：
ThorVG 已支持的 clipPath/mask/pattern/filter/style 不再报；内嵌
png/jpeg/webp 也按支持处理，`image/gif`（ThorVG 的 SVG mime 白名单不接受）
和 `<text>`（没加载字体时）仍会提示。默认退出码：有 `fail/block` 为 1；
`--strict` 时 `warn` 也算失败。汇总会按特性列出"这一批资产里踩了多少次"，
用来决定"哪些资产必须预渲染、要不要换后端"。

一个真实资产库的推荐流程：

```bash
./build/qlsvgprobe ~/icons --strict > icons.txt   # 先看失败/告警清单
# 复杂插画（clipPath/filter/text）走设计工具导出 PNG；图标继续用 SVG
```

---

## 7. 第三方代码与许可

`third_party/` 下 vendor 了两份 MIT 源码（版本与来源见 `third_party/README.md`）：

| 组件 | 版本 | 用途 |
|---|---|---|
| PlutoVG | 1.3.3 | 2D 路径/填充/渐变/光栅化（默认后端 + clip-path path） |
| PlutoSVG | 0.0.8 | SVG XML → PlutoVG 绘制（备用后端） |
| ThorVG | 1.2.0（CPU 引擎 + 全量 loader + C API） | **默认后端**；`QLGL_BUILD_THORVG=ON`（桌面默认） |

两者都是纯 C、无外部依赖；直接静态编进 `libqlgl`，不安装任何头文件。
升级步骤：替换 `third_party/` 源码 → 重跑 `test_image_svg` / `test_qlsvg` /
`example_46_svg_icon` 截图。

---

## 8. `clip-path: path()`（已完成）

`path("…")` 不再"明确报错"：`qlui_parse.c` 用 PlutoVG 解析路径，按 0.75px
误差把三次贝塞尔展平成多边形，交给现有的单多边形覆盖率遮罩（CPU / GPU 同公式）。

限制（构建期报错，不静默降级）：

- 只支持**一条子路径**（多个 `M` 会形成互相覆盖的轮廓，遮罩只有一层）；
- 坐标就是 CSS px（`path()` 不支持百分比）；
- 展平后顶点 ≤ `QL_CFG_UI_CLIP_PTS_MAX`（默认 64）；
- `url(#id)` 仍不支持（要 DOM/内联 SVG）；
- 需要 `QLGL_BUILD_CLIP_PATH=ON`（复用 vendored PlutoVG），关掉时明确报错。

## 9. 还没做（按需）

1. **ThorVG Lottie 的动画 API**：loader 已编，但 `ql_image_*` 只暴露静态 SVG；
   要播放 Lottie 需要另加时间轴/seek 接口。
2. **更多 SVG filter**：ThorVG 目前只覆盖部分 filter（`feGaussianBlur` 等），
   其余 `fe*` 仍会被跳过。
3. **内联 `<svg>`**：维持占位；要支持就按"构建期捕获片段 → 匿名资源"做。
