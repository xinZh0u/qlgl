# Web 兼容设计（目标：HTML + CSS + JS 直接搬过来用）

> 本文回答两个问题：**"完美兼容"能不能做到**、**要做到"基本不用改"需要付出什么**。
> 结论先行：**方向可行，但"完美兼容"不可行**；可以做到的是"**Web 子集 + 可度量的兼容率**"。
> 文中所有代码量/时间都是按单人全职、含测试估算的**工程估计**，不是承诺。

---

## 0. 一句话结论

| 你想要的 | 现实 |
|---|---|
| 任意 HTML/CSS/JS 页面直接跑 | ❌ 这等于重做一个浏览器（Blink/WebKit 是千万行级工程） |
| 自己写的页面（用了常见 HTML/CSS/JS）基本不改 | ✅ 可行，代价是 **3~4.5 万行 C** + **6~12 个月** |
| 不支持的属性/标签**立刻报错**而不是静默错位 | ✅ 可行，且必须做，这是"不用改"的前提 |
| 同一份页面在 MCU 上跑 | ❌ 与全量 Web 兼容冲突，需分级裁剪（见 §7） |

所以务实的目标应该写成一句可验收的话：

> **给定一个"Web 子集"页面，QLGL 的渲染结果与 Chromium 的差异在既定阈值内，未支持的属性在构建期报错。**

---

## 1. 先说清楚：照搬 Web 意味着放弃现在的"控件类"

这是最重要的一条，**会决定后续所有代码的形态**。

Web 里**没有"控件类"**。浏览器里只有：

```
HTML 标签  →  元素对象（div/span/button/img/input...）
            +  UA 样式表（浏览器内置的默认 CSS）
            +  属性/类名/id
            +  布局算法（由 display 等 CSS 决定）
```

`<button>` 之所以"长得像按钮"，是因为 UA 样式表给了它 `display:inline-block; padding:...; border:...; background:...`，
**不是因为有个 `QLButton` C 类**。

所以"照搬 Web"要求我们：

| 现在 | Web 模型下 |
|---|---|
| `QLWidget` 子类体系（QLButton/QLabel） | **标签名 → 元素行为**（tag handler）+ **UA 样式表** |
| `ql_object_set_property(WIDTH, 100)` | `element.style.width = "100px"` / CSS 规则 |
| `x/y/w/h` 四个 int | `auto` / `%` / `em` / `flex` 算出来的**布局结果** |
| 属性 id 区间 | 内部实现细节，对外**完全不可见**；对外是 attribute / style |
| 事件 `ql_connect` | `addEventListener` + 冒泡/捕获 + `Event` 对象 |

**影响评估**：`src/core/`（对象/事件/内存）、`src/draw/`（画布/渲染器）、`src/text/`（字体）**全部保留**；
`src/widget/qlwidget.c` 的"属性 + 绝对几何"要拆成 **DOM 元素（样式）** 与 **布局盒（几何）** 两层，
原来的 `ql_widget_*` API 变成内部实现或废弃。

> 这不是推翻重来，而是把"控件"这一层从"类继承"换成"标签 + CSS"。底下四层地基不动。

---

## 2. 分层架构（建议）

```
┌─────────────────────────────────────────────┐
│ 应用层：页面 HTML/CSS/JS（用户写的）           │
├─────────────────────────────────────────────┤
│ JS 引擎：QuickJS / Duktape（嵌入式）           │  §6
├─────────────────────────────────────────────┤
│ DOM 绑定：document/element/Event/style         │  §5
├─────────────────────────────────────────────┤
│ DOM 树：元素/文本节点/属性/类名/id             │  §4
├─────────────────────────────────────────────┤
│ CSSOM：解析 → 选择器匹配 → 级联 → 计算样式      │  §3
├─────────────────────────────────────────────┤
│ 布局：盒模型 → block/inline → flex → 几何      │  §3.4
├─────────────────────────────────────────────┤
│ 渲染：脏矩形 → 画布 → 后端（X11/Wayland/mem）   │  ✅ 已完成
├─────────────────────────────────────────────┤
│ 基础：对象/事件/内存/字体                      │  ✅ 已完成
└─────────────────────────────────────────────┘
```

---

## 3. CSS：能做到什么程度

### 3.1 分级（按"实现成本 / 收益"排序）

| 级别 | 内容 | 状态 | 代码量估计 |
|---|---|---|---|
| **C0** | tokenizer、选择器匹配、优先级、级联、继承、`!important` | 必做，其他都依赖它 | 6~9k |
| **C1** | 盒模型（margin/padding/border/box-sizing）、`display: block/inline/inline-block/none`、`width/height/min/max`、`px/%/em/rem` | 决定"能不能排版" | 8~12k |
| **C2** | flexbox（`row/column`、`justify-content`、`align-items`、`flex-grow/shrink/basis`、`gap`） | 现代页面 90% 的布局靠它 | 6~10k（自研）或 引入 Yoga |
| **C3** | `position: static/relative/absolute/fixed`、`z-index`、`overflow: hidden/auto` | 弹窗、下拉、滚动容器 | 4~6k |
| **C4** | 背景（`background-color/image`）、`border-radius`、`opacity`、`box-shadow`(可选) | 视觉还原 | 3~5k |
| **C5** | 文本（`font-*`、`line-height`、`text-align`、`white-space`、`letter-spacing`） | 已有字体基础 | 3~5k |
| **C6** | `transition`/`animation`、CSS 变量、`@media`、`grid` | **建议不做**（见 §3.3） | — |

### 3.2 选择器（C0 必做）

| 选择器 | 做 | 说明 |
|---|---|---|
| `tag` / `.class` / `#id` | ✅ | |
| `a b`（后代）、`a > b`（子代）、`a + b`、`a ~ b` | ✅ | |
| `[attr]` / `[attr=v]` | ✅ | |
| `:hover` `:active` `:focus` `:disabled` `:checked` | ✅ | 需要状态位 + 样式重算 |
| `:first-child` `:last-child` `:nth-child(n)` | ✅ | |
| `::before` / `::after`（`content`） | ⚠️ 只支持纯文本 content | 图标字体常用，值得做 |
| `:not(a)` | ✅ 简单形式 | 不支持嵌套 |
| 其他伪类/伪元素 | ❌ | 构建期报错 |

**优先级**（specificity）必须与规范一致：`(id, class/attr/伪类, tag/伪元素)` 三元组比较，
否则同一份 CSS 在两个引擎上的渲染会不一致 —— 这不是"锦上添花"，是**正确性**。

### 3.3 明确不做（并要求构建期报错）

| 不做 | 理由 |
|---|---|
| CSS Grid | 实现成本接近 flex 的两倍，且可用 flex 替代 |
| `float` 布局 | 与 flex 混用时行为诡异，现代页面基本不用 |
| `transition` / `animation` / `@keyframes` | 需要时间轴与插值引擎，属于"另做一个子系统" |
| CSS 变量（`--x` / `var()`） | 会让"运行期样式"变复杂，破坏构建期预计算的收益 |
| `@media`（响应式断点） | 目标设备屏幕固定，多数场景可用固定尺寸替代 |
| `table` 布局 | 表格算法极其繁琐，建议用 div+flex 重写 |
| `box-shadow` / 渐变 / 滤镜 | 可以后续作为独立"绘制效果"补，不阻塞布局 |
| SVG / Canvas / iframe / video / Web Components / Shadow DOM | 都是独立子系统 |

> **关键要求**：以上任何一项出现在输入里，`qcssc`/运行时都必须**报错并给出行号**，
> 而不是"忽略这条规则继续渲染"。静默降级是"PC 上看着对、板子上错位"的根源。

---

## 4. DOM 树

### 4.1 节点模型

```c
typedef enum {
    QL_NODE_DOCUMENT, QL_NODE_ELEMENT, QL_NODE_TEXT, QL_NODE_COMMENT,
} QLNodeType;

typedef struct QLNode {
    QLNodeType        type;
    struct QLNode    *parent;
    struct QLNode    *first_child;
    struct QLNode    *next_sibling;

    /* element only */
    const char       *tag;          /* "div" / "span" / "button" */
    QLAttr           *attrs;        /* 属性链表或小数组 */
    char             *id;
    char             *classes;      /* 空格分隔 */
    QLComputedStyle  *style;        /* 计算样式缓存 */
    QLLayoutBox      *box;          /* 布局结果缓存 */
    uint32_t          flags;        /* hover/active/focus/disabled/checked + dirty 位 */

    /* text only */
    char             *text;
} QLNode;
```

### 4.2 与现有对象系统的关系

现有 `QLObject`（引用计数、父子链、私有区、事件池）可以继续作为节点基类 ——
**它已经就是一个最小 DOM**（`first_child/next_sibling/parent` 与 DOM 完全同构）。
建议：

- `QLNode` 继承 `QLObject`（复用内存布局与对象树）；
- 原来的"属性 id 区间"机制保留，但只用于**内部实现字段**；
- 对外统一走 `attribute` / `style` 两层：`style` 最终映射到内部属性。

### 4.3 标签与元素行为（替代控件类）

| tag | 默认 UA 样式 | 特殊行为 |
|---|---|---|
| `div` `p` `section` | `display:block` | 无 |
| `span` `b` `i` `em` `strong` | `display:inline` | 无 |
| `button` | `display:inline-block` + 内边距/边框/背景 | `:active` 状态、发 `click` |
| `input` | `display:inline-block` | `text` 类型可编辑（需要文本输入子系统） |
| `img` | `display:inline-block` | 资源 id → 图片 |
| `ul/ol/li` | `display:block` + `list-item` | 项目符号（可用 `::before` 实现） |
| `a` | `display:inline` + 颜色/下划线 | `href` 触发导航回调（不内置跳转） |

> 这样新增"控件"的方式变成了**写 UA CSS + 可能的 tag handler**，与 Web 一致；
> 不再需要 `QL_DEFINE_WITH_PRIVATE` 一套类注册。

---

## 5. DOM API 与事件（对齐 Web）

### 5.1 必须提供的 API（按使用频率）

```js
// 查询
document.getElementById / querySelector / querySelectorAll / getElementsByClassName / getElementsByTagName
// 结构
createElement / createTextNode / appendChild / insertBefore / removeChild / replaceChild
// 属性与样式
getAttribute / setAttribute / removeAttribute / classList.add/remove/toggle/contains
element.style.width = "50%" / element.style.setProperty(...)
dataset（可选）
// 内容
textContent / innerText / innerHTML（受限：只接受子集，其余报错）
// 事件
addEventListener(type, fn, {capture, once, passive}) / removeEventListener / dispatchEvent
// 布局（只读）
getBoundingClientRect / offsetWidth / offsetHeight / clientWidth / clientHeight
```

### 5.2 事件模型（必须与 Web 一致）

```
capture 阶段（根 → 目标父）
  ↓
target 阶段
  ↓
bubble 阶段（目标父 → 根）
```

- `event.target` / `currentTarget` / `eventPhase`
- `stopPropagation()` / `stopImmediatePropagation()` / `preventDefault()`
- `event.type`、`event.clientX/Y`、`event.key`、`event.detail`
- **默认行为**：`click` 在 `button` 上触发 `:active`；`a` 的 `href` 触发导航回调

⚠️ 我们现有的 `ql_emit` 已经实现了"捕获→目标→冒泡 + stop_propagation"，
**语义可以对齐**，但需要把 `QLEvent` 换成 Web 的 `Event` 对象字段命名，并补 `stopImmediatePropagation`。

### 5.3 重排 / 重绘调度

Web 语义是"改样式立即生效，但浏览器在帧边界统一重排"。
我们必须一致，否则 `offsetWidth` 读到的值会与浏览器不同：

```js
el.style.width = "100px";
console.log(el.offsetWidth);   // 浏览器：立刻拿到 100（强制同步布局）
```

实现：`getBoundingClientRect` 等读取几何的 API 前**先冲刷脏布局队列**（forced reflow）。

---

## 6. JS 引擎选型

自己写 JS 引擎 = 另一个 10 万行工程，**必须用现成的**。

| 引擎 | 许可 | 体积 | ES 版本 | 嵌入难度 | 结论 |
|---|---|---|---|---|---|
| **QuickJS** | MIT | ~210 KB 代码 + 生成器/正则 | ES2023（含 async/generator） | 单文件 4 个 .c，需 `make` 编译 | **推荐**：现代语法覆盖好，单文件易裁剪 |
| Duktape | MIT | ~200 KB | ES5.1 + 部分 ES6 | 两个文件，C89 兼容 | 备选：MCU 友好，但没有 `let/class/箭头函数` |
| MuJS | ISC | ~100 KB | ES5 | 极简 | 过于陈旧，不建议 |
| V8 / JavaScriptCore / SpiderMonkey | 各自 | 数 MB~数十 MB | 最新 | 需平台适配 | 不适合嵌入式目标 |

**建议**：默认 **QuickJS**（vendored 到 `third_party/quickjs/`，可裁剪），
MCU 目标用 **Duktape** 或直接关闭 JS。

> 说明：本机只有 duktape 运行库（无头文件）、无 quickjs，两者都需要 vendor 进仓库。
> 这是引入第三方依赖的重大决定，需要你确认（见 §9）。

---

## 7. 目标平台冲突：必须分级裁剪

你之前定的目标是 "MCU、MPU、PC、移动终端"。全量 Web 兼容与 MCU 是**冲突**的：

| 目标 | 可行内容 | 内存预算（估） |
|---|---|---|
| Cortex-M4/M7（无 MMU） | HTML+CSS 子集（C0~C2 精简），**无 JS**，预编译为二进制 | 64~256 KB |
| Cortex-A / MPU / PC / 移动 | HTML+CSS 子集 + JS（QuickJS） | 数 MB |
| PC（工具链） | 全量 + 差分测试 | 不受限 |

建议把"MCU 支持"重新定义为："**能跑 L1 静态页面子集**"，JS 只在 MPU 及以上开启。
这需要改 `qlconfig.h` 的裁剪策略（按"能力"而不是按"模块"裁剪）。

---

## 8. 怎么验证"基本不用改"——差分测试

这是把"感觉兼容"变成"数据兼容"的关键。

### 8.1 方案

```
同一份 HTML/CSS(/JS)
   ├─→ Chromium headless（参考实现）→ 截图 + 元素盒模型 JSON
   └─→ QLGL（被测实现）           → 截图 + 元素盒模型 JSON
                     ↓
        逐元素比较 x/y/w/h（阈值 ±1px）+ 逐像素比较（允许抗锯齿差异）
                     ↓
                 生成兼容率报告
```

- 参考浏览器：本机**未安装** chromium/firefox，需 `npx playwright install chromium` 或系统安装；
  测试脚本用 `node`（本机已有 v22）。
- 图片比较：可以复用现有的 PPM 导出 + `ImageMagick compare`（本机已有）。
- 用例来源：
  1. 自建用例库（每个 CSS 特性一个最小用例，便于定位问题）；
  2. **Web Platform Tests（WPT）子集**：WPT 是官方一致性测试集，可以挑选纯 HTML/CSS 的用例接入，
     这是最有说服力的"兼容率"口径。

### 8.2 验收指标（建议）

| 指标 | 目标 |
|---|---|
| 自建用例通过率（盒模型 ±1px） | ≥ 95% |
| 自建用例整图像素差 | 平均 < 2%，最大 < 8%（抗锯齿差异） |
| WPT 选定子集通过率 | 首版 ≥ 60%，逐版提升 |
| 未支持特性 | 100% 构建期报错（不允许静默降级） |

---

## 9. 分阶段路线与工作量

按"每阶段都能独立验收"切分：

| 阶段 | 内容 | 代码量（估） | 时间（单人） |
|---|---|---|---|
| **W0** | CSS tokenizer/parser + 选择器 + 级联 + 计算样式（纯数据，可单测） | 6~9k | 3~5 周 |
| **W1** | 盒模型 + block/inline 布局 + `display` + 单位换算 | 8~12k | 5~8 周 |
| **W2** | flexbox 布局 | 6~10k | 4~6 周 |
| **W3** | HTML 解析 → DOM 树 + 属性/class/id + UA 样式表 + tag handler | 5~7k | 3~5 周 |
| **W4** | DOM API + Web 事件模型 + 强制同步布局 | 4~6k | 3~4 周 |
| **W5** | JS 引擎接入（QuickJS）+ 绑定 + `requestAnimationFrame` | 5~8k | 4~6 周 |
| **W6** | 差分测试框架 + 用例库 + WPT 子集接入 | 2~3k | 2~4 周 |
| **合计** | | **3.6~5.5 万行** | **24~40 周（6~10 个月）** |

对比参考：
- 现有已完成的四层（对象/绘制/文本/后端）约 **6.3k 行**，用了约 1 天（含文档与测试）。
- LiteHTML（HTML/CSS 子集渲染器）约 2 万行 C++；Yoga（flexbox）约 1 万行 C++。
  我们的估算与之同量级，属于**合理不是乐观**。

### 9.1 最小可交付（建议的第一版）

先做 **W0 + W1 + W3 的精简版**：

- 静态 HTML + CSS，支持 `block/inline/inline-block`、盒模型、`px/%`、颜色/字体/边框；
- 不支持 flex（先用 `inline-block` + 文本对齐凑）；
- 无 JS，交互只到 `:hover/:active` 与 `click` 回调（C 侧注册）。

这个版本约 **1.2~1.8 万行、2~4 个月**，已经能渲染"看起来像 Web 页面"的东西，
并且能为后面的 flex/JS 打好接口。**建议先做这个**。

---

## 10. 需要你拍板的 4 件事

1. **是否接受"控件 = 标签 + UA CSS"**（放弃现有 QLWidget 子类体系，只保留底层四层）？
   这是照搬 Web 的前提，影响最大。
2. **是否接受引入第三方 JS 引擎**（vendor QuickJS 进仓库）？还是先做"无 JS 版"，
   JS 放到最后？
3. **MCU 支持是否降级**为"L1 静态页面子集、无 JS"？还是要坚持 MCU 上也能跑全量？
4. **第一版范围**：按 §9.1 的"最小可交付（静态 HTML+CSS，无 flex 无 JS）"，
   还是直接上 flex？

---

## 11. 附：词汇对照（避免团队沟通歧义）

| Web | QLGL 现有 | 说明 |
|---|---|---|
| element / tag | 控件 | Web 里没有"控件类"，只有标签 |
| `style.width` | `QL_WIDGET_PROPERTY_WIDTH` | 对外必须暴露成 CSS 字符串 |
| layout / reflow | 无（绝对坐标） | 需要新增布局趟 |
| repaint | 脏矩形重绘 ✅ | 已有 |
| `addEventListener` | `ql_connect` ✅ | 语义对齐即可 |
| `EventTarget` | `QLObject` 事件池 ✅ | 已有 |
| `document` | 渲染根 ✅ | |
| UA stylesheet | 无 | 需要新增 |
