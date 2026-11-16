# QLGL JS 宿主与前端框架支持（设计草案）

> 状态：**草案，未排期**。本文只描述方案与边界，不代表已承诺实现。
>
> 一句话：**qlgl 依旧是纯 C、依旧不含 JS 引擎**；把 qlgl 当成"原生 UI 工具包"，
> 由 Node.js 这类宿主进程通过 N-API 调用它 —— 于是 HTML/CSS/JS 三件套里，
> HTML/CSS 由 qlgl 负责（构建期编译），JS 由宿主负责（V8/QuickJS 都行）。
> 再进一步，Vue/React 用**自定义渲染器**（而不是 DOM 仿真）接到 qlgl 上。

---

## 1. 目标与非目标

### 1.1 目标

| # | 目标 | 验收形态 |
|---|---|---|
| G1 | qlgl 的 C API 能被 Node.js 直接调用（N-API 绑定） | `require('qlgl')` 能开窗、加载 `.qlui`、跑帧、收事件 |
| G2 | 宿主可以自由驱动帧与事件（不复用 `QLWindow` 的主循环） | 用 `uv_timer` 泵帧的 demo 跑满 60fps |
| G3 | 运行期可改界面（建/删/移动节点、改文本、换样式） | JS 里点击按钮 → 新节点出现且**只重画脏区** |
| G4 | 用 Vue/React 写界面，产物落到 qlgl | 一个 Todo 示例：组件化写法、无 DOM API |
| G5 | MCU / 移动档**零影响** | 关掉开关后编译产物与今天逐字节相同 |

### 1.2 非目标（明确不做）

| # | 不做 | 原因 |
|---|---|---|
| N1 | 在 qlgl 内部嵌 JS 引擎 / WebView | 保持纯 C、可裁剪、`-Werror` 无外部运行时（沿用 [开发规划 §6](./开发规划.md) 红线） |
| N2 | 模拟浏览器 DOM / CSSOM（jsdom 路线） | 那等于自己写半个浏览器，且要求运行期选择器匹配——直接违背红线 |
| N3 | "现成网页/网页库零改动移植" | qlgl 的布局/选择器是 Web 的**子集**，边界见 [10_vs_web](tutorial/10_vs_web.md) |
| N4 | 在 MCU 上跑 JS | V8 几十 MB、QuickJS 也需要几百 KB + 堆；MCU 档只保留"构建期 HTML/CSS" |
| N5 | 多语言绑定一次做全 | 先把 Node 这条打通，其它宿主（Python/Rust）等 API 稳定后照抄 |

---

## 2. 定位：qlgl 是"工具包"，不是"运行时"

```
┌──────────────── 宿主进程（Node.js，V8 跑应用逻辑）────────────────┐
│  Vue/React 组件                                                   │
│        │ 自定义渲染器（react-reconciler / createRenderer）        │
│        ▼                                                          │
│  qlgl 绑定层（N-API，薄）                                         │
│        │ 建节点 / 改 class / setText / 读 rect / 注册回调          │
└────────┼──────────────────────────────────────────────────────────┘
         ▼
┌──────────────── qlgl（纯 C）──────────────────────────────────────┐
│ 布局 · 绘制（CPU/GL）· 命中测试 · 输入/IME/剪贴板 · 脏区          │
└───────────────────────────────────────────────────────────────────┘
         ▲ 每帧：poll 事件 → 输入派发 → （必要时）布局 → begin/render/end
└────────┴── 构建期：HTML+CSS → .qlui（含"类名→样式"查表）
```

三个关键判断：

1. **帧与事件已经可被外部驱动**。`ql_gfx_gl_begin()/begin_rects()` → `ql_ui_render*()` →
   `ql_gfx_gl_end()` 是非阻塞的；事件走 `ql_gfx_gl_poll()` + `ql_ui_input_event()`。
   所以"换一个主循环"不需要改渲染骨架。
2. **绑定只是"最后一公里"**。真正的工程量在"运行期可变"（§4），那部分是引擎特性，
   与 JS 无关。
3. **框架接的是渲染器，不是 DOM**。React 用 `react-reconciler` 的 host config，
   Vue 用 `@vue/runtime-core` 的 `createRenderer`（NativeScript / uni-app 同款做法）。
   这样 `document`、`querySelector`、`window` 全都不需要。

---

## 3. 线程与帧模型

### 3.1 两种方案

| 方案 | 结构 | 优点 | 缺点 |
|---|---|---|---|
| **A. 同线程**（推荐起步） | 窗口 + GL + qlgl 全在 Node 主线程；`uv_timer`（或 vsync 等待）每帧泵一次 | 绑定的每个调用都是**同步**的，`rect()`/`setText()` 语义干净；实现最省 | 长 JS 任务（>16ms）会掉帧；JS 卡死 = 界面卡死 |
| **B. 分离线程**（后续优化） | `worker_threads` 的 worker 独占窗口 + GL；主线程 JS 通过 `napi_threadsafe_function` 投递 | JS 与渲染互不阻塞 | **读尺寸只能返回上一帧结果**；框架的"强制布局后读"语义要改写；调试更难 |

**结论**：P0–P3 一律走 A。qlgl 单帧成本很低（本机 720p CPU 光栅 1.37ms、GL <0.5ms），
真正的风险是"JS 自己算太久"，而那是应用的问题，不是绑定也能救的。
B 作为 P4 可选优化，**必须先有"读尺寸=上一帧"的语义共识**再动手。

### 3.2 每帧的顺序（方案 A）

```c
/* uv_timer 回调，或 vsync 等待返回后 */
while (ql_gfx_gl_poll(gfx) > 0) {          /* 窗口事件 */
    /* 键盘/指针事件由 qlgl 转成 QLInputEvent，再 ql_ui_input_event() 送进去 */
}
int n = ql_ui_consume_dirty(ui, areas, MAX);
int full = ql_gfx_gl_begin_rects(gfx, bg, areas, n);
if (full) ql_ui_render(ui, d2d);
else      for (i < n) ql_ui_render_rect(ui, d2d, areas[i]);
ql_gfx_gl_end(gfx);
/* 帧末：跑 requestAnimationFrame 回调（下一帧才会生效） */
```

**布局时机**：宿主改树 → 标脏（`QL_UI_RT_DIRTY` + 子树标记）→ 帧开始时若有
"需要重排"标记，先 `ql_ui_layout()` 再画。**回调里不当场重排**（与现有
`set_state_chain` 的"标脏 + 帧末派发"保持一致）。

---

## 4. 引擎侧必须补的能力（真正的关口）

### 4.1 运行期树变更

现状：`doc → QLUIRoot` 之后树基本只读，只有 `ql_ui_set_text()`。

需要的最小集合（P1）：

```c
typedef uint16_t QLUINodeId;            /* 即 nodes[] 下标，0xFFFF = 无效 */

QLErr ql_ui_node_create (QLUIRoot *ui, QLUINodeId parent,
                            uint16_t style, QLUINodeId *out);      /* 分配 + 挂到 parent 末尾 */
QLErr ql_ui_node_destroy(QLUIRoot *ui, QLUINodeId id);          /* 子树整体回收 */
QLErr ql_ui_node_insert (QLUIRoot *ui, QLUINodeId parent,
                            QLUINodeId child, QLUINodeId before);  /* 0xFFFF = 末尾 */
QLErr ql_ui_node_set_text(QLUIRoot *ui, QLUINodeId id, const char *utf8);
QLErr ql_ui_node_set_style(QLUIRoot *ui, QLUINodeId id, uint16_t style);
QLErr ql_ui_node_set_class(QLUIRoot *ui, QLUINodeId id, uint32_t class_mask);
QLErr ql_ui_node_set_inline(QLUIRoot *ui, QLUINodeId id, const char *decls); /* "color:red;..." */
```

约束（对应红线）：

- **节点池预分配**：`QL_CFG_UI_MAX_NODES` 固定容量，`create` 失败返回错误而不是扩容；
  "稳态零 malloc" 靠"池满即失败 + 构建期给足上限"守住。
- **销毁不挪动其它节点**：`nodes[]` 用空闲链表，`QLUINodeId` 稳定（框架会持有它做 diff）。
- **文本/内联样式的字节池同样有上限**（`QL_CFG_UI_MAX_RT_TEXT` / `…_RT_STYLE`），
  外来字符串一律有界拷贝。

### 4.2 运行期样式：用"查表"绕开红线 ★

红线是"**不做运行期选择器匹配**"。这条不能破，但可以**换一种等价的东西**：

1. 构建期把每条规则编译进 **`类名 → 样式条目`** 的表；
2. 每个类分配一个 **bit**（`class_mask` 最多 32 个类/节点，覆盖组件库足够）；
3. 运行期"换 class" = 改一个 `uint32` + **查预计算表** 得到最终 `QLUIStyle` 索引。

两种查找方式，按命中率取舍：

| 方式 | 做法 | 代价 |
|---|---|---|
| 预计算（优先） | 构建期枚举"常见类组合 → 样式索引"，运行时 O(1)；组合数超过上限就对剩余组合留空 | 体积略增；组合爆炸有上限兜底 |
| 现场级联（兜底） | 用构建期留下的"规则表 + 特异性/书写顺序"，对当前 mask 走一遍级联 | 与"运行期匹配"不同：**不做选择器匹配**，只对已知类集合做排序合并 |

> 说明：级联兜底**不解析选择器**（选择器在构建期已经消解成"类集合 + 优先级"），
> 所以红线"不做运行期 CSS 匹配"依然成立。这一点必须在实现时写进代码注释，
> 否则以后容易被误读成"打开了运行期匹配"。

内联样式（`set_inline`）走**现有解析器**（`qlui_parse.c` 里那段声明解析），
写进该节点的 `style_copy`；解析器可以被编译开关整体摘掉（MCU 档必然是摘掉的）。

### 4.3 事件模型（P1）

qlgl 已有命中测试 + hover/active/focus + 点击合成。绑定层需要补的只是**语义**：

- 命中节点 → 沿父链**冒泡**，JS 侧拿到 `{ type, target, currentTarget, clientX/Y, key }`；
- 支持 `stopPropagation()`；`preventDefault()` 只对 qlgl 自己实现过的默认行为生效
  （滚动、文本框输入），其余情况是 no-op（文档里写清楚）；
- 捕获阶段先不做（组件库罕用，先留接口）；
- 键盘：`keydown/keyup` 从 `QLInputEvent` 映射，`key` 用 DOM 的名字表（子集）；
- `requestAnimationFrame` = 下一帧泵之前统一回调；`queueMicrotask` 交给 V8 自己。

### 4.4 增量布局：先测再优化

现状 `ql_ui_layout(ui,w,h)` 是**整树**。基准数据：211 节点全树 layout 0.034ms，
所以 P1 先**不做增量布局**，每帧需要时整树重排即可；等真实应用出现"千节点 + 高频改"
再考虑"只重排脏子树"，那时必须有 bench 支撑，不许凭感觉提前优化。

---

## 5. 绑定层设计（N-API）

### 5.1 形态

- 一个 `addon.node`：链接 `libqlgl.a` + 平台依赖（EGL/Wayland/X11/FreeType）。
- JS 句柄 = **整数节点 id**（可序列化、可跨 worker、无需 finalizer 兜底）。
- JS 侧不持有裸指针；所有调用第一参数是 `QLUIRoot *`（包在 `External` 里，一个 root 一个）。
- 版本校验：加载 `.qlui` 时比对 `QL_UI_BLOB_VERSION`，不一致**明确报错**（不静默降级）。

### 5.2 JS 侧 API 草案（命令式，供自定义渲染器使用）

```js
const qlgl = require('qlgl');

const win = qlgl.createWindow({ w: 720, h: 460, title: 'demo' });
const root = win.createRootFromFile('app.qlui');   // 或 createRootFromSource(html, css)

const id = root.createNode(root.rootId, root.styleByClass('btn')); // style id
root.setText(id, '点我');
root.setClassMask(id, root.classBit('btn') | root.classBit('btn-primary'));
root.on(id, 'click', (ev) => { /* ... */ });

root.frame();            // 手动泵一帧（示例/测试用）
win.setFrameDriver('vsync');   // 由绑定层接管帧（正式用法）
root.requestAnimationFrame((t) => { ... });
root.rect(id);           // => {x,y,w,h}（上一帧的边框盒）
```

`styleByClass(name)` / `classBit(name)` 由构建期产出的"样式表"决定；
`.qlui` 里已经带着这份表，所以**不需要在运行时再解析 CSS 文本**。

---

## 6. 框架接入

### 6.1 React（P2）

`react-reconciler` 的 host config 只实现这十来个方法即可（**不碰 DOM**）：

| host config | 落到 qlgl |
|---|---|
| `createInstance(type, props)` | `createNode` + 把 `className` 拆成 `class_mask` |
| `createTextInstance(text)` | `createNode(textNode)` + `setText` |
| `appendChild / insertBefore / removeChild` | `insert / destroy` |
| `commitUpdate(type, old, new)` | 只 diff 变化项：`setClassMask` / `setText` / `setInline` |
| `prepareUpdate` | 返回差异位图，避免整 props 重放 |

事件：`onClick` 等由绑定层的"冒泡 → JS 分发"提供，React 侧用极小的合成事件包装。
调度：`requestAnimationFrame` + `queueMicrotask`；React 18 的 `MessageChannel` 需求
用一个 `setImmediate` 垫片满足。

**工具链**：`vite-plugin-qlgl` —— 构建期做三件事：
1. 收集 CSS → 调 `tools/qlui` 的解析器产出"类名 → 样式"表 + `.qlui`；
2. 生成首帧静态树（首屏可以是"已经排版好的 HTML"）；
3. 把 JS bundle 作为外部资源交给宿主（qlgl 不关心）。

### 6.2 Vue 3（P3）

`@vue/runtime-core` 的 `createRenderer({ createElement, insert, remove, patchProp, … })`，
映射与上表一一对应。Vue 的 `patchProp` 是逐属性调用，天然适配 `setInline`。
唯一额外需求：`nextTick`（`queueMicrotask` 即可）。

---

## 7. 红线修订建议（需先批准，才动代码）

| 红线（现状） | 建议改成 | 理由 |
|---|---|---|
| 不做 DOM API / JS 引擎 | **qlgl 本体不做**；允许**宿主进程**（Node/QuickJS）提供 JS 与 DOM 子集，`desktop-js` 档启用 | 保持纯 C 与可裁剪；JS 在进程里但不在库里 |
| 不做运行期 CSS 匹配 | 允许**运行期按"类集合"查表/合并**，**禁止**运行期解析选择器 | 选择器仍在构建期消解，性能根基不变 |
| 稳态零 malloc | 运行期建树改成**定容节点池**；池满即失败（不静默扩容） | 保住 MCU 的确定性 |
| 同一份源，两个平台 | 新增平台档 **`desktop-js`**（Node 宿主）；MCU/移动档不编 JS 相关代码 | 与 `qlui --platform` 的降级报告机制一致 |

新增一条需要写进规范的约束：

> **JS 宿主是可选档**：`QLGL_BUILD_JS_HOST=OFF` 时，编译产物（`.a` 内容、golden）
> 与今天逐字节一致。

---

## 8. 里程碑与验收

| 阶段 | 内容 | 验收（可测） |
|---|---|---|
| **P0**（约 1 周） | N-API 绑定：开窗、`load .qlui`、`setText`、点击/输入回调、uv 泵帧 | Node 里跑通现有某个例程；点击后改一行文字；连续 300 帧无泄漏（ASan） |
| **P1**（2–4 周） | §4.1 树变更 + §4.2 类查找 + §4.3 事件冒泡 | JS 命令式 demo：动态列表增删 100 项，脏区只重画改动块；节点池满返回明确错误 |
| **P2**（2–3 周） | `qlgl-reconciler` + `vite-plugin-qlgl` | React 版 Todo：增删改、输入框、无 DOM API 依赖 |
| **P3**（约 2 周） | Vue `createRenderer` 适配 | Vue 版同一 Todo，行为与 P2 一致 |
| **P4**（可选） | 预编译分发、TS 类型、README、CI 矩阵；视需要做 worker 线程方案 | `npm i` 后在 Linux/macOS 上开箱跑；CI 覆盖 binding |

每阶段结束必须：`ctest` 全绿、`bench` 无回归、文档同步更新。

---

## 9. 性能与裁剪成本

三块能力都能做成**功能包**（`QL_FEAT_*`），关闭时对静态路径零成本。
唯一需要小心的是"删节点把紧凑数组打出洞"这一处，方案见 9.1。

| 能力 | 静态档（不开） | 开启后 | MCU |
|---|---|---|---|
| 运行期树变更 | 池头部字段与函数全编掉；`nodes[]` 仍由 doc 精确决定 | 删除/插入 = O(children) 链接修复，低频 | 预设全 0 |
| 运行期样式（类查表） | 查表不进 blob | 每表项 4–8B，几百项 ≈ 几 KB；切换 = 改一个 `uint32` + 查表 | 预设全 0 |
| 增量布局 | 不存在 | 属**优化**而非功能：默认关，整树 layout 仍是默认行为 | 永不启用 |

### 9.1 节点删除：为什么可以不付代价

`nodes[]` 现在是**构建期按 doc 节点数分配的紧凑数组**，布局/绘制/脏区都按
`node_count` 顺序扫。删节点若留"墓碑"，每个热循环都要多一次 flag 判断（经验值 1–3%）。

三个选项：

| 方案 | 静态路径开销 | 句柄稳定性 | 结论 |
|---|---|---|---|
| 墓碑（保留洞） | 热循环各 +1 次判断 | 稳定 | 简单，但不理想 |
| **swap-remove（紧凑）** | **0**（数组永远紧凑） | 句柄 = `(slot << k) \| 代际` | ✅ 推荐 |
| 侵入式链表 | 指针追逐，缓存不友好 | 稳定 | 不推荐 |

swap-remove 的代价集中在删除那一刻：把最后一个节点搬进空位，修 `parent/first_child/
next_sibling` 中的引用，并把该 slot 的代际 +1（旧句柄自然失效 → 顺手挡掉悬垂句柄）。
删除是低频操作，这个代价可接受；换来的是**热路径一行都不用改**。

### 9.2 内存：只有一项"不用也涨"

节点池要预留增量空间（运行期新建节点用），这是唯一"即使不开功能也占内存"的部分。
做成显式配置，避免 MCU 被默默吃掉：

```c
#ifndef QL_CFG_UI_RT_NODE_HEADROOM
#define QL_CFG_UI_RT_NODE_HEADROOM   256   /* MCU 预设设 0 */
#endif
```

### 9.3 验收阈值（写进 CI）

- `bench100` / `showcase`：全树 layout、全量重绘时间变化 **≤ 1%**
- `libqlgl.a` 文本段：关闭时 **0 字节**变化；开启时 **≤ 4 KB**
- 关闭时静态档 golden **逐字节一致**（现有 `test_golden` 已能验）
- 打开 `QL_FEAT_UI_RT_MUTATE=1` 后跑 ASan/UBSan，节点池满必须返回明确错误

### 9.4 不要用 `#ifdef` 散落实现

沿用既有做法：能力差异走 `QL_FEAT_*` + `qlui --platform` 的**构建期降级报告**，
用到了没编进来的能力就**报错**（"不静默降级"红线）。

---

## 10. 现有架构评估（JS 宿主 / Vue 前置）

> 结论：**不需要推倒重写布局、绘制、文本、动画和脏区管线**，这些算法可以原样复用；
> 但有三块属于“结构性增量”，不能靠小补丁绕过：
>
> 1. **节点身份 + 运行期树生命周期**；
> 2. **运行期样式 / class**；
> 3. **事件源（target + path + 默认行为）**。
>
> 另外还有两个容易低估的坑：**文本所有权/文本节点**，以及**删节点时所有辅助池的清理**。
> 这些能力都可以放在 `QL_FEAT_UI_*` / `QLGL_BUILD_JS_HOST` 后面，静态档和 MCU 不付成本。

### 10.1 可以直接复用的部分

| 能力 | 现状 | 结论 |
|---|---|---|
| 布局 | `ql_ui_layout()` 整树布局，递归走 `parent/first_child/next_sibling` | 复用；后续只需加“布局是否脏”的调度 |
| 绘制 | `ql_ui_render()` / `ql_ui_render_rect()`，按 `paint_box` 剪枝 | 复用 |
| 脏区 | `ql_ui_consume_dirty()` + `QL_UI_RT_DIRTY` | 复用；JS 宿主按脏区驱动即可 |
| 动画 | `ql_ui_anim_tick()`、过渡池、关键帧池 | 复用；`requestAnimationFrame` 映射到每帧 tick |
| 输入队列 | `QLInputEvent`、`ql_ui_input_event()`、触摸/捏合识别 | 复用；JS 侧只需要把原始事件再包一层 DOM 语义 |
| 文档/blob | `QLUIDoc`、`.qlui`、v39、`qltext` | 复用；运行期能力需要新增可选段 |
| 命中测试 | `ql_ui_hit_test()`、`ql_ui_area_at()` | 复用；返回句柄而不是裸指针即可 |

`ql_ui_repeat()` 已经证明“运行期复制子树”在现有布局/绘制下能工作，但它是**受限的模板克隆**：
必须在 `ql_ui_layout()` 之前调用，会重新分配整个 `ui->nodes[]`，并让所有 `QLUINodeRT*` 失效。
它可以作为参考实现，但不能直接当 React/Vue 的 `create/insert/remove`。

### 10.2 硬缺口

#### 10.2.1 节点身份与运行期树

现状：

- 所有 C API 用 `QLUINodeRT *`；`ui->nodes` 是按 `doc->node_count` 精确分配的紧凑数组；
- `ql_ui_repeat()` 会 `realloc` 整个数组，并在头文件里明确警告“之后所有 `QLUINodeRT*` 都可能失效”；
- `ql_ui_find_by_id()` 查的是 HTML `id` 字符串，不是给 JS 用的稳定节点句柄；
- 没有 `ql_ui_node_create/destroy/insert/move`；
- `QLUIRoot` 里很多辅助表按节点指针/数组下标存：`hover`、`focus`、`click_binds`、`trans`、`runs`、`anim_ev`、`scroll_drag`、`touch_scroll`、`inputs`、`images`、`progresses`、`media_nodes`、`selects`、`open_select` 等；
- `doc->node_media[]` 按节点数组下标索引；运行期新增节点没有对应项；
- `QLUINodeRT.src` 指向只读的 `QLUINode`，运行期创建的节点没有 `src` 描述。

影响：

- JS 不能持有 `QLUINodeRT *`。React/Vue 会把节点句柄存进 fiber/vnode，删节点后旧指针就是悬空的；
- 删除子树不只是修 `parent/first_child/next_sibling`，还要清掉上面所有辅助表里的指针；
- 运行期创建 `<input>` / `<select>` / `<img>` / `<progress>` 时，对应池是在 `ql_ui_build()` 时按 doc 节点数一次性分配的，没有空位；
- `ql_ui_repeat()` 的“先布局再复制”约束不满足 React/Vue 的任意提交顺序。

建议：

- 引入 JS 侧的 `QLUINodeId`（稳定的 slot + generation），C 内部只在同步调用里用指针；
- 不改静态档的 `ui->nodes` 紧凑模型；JS/运行期树模式用可选节点池：
  - 方案 A：**固定槽 + 墓碑**。句柄稳定、指针稳定，删节点只置位；整树扫描多一次 `alive` 判断；
  - 方案 B：**swap-remove 紧凑数组**。热路径零分支，但所有指针/下标都会变，句柄必须带 generation，`node_media` 等按下标存的数据也要重排。
- 两种方案都可行，建议先用 A（简单、生命周期安全），把 B 留给有 bench 证据时再优化；A 只影响 JS 档，静态档不加字段、不加分支；
- 运行期节点使用一份可写的 `QLUINode` 描述（tag/style/text/flags），或给 `QLUINodeRT` 增加运行期分支字段；不要改 `doc->nodes`；
- `ql_ui_node_destroy()` 集中清理所有辅助表，不能只修树链；
- `ql_ui_repeat()` 保持现状，作为静态模板 API；不要把它扩展成通用 diff 接口。

#### 10.2.2 运行期样式 / class

现状：

- 样式在构建期已经完全解析成每个节点的 `QLUIStyle`（`doc->styles[src->style]`）；
- `node_classes` 只是“class 名 → 节点”的查询表，**不含 class 掩码，也不含 class → 样式规则**；
- 没有 `ql_ui_node_set_class()` / `ql_ui_node_set_inline()` / `ql_ui_node_set_style()`；
- `QLUINodeRT.style_copy` 是**布局期的可写副本**，`ql_ui_layout()` 每轮开头会把 `used_copy` 的副本全部释放并复位——它不能拿来存运行期样式；
- `QLUIStyle` 约 420 字节；如果每个改样式的节点都拷一份，内存和分配成本都很高。

影响：

- Vue 的 `v-show`、`v-model` 的 class 切换、`<Transition>` 的 `v-enter-active` 类，React 的 `className` / `style` 更新，都需要运行期样式；
- 没有 class 掩码，`className` 变化无法 O(1) 定位到样式；
- 没有持久化覆盖，任何运行期样式写入都会被下一次 `ql_ui_layout()` 清掉。

建议：

- 构建期额外产出一张**类名 → 样式条目**表（可选 blob 段），每个类一个 bit；节点存 `class_mask`（最多 32 个类，组件库够用）；
- 运行期改 class = 改一个 `uint32` + 查表/级联合并，**不做运行期选择器匹配**；
- 预计算常见组合优先，组合数超限走“已知规则 + 特异性/顺序”的级联合并；两种情况都不解析选择器；
- 运行期样式和布局 scratch 必须分开：
  - `style_copy` 继续只做布局期强制尺寸；
  - 新增 `style_user` / `style_override`（或样式索引 + inline diff），由 `ql_ui_layout()` 保留；
- inline style 建议存**差异**（属性位图 + 值）或进样式缓存，不要每个节点背一份 420B 的 `QLUIStyle`；
- 运行期 class 与 `@media` 变体要一起考虑：当前 `node_media[]` 按下标存，新增节点/改 class 后要能重新选到对应断点样式；
- MCU 不启用这套表，静态档样式仍是构建期完全固化。

#### 10.2.3 事件模型

现状：

- 只有点击有节点级绑定 `ql_ui_set_node_click_cb()`；`ql_ui_set_click_cb()` 是全局兜底；
- 派发是“命中节点 → 父链找第一个绑定 → 调用并停止”，没有 `target`、没有事件对象、没有 path、没有 capture；
- 绑定表上限 `QL_CFG_UI_CLICK_BIND_MAX`（默认 16），不适合 JS 应用；
- 没有 `pointerdown/up/move`、`keydown/up`、`focus/blur`、`scroll`、`composition*` 的 JS 回调；
- `QL_INPUT_KEY_DOWN` 被输入框/下拉框直接消费，`QL_INPUT_KEY_UP` 没有处理；
- 复选框在 `ql_ui_pointer_down()` 里就切换并立即派发 click；
- `ql_ui_pointer_up()` 用“抬起点命中 + 是否存在任意 ACTIVE”判断点击，按下和抬起不是同一个节点时也可能点到抬起点；
- `ql_ui_set_area_cb()` 命中热区时**替代** click，不派发 click；
- 输入回调在文本已经改完之后触发，JS 没有机会 `preventDefault()`；
- `ql_ui_set_select_cb()` 和 `ql_ui_set_input_cb()` 共用 `ui->input_cb`。

影响：

- Vue/React 需要 `event.target` / `currentTarget`、capture/bubble、`stopPropagation`、`preventDefault`、多监听器、事件委托；
- 受控输入需要“先派发 keydown/input，未 preventDefault 再改值”的顺序；
- `onClickCapture`、`.capture`、`.once`、`.passive` 都没有承载点；
- React 的 root delegation、Vue 的逐元素 `patchEvent` 都无法直接接上；
- 点击目标语义与 DOM 不一致，框架里会出现“点 A 触发 B”的怪异行为。

建议：

- C 侧不要继续扩展“最近绑定优先”的 click bind；新增一个**事件源**：
  - `QLUIEvent { type, target, path[], flags, x/y, key, delta, time... }`
  - `ql_ui_set_event_sink(ui, sink, user)`，一次事件只跨 C→JS 一次；
  - path 是 `target → root` 的稳定 ID 快照；
- JS 绑定层实现 DOM 语义：capture → target → bubble、监听器表、`stopPropagation` / `stopImmediatePropagation` / `preventDefault`、`currentTarget`；
- C 保留 `ql_ui_set_node_click_cb()` 给纯 C/小应用；JS 模式装 sink 后由 JS 接管，避免两套冒泡打架；
- 默认行为改成“事件派发之后执行”：click 后再 toggle/打开 select/details，keydown 未 preventDefault 才插入文本，wheel/pointer 未 preventDefault 才滚动；
- `pointerdown/up` 要记录按下目标，click 只在同一目标（或 DOM 规定的最近公共祖先）上合成；
- 补齐事件类型：pointer、touch、click、keydown/up、text/input、focus/blur（focusin/focusout）、wheel/scroll、composition；
- 图像热区在 JS 事件里应表现为对 `<img>`（或 area 描述）的点击，并保持“不重复派发 click”的语义；
- 事件目标必须是元素节点，不能把内部匿名文本节点暴露成 `event.target`。

#### 10.2.4 文本所有权与文本节点

现状：

- `ql_ui_set_text()` **不复制**字符串，直接把指针存进 `node->text`；
- 有子节点的元素不能直接放文本（`first_child != NULL` 返回 `QL_ERR_INVAL`）；
- 引擎没有公开的文本节点类型；`is_anon_text` 的匿名 span 是解析器内部为了伪元素排序临时造出来的；
- 运行期字符串只有 `rep_texts` 这一处（模板 `{i}` 替换）由 root 管理。

影响：

- N-API 传进来的 JS 字符串生命周期不受 C 控制，直接存指针会悬空；
- React `createTextInstance()` / Vue `createText()` 需要文本节点；`<div>Hello <b>world</b></div>` 这种混合内容不能只靠“父节点一个 text 字段”表达；
- `ql_ui_set_text()` 对有子节点的元素直接拒绝，和框架的文本节点模型冲突；
- 每次文本变化都调用 `ql_ui_layout()` 的成本和字符串分配需要控制。

建议：

- 增加“复制进 root 所有文本池”的 API（有界、可回收），JS 绑定默认用它；`ql_ui_set_text()` 保持零拷贝给 C；
- 文本池按 root 管理，支持节点销毁时回收；文本变更只标“需要重排”，不要每次立即全树布局；
- 增加公开文本节点（或至少让 host 的 `createTextInstance` 有稳定句柄），支持元素内多段文本/混排；
- 指针事件的 `target` 做元素重定向：点到文本节点时，对 click/pointer 暴露父元素；
- 如果 P0 暂不做文本节点，就在 Vue/React 适配层明确限制“一个元素只能有一个文本子节点”，但这会限制组件库，不建议作为长期方案。

#### 10.2.5 布局/测量时序

现状：

- `ql_ui_node_rect()` 返回上一次 `ql_ui_layout()` 的结果；
- 没有 `layout_dirty` 标记；`ql_ui_set_text()` 只标 `QL_UI_RT_DIRTY`，不触发重排；
- `ql_ui_layout()` 每次全树，且会清理 `style_copy`；
- `ql_ui_repeat()` 要求“build 之后、layout 之前”。

影响：

- React/Vue 的 `ref` + `getBoundingClientRect()`/`offsetWidth` 读取要求“改完树后能立即量到”；
- 如果宿主每改一个属性就全树 layout，列表/输入场景会浪费；
- 改文本/class 后如果忘记 layout，读到的是旧几何。

建议：

- 加 `layout_dirty` / `layout_generation`：运行期修改只标脏，帧开始或第一次读 rect 时统一 layout；
- `ql_ui_node_rect()` 可以选择“需要时自动 layout”（或提供 `ql_ui_node_rect_measure()`），并在文档里写清同步代价；
- React/Vue 提交阶段批量修改，帧末统一 layout；不要在每个 `setText`/`setClass` 内部直接全树布局；
- 运行期样式覆盖不能被 layout 清掉（见 10.2.2）。

#### 10.2.6 删除节点时的辅助状态清理

这是最容易低估的一项。`ql_ui_node_destroy()` 至少要让以下位置不再指向被删节点：

- `ui->hover` / `ui->focus`；
- `click_binds[]`；
- `trans[]`（过渡槽）；
- `runs[]`（关键帧动画槽）；
- `anim_ev[]`（已排队但未派发的动画结束事件）；
- `scroll_drag` / `touch_scroll`；
- `inputs[]`（输入框池）与 `ta_arena`；
- `images[]`（`<img>` 运行期状态）；
- `progresses[]`；
- `media_nodes[]` / `media_handles[]`；
- `selects[]` / `open_select`；
- `dialog` 栈（如果有）；
- 运行期文本池、style override、inline 缓存；
- `doc->node_media` / class 表对应的运行期节点条目。

如果漏掉，表现通常不是立即崩溃，而是“删掉的输入框还能接收输入”“动画还在往已释放节点写”“下拉框展开在已删除的 select 上”。
建议把销毁逻辑集中到一个函数，并在 ASan/UBSan 下做“动态列表增删 100 项”的稳定性测试。

#### 10.2.7 工具链与 blob

现状：

- `.qlui` v39 有 `node_ids` / `node_classes`，但 class 只用于查询，不含 class 规则表；
- 没有运行期样式表、文本节点描述、运行期节点模板；
- `qltext` 生成静态 C 表；JS 宿主加载 `.qlui`；
- `QLGL_BUILD_TEXT_PARSER=OFF` 时没有运行期 CSS/HTML 解析器。

影响：

- Vue/React 的 `className` / `style` 更新没有构建期数据可用；
- 运行期创建节点要么走“预定义模板”，要么需要运行期 tag/style 描述；
- inline style 如果需要解析，桌面 JS 档要保留声明解析器（或有界的小解析器）。

建议：

- 新增可选 blob 段：类名 → 样式条目/规则、class bit、运行期节点模板；版本号 +1，旧 `.qlui` 明确拒绝；
- 静态 C 表不需要这些段，MCU 不生成/不加载；
- `qltext`/`qlui` 增加“组件库 class 清单”输出；不要运行期解析选择器；
- JS 宿主只接受带运行期段的 `.qlui`，或明确报“该文档不支持运行期 class”。

### 10.3 性能影响

| 路径 | 影响 | 说明 |
|---|---|---|
| 静态 C / MCU | **应为 0** | 所有运行期能力用 `QL_FEAT_*` 编掉；布局/绘制/文本循环不加分支 |
| JS 事件 | 很小 | 一次 C→JS sink + O(depth) path；depth 上限通常 < 32 |
| 稳定句柄 | O(1) | `slot + generation` 查表；不要做指针线性扫描 |
| class 切换 | O(规则数) 或 O(1) | 预计算组合/缓存；不要全表重算，不要做选择器匹配 |
| 运行期建删 | 低频、可接受 | React/Vue 提交阶段才发生；重点是不要每次 realloc 整个数组 |
| 文本更新 | 取决于池设计 | 避免每字符 malloc；用 root 文本池 + 脏标记 |
| 布局 | 全树 0.034ms / 211 节点量级 | 1000 节点约 0.16ms（按现有数据外推，需实测）；不要每次属性修改都全树 layout |
| 运行期样式 | **主要内存风险** | `QLUIStyle` 约 420B；不要每节点拷一份；用样式索引/diff/cache |
| 墓碑方案 | 整树扫描 +1 分支 | 估计 1–3%；只影响 JS 档，静态档编掉 |

需要守住的性能门槛：

- `bench100` / `showcase`：全树 layout、全量重绘时间变化 **≤ 1%**；
- 静态档 `libqlgl.a` 文本段 **0 字节**变化；
- 静态档 golden **逐字节一致**；
- JS 档 ASan/UBSan 下动态列表增删 100 项无泄漏、无悬空；
- 运行期样式/文本池满必须返回明确错误，不静默扩容。

### 10.4 对 MCU 的影响

只要按下面做，MCU 端**功能、RAM、性能都不受影响**：

- 默认 `QLGL_BUILD_JS_HOST=OFF`；运行期能力用独立开关：
  - `QL_FEAT_UI_RT_MUTATE`（建/删/插/移）
  - `QL_FEAT_UI_RT_STYLE`（class/inline/样式表）
  - `QL_FEAT_UI_EVENT_SINK`（事件源 + path）
  - `QL_FEAT_UI_TEXT_OWNED`（运行期文本池）
- 关闭时 `QLUIRoot` / `QLUINodeRT` / `QLUIDoc` 的字段布局保持不变；**不要无条件**给 `QLUINodeRT` 加 `id`、`class_mask`、`style_user` 指针；
  - 稳定句柄用可选表（每节点 4B 只在 JS 档分配），或从数组下标 + 可选 generation 表推导；
  - `alive` 用现有 `flags` 的空闲位（当前 bit0–bit6 已用，bit7 可用），不加字段；
  - 运行期样式/文本/辅助表也用可选表，静态档不分配。
- 节点池 headroom 默认 0：`QL_CFG_UI_RT_NODE_HEADROOM` 只在 JS/桌面档设非 0；
- 运行期 input/select/img/progress 池需要额外容量时，用显式配置，不要让 `ql_ui_build()` 在 MCU 上多分配；
- 代码放在独立文件或用 `#if QL_FEAT_*` 包住，避免静态档出现“永远为 false 的分支”；
- MCU 验收不变：`test/mcu/build.sh window`、`test_golden`、默认/San/Release/无 SVG/媒体关闭矩阵；再加一项“关闭所有 JS/RT 开关时 `.a` 与今天一致”。

**最危险的 MCU 改动**是无条件给 `QLUINodeRT` 加字段：当前约 192B/节点，4096 节点每加 4B 就是 16KB，加 16B 就是 64KB。任何新字段都必须先问“MCU 静态档真的需要吗”。

### 10.5 建议的落地顺序

1. **P0：事件源 + 稳定句柄（只读树）**
   - `QLUINodeId`、`ql_ui_node_id()` / `ql_ui_node_by_id()`；
   - `ql_ui_set_event_sink()`、`QLUIEvent`、path 快照；
   - 先支持 click / pointer / key / focus / input 的原始事件；
   - 不做树变更、不做运行期样式。这样 Node 命令式 API 可以先跑通。

2. **P1：文本所有权 + 运行期样式**
   - root 文本池 / `set_text_copy`；
   - class 表 + class mask + inline 覆盖；
   - 运行期样式与 `style_copy` 分离；
   - 文本节点或 host 文本句柄（决定 Vue/React 的文本模型）。

3. **P2：运行期树**
   - create/destroy/insert/move、节点池、辅助表清理；
   - 动态 input/select/img/progress 的池策略；
   - 布局脏标记 + 批量提交。

4. **P3：React / Vue 适配**
   - React `react-reconciler` + host config + SyntheticEvent；
   - Vue `createRenderer` + `patchProp`；
   - 到这一步再决定“React 先行还是 Vue 先行”，不要反过来用框架需求倒逼 C 核心 API。

### 10.6 需要先拍板的决策

1. 运行期树用**墓碑 + 稳定指针**，还是 **swap-remove + generation 句柄**？
2. 文本模型是**第一类文本节点**，还是“元素单文本子节点”的受限模型？
3. class 样式用**预计算组合**为主，还是**已知规则级联**为主？组合上限多少？
4. C 原生节点回调与 JS event sink 是**互斥**，还是允许同时挂并有明确优先级？
5. `ql_ui_node_rect()` 是否允许**隐式触发布局**，还是必须显式 `ql_ui_layout()`？
6. 运行期 `<input>/<select>/<img>` 用**固定池 + 池满报错**，还是桌面档允许扩容？

---

## 11. 风险

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| 运行期建树把"零 malloc"搞崩 | 中 | MCU 档退化 | 定容节点池 + 池满即失败 + 断言；P1 必须跑 UBSan/ASan |
| 类组合爆炸导致预计算表过大 | 中 | 体积涨 | 组合数设上限，超出走现场级联；组件库层面约定少类名 |
| 长 JS 任务掉帧被当成"qlgl 性能差" | 高 | 口碑 | 帧时间拆分（`qlperf`）里区分"JS 耗时 / 布局 / 绘制"，文档写清 |
| 绑定层跨平台依赖（EGL/Wayland/X11） | 中 | 安装困难 | 预编译二进制 + 明确的运行期依赖报错 |
| 框架版本漂移（reconciler 内部 API 变动） | 中 | 维护成本 | P2 锁版本；JS 侧适配层单独一个包，可独立升级 |
| 需求漂移成"做浏览器" | 中 | 方向失控 | 本文 §1.2 非目标 + [10_vs_web](tutorial/10_vs_web.md) 边界表 |

---

## 12. 待决问题

1. **宿主线程**：P0 用方案 A（同线程）是否可接受？B 的触发条件是什么（例如"UI 出现 >2000 节点"）。
2. **框架优先级**：React 先行还是 Vue 先行（哪个组件库生态更贴近目标用户）。
3. **打包形态**：单进程（Node + qlgl 同进程）还是"渲染子进程 + IPC"（更稳、更慢）？
4. **文本输入**：IME 组合过程要不要暴露给 JS（`compositionstart/update/end`）？
5. **是否发布 npm 包**：发布意味着要维护版本矩阵与 ABI 承诺，需要先有 CI 跨平台构建能力。

---

## 13. 与其它文档的关系

- 兼容边界：[10_vs_web](tutorial/10_vs_web.md)、[Web兼容设计](./Web兼容设计.md) §7.3
- 红线与排期：[开发规划](./开发规划.md) §6
- 绘制/脏区机制：[底层实现设计](./底层实现设计.md)、[性能诊断](./性能诊断.md)
- 构建期编译链：`tools/qlui`、[底层实现设计](./底层实现设计.md) 的 "HTML/CSS → blob" 一节
