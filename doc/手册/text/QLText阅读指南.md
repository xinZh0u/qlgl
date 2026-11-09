# QLText / 字体系统 阅读指南

> 本部分包含三层：**字体**（qlfont，静态位图 / FreeType 动态）→ **字符串**（qlstr）→
> **画布文本原语**（`ql_canvas_draw_text`）；界面层的文本样式（`font-size` 等）见
> [QLUI 阅读指南](../ui/QLUI阅读指南.md)。
> 目标读者：要读懂或修改 `src/text/`、`src/core/qlstr.c`、文本绘制相关代码的人。
> 只想用 API，请看 [QLText API手册](./QLText%20API手册.md)。

---

## 1. 文件地图

| 文件 | 职责 | 阅读顺序 |
|---|---|---|
| `tools/qlfontgen/gen_bitmap_font.py` | 从 TTF 生成内置位图字体 | ① |
| `src/text/qlfont_ascii.c` | **自动生成**的字形数据（勿手改） | ② |
| `include/qlgl/qlfont.h` | `QLFont` / `QLGlyph` / `QLFontBackend` 结构与查找、度量接口 | ③ |
| `src/text/qlfont.c` | 二分查找、位图行访问、UTF-8 编解码、文本度量、默认字体表 | ④ |
| `include/qlgl/qlfont_ft.h` | FreeType 后端接口（加载字体、设默认字体、缓存上限） | ⑤ |
| `src/text/qlfont_ft.c` | 按需光栅化 + 有序字形缓存 + TTC 字面选择 | ⑥ |
| `include/qlgl/qlstr.h` | 自管理字符串与两种容量策略 | ⑤ |
| `src/core/qlstr.c` | 容量增长、只读字面量转自有内存 | ⑥ |
| `src/draw/qlcanvas.c` | `ql_canvas_draw_text` / `draw_glyph`（在文件末尾） | ⑦ |
| `src/ui/qlui.c` | 界面层：按样式测量与绘制文本 | ⑧ |
| `test/text/*.c` | 可执行的说明 | ⑨ |

---

## 2. 字体数据模型

```
QLFont
 ├── bpp           1 / 4 / 8，决定每个像素的覆盖率精度
 ├── line_height   ascent + descent
 ├── ascent        基线到行顶
 ├── fallback      缺字时改用的 codepoint（内置字体是 '?'）
 ├── glyphs[]      按 codepoint 升序的定长表，二分查找
 └── bitmap[]      所有字形位图首尾相连
      └── QLGlyph.data_offset 指向自己那段
```

一个字形怎么定位：

```
pen_x, baseline_y  ──┐
                     ├─→ 墨迹左上角 = (pen_x + ofs_x, baseline_y + ofs_y)
                     │
          ┌──────────┴──────────┐
          │   box_w × box_h      │  ← bitmap[data_offset] 开始，每行 stride 字节
          └──────────────────────┘
          └─ 画完后 pen_x += adv_w
```

`ofs_y` 是**负值**（墨迹在基线之上），这正是"基线对齐"的由来：不同字形的
`box_h` 与 `ofs_y` 不同，但它们的基线一致，所以同一行文字看起来是齐的。

### 为什么用二分查找

字形表按 codepoint 升序排列，查找是 O(log n)：95 个字符只要 7 次比较，
比线性扫描快，也不需要哈希表（MCU 上没有额外内存开销）。

### 缺字兜底

```
ql_font_find_glyph(font, cp)
    ├─ 二分找到 → 返回
    ├─ 没找到且 font->fallback != 0 → 递归查 fallback
    └─ 还是没找到（fallback 也缺）→ NULL
```

内置字体只覆盖 ASCII 32..126，所以 `"你好"` 会渲染成两个 `?`。
这不是 bug，而是**明确的降级行为** —— 文档里要写清楚，用户才不会以为是编码问题。

---

## 3. 位图格式（4bpp）

每个像素 4 位，表示覆盖率 0..15，写入时乘 17 扩成 0..255 参与 alpha 混合。

```
一行 = ceil(box_w / 2) 字节
字节内：高半字节 = 左像素，低半字节 = 右像素
```

取灰度的实现（`ql_font_glyph_alpha`）：

```c
case 4:
    byte   = row[x / 2];
    nibble = (x % 2 == 0) ? (byte >> 4) : (byte & 0x0F);
    return nibble * 17;      /* 0..15 -> 0..255 */
```

因此**4bpp 字体的覆盖率一定是 17 的倍数**，测试里就用这条做断言。

1bpp 与 8bpp 也实现了（`bpp` 字段切换），只是内置字体没有用：
- 1bpp：MCU 省空间，但边缘有锯齿；
- 8bpp：质量最好，体积是 4bpp 的两倍。

---

## 4. 文本度量

```c
void ql_font_measure_text(font, text, len, &w, &h);
```

规则：

| 项 | 规则 |
|---|---|
| `len` | `< 0` 表示以 `'\0'` 结尾；否则只处理前 len 字节 |
| 宽度 | 取**最宽的一行** |
| 高度 | 行数 × `line_height` |
| 换行 | 只认 `'\n'`，不自动折行 |
| 缺字 | 按 fallback 字形的 `adv_w` 前进，不会卡死 |

**踩过的坑**：最初的循环条件写成 `while (i <= len || text[i] != '\0')`，
导致遇到结尾 `'\0'` 时直接退出循环、永远不结算最后一行，宽度恒为 0。
现在改成"循环内部判断是否到达结尾"，见 `src/text/qlfont.c`。

---

## 5. UTF-8 处理

`ql_utf8_decode` 返回**消费的字节数**（1..4），非法序列返回 1 并把 codepoint
设为 `0xFFFD`（替换字符）。这条"非法也消费 1 字节"的规则很关键 ——
否则遇到坏字节就会原地死循环。

```c
int32_t used = ql_utf8_decode(text + i, &cp);
if (used <= 0) break;
...
i += used;
```

`ql_utf8_encode` 是对应方向的编码，用于把 codepoint 写回缓冲区（测试里做往返验证）。

---

## 6. 字符串与两种容量策略

`QLStr` 自己管理一块内存，`own` 标记这块内存是不是它分配的：

| 策略 | 行为 | 适用 |
|---|---|---|
| `QL_STR_COMPACT` | 容量 = 长度 + 1，每次精确分配；删除即缩容 | MCU 高频刷新，内存紧张 |
| `QL_STR_AMORTIZED` | 容量按 2 倍增长，缩短内容不缩容 | MPU/PC，减少分配次数 |

策略是**每个字符串独立的**（`ql_str_set_policy`），不是编译期开关 ——
这样同一份固件里，频繁刷新的状态栏可以用 compact，日志面板可以用 amortized。

### 只读字面量

`ql_str_set_static` 直接引用外部字符串（`own = 0`），适合常量。
一旦需要写入（`append` / `set`），`ensure_capacity` 会**新分配一块内存并把内容拷过去**，
不会去写只读内存。

> **踩过的坑**：最初的 `ensure_capacity` 在"只读串需要扩容"时调用 `ql_str_set_n`，
> 而 `ql_str_set_n` 又调 `ensure_capacity` —— 无限递归直接爆栈（ASan 报 stack-overflow）。
> 现在改为在两处各自处理：`ensure_capacity` 内部完成"分配 + 拷贝"，不再回调。

---

## 7. 画布文本原语

```c
void ql_canvas_draw_text(canvas, font, text, len, box, color, align);
```

流程：

```
1. 裁剪 = canvas->clip ∩ box                 ← 文字不会画到控件外面
2. baseline = box.y + font->ascent            ← 首行基线
3. 逐字节扫描：
     遇到 '\n' 或结尾 → 结算这一行
         line_w = 该行宽度
         pen_x  = box.x / 居中 / 右对齐
         逐个字形 draw_glyph(pen_x, baseline)
         baseline += line_height
4. 恢复裁剪
```

`draw_glyph` 对每个像素调 `ql_canvas_write_pixel(color, alpha)`，
alpha 就是字形的覆盖率，因此**抗锯齿是天然支持的**，不需要额外的混合代码路径。

绘制顺序要点：界面层先画节点自己的背景/边框，再画文字，所以文字在背景之上。

---

## 8. 文本由谁画：没有"文本控件"

QLGL 里**没有** `QLLabel` 这类控件类。文本要么是 HTML 元素，要么直接画在画布上：

| 场景 | 谁负责排版 | 入口 |
|---|---|---|
| 有界面（推荐） | 界面层（HTML/CSS 的 `font-size` / `color` / `text-align` / `line-height`） | `ql_ui_render_canvas()` |
| 只有画布 | 你自己给一个 `box` | `ql_canvas_draw_text()` |
| GPU 直绘 | 你自己 | `QLDraw2D->draw_text()` |

三者用的是**同一套字体层**（`ql_font_measure_text` / `draw_glyph`），所以字形完全一致。

### 文本在矩形内的位置

- 水平：`QL_TEXT_ALIGN_LEFT` / `CENTER` / `RIGHT`（界面层对应 `text-align`）；
- 垂直：**永远从 `box.y` 开始**，不垂直居中 —— 要居中就自己算 `y` 偏移，
  或者给界面层写 `line-height`（它按行高居中的那套算法处理）。

### 多行

`ql_canvas_draw_text()` 只处理 `'\n'`，**不自动折行**。界面层额外支持
`white-space: pre`（保留 `\n` 与空格）与 `nowrap`；自动折行由布局完成，
见 [QLUI 阅读指南](../ui/QLUI阅读指南.md) 的文本测量一节。

---

## 9. 常见坑

| 坑 | 现象 | 说明 |
|---|---|---|
| 中文显示成 `?` | 字形缺失 | 内置字体只有 ASCII；需要自带 CJK 字体或开 FreeType |
| 文字画到盒子外 | 看到文字溢出 | 画布路径会裁到 `box ∩ canvas->clip`；看不到说明 `box` 太小 |
| 文字垂直不居中 | 文字偏上 | 只支持顶端对齐，需要自己算 `y` 偏移或用 `line-height` |
| 长文本不换行 | 超出部分被裁 | 只支持 `'\n'`，不自动折行；界面层可写 `white-space` |
| 自定义字体不显示 | 全是空白 | 检查 `bpp` 是否为 1/4/8，`glyphs` 是否按 codepoint 升序 |

---

## 10. 如何添加自定义字体

### 10.1 静态位图字体（MCU 路线）

1. 用 `tools/qlfontgen/gen_bitmap_font.py` 的方式生成（或自己写生成脚本），
   产物是一组 `QLGlyph` + 一块 `uint8_t bitmap[]` + 一个 `const QLFont`；
2. 要求：
   - `glyphs` 按 `codepoint` **升序**；
   - 每个 `QLGlyph.data_offset` 指向 `bitmap` 内自己那段；
   - `bpp` ∈ {1,4,8}，行字节数符合 `(box_w+7)/8` / `(box_w+1)/2` / `box_w`；
3. 界面上用 `font-family` 指向它，或画布路径直接把 `&my_font` 传给 `ql_canvas_draw_text()`。

### 10.2 动态矢量字体（PC/MPU 路线）

```c
QLFont *f = ql_font_ft_load("/path/to/NotoSansCJK-Regular.ttc", -1, 16);
ql_canvas_draw_text(&cv, f, "你好", -1, box, color, QL_TEXT_ALIGN_LEFT);
/* 用完 ql_font_destroy(f) */
```

或者设成"默认字体"，之后按字号到处用：

```c
ql_font_set_default_file("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", -1);
const QLFont *f18 = ql_font_default(18);    /* 界面层同样走这里 */
```

`qlui` 的 `font-size: Npx` 会自动走 `ql_font_default(N)`，无需额外代码。

### 10.2.1 字号缓存：`transform: scale()` 会把它撑爆（踩过的坑）

`ql_font_default(size)` 背后是一张"字号 → `QLFont*`"的小表。**槽位原来是 8 个**，
而 `transform: scale()` 期间"设备字号 = CSS 字号 × scale"每帧都在变：

```
scaleX 1.0 → 0：15px 的字会依次要 15,14,13,…,1 —— 15 个不同字号
```

第 9 个字号开始槽位就满了，而旧实现在"满了"时的做法是**造一个新字体但不缓存**：
于是**每一帧都要重新加载一次 TTF 字面、重新光栅化所有字形**，而且从不释放。

实测（`example_showcase_window`，520×400，一页 6 段动画，其中两段带缩放）：

| | 每帧耗时 |
|---|---|
| 时刻不动（字号不变） | 0.567 ms |
| 每帧推进 16ms（字号连续变） | **8.077 ms** |
| 修好之后 | **0.570 ms** |

两处改动：

1. 槽位 8 → **32**（覆盖一整段 1.0→1.5 缩放），可 `-DQL_FONT_DEFAULT_SLOTS=` 覆盖；
2. 满了以后**轮转淘汰最老的一个**并 `ql_font_destroy`，不再泄漏。

> 教训：**"缓存满了就绕过去"等于把性能问题和内存泄漏一起藏起来**。
> 凡是"按尺寸/按参数取缓存对象"的接口，都必须有一个明确的"满了怎么办"。

---

## 10.5 动态字体是怎么工作的（M2）

### 数据布局

```c
QLFont {
    glyphs[i]         按 codepoint 升序（插入时 memmove 保持有序）
    glyph_bitmaps[i]  第 i 个字形的 8bpp 位图（每行 box_w 字节，独立 malloc）
    backend           &s_ft_backend
    impl              QLFTFont{ FT_Library, FT_Face, 文件内存 }
}
```

关键点：**静态字体与动态字体共用同一套读取接口**。
`ql_font_glyph_row()` 内部判断 `glyph_bitmaps != NULL` 决定从哪取行指针，
所以画布、`QLDraw2D`、GPU 图集三条绘制路径完全不需要区分字体类型。

### 查找流程

```
ql_font_find_glyph(font, cp)
  ├─ backend == NULL → 纯二分（静态字体，永不分配）
  └─ backend != NULL → 先二分，未命中再 backend->ensure_glyph()
                         ├─ FT_Get_Char_Index  为 0 → 字体没这个字，返回 NULL
                         ├─ FT_Load_Glyph + FT_Render_Glyph（8bpp 灰度）
                         ├─ memcpy 出位图（FreeType 的 buffer 下次渲染就失效）
                         └─ insert_glyph()：扩容 + 二分插入点 + memmove 保持有序
```

`find_glyph` 返回的指针在下一次查找前有效（插入可能 realloc）。
绘制路径都是"取一个、画一个"，天然满足。

### 字形缓存上限

`ql_font_ft_set_cache_limit(N)`（全局，默认 2048）。满了之后**拒绝新字形**并
告警一次，而不是做 LRU 淘汰——交互场景里淘汰会导致同一屏内的字形反复
光栅化，卡顿比"少画一个生僻字"更糟。要更高上限就调大或按页准备子集。

### GPU 图集（M2 起是动态的）

- 图集 1024×1024 R8（1 MB），货架（shelf）分配；
- **启动时**把两个内置位图字体的字形一次性拼进 CPU 镜像、一次 `glTexImage2D` 上传
  （比逐字形上传快一个数量级）；
- 之后动态字体新出现的字形走 `glTexSubImage2D` 增量上传，小字形先拼进
  64×64 临时缓冲，一次调用传完；
- UV 缓存是开放寻址哈希，键是 **`(字体指针, codepoint)`**。

> ⚠️ 踩过的坑：UV 缓存最初用"字形在数组里的下标"做键。动态字体的缓存表是
> **插入后仍保持按 codepoint 有序**的，新字形插到中间会把后面所有字形挪位，
> 下标不是稳定标识 —— 表现为"部分文字被画成别的字"。
> 任何按下标索引动态字形的缓存都必须改按键值（codepoint）。

### 4bpp 行打包的坑

静态字体可能是 4bpp/1bpp（每行 `(box_w+1)/2` 或 `(box_w+7)/8` 字节）。
往 8bpp 图集搬像素时**不能** `memcpy(box_w)`：奇数宽度时 stride < box_w，直接越界
（ASan 抓到的就是这个）。统一走 `fetch_glyph_row()`：bpp==8 走 memcpy，
其他 bpp 逐像素用 `ql_font_glyph_alpha()` 展开。

---

## 11. 建议的阅读 / 验证顺序

1. `ctest --test-dir build --output-on-failure` 全绿；
2. `./build/example_text`，打开输出的 PPM，先看结果；
3. 读 `test/text/test_font.c` —— 字形查找、缺字、UTF-8、度量的可执行说明；
4. 读 `test/text/test_str.c` —— 两种策略的容量行为；
5. 读 `test/text/test_label.c` —— 所有权、自动尺寸、对齐的像素级断言；
6. 读 `src/text/qlfont.c` 与 `src/draw/qlcanvas.c` 末尾的文本部分。
