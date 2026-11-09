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
| 同一份页面在 MCU 上跑 | ❌ MCU 走纯 C，不做 Web 兼容（见 §7 定案） |

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

| 旧模型（已删除） | 现行模型 |
|---|---|
| ~~`QLWidget` 子类体系（QLButton/QLabel）~~ | **标签名 → 元素行为**（tag handler）+ **UA 样式表** |
| `ql_object_set_property(WIDTH, 100)` | `element.style.width = "100px"` / CSS 规则 |
| `x/y/w/h` 四个 int | `auto` / `%` / `em` / `flex` 算出来的**布局结果** |
| 属性 id 区间 | 内部实现细节，对外**完全不可见**；对外是 attribute / style |
| 事件 `ql_connect` | `addEventListener` + 冒泡/捕获 + `Event` 对象 |

**落地结果**：`src/core/`（对象/事件/内存）、`src/draw/`（画布/绘制接口）、`src/text/`（字体）全部保留；
`src/widget/`（`QLWidget` / `QLLabel`）**已删除** —— "属性 + 绝对几何"被拆成
**节点 + 样式（`QLUIStyle`）** 与 **布局盒（`border_box` / `content_box`）** 两层。

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
| `:hover` `:active` `:focus` | ✅ 已实现（M1）：构建期编译成状态变体样式表，运行期只切下标 |
| `:disabled` `:checked` | ⏳ | 需要状态位 + 样式变体，机制已就绪 |
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

全量 114 个元素的现状（哪些已支持、哪些靠哪个能力包、MCU 怎么裁）见
[HTML5 标签覆盖与裁剪](./手册/ui/HTML5标签覆盖与裁剪.md)。

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

## 7. 【定案】平台分级：MCU 用 C，不追求 Web 兼容；PC 用多语言追求高兼容

这一条把"完美兼容"的答案说清楚了：**不是一套实现打天下，而是同一份资源、两套引擎**。

| 平台 | 界面怎么写 | 渲染实现 | 兼容目标 |
|---|---|---|---|
| **MCU**（Cortex-M） | HTML/CSS，**构建期编译成静态 doc**（`qltext cc`） | QLGL 原生实现（自制布局/绘制） | **子集兼容**：标签全认，能力按开关裁；追求小体积、确定性 |
| **MPU / 嵌入式 Linux** | HTML + CSS 子集，构建期编译成 C（同上）或运行期解析 | QLGL 原生实现 | Web **子集**兼容（差分测试达标） |
| **PC / 移动** | HTML + CSS + JS 原样使用 | **JS 引擎 + 系统 WebView / 浏览器引擎** | **高兼容**，接近真浏览器 |

### 7.1 MCU：**同一份 HTML/CSS**，裁的是能力不是语言

> 早期结论是"MCU 用 C 写界面"，**已推翻**。原因见
> [HTML5 标签覆盖与裁剪](./手册/ui/HTML5标签覆盖与裁剪.md) §3：分两套语言意味着
> 两套界面、两份教程、两倍回归，"设计师出图"这件事在 MCU 上直接消失。

现在的做法是**一套代码 + 一组开关**（Linux Kconfig 那套）：

- 标签全认，`QLUITag` 枚举**不可条件化**（数值写进 blob，裁枚举会让同一份 `.qlui`
  解释成不同标签）；
- 裁的是**代码路径**：`QL_FEAT_UI_TABLE=0` 就把表格布局算法整段摘掉，
  `<table>` 在构建期报"被降级"；
- 界面资源在开发机上用 `qltext cc` 编译成静态 doc，MCU 侧**零解析、零 CSS 匹配**，
  图片默认转成目标像素格式的 `RAW`（`owned = 0`，图片 RAM 为 0）；
- 在 MCU 上做 CSS 级联 + JS 引擎仍然不做 —— JS 是完全另一件事，见 §6。

### 7.2 MPU：HTML/CSS 构建期编译成 C

这是"一份页面，两端跑"的桥梁：设计师写的 HTML/CSS 在构建期被 `qltext`
翻译成静态 C 数据（节点表 + 样式表 + 文本池 + 资源段），运行时零解析。
兼容范围是**子集**，不支持项构建期报错（§3.3）。

### 7.3 PC：高兼容靠"多语言 + 真引擎"，不靠自己重写浏览器

在 PC/MCU 之外，如果要"基本不用改就能跑"，唯一务实的做法是**别自己实现 CSS/JS**：

| 方案 | 兼容度 | 代价 | 结论 |
|---|---|---|---|
| A. QLGL 自研 CSS+JS 子集 | 60~80%（永远追不完） | 6~12 个月 | 适合 MPU，不适合"高兼容" |
| B. **嵌入系统 WebView**（WebView2 / WKWebView / WebKitGTK） | ~99% | 几乎为零（系统自带） | **PC 首选** |
| C. 嵌入 CEF / Electron | ~99% | 几十 MB 体积 | 桌面大型应用可用 |
| D. QLGL 只做渲染后端，前端逻辑用 JS 绑 C API | 取决于 JS 层 | 中 | 适合"自定义控件 + 脚本"场景 |

**推荐组合**：

```
PC 端：同一份 index.html/css/js
        ├─ 默认走系统 WebView（真浏览器，兼容度 ~99%）
        └─ 需要"和嵌入式一致"时走 qlui → C（子集渲染）

MPU 端：qlui → C（子集渲染，无 JS 或 QuickJS）

MCU 端：纯 C API（无 Web 层）
```

同一份 HTML/CSS 在两端渲染出来的差异，用 §8 的差分测试量化，
保证"PC 上看到的效果 ≈ 板子上的效果"，而不是"两边各写一套界面"。

### 7.4 多语言绑定（PC/MPU）

PC 上"用多种语言"的落地方式是**给 C 核心加绑定**，而不是把核心改成脚本语言：

| 语言 | 方式 | 用途 |
|---|---|---|
| JS | QuickJS 嵌入 | 页面交互脚本、动态改样式/内容 |
| Python | ctypes / CFFI 包一层 | 快速原型、测试脚本、工具 |
| C++ | 头文件直接包含（C ABI 稳定） | 宿主程序 |
| 其他 | 只要 C ABI 稳定都能包 | — |

绑定层只需要三个入口（见 [QLObject API手册](./手册/object/QLObject%20API手册.md) §12）：
`ql_object_get_property` / `set_property` / `ql_connect`。
QLUIStyle / QLUIDoc 是 POD，也可以直接暴露给脚本层。

### 7.5 所以"完美兼容"到底能不能做到

| 问法 | 答案 |
|---|---|
| 用 QLGL 自己实现，做到完美兼容？ | ❌ 不可能（=重做浏览器） |
| 在 PC 上用系统 WebView，做到完美兼容？ | ✅ 那就是真浏览器 |
| 一份 HTML/CSS 在 PC 和板子上都跑？ | ✅ 子集内可行，用差分测试量化差异 |
| 板子上跑任意网页？ | ❌ 不可行，也不必要 |

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

## 10. 已定案 / 待定

### 已定案

| 项 | 结论 |
|---|---|
| 平台分级 | **一套 HTML/CSS + 一组能力开关**，任何平台都用同一份界面描述；PC 若追求高兼容可另接 WebView（§7） |
| HTML/CSS 处理方式 | **构建期编译成 C 文件**，运行时零解析（§12） |
| 不支持的特性 | **构建期报错**，不静默降级 |
| 控件模型 | **只有一种**："控件 = 标签 + UA CSS + 状态样式"。`QLWidget` 类体系已删除 |

### 待定（下一步要拍板）

1. **PC 端要不要真的接系统 WebView**（Windows WebView2 / Linux WebKitGTK / macOS WKWebView）？
   接的话 QLGL 在 PC 上就退化为"与嵌入式同款的备用渲染器 + 工具链"，而不是主渲染器。
2. **MPU 端要不要 JS**？如果要，用 QuickJS（vendor 进仓库，约 210KB 代码）。
3. **第一版做到哪**：目前 `tools/qlui` 已能跑通"block + inline + 盒模型 + px/%"，
   下一块是 **flexbox**，还是先补 **CSS 选择器/伪类**（`:hover` 等状态样式）？

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

---

## 12. 【定案】HTML/CSS 编译成 C 文件，运行时零解析

对应 Qt Creator 的做法：`.ui`（XML）→ `uic` → `ui_xxx.h`/`.cpp` 编译进程序。
QLGL 完全照搬这个模型：

```
ui/index.html  ┐
ui/index.css   ├─→ qltext (tools/qltext) ─→ build/ui_index.c ─→ 编译器 ─→ .rodata
ui/index.js    ┘        （构建期）          build/ui_index.h
```

### 12.1 为什么这样最合适

| 好处 | 说明 |
|---|---|
| 运行时零解析 | 不做 CSS tokenize/选择器匹配/级联，省下 1 万行代码与全部解析开销 |
| MCU 可承受 | 数据躺在 `.rodata`（Flash），RAM 只放计算后的布局框 |
| 编译器把关 | 生成的是 C，类型错误、越界、未定义符号在**编译期**就暴露 |
| 可裁剪 | 生成的 C 只含实际用到的元素与样式，不用整个 CSS 引擎 |
| 与现有工具链一致 | `qlgen`（类型 id）、`qlfontgen`（字体）已经都是"构建期生成 C" |

**代价**：运行期不能改 CSS（不能 `el.style.width=...` 走完整 CSS 语义）。
折中方案（见 §12.5）：允许运行期改**少量**属性，走"直接改布局框"的快路径，不走级联。

### 12.2 生成物形状

生成器输出的 `ui_index.c` 只包含数据，不含逻辑：

```c
/* 由 tools/qlui 生成，勿手改 */
#include <qlgl/qlgl.h>

static const QLUINode ui_index_nodes[] = {
    /*  tag            parent first  next   style  text  class */
    {  QL_UI_TAG_DIV,    0xFFFF,     1, 0xFFFF,     0, 0xFFFF,    0 },  /* #root */
    {  QL_UI_TAG_P,         0, 0xFFFF, 0xFFFF,     1,     0,    0 },
};
static const QLUIStyle ui_index_styles[] = {
    { .display = QL_UI_DISPLAY_BLOCK, .width_kind = QL_UI_UNIT_AUTO, ... },
};
static const char ui_index_text[] = "Hello\0";
static const uint16_t ui_index_text_ofs[] = { 0 };

const QLUIDoc ui_index_doc = {
    .name = "index",
    .node_count = 2,
    .nodes = ui_index_nodes,
    .styles = ui_index_styles,
    .text_pool = ui_index_text,
    .text_offsets = ui_index_text_ofs,
};
```

配套 `ui_index.h`：

```c
#ifndef UI_INDEX_H
#define UI_INDEX_H
#include <qlgl/qlgl.h>
extern const QLUIDoc ui_index_doc;
#endif
```

### 12.3 运行时要做的三件事

```c
QLUIRoot *ui = ql_ui_build(&ui_index_doc);        /* 1. 物化：静态表 → 运行期节点树 */
ql_ui_layout(ui, 320, 240);                       /* 2. 布局：算出每个节点的绝对矩形 */
ql_ui_render(ui, canvas);                        /* 3. 绘制：背景/边框/文本 */
```

- **物化**：一次性分配，之后零 malloc；
- **布局**：block + inline-block 流式布局（v0 不含 flex，见 §13）；
- **绘制**：按树顺序画，父先子后。

### 12.4 构建集成

```cmake
set(UI_SRC  ${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html
            ${CMAKE_CURRENT_SOURCE_DIR}/ui/index.css)
set(UI_GEN  ${CMAKE_CURRENT_BINARY_DIR}/ui_index.c
            ${CMAKE_CURRENT_BINARY_DIR}/ui_index.h)

# qltext 是宿主工具（链库自己的解析器），所以不需要 Python
add_executable(qltext tools/qltext/qltext.c)
target_link_libraries(qltext qlgl)

add_custom_command(
    OUTPUT  ${UI_GEN}
    COMMAND qltext cc --html ui/index.html --css ui/index.css
            --out ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${UI_SRC} qltext
    COMMENT "编译 HTML+CSS 为 C 文件")

add_executable(app main.c ${UI_GEN})
target_link_libraries(app qlgl)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
```

改 HTML/CSS → `cmake --build` 自动重新生成 → 重新编译。与 Qt 的 `.ui` 体验一致。

### 12.5 运行期改样式的边界

Web 里 `el.style.width = "50%"` 会触发级联 + 重排。我们**不做级联**，只做：

| 操作 | 支持 | 实现 |
|---|---|---|
| 改文本 `textContent` | ✅ | 改文本指针 + 重算该节点尺寸 |
| 改样式类 `.classList.add` | ❌ | 依赖选择器匹配，运行期不做 |
| 改内联样式 `style.width` | ⚠️ 仅限"直接改布局框" | 改 `QLUIStyle` 的副本字段 + 重排 |
| 增删节点 | ⚠️ 有限 | 只允许在预留槽位内（生成期预留 `max_nodes`） |

> 这条边界必须写进 API 文档：**页面结构在构建期固定，运行期只能改内容与少量尺寸**。
> 这与"MCU 静态界面"的定位是一致的；需要完全动态的界面请用 PC/MPU 配置。

### 12.6 JS 怎么办

JS 无法编译成 C（除非用静态分析把脚本转成 C，复杂度远超收益）。所以：

| 配置 | HTML/CSS | JS |
|---|---|---|
| `QL_CFG_UI_JS = 0`（MCU/默认） | 构建期编译成 C | 不支持；交互用 C 侧注册回调（`ql_ui_on_click`） |
| `QL_CFG_UI_JS = 1`（MPU/PC） | 构建期编译成 C | 运行期嵌入 QuickJS，DOM 绑定见 §5 |

也就是说：**"HTML+CSS 零解析"已经定案；JS 作为可选能力，按配置裁剪。**
