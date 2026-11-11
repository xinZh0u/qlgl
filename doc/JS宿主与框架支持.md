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
| G2 | 宿主可以自由驱动帧与事件（不复用 `ex_run`） | 用 `uv_timer` 泵帧的 demo 跑满 60fps |
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

ql_err_t ql_ui_node_create (QLUIRoot *ui, QLUINodeId parent,
                            uint16_t style, QLUINodeId *out);      /* 分配 + 挂到 parent 末尾 */
ql_err_t ql_ui_node_destroy(QLUIRoot *ui, QLUINodeId id);          /* 子树整体回收 */
ql_err_t ql_ui_node_insert (QLUIRoot *ui, QLUINodeId parent,
                            QLUINodeId child, QLUINodeId before);  /* 0xFFFF = 末尾 */
ql_err_t ql_ui_node_set_text(QLUIRoot *ui, QLUINodeId id, const char *utf8);
ql_err_t ql_ui_node_set_style(QLUIRoot *ui, QLUINodeId id, uint16_t style);
ql_err_t ql_ui_node_set_class(QLUIRoot *ui, QLUINodeId id, uint32_t class_mask);
ql_err_t ql_ui_node_set_inline(QLUIRoot *ui, QLUINodeId id, const char *decls); /* "color:red;..." */
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

## 10. 风险

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| 运行期建树把"零 malloc"搞崩 | 中 | MCU 档退化 | 定容节点池 + 池满即失败 + 断言；P1 必须跑 UBSan/ASan |
| 类组合爆炸导致预计算表过大 | 中 | 体积涨 | 组合数设上限，超出走现场级联；组件库层面约定少类名 |
| 长 JS 任务掉帧被当成"qlgl 性能差" | 高 | 口碑 | 帧时间拆分（`qlperf`）里区分"JS 耗时 / 布局 / 绘制"，文档写清 |
| 绑定层跨平台依赖（EGL/Wayland/X11） | 中 | 安装困难 | 预编译二进制 + 明确的运行期依赖报错 |
| 框架版本漂移（reconciler 内部 API 变动） | 中 | 维护成本 | P2 锁版本；JS 侧适配层单独一个包，可独立升级 |
| 需求漂移成"做浏览器" | 中 | 方向失控 | 本文 §1.2 非目标 + [10_vs_web](tutorial/10_vs_web.md) 边界表 |

---

## 11. 待决问题

1. **宿主线程**：P0 用方案 A（同线程）是否可接受？B 的触发条件是什么（例如"UI 出现 >2000 节点"）。
2. **框架优先级**：React 先行还是 Vue 先行（哪个组件库生态更贴近目标用户）。
3. **打包形态**：单进程（Node + qlgl 同进程）还是"渲染子进程 + IPC"（更稳、更慢）？
4. **文本输入**：IME 组合过程要不要暴露给 JS（`compositionstart/update/end`）？
5. **是否发布 npm 包**：发布意味着要维护版本矩阵与 ABI 承诺，需要先有 CI 跨平台构建能力。

---

## 12. 与其它文档的关系

- 兼容边界：[10_vs_web](tutorial/10_vs_web.md)、[Web兼容设计](./Web兼容设计.md) §7.3
- 红线与排期：[开发规划](./开发规划.md) §6
- 绘制/脏区机制：[底层实现设计](./底层实现设计.md)、[性能诊断](./性能诊断.md)
- 构建期编译链：`tools/qlui`、[底层实现设计](./底层实现设计.md) 的 "HTML/CSS → blob" 一节
