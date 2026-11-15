# SVG 支持设计（构建期栅格化 + 可选运行期后端）

> 状态：**设计草案（2026-11-15）**。目标读者：要接 SVG 图标、改 `qltext` 资源管线、
> 或实现 `clip-path: path()` 的人。
> 相关：[图像管线设计](./图像管线设计.md)（解码/资源表/`blit_image`）、
> [平台后端设计](./平台后端设计.md)（vtable + 弱符号套路）、
> [CSS3 覆盖与裁剪](./手册/ui/CSS3覆盖与裁剪.md)（`clip-path: path()` 现状是明确报错）。

这份文档定三件事：**SVG 什么时候变成像素**、**运行期要不要矢量渲染器**、
**内联 `<svg>` 为什么不做**。核心判断只有一句：

> **图标是"资源"，不是"文档"。** 绝大多数 SVG 在构建期就能变成目标像素格式，
> 运行期的矢量渲染只作为可选能力；这样 MCU 和桌面用的是同一条像素管线。

---

## 0. 结论摘要

| 问题 | 决定 |
|---|---|
| 默认路线 | **构建期栅格化**：`qltext` 把 `.svg` 转成 `RAW` 资源，运行时零依赖、零解析 |
| 运行期要不要库 | 可选。`QLImageSvgBackend` 接缝，**默认不启用**；PC/Mobile 想要就开 |
| 选哪个库 | **PlutoSVG + PlutoVG**（纯 C、MIT、PC/Mobile 都能用）；ThorVG 作为"高质量可选后端" |
| 内联 `<svg>` | **不做**。继续"占位盒 + 内容跳过"；要图标请用 `<img src="x.svg">` 或 `background-image` |
| `<img src=x.svg>` | 构建期：转成普通位图（`--pixel-format` 决定 RGB565/A8…）；运行期：注册解码 |
| 单色图标 | 构建期转 **A8 遮罩**，运行期用 `color`/mask 染色，一套图标支持任意主题色 |
| 尺寸与 DPR | 构建期按 `--svg-scale` 定型；运行期按 `元素尺寸 × DPR` 栅格化并缓存 `(名字, w, h)` |
| `clip-path: path()` | 复用 PlutoVG 的路径解析 + 遮罩化，作为独立小特性（不依赖完整 SVG 支持） |
| 安全 | 外来 SVG 是 XML：禁外链/脚本/字体/滤镜，尺寸/字节数/递归深度/渲染时间全部有上限 |
| MCU | 构建期路线完全可用（A8/565 小图标）；运行期路线默认关 |

---

## 1. 目标与非目标

**目标**

1. `<img src="icon.svg">`、`background-image: url(icon.svg)`、`mask-image: url(icon.svg)`
   三条路径和 PNG 一样可用，像素结果确定、可缓存。
2. 图标在 1x/2x/3x 屏上都清晰：构建期按目标 DPR 栅格化，运行期按需重栅格化。
3. 单色图标能跟随主题色，不需要为每种颜色重新导出资源。
4. 支持 SVG 的"图标级子集"：`path`、`rect/circle/ellipse/line/polygon/polyline`、
   渐变、`transform`、`clipPath`、`mask`、`use`（文档内）、`opacity`。
5. 明确拒绝：脚本、外链图片、外链字体、滤镜、SMIL、CSS 动画。
6. 所有外来输入有界：字节数、viewBox、输出尺寸、递归深度、渲染步数。

**非目标**

- **内联 `<svg>` 子树**（`<svg><path/></svg>` 直接写在 HTML 里）：
  那需要 DOM/CSS 继承/`<use>`/`viewBox` 嵌套，等于第二个渲染器。
  维持现状：内容整段跳过 + 占位盒（`qlui_parse.c` 的 `tag_skips_content`）。
- SVG 动画（SMIL / CSS animation / `<animate>`）。
- 滤镜（`feGaussianBlur` 等）与 `filter: url(#...)`。
- 外链资源：`<image href="https://...">`、`@font-face`、`url(http://...)`。
- 完整文本排版：`<text>` 只在运行期后端里尽量给（依赖字体），
  构建期路线建议直接把文字转曲（导出时 outline）。
- SVG 写回/编辑。

---

## 2. 现状（2026-11）

| 位置 | 现状 |
|---|---|
| `<svg>` 标签 | `QL_UI_TAG_SVG`，占位盒；**内容整段跳过**（`tag_skips_content`） |
| `<img src="x.svg">` | 构建期 `qltext --root` 会把它当图片打开 → 现阶段会解码失败；运行期 `QL_IMAGE_SRC_AUTO` 也不认 SVG |
| `background-image` / `mask-image` / `border-image` | 与 `<img>` 共用资源表；同样只认 BMP/QOI/PNG/GIF/JPEG |
| `clip-path: path()` / `url()` | 解析期明确报错（"需要 SVG 路径解析器"） |
| 文献 | [HTML5 标签覆盖与裁剪](./手册/ui/HTML5标签覆盖与裁剪.md) §5：`svg/math` 归为"需要独立渲染器" |

也就是说：**现在没有任何 SVG 能力**，但资源表、构建期转码、A8 蒙版、
`blit_image` 都已经就位，差的只是"把 SVG 变成像素"这一步。

---

## 3. 三条路线与取舍

| 路线 | 运行时依赖 | 优点 | 缺点 | 用途 |
|---|---|---|---|---|
| **A. 构建期栅格化** | 0 | 零解析、零依赖、像素确定、MCU 可用、可做 A8 染色 | 尺寸固定；换色/换尺寸要重新构建 | **默认**：图标、logo、静态插画 |
| **B. 运行期矢量渲染** | 一个 SVG 库 | 任意缩放、外部文件、动态换色 | 体积、内存、SVG 子集、浮点差异 | 可选：动态图标、用户提供的 SVG |
| **C. 自己写子集** | 0 | 完全可控 | 等于写半个渲染器（贝塞尔、填充规则、渐变、裁剪） | **不做** |

结论：**A 是主路径，B 是加装件，C 不碰。**

### 3.1 路线 A：构建期栅格化

`qltext` 已经具备能力：`--root DIR` 把 `<img src>` 指向的图片在构建期解码，
按 `--pixel-format`（`rgb565/rgb888/rgba8888/a8`）重排后嵌入 C 表。

需要加的是：**遇到 `.svg` 时先栅格化，再走同一条嵌入路径**。

```
qltext cc --html app.html --css app.css --root assets \
       --pixel-format rgb565 --svg-scale 2
```

- `--svg-scale N`：按 SVG 声明尺寸的 N 倍栅格化（对应 2x/3x 屏）。
- `--svg-size WxH`：强制输出尺寸（覆盖 SVG 的 width/height）。
- 单色图标建议先导出成 `<path fill="currentColor">`，再用 `--pixel-format a8`：
  运行期用 `color` 染色（见 §7.3），一套资源适配亮/暗主题。

**实现方式**（二选一）：

1. **外部工具**（推荐先做）：配置期探测 `resvg` / `rsvg-convert`，
   `qltext` 用子进程把 SVG 转成 PNG（临时文件或 stdout），再走现有解码。
   好处：库本身零依赖；坏处：构建机要装工具。
2. **链接 PlutoSVG**：`qltext` 直接调 `plutosvg_document_load_from_file` +
   `document_render_to_surface`，转成 RGBA 后按目标格式重排。
   好处：构建产物在装没装工具的机器上都一致；坏处：宿主工具多一个依赖。

建议：**两者都支持**，默认优先外部工具（保持库轻），找不到再退回链接后端。

> 与 `qltext` 的既有约定一致：构建期解码是"把外来数据变成目标像素格式"，
> 运行期只做搬运。SVG 只是多了一种源格式。

### 3.2 路线 B：运行期矢量渲染

只在"资源在运行期才拿到"或"尺寸/颜色会变"时需要：

- 从磁盘/网络加载的 `.svg`（主题包、插件）；
- 同一图标要按不同 DPR/尺寸渲染（窗口拖到 4K 屏）；
- `currentColor` 染色（A8 方案在构建期也能做，见 §7.3）。

运行期后端接口见 §5。**默认 `QLGL_BUILD_SVG=OFF`**：不链任何 SVG 库，
`<img src=x.svg>` 在构建期已经变成位图的话完全无感。

---

## 4. 能力开关

沿用"能力正交、各自可裁"的规则：

| 能力 | 宏 | 默认 | 关掉的后果 |
|---|---|---|---|
| 构建期 SVG 栅格化 | `QLTEXT_SVG`（构建工具侧） | 探测到工具/库才开 | `qltext` 对 `.svg` 报错并给出安装提示 |
| 运行期 SVG 接口 | `QL_FEAT_IMG_SVG` | 0（桌面预设可开） | 运行期 `.svg` → `QL_ERR_UNSUPPORTED` |
| PlutoSVG 后端 | `QL_FEAT_IMG_SVG_PLUTOSVG` | = `QL_FEAT_IMG_SVG` | 没有默认后端 |
| ThorVG 后端 | `QL_FEAT_IMG_SVG_THORVG` | 0（可选） | 桌面高质量路径不可用 |
| NanoSVG 后端 | `QL_FEAT_IMG_SVG_NANOSVG` | 0（极小构建） | 只认简单图标 |
| `clip-path: path()` | `QL_FEAT_CSS_CLIP_PATH_FILE` | 0（二期） | 保持现在的"明确报错" |

容量上限（`qlconfig.h`）：

| 宏 | 默认 | 含义 |
|---|---|---|
| `QL_CFG_SVG_MAX_BYTES` | 1 MiB | 单个 SVG 文件字节上限 |
| `QL_CFG_SVG_MAX_DIM` | 2048 | 输出单边像素上限 |
| `QL_CFG_SVG_MAX_PIXELS` | 2048×2048 | 输出总像素上限 |
| `QL_CFG_SVG_CACHE_ENTRIES` | 4 | 每个资源名缓存几个尺寸 |
| `QL_CFG_SVG_CACHE_BYTES` | 8 MiB | 全局栅格化缓存预算 |
| `QL_CFG_SVG_MAX_DEPTH` | 32 | `<g>`/`<use>` 嵌套上限 |

---

## 5. 运行期接缝：`QLImageSvgBackend`

和 `QLImageBackend`（JPEG 平台后端）并列，但**多一个目标尺寸**——
SVG 不"解码一次得到固定位图"，它是"按尺寸渲染"：

```c
typedef struct QLImageSvgBackend {
    const char *name;              /* "plutosvg" / "thorvg" / "nanosvg" */

    /*
     * 把 SVG 字节渲染成 out_format 的像素。
     *   w, h       目标像素尺寸（已含 DPR，调用方按元素尺寸算好）
     *   out        成功时填 px/len/w/h/stride/format，owned = 1（ql_mem_alloc 分配）
     *   err/err_cap 人类可读失败原因
     * 失败返回 QL_ERR_INVAL / QL_ERR_NOMEM / QL_ERR_UNSUPPORTED。
     */
    ql_err_t (*render)(const void *data, size_t len,
                       ql_coord_t w, ql_coord_t h, uint8_t out_format,
                       QLImage *out, char *err, size_t err_cap);

    /* 可选：直接渲染进调用方的缓冲（GL 上传零拷贝、A8 蒙版） */
    ql_err_t (*render_to)(const void *data, size_t len,
                          uint8_t *dst, uint32_t stride,
                          ql_coord_t w, ql_coord_t h, uint8_t out_format,
                          char *err, size_t err_cap);

    void *user;
} QLImageSvgBackend;

void ql_image_set_svg_backend(const QLImageSvgBackend *be);   /* NULL = 卸载 */
const QLImageSvgBackend *ql_image_svg_backend(void);
```

约定：

- **输出非预乘**，与 `QLImage` 的 `RGBA8888` 定义一致。PlutoVG 的 surface 是
  **预乘 ARGB32**（小端内存序 B,G,R,A），适配器里必须反预乘 + 换序；
  `render_to` 允许后端直接写目标格式以减少一次拷贝。
- `w/h` 由调用方给：后端不许自己按 `viewBox` 猜（尺寸策略在 QLUI 层，见 §7）。
- 后端不缓存、不持有多媒体/文档对象之外的状态；每次 `render` 都是独立的。
- 失败必须写 `err`（例如 `"<use> 指向外部文件，已拒绝"`），便于定位。

### 5.1 适配器实现要点（PlutoSVG）

```c
/* 1) 解析（带上限：先查字节数/根元素尺寸） */
plutosvg_document_t *doc = plutosvg_document_load_from_data(data, (int)len,
                                                            -1, -1);
/* 2) 目标 surface：优先 surface_create_for_data 直接写调用方缓冲 */
plutovg_surface_t *surf = plutovg_surface_create_for_data(dst, w, h, stride);
plutovg_canvas_t  *cv   = plutovg_canvas_create(surf);
/* 3) 渲染：viewBox → 目标矩形；current_color 用于解析 CSS currentColor */
plutosvg_document_render(doc, NULL, cv, &current_color, NULL, NULL);
/* 4) 反预乘 + BGR→RGB（仅 render() 路径需要） */
```

- PlutoVG 的 `plutovg_surface_create_for_data()` 让我们**直接渲染进目标缓冲**，
  不需要中间 surface；这对 A8 蒙版和 GL 纹理上传都很关键。
- `plutosvg_document_render()` 自带 `current_color` 参数：单色 SVG 可以在**栅格化时**
  就换成主题色，不必先出 A8 再染色（A8 仍是对 CPU 最省的路径）。
- 解析用 `plutosvg_document_load_from_data()`：**它不拷贝输入缓冲**，
  缓冲必须活到 `plutosvg_document_destroy()`（所以运行期缓存要连字节一起管）。
- 文本：PlutoVG 自带 TrueType 字体接口（`plutovg_font_face_load_from_*`），
  但图标里基本用不到；运行期后端可以关闭文本，遇到 `<text>` 返回
  `QL_ERR_UNSUPPORTED`（比渲染成乱码好）。

---

## 6. 构建期管线（`qltext`）

### 6.1 命令与开关

```bash
# 已支持：--root 把图片按目标像素格式嵌入
qltext cc --html app.html --css app.css --root assets \
          --pixel-format rgb565

# 新增（本设计）：
#   --svg-scale N      按声明尺寸的 N 倍栅格化（默认 1）
#   --svg-size WxH     强制输出尺寸
#   --svg-tool PATH    指定 resvg / rsvg-convert（默认自动探测）
#   --svg-engine auto|tool|plutosvg   栅格化实现（auto = 先工具后库）
```

`--root` 遇到 `foo.svg` 时：

1. 读文件，检查字节数 ≤ `QL_CFG_SVG_MAX_BYTES`；
2. 解析根元素拿 `width/height/viewBox`，算出目标尺寸（按 `--svg-scale`）；
3. 用 `--svg-engine` 栅格化到 RGBA8888；
4. 按 `--pixel-format` 重排（`rgb565` / `a8` / …）；
5. 走现有 `Asset` 流程：进 `image_data` 段，`src_format = QL_IMAGE_SRC_RAW`。

**结果**：运行期和 PNG 图标**完全一样**——没有 SVG 分支、没有解析、没有依赖。

### 6.2 单色图标的 A8 染色

```bash
qltext cc --html app.html --css app.css --root icons --pixel-format a8
```

```css
.icon { width: 24px; height: 24px; background-color: currentColor;
        mask-image: url(star.svg); mask-size: 100% 100%; }
```

- 构建期把 `star.svg` 栅格成 **A8**（透明度即形状）；
- 运行期 `mask-image` 已支持（`QLUIStyle.mask_image` + 资源表），
  颜色取 `background-color`/`color`，天然跟随主题。
- 对多色图标仍用 `rgb565/rgba8888`；两条路可以混用。

### 6.3 内联 `<svg>` 的处置

保持"跳过内容 + 占位盒"，但**构建期可以救**（可选，二期）：

- `qltext` 在跳过前把 `<svg ...> … </svg>` 的**原始片段**收集下来，
  按 `width/height` 栅格化成一个匿名资源，然后把节点改写成 `<img src="…">`。
- 这样"内联图标"也能用，且运行期仍然只有位图。
- 风险：内联 SVG 可能依赖 CSS 继承/`currentColor`/`<use>` 外链；
  一期只处理"自包含、无外链"的片段，其余维持占位。

---

## 7. 运行期管线与 QLUI 集成

### 7.1 什么时候栅格化

| 触发 | 行为 |
|---|---|
| `ql_ui_build()` | 不解码；只登记资源名（与 `<img>` 的 RAW 资源一致） |
| 首次绘制 `<img src=x.svg>` | 按"元素内容框 × DPR"算出 `w/h`，调后端 `render`，结果进缓存 |
| CSS 尺寸变化 | 先继续用旧位图拉伸（一帧），把新尺寸放进"待重栅格化"队列 |
| 尺寸稳定 1~2 帧 / 布局结束 | 重栅格化并替换（避免拖动窗口时每帧重渲染） |
| 缓存超预算 | 按 LRU 淘汰（同一资源的旧尺寸优先） |

**上限**：`w/h` 先夹到 `QL_CFG_SVG_MAX_DIM`；总像素超上限时按比例缩小，
并打一次 `QL_LOGW`（不静默改尺寸）。

### 7.2 缓存键

```
(name, css_w_rounded, css_h_rounded, scale_fx, format)
```

- `scale_fx` 是 26.6 定点 DPR（与画布同一套）；
- `format` 区分 `RGBA8888` / `A8`（同名字可能两种用途）；
- 不做"颜色"进键：染色在绘制期做（A8 蒙版），多色图标本来就是多色。

### 7.3 染色（currentColor）

- A8 资源：绘制期用 `mask` + 纯色填充，颜色取自样式（`color`），
  与现有 `mask-image` 路径完全一致，**不需要 SVG 层参与**。
- RGBA 资源：需要"只替换 RGB、保留 alpha"的 tint；作为 `QLDraw2D` 的可选能力
  （`blit_image_tinted`），CPU 逐像素、GL 用 `u_tint` uniform。

### 7.4 布局尺寸

- 有 `width/height`：用它们（CSS `width/height` 优先，盒模型不变）。
- 只有 `viewBox`：固有尺寸 = viewBox 尺寸（SVG 规范对替换元素的行为）。
- 都没有：按 300×150 兜底（与 `<img>` 的"无内在尺寸"一致）。
- 运行期可以从 `plutosvg_document_get_width()/get_height()` 取固有尺寸；
  构建期由 `qltext` 解析根元素得到同样的值。
- `object-fit` / `object-position` 复用 `<img>` 的实现，不改。

---

## 8. `clip-path: path()` / `url()`（顺带收益）

现在的状态是"解析期明确报错"（`CSS3覆盖与裁剪.md` §517）。有了路径解析器后：

```css
.badge { clip-path: path("M0,0 L40,0 L40,40 Z"); }
```

实现路线：

1. `plutovg_path_parse()` 解析路径 → 展平成多边形（`plutovg_path_clone_flatten`）；
2. 顶点归档进 doc 池（复用现有 `clip_pts` 存储与去重）；
3. 绘制期用现有"奇偶内点 + 到最近边距离"的覆盖率算法（CPU/GPU 同一公式）。

- 只支持绝对/相对命令 + 直线/曲线展平；`url(#id)`（引用文档内 `<clipPath>`）
  仍不支持（那要 DOM 和 `<svg>` 树）。
- 单独开关 `QL_FEAT_CSS_CLIP_PATH_FILE`，不依赖完整 SVG 资源支持。
- 收益：`clip-path: path()`（明确报错项）+ 未来 CSS `shape()` 都能复用同一个展平器。

---

## 9. 安全与有界（SVG 是 XML，别当图片看）

| 风险 | 应对 |
|---|---|
| 实体膨胀（billion laughs） | 解析器禁用 DTD/实体展开；`plutosvg` 只认元素/属性，不展开实体；比 XML 通用解析器安全 |
| 外部引用（`<image href>`、`xlink:href`） | 一律拒绝并记 `err`；不发起任何网络/文件访问 |
| 脚本（`<script>`、`on*`） | 不执行；解析时直接忽略 |
| 超大 `viewBox` / 输出尺寸 | 输出前夹到 `QL_CFG_SVG_MAX_DIM` / `MAX_PIXELS` |
| 深递归（`<g>` 嵌套 / `<use>` 环） | 深度上限 + `<use>` 访问集合防环 |
| 渲染时间（复杂路径） | 构建期无限制（开发机）；运行期按"像素预算"限制，超了降分辨率并告警 |
| 字体（`@font-face`、系统字体扫描） | 运行期后端禁用文本/字体，`<text>` 返回 UNSUPPORTED |
| 文件路径穿越 | 构建期只读 `--root` 之下的文件；运行期由调用方给字节 |

> 这条和图像管线的红线一致：**外来数据必须有界**。SVG 比 PNG 更危险，
> 因为它是"可执行的描述"（引用、嵌套、字体）。

---

## 10. 线程与性能

- **构建期**：`qltext` 单线程；一个图标几十毫秒无所谓。
- **运行期**：
  - 一期**同步**栅格化（首次绘制时），单图标（≤128×128）目标 < 2ms；
  - 超时/超像素预算的内容降分辨率，不阻塞主循环；
  - 二期可选后台线程：`render` 在 worker，UI 继续用旧位图/占位，完成后替换。
- **缓存**：栅格化结果按 §7.2 缓存；静止页面零渲染、零上传。
- **GL**：优先 `render_to` 直接写纹理 staging buffer，避免"渲染到 RGBA → 再拷一遍"。
- **CPU**：`render_to` 直接写画布区域（A8 蒙版尤其划算）。

---

## 11. 构建与许可

### 11.1 CMake

```cmake
option(QLGL_BUILD_SVG "运行期 SVG 栅格化（可选）" OFF)
set(QLGL_SVG_BACKEND "plutosvg" CACHE STRING "plutosvg|thorvg|nanosvg|none")

if(QLGL_BUILD_SVG AND QLGL_SVG_BACKEND STREQUAL "plutosvg")
    pkg_check_modules(PLUTOSVG QUIET plutosvg)
    if(NOT PLUTOSVG_FOUND)
        find_package(plutosvg QUIET)          # 上游提供 CMake 包
    endif()
    if(PLUTOSVG_FOUND)
        target_sources(qlgl PRIVATE src/port/qlimage_svg_plutosvg.c)
        target_link_libraries(qlgl PRIVATE ${PLUTOSVG_LIBRARIES})
        target_compile_definitions(qlgl PUBLIC QL_FEAT_IMG_SVG=1
                                             QL_FEAT_IMG_SVG_PLUTOSVG=1)
    else()
        message(STATUS "QLGL: 未找到 PlutoSVG，运行期 SVG 关闭")
    endif()
endif()
```

- 构建期路线**不需要**任何 CMake 依赖：`qltext` 运行时探测 `resvg`/`rsvg-convert`。
- 若选择"qltext 链接 PlutoSVG"，只在宿主工具上加依赖，不进库。
- 上游是 CMake/Meson 工程，纯 C；也可以用 `FetchContent`（默认不做，
  保持"系统依赖"的仓库习惯）。

### 11.2 许可

| 组件 | 许可 | 说明 |
|---|---|---|
| PlutoSVG / PlutoVG | **MIT** | 可静态/动态链接，商用友好 |
| NanoSVG | zlib | 单头文件，可 vendor |
| ThorVG | MIT | C++ 内核 + C API；允许静态链接 |
| resvg（构建期工具） | MPL-2.0 | 只跑在开发机，不进产物 |
| rsvg-convert（构建期工具） | LGPL-2.1+ | 同上，只是工具 |

> 与 FFmpeg 的结论一样：**工具链许可 ≠ 产物许可**。构建期用 GPL/MPL 工具
> 不会改变本库的许可；但把库静态链进产物要按库自己的许可办。

### 11.3 体积参考（量级，非承诺）

| 方案 | 代码/二进制量级 | MCU |
|---|---|---|
| 构建期（无运行期库） | 0 | ✅ |
| NanoSVG | 单头 ~3k 行，几十 KB | ✅ |
| PlutoSVG + PlutoVG | 约两三万行，百 KB 级（接入时实测） | ⚠️ 小图标可以 |
| ThorVG | 百 KB 级（可裁） | ⚠️ 需 C++ |

> 具体数字在接入时实测，写进 `test/mcu/README.md` 的体积表。

---

## 12. 测试计划

### 12.1 构建期

| 用例 | 断言 |
|---|---|
| `icon.svg` → `rgb565` | 产物尺寸/像素格式正确；关键像素颜色在容差内 |
| `--svg-scale 2` | 输出是 1x 的两倍；A8 覆盖率单调 |
| `--svg-size` 覆盖 | 输出尺寸被强制 |
| `.svg` 损坏 / 无 `viewBox` | 明确报错，构建失败而不是产出 0×0 |
| 超过 `--svg` 上限 | 明确报错（不静默降级） |
| 与 PNG 同图对比 | 结构相似度（不逐像素，渲染器不同） |

### 12.2 运行期后端

| 用例 | 断言 |
|---|---|
| 假后端 | 不依赖任何 SVG 库，验证 QLUI 的尺寸/缓存/失效逻辑（和 `test_window_fake` 同思路） |
| PlutoSVG 后端 | 图标集（path/渐变/clip/mask/transform）逐像素 golden，**带 ±2 容差** |
| DPR | 1x/2x 输出的覆盖率关系（2x 边缘更平滑，不是简单放大） |
| 缓存 | 同名同尺寸只 render 一次；LRU 淘汰后重新 render |
| 拒绝项 | 外链 `<image>`、`<script>`、`<text>`（禁文本时）、超尺寸 → 明确错误码，不崩 |
| 畸形输入 | 截断的 SVG、超深 `<g>`、`<use>` 环 → 不崩、不死循环（ASan + 超时） |
| 版本固定 | PlutoSVG 仍是 `0.0.x`：记录/锁定版本（或 vendor 一份），升级时重跑图标 golden |
| 无后端构建 | `QL_FEAT_IMG_SVG=0` 时 `.svg` → `QL_ERR_UNSUPPORTED`，页面显示占位 |

### 12.3 手工验收

- 高 DPI 屏上图标是否清晰（1x 拉伸 vs 2x 重栅格化）；
- 拖动窗口时是否出现"每帧重渲染"（用 `--stats` 看帧时间）；
- 深色/浅色主题切换时 `currentColor` 图标是否即时变色（应零重栅格化）。

---

## 13. 里程碑

| 阶段 | 内容 | 验收 |
|---|---|---|
| **S1（构建期）** | `qltext` 支持 `.svg`（外部工具 + `--svg-scale/--svg-size`）；A8 染色文档与示例 | `examples` 里一个纯 SVG 图标页，MCU 配置也能构建、能显示 |
| **S2（运行期最小）** | `QLImageSvgBackend` + PlutoSVG 适配器 + 尺寸/缓存/失效 | 运行期加载 `.svg`、窗口缩放后重栅格化正确；缓存命中率可观测 |
| **S3（染色与裁剪）** | `blit_image_tinted`（RGBA tint）+ `clip-path: path()` | 单色 SVG 跟 `color` 变；`path()` 用例通过 golden |
| **S4（可选）** | ThorVG 后端（桌面高质量）、内联 `<svg>` 构建期栅格化、后台线程渲染 | 对比 PlutoSVG 的覆盖差异；内联图标可用 |

每一阶段都保持"关掉能力 = 占位盒/明确报错"，不允许半开半挂。

---

## 14. 与其它方案对照

| 维度 | 构建期栅格 | PlutoSVG 运行期 | ThorVG 运行期 | librsvg / resvg 运行期 |
|---|---|---|---|---|
| 依赖 | 0 | MIT、纯 C、百 KB 级 | MIT、C++ 内核 | Rust + Cairo/Pango，数 MB |
| PC/Mobile | ✅ | ✅ | ✅ | ✅ |
| MCU | ✅ | ⚠️（小图标） | ⚠️（需 C++） | ❌ |
| 任意尺寸/DPR | ❌（要预生成多套） | ✅ | ✅ | ✅ |
| 动态换色 | A8 染色即可 | ✅（tint/currentColor） | ✅ | ✅ |
| SVG 覆盖 | 由工具决定（最全） | 图标级子集 | 图标级 + 部分滤镜/Lottie | 最全 |
| 与自绘管线冲突 | 无 | 输出位图，无冲突 | 输出位图/画布，无冲突 | 输出位图，无冲突 |

Qt 的 `QSvgRenderer` 属于"Qt SVG 模块"（SVG Tiny 1.2 子集，Tiny 1.2 不支持滤镜/脚本），
思路和这里的运行期后端一致；但它是 C++/Qt 依赖，不适用于本库。

---

## 15. 开放问题

1. 构建期默认用**外部工具**还是**链接 PlutoSVG**？
   （建议：auto = 先找工具，找不到再退回库；CI 里固定一种，避免像素漂移。）
2. 运行期默认开关：桌面 ON / MCU OFF，还是全局 OFF 由用户显式打开？
   （建议后者，保持"零惊喜依赖"。）
3. A8 染色是否需要支持**多色 tint**（比如双色图标）？一期先单色。
4. `clip-path: path()` 的顶点数上限（建议 256，与现有 `clip_pts` 预算对齐）。
5. 内联 `<svg>` 的构建期栅格化是否值得做？还是永远"请改用 `<img>`"？
6. 运行期 SVG 的文本：返回 `UNSUPPORTED`，还是用 PlutoVG 的字体接口尽力渲染？
   （建议前者：图标场景不需要，渲染错字体比不渲染更糟。）
