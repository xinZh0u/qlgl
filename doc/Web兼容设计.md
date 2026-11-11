# Web 兼容设计（HTML/CSS 子集；**不做 JS，不碰 WebView**）

> 本文回答一个问题：**QLGL 和 Web 的关系到底是什么**。
> 结论先行：**借写法，不借引擎**。HTML/CSS 是界面的描述语言（构建期编译成静态数据），
> **JS 引擎、DOM API、系统 WebView 一律不做**，任何平台都一样。
> 文中代码量/时间是单人全职、含测试的**工程估计**，不是承诺。

---

## 0. 一句话结论

| 你想要的 | 现实 |
|---|---|
| 任意 HTML/CSS 页面直接跑 | ❌ 那等于重做一个浏览器引擎 |
| 自己写的页面（HTML/CSS 子集）基本不改 | ✅ 可行；缺的项**构建期报错**并给替代方案 |
| 页面里带 JS / 需要 DOM 操作 | ❌ **不做**：没有 `script` 执行、没有 DOM API（见 §6） |
| 在 PC/手机上嵌系统 WebView 兜底 | ❌ **不做**：任何平台都是本库自己绘制（见 §7.3） |
| 同一份 HTML/CSS 在 MCU 与 PC 上都跑 | ✅ 一套描述 + 一组能力包开关（见 §7.1） |

所以务实的目标是一句可验收的话：

> **给定一份 HTML/CSS 子集页面，QLGL 的渲染结果与 Chromium 的差异在既定阈值内；
> 超出子集的部分在构建期报错并给出替代写法。**

覆盖率现状按真实语料实测（23 份 CSS / 5391 条声明）：**属性名级 38%、声明级 73.6%**，
细节与批次见 [CSS3 覆盖与裁剪](./手册/ui/CSS3覆盖与裁剪.md)。

---

## 1. 现行模型：只有"标签 + 样式"，没有"控件类"

这一条曾经是"要放弃什么"，现在**已经是既成事实** —— 不用再讨论。

Web 里没有"控件类"：`<button>` 之所以像按钮，是因为 UA 样式表给了它
`display:inline-block; padding; border; background`，**不是因为有个 `QLButton` C 类**。
QLGL 现在就是照这个模型长的：

| 旧模型（已删除，仅作历史） | 现行模型 |
|---|---|
| `QLWidget` 子类体系（`QLButton` / `QLLabel`） | **标签 → 元素** + **UA 样式表** + **状态样式** |
| `ql_object_set_property(WIDTH, 100)` | CSS 声明（`width: 100px`）或行内 `style` |
| `x/y/w/h` 四个 int | `auto` / `%` / flex 算出来的**布局结果** |
| 属性 id 区间 | 内部实现细节，对外不可见 |
| `ql_connect` 信号槽 | 命中测试 + 状态位 + `ql_ui_set_click_cb()` 等回调 |

**落地结果**：`src/core/`（对象/事件/内存）、`src/draw/`（画布/绘制接口）、`src/text/`（字体）
保留；`src/widget/`（`QLWidget` / `QLLabel`）**已删除** —— "属性 + 绝对几何"被拆成
**节点 + 样式（`QLUIStyle`）** 与 **布局盒（`border_box` / `content_box`）** 两层。

> 如果某天要在 C 侧直接操作界面，走的是"改节点内容/值"那几个 API
> （`ql_ui_set_text` / `ql_ui_input_set_value` / `ql_ui_progress_set` / 滚动），
> 而不是"new 一个控件、设 x/y/w/h"。这条是架构红线。

---

## 2. 分层架构（现状）

```
┌─────────────────────────────────────────────┐
│ 应用层：页面 HTML/CSS（宿主用自己的语言写）      │
├─────────────────────────────────────────────┤
│ 解析：HTML 标签/属性 → 树（构建期或加载时一次）  │  §4
├─────────────────────────────────────────────┤
│ 样式：选择器匹配 → 级联 → 计算 QLUIStyle        │  §3
├─────────────────────────────────────────────┤
│ 布局：盒模型 → block/inline → flex → 几何      │
├─────────────────────────────────────────────┤
│ 绘制：脏矩形 → QLDraw2D → CPU 光栅 / GLES3      │
├─────────────────────────────────────────────┤
│ 基础：对象/事件/内存/字体                       │
└─────────────────────────────────────────────┘

没有的两层（刻意）：
  ✗ JS 引擎 + DOM API   —— §6
  ✗ 系统 WebView / 浏览器引擎 —— §7.3
```

---

## 3. CSS：能做到什么程度

**逐条清单搬到了独立文档**：[CSS3 覆盖与裁剪](./手册/ui/CSS3覆盖与裁剪.md)
（按模块穷举、每条给原因码、并给出 5 批计划与 CSS 能力包）。这里只留结论：

| 口径 | 现状 |
|---|---|
| 属性名级（去重） | 38% |
| **声明级（按真实出现次数加权）** | **73.6%** |
| 能直接照搬（值里不含 `var()` / `calc()` / `!important`） | 61.3% |

强项：flexbox、定位、文本、盒模型、动画；弱项集中在 `::before`/`content`（图标）、
`min/max-*`、文本修饰、四边独立边框。

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

### 3.3 分档处理（2026-11 重新定档）

**这一节原来叫"明确不做"，现在不成立了。** 原因很简单：本库是两个目标差距悬殊的档位
共用一份源码，而"不做"把一个档位的取舍强加给了另一个档位。

| 能力 | PC / Mobile 档 | MCU 档 | 说明 |
|---|---|---|---|
| **CSS Grid** | ✅ 必做（B6） | ⚙️ 能力包，默认关 | 实测 Grid 比"用嵌套 flex 拼网格"**更快**（省 wrapper 层，布局 −46%）——见[覆盖表 §11](./手册/ui/CSS3覆盖与裁剪.md) |
| **表格布局** | ✅ 必做（B7） | ⚙️ 能力包，默认关 | 后台管理界面的刚需；`table-layout: auto` 要排两遍 |
| **多栏** | ✅ 必做（B7） | ⚙️ 能力包，默认关 | 长文正文会用到 |
| **`border-image`** | ✅ 必做（B6） | ⚙️ 能力包，默认关 | 复用现有 blit，最便宜的一项 |
| **`transition` / `animation` / `@keyframes`** | ✅ 已有 | ⚙️ 可裁 | M5.5 已落地；裁掉等于"强制静止界面"，省电 |
| **CSS 变量 `--x` / `var()`** | ✅ 已有 | ✅ | **构建期替换**，运行期零成本（原判断有误，见 [CSS 开发日志 §18.1](./手册/ui/CSS3开发日志.html)） |
| **`@media` 宽度断点** | ✅ 已有 | ✅ | 构建期切成视口宽区间；原判断有误（[CSS 开发日志 §18.2](./手册/ui/CSS3开发日志.html)） |
| **`box-shadow` / 渐变 / 滤镜** | ✅ 已有 / ⏳ 排期 | ⚙️ 可裁 | 阴影与渐变早就做了；`filter` 进"离屏图层"立项 |
| **真字体栈（`@font-face` / 整形 / 双向）** | ✅ 必做（B8） | ❌ 恒裁 | 依赖 FreeType + HarfBuzz（合计 ≈2 MB）；中文以外语言与真字重全靠它 |
| **`float` 布局** | ❌ 不做 | ❌ | 与 flex 混用时行为诡异，本库没有"文字环绕" |
| **SVG / Canvas / iframe / video / Web Components / Shadow DOM** | ⚙️ EMBED 占位盒 | ⚙️ 同上 | 都是独立子系统；标签能解析、能排版，不渲染内容 |

> **关键要求不变**：档位里**没有实现**的东西出现在输入里，构建期必须**报错并给出行号**，
> 而不是"忽略这条规则继续渲染"。静默降级是"PC 上看着对、板子上错位"的根源。
> 而"MCU 档裁掉了、PC 档有"的差异由能力包表达（`QL_FEAT_CSS_*`），不靠报错文案。

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

## 5. DOM API 与事件（**不做** —— 保留此节作为"我们明确没做哪些"的清单）

> 下面是**真浏览器会提供、而本库不提供**的清单。列出来是为了两件事：
> 1. 说明"为什么带 JS 的页面搬不过来"（不是漏做，是不做）；
> 2. 如果将来有人提"加个 `querySelector` 吧"，这里能直接回答"那要先把运行期 DOM 引进来"。

### 5.1 真浏览器有、本库没有的 API（按使用频率）

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

## 6. 【定案】不做 JS 引擎

早期版本认真评估过 QuickJS / Duktape / JerryScript（都能塞进来），**结论是不做**。
不是因为接不进来，而是因为**接进来会撞三条红线**：

| 脚本要什么 | 撞哪条红线 |
|---|---|
| `el.style.color = "red"` | 运行期重算样式 → 与"样式构建期定型"冲突 |
| `appendChild()` / `innerHTML` | 运行期建/删节点 → 与"节点表静态、blob 可导出"冲突 |
| `requestAnimationFrame` 持续改状态 | 与"静止界面不重绘、按需唤醒"的省电目标冲突 |

所以：

- `<script>` 标签**整段跳过**（内容不进树、不执行），见 [EMBED 后端设计 §script](./EMBED后端设计.md)；
- 没有 DOM API，也没有 `addEventListener`；
- 交互靠**声明式**：`:hover` / `:active` / `:focus` 状态样式、`transition` / `@keyframes`、
  以及宿主 C 侧的几个回调（点击、输入、进度）。

> 需要脚本化逻辑时，正确做法是**在宿主 C 里写**，或者把状态算好再
> `ql_ui_set_text()` / `ql_ui_input_set_value()`。这不是缺陷，是本库的定位。

## 7. 【定案】平台分级：一套描述 + 一组能力包，任何平台都不接 WebView

**不是"一套实现打天下"，也不是"PC 换引擎"，而是"同一份资源、同一套渲染、不同的能力包"。**

| 平台 | 界面怎么写 | 渲染实现 | 目标 |
|---|---|---|---|
| **MCU**（Cortex-M） | HTML/CSS，`qltext cc` 构建期编译成静态 doc | QLGL 原生（自制布局/绘制） | 子集兼容 + 小体积；能力包几乎全关 |
| **MPU / 嵌入式 Linux** | HTML/CSS，构建期编译或运行期解析 | QLGL 原生 | 子集兼容；按需开包 |
| **PC / 移动（含 Linux 手机）** | HTML/CSS，**同一套** | QLGL 原生（CPU 光栅或 GL/EGL） | 全量能力包 |

### 7.0 三条红线（任何平台都一样）

1. **不嵌 WebView / 浏览器引擎**（WebView2 / WKWebView / WebKitGTK / CEF 一律不用）；
2. **不做 JS 引擎、不做 DOM API**（见 §6）；
3. 超出子集的东西**构建期报错**并给替代写法，绝不静默降级。

### 7.1 MCU：同一份 HTML/CSS，裁的是能力不是语言

> 早期结论"MCU 用纯 C 写界面"**已推翻**。原因见
> [HTML5 标签覆盖与裁剪](./手册/ui/HTML5标签覆盖与裁剪.md) §3：两套语言 = 两套界面、
> 两份教程、两倍回归，"设计师出图"在 MCU 上直接消失。

- 标签全认，`QLUITag` 枚举**不可条件化**（数值写进 blob）；
- 裁的是**代码路径**：`QL_FEAT_UI_TABLE=0` 就把表格布局整段摘掉，`<table>` 构建期报错；
- 界面在开发机上用 `qltext cc` 编译成静态 doc，MCU 侧零解析、零 CSS 匹配；
  图片默认转成目标像素格式的 `RAW`（图片 RAM 为 0）；
- CSS 侧同理：`QL_FEAT_CSS_*` 一组包，见 [CSS3 覆盖与裁剪 §21](./手册/ui/CSS3覆盖与裁剪.md)。

### 7.2 MPU：构建期编译成 C

同一份页面在构建期被 `qltext` 翻译成静态 C 数据（节点表 + 样式表 + 文本池 + 资源段），
运行期零解析。这是"设计稿 → 板子"的桥。

### 7.3 PC / 移动：**还是自己画**，不接 WebView

这是与早期设计**最重要的改变**。早期写的是"PC 高兼容就嵌系统 WebView"，现在明确：

| 方案 | 结论 |
|---|---|
| 嵌系统 WebView / CEF / Electron | ❌ **不做** —— 那就不再是 QLGL 了（1.5 GB 运行时、150~300 MB 内存） |
| QLGL 自研 HTML/CSS 子集，全平台同一套 | ✅ **现行方案** |

理由很直接：QLGL 的价值就是"小、确定性、可裁到 MCU"。一旦在 PC 上换引擎，
两端渲染必然分叉（"PC 上对、板子上错"），而这份库最贵的东西恰恰是两端一致。

代价是 PC 上也只能用子集 —— 这个代价用**覆盖率**兜：声明级 73.6%，
补完 B1~B5 后约 94.5%（详见 CSS 覆盖表 §19）。

### 7.4 多语言绑定（可选，不是必须）

宿主程序用任何语言都行，**只要通过 C ABI**。本库不为某个语言做特殊设计：

| 语言 | 方式 |
|---|---|
| C / C++ | 直接包含头文件（C11 ABI） |
| Python | ctypes / CFFI 包一层（工具与测试用） |
| 其他 | 只要 C ABI 稳定都能包 |

绑定层需要的能力很少：改文本 / 改输入框值 / 改进度 / 滚动 / 设回调。
**没有"运行期改样式"的入口** —— 那是刻意的（见 §6）。

### 7.5 所以"完美兼容"到底能不能做到

| 问法 | 答案 |
|---|---|
| 用 QLGL 自己实现，做到完美兼容？ | ❌ 不可能（= 重做浏览器） |
| 靠 WebView 做到完美兼容？ | 能，但**那已经不是本项目**（见 §7.3） |
| 那就没有出路了？ | 有：**子集 + 构建期报错 + 覆盖率可度量**。这是本库全部的设计前提 |

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

| 阶段 | 内容 | 状态 |
|---|---|---|
| **W0** | CSS tokenizer/parser + 选择器 + 级联 + 计算样式 | ✅ 已完成（缺 `::before` 之外的生成内容，见 CSS 覆盖表） |
| **W1** | 盒模型 + block/inline 布局 + `display` + 单位换算 | ✅ 已完成（`em/rem`、`calc()` 待补） |
| **W2** | flexbox 布局 | ✅ 已完成（`align-self`/`order` 待补） |
| **W3** | HTML 解析 → 树 + 属性/class/id + UA 样式表 | ✅ 已完成（114/114 标签） |
| **W4** | DOM API + Web 事件模型 | ❌ **不做**（§6） |
| **W5** | JS 引擎接入 | ❌ **不做**（§6） |
| **W6** | 差分测试框架 + 用例库 | ✅ 已有 `test_golden` / `test_gl_vs_cpu`；WPT 子集未接 |

**剩余工作不再是"搭骨架"，而是"补细节"** —— 逐条见
[CSS3 进度规划 §22](./手册/ui/CSS3进度规划.md) 的五批计划。

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
| 平台分级 | **一套 HTML/CSS + 一组能力包**，所有平台同一份描述、同一套渲染（§7） |
| WebView | **任何平台都不接**（PC / 移动也一样）—— 见 §7.3 |
| JS / DOM API | **不做**（§6） |
| HTML/CSS 处理方式 | 构建期编译成 C，或加载时解析一次（产物逐字节相同，§12） |
| 不支持的特性 | **构建期报错**，不静默降级 |
| 界面模型 | **只有一种**：标签 + UA CSS + 状态样式；`QLWidget` 类体系已删除（§1） |
| CSS 能力包 | 与标签包同构，14 个包 × 三档预设（[CSS3 覆盖与裁剪 §21](./手册/ui/CSS3覆盖与裁剪.md)） |

### 待定

1. **CSS 批次的推进顺序**：当前按 B1 → B2 → B3 → B4 → B5，B1/B2 已落地第一片
   （`min/max-*`、`!important`、`hsl()`、结构伪类、属性选择器、`::before`）。
2. **`transform: rotate()` 与真 group `opacity`** 谁先做 —— 两者都要新的光栅/离屏能力。
3. **MCU 上 `CSS_SELECTOR_ADV` 要不要保留**（建议保留：纯构建期，裁了只省几 KB 解析器）。

## 11. 附：词汇对照（避免团队沟通歧义）

| Web | QLGL 现有 | 说明 |
|---|---|---|
| element / tag | 标签（`QLUITag`）+ 节点 | 没有"控件类"，只有标签与样式 |
| `style.width` | `QLUIStyle.width`（构建期算好的 `{值, 单位}`） | 界面用 CSS 写，运行期只读 |
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

**JS 一律不做**（§6 已定案，不再有 `QL_CFG_UI_JS` 这种开关）。

| 配置 | HTML/CSS | JS |
|---|---|---|
| 任何平台 | 构建期编译成 C，或加载时解析一次 | ❌ 不支持。`<script>` 整段跳过、不执行 |

交互靠声明式（`:hover` / `:active` / `:focus` + `transition` / `@keyframes`）
与宿主 C 回调（点击 / 输入 / 进度）。
