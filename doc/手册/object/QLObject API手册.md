# QLObject API 手册（对象系统 v0.1）

配套阅读：[QLObject阅读指南](./QLObject阅读指南.md) · 设计依据：[底层实现设计](../../底层实现设计.md)

- 头文件：`#include <qlgl/qlgl.h>`
- 链接：`target_link_libraries(xxx PRIVATE qlgl)`
- 语言标准：C11（编译选项为 `-std=c11`，不依赖任何 POSIX/编译器扩展）

---

## 1. 初始化

```c
ql_err_t qlgl_init(void);
void     qlgl_shutdown(void);
const char *qlgl_version(void);
```

| 函数 | 说明 |
|---|---|
| `qlgl_init` | 复位统计、清空事件池、按生成表注册内置类，并校验 id 与枚举一致。重复调用返回 `QL_OK` |
| `qlgl_shutdown` | 复位统计/类表/事件池。**不会**替你释放仍在使用的对象 |
| `qlgl_version` | 返回 `"0.1.0"` 这类字符串 |

必须在创建任何对象之前调用。初始化失败的可能原因：生成表为空、id 与枚举不一致（需重跑 `tools/qlgen`）。

---

## 2. 配置项（`qlconfig.h`）

全部可以用 `-DQL_CFG_xxx=...` 在编译期覆盖。

| 宏 | 默认 | 说明 |
|---|---|---|
| `QL_CFG_MAX_CLASS` | 64 | 类表容量 |
| `QL_CFG_MAX_INHERIT_DEPTH` | 8 | 继承链深度上限 |
| `QL_CFG_PROPERTY_CAPACITY` | 100 | 每类属性 id 数量 |
| `QL_CFG_EVENT_CAPACITY` | 100 | 每类事件 id 数量 |
| `QL_CFG_EVENT_ARG_MAX` | 4 | 单次事件参数上限 |
| `QL_CFG_EVENT_CONN_POOL` | 256 | 全局事件连接池容量 |
| `QL_CFG_EVENT_PATH_MAX` | 8 | 传播路径深度上限 |
| `QL_CFG_MEM_TRACK` | 1 | 内存统计与头部校验（关掉可省 16 字节/次分配） |
| `QL_CFG_LOG_LEVEL` | 2 | 0 关闭 / 1 错误 / 2 警告 / 3 信息 / 4 调试 |

---

## 3. 内存

```c
typedef struct { void *(*alloc)(size_t); void *(*realloc)(void *, size_t); void (*free)(void *); } QLAllocator;
typedef struct { size_t alloc_count, free_count, bytes_live, bytes_peak; } QLMemStats;

void  ql_mem_set_allocator(const QLAllocator *alloc);   /* NULL = 恢复 malloc 族 */
void *ql_mem_alloc(size_t size);
void *ql_mem_calloc(size_t count, size_t size);
void *ql_mem_realloc(void *ptr, size_t size);
void  ql_mem_free(void *ptr);
void  ql_mem_stats(QLMemStats *out);
void  ql_mem_reset_stats(void);
```

- `ql_mem_set_allocator` 应在 `qlgl_init` **之前**调用；
- 三个回调可部分为 `NULL`，缺省部分自动回落到 `malloc/realloc/free`；
- `QL_CFG_MEM_TRACK=1` 时每次分配带 16 字节头部：记录大小 + magic，重复释放/越界写会打错误日志而不是静默崩溃；
- `ql_mem_stats` 的 `bytes_live` 是**当前存活的 payload 字节数**（不含头部），测试里用它做泄漏断言。

---

## 4. 值 `QLValue`

```c
typedef enum {
    QL_VALUE_NONE, QL_VALUE_INT, QL_VALUE_UINT, QL_VALUE_FIXED, QL_VALUE_BOOL,
    QL_VALUE_STRING, QL_VALUE_POINTER, QL_VALUE_OBJECT, QL_VALUE_COLOR
} QLValueType;

typedef struct {
    uint8_t type, flags; uint16_t aux;
    union { int32_t i; uint32_t u; ql_fixed_t f; int32_t b;
            const char *s; void *p; QLObject *o; } v;
} QLValue;
```

| 构造 | 说明 |
|---|---|
| `ql_value_none()` | 空值；宏 `QL_VALUE_NULL` 等价 |
| `ql_value_int(int32_t)` | 有符号整数 |
| `ql_value_uint(uint32_t)` | 无符号整数 |
| `ql_value_fixed(ql_fixed_t)` | Q16.16 定点 |
| `ql_value_bool(int)` | 0/1 |
| `ql_value_string(const char *)` | **弱引用**，不复制、不接管所有权 |
| `ql_value_ptr(void *)` | 弱引用 |
| `ql_value_object(QLObject *)` | 弱引用，不改变引用计数 |
| `ql_value_color(uint32_t)` | 颜色值 |

| 读取（类型不符返回 fallback） | 说明 |
|---|---|
| `ql_value_get_int(v, fb)` | int/uint/bool/fixed/color 都接受 |
| `ql_value_get_uint(v, fb)` | 同上 |
| `ql_value_get_fixed(v, fb)` | int/uint 会自动换算成定点 |
| `ql_value_get_string(v, fb)` | 仅 STRING |
| `ql_value_get_ptr(v, fb)` | POINTER / OBJECT |
| `ql_value_get_object(v, fb)` | 仅 OBJECT |
| `ql_value_type_name(type)` | `"int"` / `"fixed"` … |

### 定点运算

```c
typedef int32_t ql_fixed_t;      /* Q16.16 */
#define QL_FIXED_ONE 65536

ql_fixed_t ql_fixed_from_int(int32_t);
ql_fixed_t ql_fixed_mul(ql_fixed_t, ql_fixed_t);
ql_fixed_t ql_fixed_div(ql_fixed_t, ql_fixed_t);   /* 除零返回 0 */
int32_t    ql_fixed_to_int(ql_fixed_t);            /* 四舍五入 */
```

**规范要求**：属性与事件里禁止直接存 `float/double`，统一用 `Q16.16`（32/64 位等长）。

---

## 5. 类与类型

```c
typedef uint32_t QLType;
#define QL_TYPE_NONE 0xFFFFFFFFu

QLType         ql_class_register(QLObjectClass *klass, QLType parent_type);
QLObjectClass *ql_class_get(QLType type);
uint16_t       ql_class_count(void);
const char    *ql_class_name(QLType type);
```

- 注册返回运行期 id；父类未注册、超深、私有区超 64KB、类表满都返回 `QL_TYPE_NONE`；
- 内置类由 `qlgl_init` 自动注册；测试/插件可以自己注册（id 从 `QL_TYPE_COUNT` 起继续分配）。

### 5.1 `QLObjectClass` 关键字段

| 字段 | 含义 |
|---|---|
| `type` / `name` | 类型 id / 类型名 |
| `instance_size` | 本类结构体大小（含父类字段） |
| `own_private_size` | 本类**自己**的私有区大小 |
| `priv_offset` / `priv_total` | 私有区相对块首偏移 / 整链私有区总量，注册时算好 |
| `property_start` / `property_end` | 属性 id 闭区间（含哨兵） |
| `props` / `prop_count` | 可选属性描述表 |
| `events` / `event_count` | 事件描述表 |
| `parent` | 父类 |
| `init` / `deinit` | 实例构造/析构钩子（父→子 / 子→父） |
| `class_init` | 注册时调用一次，用来挂 `set_property` 等虚函数 |
| `set_property` / `get_property` | 属性读写（返回 `ql_err_t` / `QLValue`） |
| `event` | 默认事件处理，只在目标节点、事件带 `DEFAULT` 标志时调用 |

### 5.2 定义一个新类

```c
/* ① 属性 / 事件 id */
QL_PROPERTY_ENUM_START(QL_FOO, QLFoo, QL_OBJECT)
    QL_FOO_PROPERTY_VALUE          /* 最后一项不写逗号 */
QL_PROPERTY_ENUM_END(QL_FOO, 10)

QL_EVENT_ENUM_START(QL_FOO, QLFoo, QL_OBJECT)
    QL_FOO_EVENT_CHANGED
QL_EVENT_ENUM_END(QL_FOO, 10)

/* ② 头/源文件通用的声明 */
QL_DECLARE(QLFoo, ql_foo)

struct QLFoo      { QLObject parent; };
struct QLFooClass { QLObjectClass parent; };
struct QLFooPrivate { int32_t value; };

/* ③ 生成 get_type / private_size / get_private 与类描述 */
QL_DEFINE_WITH_PRIVATE(QL_FOO, QLFoo, ql_foo, ql_object)

/* ④ 三个钩子必须实现（可以是空实现） */
static void ql_foo_class_init(QLFooClass *klass) { QL_UNUSED(klass); }
static void ql_foo_init(QLFoo *self)   { QL_UNUSED(self); }
static void ql_foo_deinit(QLFoo *self) { QL_UNUSED(self); }
```

| 宏 | 说明 |
|---|---|
| `QL_DECLARE(Name, name)` | 声明 `Name`/`NameClass`/`NamePrivate`、`name##_get_type`/`_private_size`/`_get_private`、`name##_class`。**自带分号，调用处不要再加** |
| `QL_DEFINE_WITH_PRIVATE(NAME, Name, name, Inherit)` | 第 4 个参数是父类函数前缀（qlgen 靠它排序）；要求之前已定义 `struct Name##Private`；**自带分号** |
| `QL_PROPERTY_ENUM_START/END` | 属性 id 枚举；END 自带前导逗号，所以最后一项不能写逗号 |
| `QL_PROPERTY_ENUM_EMPTY(NAME, Name, INHERIT, cap)` | 暂时没有属性的类 |
| `QL_EVENT_ENUM_START/END` | 事件 id 枚举，规则同上 |
| `QL_EVENT_ENUM_EMPTY(NAME, Name, INHERIT, cap)` | 暂时没有事件的类 |
| `QL_PRIV(Name, name, self)` | 取私有区（`struct Name##Private *`） |

### 5.3 加完类之后

```bash
python3 tools/qlgen/qlgen.py            # 重新生成 include/qlgl/qltype_gen.h 与 src/generated/qlclass_gen.c
python3 tools/qlgen/qlgen.py --check    # CI 校验用，不同步返回 1
```

### 5.4 根类

```c
QLType ql_object_get_type(void);      /* 恒为 QL_TYPE_OBJECT */
extern QLObjectClass ql_object_class;
```

---

## 6. 对象

```c
QLObject *ql_object_new(QLType type);
QLObject *ql_object_ref(QLObject *self);
void      ql_object_unref(QLObject *self);
```

| 函数 | 说明 |
|---|---|
| `ql_object_new` | 一次分配 `instance_size + priv_total`；失败返回 `NULL`；未注册类型返回 `NULL` 并打错误日志 |
| `ql_object_ref` | 计数 +1；对 `DEAD` 对象返回 `NULL` |
| `ql_object_unref` | 计数 -1，归零则析构；`NULL` 安全 |

### 查询

```c
QLObjectClass *ql_object_get_class(QLObject *self);
QLType         ql_object_type(QLObject *self);
const char    *ql_object_type_name(QLObject *self);
int32_t        ql_object_ref_count(QLObject *self);
int            ql_object_is(QLObject *self, QLType type);   /* 含父类判断 */
```

`NULL` 输入一律安全：`ql_object_type(NULL)` 返回 `QL_TYPE_NONE`，`type_name` 返回 `""`。

### 对象树

```c
ql_err_t  ql_object_set_parent(QLObject *self, QLObject *parent);
ql_err_t  ql_object_add_child(QLObject *parent, QLObject *child);
QLObject *ql_object_parent(QLObject *self);
QLObject *ql_object_first_child(QLObject *self);
QLObject *ql_object_next_sibling(QLObject *self);
uint32_t  ql_object_child_count(QLObject *self);
```

| 规则 | 说明 |
|---|---|
| 引用归属 | 父对子持有**一份**引用；子不引用父 |
| `set_parent(child, p)` | 旧父 -1、新父 +1；`p == NULL` 表示摘除，可能直接销毁 child |
| `add_child(p, child)` | `set_parent` 之后归还调用方那份引用，**最常用** |
| 成环 | `parent` 是自己或自己的后代 → 返回 `QL_ERR_INVAL` |
| 级联销毁 | `unref(父)` 会先摘链再逐个 `unref` 子对象 |

### 私有区

```c
static inline void *ql_priv(QLObject *self, QLObjectClass *klass);
#define QL_PRIV(Name, name, self) name##_get_private((Name *)(self))
```

O(1)：`(char *)self - self->klass->priv_total + klass->priv_offset`。

---

## 7. 属性

```c
ql_err_t ql_object_set_property(QLObject *obj, ql_property_t property, QLValue value);
QLValue  ql_object_get_property(QLObject *obj, ql_property_t property);
int      ql_object_property_owner(QLObject *obj, ql_property_t property, QLType *owner_type);
```

| 返回值 | 含义 |
|---|---|
| `QL_OK` | 赋值成功 |
| `QL_ERR_INVAL` | id 越界、所属类没有 setter、或 setter 自己拒绝（例如类型不符） |
| `QL_VALUE_NULL` | get 找不到对应属性 |

区间规则：每类默认 100 个 id，`START` / `END` 是**哨兵**，第一个自定义属性是 `START + 1`。
根类占 `1..100`，子类从 `父 SPACE_END + 1` 起。

### 属性描述表（可选）

```c
typedef struct { ql_property_t id; uint8_t type, flags; uint16_t reserved; const char *name; } QLPropertyDesc;
/* 位标志：READ=1<<0, WRITE=1<<1, REDRAW=1<<2, RELAYOUT=1<<3, INHERIT=1<<4 */
enum { QL_PROPERTY_FLAG_READ = 1u << 0, /* ... */ };
```

仅供工具/绑定使用，不参与派发。在 `class_init` 里挂到 `klass->props` / `klass->prop_count`。

---

## 8. 事件

### 8.1 连接

```c
typedef void (*ql_event_cb_t)(QLEvent *event);

uint32_t ql_connect(QLObject *obj, ql_event_t event, ql_event_cb_t cb, void *user_data);
ql_err_t ql_disconnect(QLObject *obj, uint32_t conn_id);
int      ql_disconnect_all(QLObject *obj);
uint32_t ql_event_conn_count(QLObject *obj);
```

| 项 | 说明 |
|---|---|
| 返回值 | 非零连接 id；池满或参数非法返回 `QL_CONN_ID_NONE`(0) |
| 存储 | 静态池，**运行期零 malloc**；对象删除时自动归还 |
| `ql_disconnect` | 按 id 断开；重复断开返回 `QL_ERR_INVAL`；派发中只标记，emit 结束后回收 |
| `ql_disconnect_all` | 断开该对象全部连接，返回断开数量 |

### 8.2 发射

```c
int ql_emit(QLObject *obj, ql_event_t event, const QLEventArgs *args);   /* 返回回调次数 */
```

```c
QL_EMIT_0(obj, event);                                        /* 无参 */
QL_EMIT_1(obj, event, ql_args_int, 42);                       /* 一个参数 */
QL_EMIT_2(obj, event, ql_args_int, 1, ql_args_string, "hi");  /* 两个参数 */
QL_EMIT_3 / QL_EMIT_4                                         /* 三 / 四个参数 */
QL_EMIT_INT(obj, event, value);                               /* 最常用的一元整型 */
```

参数构造器：

```c
QLEventArgs *ql_args_reset(QLEventArgs *args);
QLEventArgs *ql_args_int / ql_args_uint / ql_args_fixed / ql_args_bool
            / ql_args_string / ql_args_ptr / ql_args_object (QLEventArgs *, 值);
```

参数上限 `QL_CFG_EVENT_ARG_MAX`（默认 4），超出静默丢弃。

### 8.3 回调里可用

```c
typedef struct QLEvent {
    ql_event_t event; QLObject *target, *current_target;
    const QLEventArgs *args; void *user_data; uint32_t conn_id;
    uint8_t stop_propagation, prevent_default;
} QLEvent;

void  ql_event_stop_propagation(QLEvent *e);   /* 停止当前节点剩余回调 + 不再传播 */
void  ql_event_prevent_default(QLEvent *e);    /* 跳过默认处理 */
uint8_t ql_event_arg_count(const QLEvent *e);
int32_t     ql_event_arg_int(e, index, fallback);
ql_fixed_t  ql_event_arg_fixed(e, index, fallback);
const char *ql_event_arg_string(e, index, fallback);
void       *ql_event_arg_ptr(e, index, fallback);
```

### 8.4 事件描述表

```c
typedef struct { const char *name; ql_event_t id; uint32_t flags; } QLEventDesc;

enum {
    QL_EVENT_FLAG_BUBBLE,     /* 向父对象冒泡 */
    QL_EVENT_FLAG_CAPTURE,    /* 先从根向目标捕获 */
    QL_EVENT_FLAG_STOPPABLE,  /* 允许 stop_propagation */
    QL_EVENT_FLAG_DEFAULT,    /* 有默认处理可调用 */
};
```

在 `class_init` 里挂到 `klass->events` / `klass->event_count`；没登记的事件按"可冒泡 + 可停止、无默认处理"处理。

### 8.5 传播规则速查

| 场景 | 结果 |
|---|---|
| 目标有 2 个回调、父有 1 个，事件带 `BUBBLE` | 目标 2 次（按连接倒序）→ 父 1 次 |
| 某回调调用 `stop_propagation` | 该节点剩余回调不执行，父节点也不执行 |
| 事件带 `DEFAULT` 且未 `prevent_default` | 目标节点的类默认处理执行 1 次 |
| 目标在回调里被 `unref` | 安全，emit 自带守卫引用 |

### 8.6 池状态（测试/调试）

```c
uint32_t ql_event_pool_usage(void);
uint32_t ql_event_pool_capacity(void);
void     ql_event_pool_init(void);
```

---

## 9. 错误码

| 值 | 名称 | 常见来源 |
|---|---|---|
| 0 | `QL_OK` | 成功 |
| -1 | `QL_ERR_NOMEM` | 分配失败 |
| -2 | `QL_ERR_INVAL` | 参数非法、id 越界、类型不符、成环 |
| -3 | `QL_ERR_NOT_INIT` | 未初始化、生成表为空 |
| -4 | `QL_ERR_OVERFLOW` | 私有区/继承深度超限 |
| -5 | `QL_ERR_EXISTS` | 重复注册（预留） |
| -6 | `QL_ERR_VER` | 版本不兼容（预留） |
| -7 | `QL_ERR_FULL` | 池满（预留） |

---

## 10. 日志

```c
void ql_log(int level, const char *fmt, ...);
#define QL_LOGE / QL_LOGW / QL_LOGI / QL_LOGD
```

输出到 `stderr`，格式 `[QLGL][LEVEL] message`，**消息文本一律英文**（便于 grep），
级别高于 `QL_CFG_LOG_LEVEL` 的直接丢弃。

---

## 11. 约束与承诺

| 项 | 承诺 |
|---|---|
| 线程 | 当前版本**非线程安全**；所有 API 需在同一线程调用 |
| 运行期分配 | 对象创建之外（事件、属性、派发）**不分配堆内存** |
| 失败处理 | 可能失败的 API 均返回 `ql_err_t` 或 `NULL`/`QL_TYPE_NONE`，不抛异常、不 abort |
| ABI | `QLObjectClass` 等公开结构体只允许在末尾追加字段；属性/事件 id 由 qlgen 保证稳定 |
| 位宽 | `QLType`/`ql_property_t`/`ql_event_t` 固定 32 位，`ql_fixed_t` 固定 `int32_t` |

---

## 12. 完整示例

见 `test/object/example_object.c`（可运行）：

```bash
cmake -B build && cmake --build build && ./build/example_object
```

其他示例：

| 文件 | 演示内容 |
|---|---|
| `test/object/test_object_lifecycle.c` | 创建 / 引用计数 / 泄漏断言 |
| `test/object/test_object_inherit.c` | 三层继承、私有区布局、属性向父类派发 |
| `test/object/test_object_property.c` | 属性区间、哨兵、类型校验、定点运算 |
| `test/object/test_object_event.c` | 连接、参数、冒泡、停止传播、连接池 |
| `test/object/test_object_tree.c` | 父子关系、成环检查、级联销毁 |
