# SVG 支持（运行期 PlutoSVG + 构建期 qlsvg）

> 状态：**已实现（2026-11-16）**。目标读者：要接 SVG 图标、改 `qltext` 资源管线、
> 或评估"哪些 SVG 画不出来"的人。
> 相关：[图像管线设计](./图像管线设计.md)、[第三方依赖](./第三方依赖.md)、
> [HTML5 标签覆盖与裁剪](./手册/ui/HTML5标签覆盖与裁剪.md)。

核心判断：

> **图标是"资源"，不是"文档"。** PC/Mobile 直接在运行期用 PlutoSVG 解析；
> MCU 不链 SVG 库，用同一个渲染器写成的宿主工具 `qlsvg` 在构建期转成 PNG/A8。

---

## 0. 结论摘要

| 问题 | 决定 |
|---|---|
| PC / Mobile 怎么解 SVG | **运行期**：`ql_image_decode()` 识别 `.svg`，调 vendored PlutoSVG + PlutoVG 渲染成位图 |
| MCU 怎么用 SVG | **构建期**：`tools/qlsvg` 转 PNG（`--alpha` 出覆盖率遮罩），再交给 `qltext --root` |
| 渲染器有几份 | **一份**：运行期与 `qlsvg` 都编译 `third_party/plutosvg` + `third_party/plutovg`，语义一致 |
| 关掉 SVG 会怎样 | `QLGL_BUILD_SVG=OFF` → `.svg` 明确返回 `QL_ERR_UNSUPPORTED`，其余格式不受影响 |
| 单色图标染色 | 运行期 `mask-image: url(x.svg)`（取 alpha）；MCU 用 `qlsvg --alpha` + `qltext -a8` |
| 内联 `<svg>` | **不做**，继续"占位盒 + 内容跳过"；要图标请用 `<img src="x.svg">` |
| SVG 动画 | **不做**（PlutoSVG 不支持 SMIL/CSS 动画）；动效做在 QLUI 层（对位图做 transform/opacity） |
| `clip-path: path()` | 还没接；PlutoVG 的路径展平器是现成的，但属于独立特性 |

---

## 1. 能力边界（很重要）

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

**结论**：单色图标、logo、渐变图标、`<use>` 图标集都没问题；
插画级 SVG（滤镜/蒙版/大量文本/CSS 主题）请在构建期用设计工具导出 PNG，
不要承诺运行期还原。

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

### 2.4 尺寸与 DPR（现状）

当前在**注册时**按固有尺寸（或调用方给的 `target_w/h`）渲染一次，
之后由 QLUI 按元素盒子缩放。没有做"元素尺寸 × DPR 变化后自动重栅格化"；
高 DPI 下需要更锐利时，注册时显式给 `target_w/h`（例如 2×）。

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
- 工具与运行期共用同一份 PlutoSVG/PlutoVG，因此"工具产物"和桌面渲染一致。
- 产物是普通 PNG，可以提交进版本库；目标机不需要 `qlsvg`。
- 顺带：如果**宿主**的 `qltext` 是在带 SVG 的构建里编出来的（桌面默认），
  `qltext cc --root` 也能直接把 `.svg` 解成目标像素格式嵌入；MCU 预设把
  `QLGL_BUILD_SVG` 关掉时才必须先用 `qlsvg` 预渲染。

---

## 4. CMake 与裁剪

```cmake
option(QLGL_BUILD_SVG "运行期 SVG 解码（vendored PlutoSVG + PlutoVG）" ON)
```

| 能力 | 宏 | 默认 | 关掉的后果 |
|---|---|---|---|
| 运行期 SVG | `QL_FEAT_IMG_SVG` | 由 `QLGL_BUILD_SVG` 决定（桌面 ON，MCU 预设 OFF） | `.svg` → `QL_ERR_UNSUPPORTED` |
| 宿主工具 | `QLGL_BUILD_QLSVG` | 非交叉编译时 ON（**与 `QLGL_BUILD_SVG` 独立**） | 不产出 `qlsvg`；MCU 仍可手写/提交预渲染 PNG |

容量上限（`qlconfig.h`）：

| 宏 | 默认 | 含义 |
|---|---|---|
| `QL_CFG_IMAGE_SVG_MAX_DIM` | 2048 | 输出单边像素上限 |
| `QL_CFG_IMAGE_SVG_MAX_DEPTH` | 64 | 元素嵌套深度上限 |
| `QL_CFG_IMAGE_SVG_MAX_ELEMENTS` | 4096 | 元素总数上限 |
| `QL_CFG_IMAGE_MAX_ENCODED_BYTES` | 32 MiB | SVG 文本字节上限（与其它编码共用） |

关掉时 `src/draw/qlimage_svg.c` 编译成空文件，第三方源码不参与 `libqlgl` 编译，
库里不出现任何 PlutoSVG 符号；但开发机上的 `qlsvg` **仍然默认构建**，
所以"MCU 库裁掉 SVG"与"构建期还能把 SVG 转 PNG"不冲突。

---

## 5. 安全与有界

PlutoSVG 把 SVG 当 XML 元素树解析，不执行脚本、不展开 DTD 实体；
但我们仍然在外层加了一道预扫描：

| 风险 | 应对 |
|---|---|
| 脚本 / 事件 | 文本里出现 `<script`、`javascript:` 直接拒绝 |
| 外部引用 | `href="http…"`、`href="//…"`、`xlink:href="http…"`、`url(http…)`、`@import` 直接拒绝 |
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

不做逐像素 golden：抗锯齿细节由 PlutoVG 版本决定，跨版本可能微调。

---

## 7. 第三方代码与许可

`third_party/` 下 vendor 了两份 MIT 源码（版本与来源见 `third_party/README.md`）：

| 组件 | 版本 | 用途 |
|---|---|---|
| PlutoVG | 1.3.3 | 2D 路径/填充/渐变/光栅化 |
| PlutoSVG | 0.0.8 | SVG XML → PlutoVG 绘制 |

两者都是纯 C、无外部依赖；直接静态编进 `libqlgl`，不安装任何头文件。
升级步骤：替换 `third_party/` 源码 → 重跑 `test_image_svg` / `test_qlsvg` /
`example_46_svg_icon` 截图。

---

## 8. 还没做（按需）

1. **DPR 自动重栅格化 / 按尺寸缓存**：现在注册时渲染一次，QLUI 负责缩放。
2. **`currentColor` 在渲染期替换**：PlutoSVG 支持 `current_color` 参数，
   但 QLUI 现在走 A8 + mask 染色；需要"多色图标里只改一个颜色"时再接。
3. **`clip-path: path()`**：复用 PlutoVG 的路径解析与展平，接入现有覆盖率算法。
4. **ThorVG 可选后端**：需要 Lottie / 更高质量时再引；会带 C++ 运行时，MCU 不用。
5. **内联 `<svg>`**：维持占位；要支持就按"构建期捕获片段 → 匿名资源"做。
