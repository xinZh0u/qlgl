# QLUIText API 手册

运行期把 HTML + CSS 编译成 `QLUIDoc`。头文件：`qlgl/qluitext.h`（已被 `qlgl/qlgl.h` 包含）。

---

## 1. 编译开关

```cmake
-DQLGL_BUILD_TEXT_PARSER=OFF     # 默认 ON
```

关掉它不会破坏任何其它接口，只是 `ql_ui_load_text` 不再存在（链接期缺失）。
MCU 侧应当关掉：界面走构建期生成的静态表，运行期没有任何文本解析代码。

---

## 2. `ql_ui_load_text`

```c
QLUIDoc *ql_ui_load_text(const char *html, const char *css);
```

| 参数 | 说明 |
|---|---|
| `html` | NUL 结尾的 UTF-8 文本，可以为 `""`（但没有元素就会报错）；至少要有**一个**根元素 |
| `css` | NUL 结尾的 UTF-8 文本，可以为 `""` |

**返回**

* 成功：`QLUIDoc *`，由 arena 持有。用完调 `ql_ui_doc_free(doc)`；
  也可以直接交给 `ql_ui_build(&doc)`——`ql_ui_build` 返回的 `QLUIRoot`
  不会接管 doc 的所有权（`ql_ui_free(ui)` 之后仍要 `ql_ui_doc_free(doc)`），
  除非用 `ql_ui_load_file()`（那条路径会接管）。
* 失败：`NULL`。原因用 `ql_ui_text_last_error()` 取。

传进来的两个缓冲**不会被引用**，返回后调用方可以立刻释放。

**典型用法**

```c
char *html = read_file("ui/app.html");
char *css  = read_file("ui/app.css");

QLUIDoc *doc = ql_ui_load_text(html, css);
free(html);
free(css);

if (doc == NULL) {
    fprintf(stderr, "界面编译失败：%s\n", ql_ui_text_last_error());
    return 1;
}

QLUIRoot *ui = ql_ui_build(doc);
ql_ui_layout(ui, 800, 600);
/* ... 主循环 ... */
ql_ui_free(ui);
ql_ui_doc_free(doc);
```

**支持的 HTML（78 个标签，HTML5 共 114 个）**

| 类 | 标签 |
|---|---|
| 容器 / 结构 | `div` `section` `header` `footer` `nav` `main` `article` `aside` `figure` `figcaption` `blockquote` |
| 标题 | `h1` `h2` `h3` `h4` `h5` `h6` |
| 文本块 / 行内 | `p` `span` `b` `i` `em` `strong` `u` `s` `mark` |
| 列表 | `ul` `ol` `li`（结构与缩进；**不画项目符号**，本库没有生成内容） |
| 空元素 | `hr`（块级细线）、`br`（换一行） |
| 控件 | `button`（只是一个样式化的盒子，点击靠事件） |
| 输入控件 | `input`（`type` 只支持 `text` / `password`）、`select` + `option` + `optgroup`（覆盖式面板） |
| 进度 / 度量 | `progress` / `meter`（`value`/`max` → 完成度；UI 那边的池见 QLUI 手册） |
| 折叠 | `details` / `summary`（`open` 是布尔属性，写不写值都算） |
| 元数据 | `base` `head` `link` `meta` `style` `title`（解析期整段跳过，不进树；`<style>` 内容不解析） |
| 纯语义 | `html` `body` `a` `abbr` `code` `kbd` `samp` `var` `q` `dfn` `cite` `data` `time` `small` `bdi` `bdo` `del` `ins` `dl` `dt` `dd` `menu` `address` `hgroup` `search` `form` `label` `fieldset` `legend` `wbr` |

属性只看 `class` / `id` / `style` / `value` / `type`；注释与 `<!DOCTYPE>` 跳过。
混合内容（`<p>a<b>b</b></p>`）在编译期消解成匿名 `span`，
**行内元素之间的空白会保留成一个空格**（`<b>a</b> <i>b</i>` 不会变成 `ab`）。
标签名可用 `ql_ui_tag_name(node->src->tag)` 取回。

**支持的 CSS**：与构建期工具完全一致（同一套属性白名单、同一个选择器子集）。
详见 [QLUI API手册 §3](../ui/QLUI%20API手册.md)。

---

## 3. `ql_ui_text_last_error`

```c
const char *ql_ui_text_last_error(void);
```

返回**上一次** `ql_ui_load_text` 失败的原因，形如：

```
css:12: 不支持的属性 'position'
css:3: @keyframes 'pulse' 没有任何关键帧
<节点>: 不支持的 display 'grid'
文档必须有且只有一个根元素，当前 2 个
```

成功时不改动（所以"上一次的错误"可以被后面的成功调用掩盖——只在失败分支里读它）。
永不返回 `NULL`；没有错误时返回 `"no error"`。

> 行号是**去掉注释之后**的文本行号（与构建期工具一致）。

---

## 4. `ql_ui_text_set_name`

```c
void ql_ui_text_set_name(const char *name);
```

设置下一次编译写进 `QLUIDoc.name` 的名字（默认 `"text"`）。传 `NULL` 复位。

名字会影响编码后的 blob（`name_len` + 名字段），所以：

* 想与构建期产物**逐字节比对**，必须用同样的名字
  （CMake 传的是 `--name flex_<case>`）；
* 否则名字只是日志/热重载匹配用的标签。

---

## 5. 与 blob 的关系

```c
QLUIDoc *doc = ql_ui_load_text(html, css);
ql_ui_doc_save(doc, "app.qlui");   /* 存成 blob，之后加载不用再解析 */
```

`ql_ui_doc_encode()` / `ql_ui_doc_save()` 对 `ql_ui_load_text()` 的产物同样可用：
它的内部结构与构建期生成的 doc 完全一致（扁平数组 + `uint16` 下标），
所以"运行期解析一次 → 缓存成 blob"是很自然的用法。

---

## 6. 上限（`qlconfig.h`）

| 宏 | 默认 | 说明 |
|---|---|---|
| `QL_CFG_UI_MAX_DEPTH` | 32 | 元素嵌套深度 |
| `QL_CFG_UI_MAX_NODES` | 4096 | 节点数 |
| `QL_CFG_UI_MAX_STYLES` | 4096 | 去重后的样式条数 |
| `QL_CFG_UI_MAX_TEXT_NODES` | 4096 | 去重后的文本条数 |
| `QL_CFG_UI_MAX_TEXT_BYTES` | 64 KB | 文本池字节数（偏移是 `uint16`） |
| `QL_CFG_UI_MAX_FONT_SIZES` | 256 | 不同字号的数量 |
| `QL_CFG_UI_MAX_CSS_RULES` | 2048 | 规则数（逗号分隔的选择器各算一条） |
| `QL_CFG_UI_MAX_DECLS` | 128 | 单条规则 / 单个元素的声明数 |
| `QL_CFG_UI_MAX_SEL_PARTS` | 16 | 复合选择器的段数 |
| `QL_CFG_UI_MAX_ANIMS` | 256 | `@keyframes` 段数 |
| `QL_CFG_UI_MAX_KEYFRAMES` | 4096 | 所有段的关键帧总数 |

超过任何一条都返回 `NULL` 并报错（不会截断）。

---

## 7. 保证

1. **同一份源文本，与构建期 `qltext` 产出的 blob 逐字节相同**（同一段代码）
   （`test_text_parse`：27 个页面的完整用例集）。
2. **接受/拒绝集合一致**（`test_text_reject`：32 个正负用例）。
3. 没有静态可变状态（除了 `ql_ui_text_set_name` 的名字与错误缓冲），
   同一进程里可以反复编译不同页面；返回的 doc 互相独立。
