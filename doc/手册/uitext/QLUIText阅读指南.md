# QLUIText 阅读指南

`ql_ui_load_text(html, css)` 把一份 HTML + CSS 文本编译成 `QLUIDoc`。

它是 `tools/qlui/qlui.py` 的 **C 版同一套语义**：同一份源文本，两边产出的 doc
编码成 `.qlui` blob 之后**逐字节相同**。

---

## 1. 它解决什么问题

构建期编译（`qlui.py`）的产物是 C 源，改一次界面就要重新编译一次程序。
`ql_ui_load_text` 让**运行期**也能吃文本，于是：

| 场景 | 做法 |
|---|---|
| 换皮肤 / 换语言 | 启动时读一份 `theme.css`，算完就是静态表，之后零解析 |
| 插件式界面 | 插件带自己的 HTML/CSS，主程序不重编译 |
| 嵌入式/无 Python 工具链的目标 | 不需要 `qlui.py` 也能出界面 |
| 单元测试写界面 | 测试里直接写字符串，不用生成 C 文件 |

代价是**加载时一次解析**（毫秒级），换来的是运行期与构建期完全相同的性能：
解析结果就是同一份静态数据，没有保留式场景图、没有每帧匹配。

---

## 2. 三条硬约束

### 2.1 只在加载时算一次

`ql_ui_load_text` 返回的 `QLUIDoc` 与构建期生成的静态 doc **结构完全一致**：
节点表、样式表、文本池、字号表、关键帧表、动画表，全是扁平数组 + `uint16` 下标。
`ql_ui_build(&doc)` 之后的一切（布局、绘制、动画）都不知道 doc 是怎么来的。

**没有**运行期 CSS 匹配，**没有**每帧解析。

### 2.2 外来数据必须有界

全库唯一吃外部文本的地方，所以每个计数/递归/尺寸都有上限（`qlconfig.h`）：

| 上限 | 默认 | 超了怎么办 |
|---|---|---|
| `QL_CFG_UI_MAX_DEPTH` | 32 | 报错（防止递归爆栈） |
| `QL_CFG_UI_MAX_NODES` | 4096 | 报错 |
| `QL_CFG_UI_MAX_STYLES` | 4096 | 报错 |
| `QL_CFG_UI_MAX_TEXT_NODES` / `_BYTES` | 4096 / 64 KB | 报错（偏移是 `uint16`） |
| `QL_CFG_UI_MAX_CSS_RULES` | 2048 | 报错 |
| `QL_CFG_UI_MAX_DECLS` | 128 | 报错 |
| `QL_CFG_UI_MAX_SEL_PARTS` | 16 | 报错 |
| `QL_CFG_UI_MAX_ANIMS` / `_KEYFRAMES` | 256 / 4096 | 报错 |

### 2.3 明确失败优于半个正确

任何不支持的东西都返回 `NULL` + `ql_ui_text_last_error()`（带 `css:行号`），
**不跳过坏数据继续画**。

---

## 3. 为什么"逐字节相同"是唯一判据

看起来"渲染结果一样"就够了，但那样抓不住这些问题：

| 差异 | 画面看得出吗 |
|---|---|
| 样式表条数不同（去重规则不一致） | 看不出，但内存占用不同 |
| 文本池里两条字符串顺序不同 | 看不出 |
| 字号表顺序不同 | 看不出，但 `font_id` 全部错位会崩 |
| 关键帧隐式帧的插入位置不同 | 可能只在中途一帧有细微差别 |
| 某个 CSS 值差 1 个色阶 | 肉眼绝对看不出 |

所以 `test_golden` + `test_text_parse` 都用**编码后的 blob 逐字节比对**。
实现上这意味着要复刻 Python 侧的每一个细节：

* 样式表的身份 = **排序后的 `(属性, 值)` 字符串集合**（不是编译后的结构体）。
  两个节点的声明集合只要字符串不同就是两条样式——哪怕编出来的 `QLUIStyle` 一样。
* 枚举顺序：字号表按**样式表首次出现顺序**登记；文本池按**节点 DFS 顺序**去重登记。
* `margin: 8px 0` 这种简写要展开成四条长写再参与身份比较。
* **空的文本池也要占一条**：构建期工具生成 `text_ofs[] = { 0 }` 占位数组
  （C 不允许零长数组），于是 `text_count == 1` 且池里是空串。

---

## 4. 代码结构（从下往上）

| 段 | 内容 | 对应 Python |
|---|---|---|
| ① | 错误与内存（`set_error` / `SB` 增长缓冲） | — |
| ② | 值解析：颜色 / 长度 / 数值 / 边距 / 渐变 / 阴影 / transform / clip-path / 时长 / flex / gap | `parse_*` |
| ③ | 声明集合 + **规则级简写展开** | `parse_decls` / `expand_shorthands` |
| ④ | CSS 词法：规则、选择器、`@keyframes` | `parse_css` |
| ⑤ | HTML 词法 + 混合内容消解 | `TreeBuilder` / `_flatten` |
| ⑥ | 级联：UA 默认 → 匹配 → 按 (specificity, order) 覆盖 → 继承 → 行内 | `collect_decls` / `finalize_style` |
| ⑦ | 编译成 `QLUIStyle` / `QLUIKeyframe` / `QLUIAnimation` | `emit()` 的样式段 |
| ⑧ | 建表：DFS 扁平化、样式去重、文本池、字号表、一次分配 arena | `emit()` 的建表段 |

### 4.1 简写必须在**单条规则内**展开

这是最容易搞错、后果最隐蔽的一条（构建期工具踩过）：

```css
.a { animation: pulse 1s }
.b { animation-name: other }
```

如果等**级联合并之后**再展开 `animation`，后出现的 `animation-name: other`
会被 shorthand 覆盖回 `pulse` —— 10 张卡片全部播同一段关键帧。
所以 `parse_decls()` 在返回前就调用 `expand_shorthands()`（`@keyframes` 块除外）。

### 4.2 HTML 的混合内容在解析期消解

```html
<p>Hello <b>world</b>!</p>
```

→ `<p><span>Hello </span><b>world</b><span>!</span></p>`

运行期的数据模型完全不用改（文档级没有"文本节点"这个概念）。
文本段的处理规则与 HTML 一致：首段 `lstrip`、末段 `rstrip`、纯空白段丢弃。

**Unicode 空白也算空白**：Python 的 `str` 版 `\s` 与 `.strip()` 认
U+00A0 / U+1680 / U+2000–200A / U+2028 / U+2029 / U+202F / U+205F / U+3000。
中文示例文本里就有一个全角空格（`空格：重播　Esc：退出`），
漏掉它会让折叠结果多留一个字符，blob 差 2 个字节。

### 4.3 一次分配 arena

解析结束后，所有区间的大小都已知，于是：

```
1 次 ql_mem_calloc(total)  ← 节点/样式/文本池/偏移/字号/关键帧/动画/名字 全在这里
1 次 ql_mem_calloc(sizeof(QLUIDoc))
```

`doc->arena` 指向第一块，`ql_ui_doc_free(doc)` 两次释放就干净了——
与 `ql_ui_doc_load_mem()` 的所有权约定完全一致（判据只有 `arena == NULL`）。

---

## 5. 已知与构建期工具的差异

| 项 | 差异 | 影响 |
|---|---|---|
| 命名 HTML 实体 | 只支持 `&amp; &lt; &gt; &quot; &apos; &nbsp;` + 数字引用；其它**报错** | Python 支持完整 HTML5 实体表。当前用例集不涉及 |
| 错误信息措辞 | 语义一致、措辞不同（`<节点>` 代替 `t:style[3]`） | 只影响排查体验 |
| CSS 里的 Unicode 空白 | 按 ASCII 空白处理 | CSS 值里出现全角空格属于异常写法 |
| 属性名大小写 | 与 HTML 一致（小写化） | 无差异 |

`test_text_reject` 用 32 个用例守着"接受/拒绝集合一致"这条线。

---

## 6. 可摘性

`src/ui/qlui_parse.c` 是独立编译单元，core 里没有任何静态引用指向它。

```cmake
-DQLGL_BUILD_TEXT_PARSER=OFF
```

不编它 = 一行代码都不进二进制，界面仍然走构建期生成的静态表。
MCU 侧应当关掉它。
