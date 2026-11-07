# QLText / 字体系统 阅读指南

> 本部分包含四层：**字体**（qlfont）→ **字符串**（qlstr）→ **画布文本原语** → **文本控件**（qllabel）。
> 目标读者：要读懂或修改 `src/text/`、`src/core/qlstr.c`、文本绘制相关代码的人。
> 只想用 API，请看 [QLText API手册](./QLText%20API手册.md)。

---

## 1. 文件地图

| 文件 | 职责 | 阅读顺序 |
|---|---|---|
| `tools/qlfontgen/gen_bitmap_font.py` | 从 TTF 生成内置位图字体 | ① |
| `src/text/qlfont_ascii.c` | **自动生成**的字形数据（勿手改） | ② |
| `include/qlgl/qlfont.h` | `QLFont` / `QLGlyph` 结构与查找、度量接口 | ③ |
| `src/text/qlfont.c` | 二分查找、位图取灰度、UTF-8 编解码、文本度量 | ④ |
| `include/qlgl/qlstr.h` | 自管理字符串与两种容量策略 | ⑤ |
| `src/core/qlstr.c` | 容量增长、只读字面量转自有内存 | ⑥ |
| `src/draw/qlcanvas.c` | `ql_canvas_draw_text` / `draw_glyph`（在文件末尾） | ⑦ |
| `include/qlgl/qllabel.h` | 文本控件 | ⑧ |
| `src/widget/qllabel.c` | 属性、自动尺寸、绘制 | ⑨ |
| `test/text/*.c` | 可执行的说明 | ⑩ |

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

绘制顺序要点：标签先画背景/边框（`ql_widget_draw_default`），再画文字，所以文字在背景之上。

---

## 8. 文本控件 QLLabel

```
QLWidget（几何、背景、边框）
  └── QLLabel（文本、字体、颜色、对齐、自动尺寸）
```

| 属性 | 说明 |
|---|---|
| `TEXT` | 会被 `QLStr` **复制**，调用方的缓冲区用完即可释放 |
| `FONT` | `const QLFont *`，null 时退回 `ql_font_mono_12` |
| `TEXT_COLOR` | 文字颜色 |
| `ALIGN` | 左 / 中 / 右 |
| `AUTO_SIZE` | 为 1 时，文本或字体变化会重算宽高 |

### 自动尺寸与手动尺寸的交互

`AUTO_SIZE=1` 时，任何文本/字体变化都会调 `ql_object_set_property(WIDTH/HEIGHT)`。
如果用户想自己控制尺寸，先 `AUTO_SIZE=0` 再设宽高 —— **顺序很重要**，
否则设完宽高又改文本会被覆盖回去。

### 文本在矩形内的位置

- 水平：按 `ALIGN` 在标签宽度内对齐；
- 垂直：**永远从矩形顶部开始**，不垂直居中（v0.1 简化）。

---

## 9. 常见坑

| 坑 | 现象 | 说明 |
|---|---|---|
| 中文显示成 `?` | 字形缺失 | 内置字体只有 ASCII；需要自带 CJK 字体 |
| 文字画到控件外 | 看到文字溢出 | 不会：已裁剪到控件矩形；看不到说明控件太小 |
| 改了源字符串内容 | 标签文本也变了？ | 不会：`TEXT` 是复制语义；显示旧值说明没重新 set |
| 设置宽高无效 | 尺寸被覆盖 | `AUTO_SIZE` 还开着，先关掉 |
| 文字垂直不居中 | 文字偏上 | v0.1 只支持顶端对齐，需要自己算 `y` 偏移 |
| 长文本不换行 | 超出部分被裁 | 只支持 `'\n'`，不自动折行 |
| 自定义字体不显示 | 全是空白 | 检查 `bpp` 是否为 1/4/8，`glyphs` 是否按 codepoint 升序 |

---

## 10. 如何添加自定义字体

1. 用 `tools/qlfontgen/gen_bitmap_font.py` 的方式生成（或自己写生成脚本），
   产物是一组 `QLGlyph` + 一块 `uint8_t bitmap[]` + 一个 `const QLFont`；
2. 要求：
   - `glyphs` 按 `codepoint` **升序**；
   - 每个 `QLGlyph.data_offset` 指向 `bitmap` 内自己那段；
   - `bpp` ∈ {1,4,8}，行字节数符合 `(box_w+7)/8` / `(box_w+1)/2` / `box_w`；
3. 使用时 `ql_label_set_font(label, &my_font)`。

---

## 11. 建议的阅读 / 验证顺序

1. `ctest --test-dir build --output-on-failure` 全绿；
2. `./build/example_text`，打开输出的 PPM，先看结果；
3. 读 `test/text/test_font.c` —— 字形查找、缺字、UTF-8、度量的可执行说明；
4. 读 `test/text/test_str.c` —— 两种策略的容量行为；
5. 读 `test/text/test_label.c` —— 所有权、自动尺寸、对齐的像素级断言；
6. 读 `src/text/qlfont.c` 与 `src/draw/qlcanvas.c` 末尾的文本部分。
