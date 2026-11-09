# QLObject 阅读指南

> 对象系统（object）是整个 QLGL 的地基：类型、对象、属性、事件、内存全在这里。
> 目标读者：要读懂或修改 `src/core/` 的人。
> 只想知道怎么用，请看 [QLObject API手册](./QLObject%20API手册.md)。

---

## 1. 先看文件地图

| 文件 | 职责 | 建议阅读顺序 |
|---|---|---|
| `include/qlgl/qlconfig.h` | 所有容量/裁剪开关 | ① |
| `include/qlgl/qlgl.h` | 总入口、错误码、日志宏 | ② |
| `include/qlgl/qlvalue.h` | 通用值容器 `QLValue`、Q16.16 定点 | ③ |
| `include/qlgl/qlclass.h` | 类描述 `QLObjectClass`、注册 API、**定义类的宏** | ④ |
| `include/qlgl/qlobject.h` | 根类 `QLObject`、生命周期、对象树 | ⑤ |
| `include/qlgl/qlprop.h` | 属性 id 区间与派发 | ⑥ |
| `include/qlgl/qlevent.h` | 事件参数、传播、发射宏 | ⑦ |
| `include/qlgl/qlmem.h` | 分配钩子与统计 | ⑧ |
| `include/qlgl/qltype_gen.h` | **自动生成**的类型 id 枚举，勿手改 | ⑨ |
| `src/core/qlmem.c` | 分配器包装 + 越界自检 | ④ |
| `src/core/qlvalue.c` | 值构造与安全读取 | ③ |
| `src/core/qlclass.c` | 类表、注册、私有区布局计算 | ⑤ |
| `src/core/qlobject.c` | 创建/引用计数/析构/对象树 | ⑥ |
| `src/core/qlprop.c` | 属性派发（沿继承链找归属类） | ⑦ |
| `src/core/qlevent.c` | 连接池、捕获/冒泡、连接回收 | ⑧ |
| `src/core/qlgl.c` | 初始化：按生成表注册内置类 | ⑨ |
| `src/generated/qlclass_gen.c` | **自动生成**的内置类注册表 | ⑩ |
| `tools/qlgen/qlgen.py` | 生成上面两个文件 | ⑪ |

带"自动生成"的文件由 `tools/qlgen/qlgen.py` 产出，CI 里用 `qlgen.py --check` 校验是否与源码同步。

---

## 2. 一次看懂内存布局

对象**只分配一次**：私有区在块首，实例在块尾。

```
   malloc 返回的块首 mem
   +------------------------------+  mem
   | QLObject 私有区（0 字节）     |  priv_offset = 0
   +------------------------------+
   | 父类私有区                    |  priv_offset = 上一层的 offset + 上一层 own_size
   +------------------------------+
   | 本类私有区                    |  priv_offset = ...
   +------------------------------+  mem + priv_total
   | QLObject 实例（结构体本体）    |  ← self = (QLObject *)(mem + priv_total)
   +------------------------------+
```

两个关键字段在注册时算好，之后只读：

- `klass->own_private_size`：本类**自己**的私有区大小；
- `klass->priv_offset`：本类私有区相对 `mem` 的偏移；
- `klass->priv_total`：整条继承链的私有区总和，也就是 `self - mem`。

因此取私有区是 O(1)：

```c
static inline void *ql_priv(QLObject *self, QLObjectClass *klass)
{
    return (char *)self - self->klass->priv_total + klass->priv_offset;
}
```

`ql_mything_get_private(self)`（`QL_DEFINE_WITH_PRIVATE` 自动生成）内部就是这一行。

**为什么不用"私有区大小累加"的老做法？** 老做法把每层的 `private_size()`（本身已含父类）再求和，
5 层继承时分配量是需求的 3 倍；现在的做法是每层只算自己那一份，零浪费。

**析构时必须释放块首**：`ql_mem_free((char *)self - self->klass->priv_total)`。
直接 `free(self)` 会释放块内指针，是堆破坏（ASan 会立刻报错）。

---

## 3. 创建与销毁的时序

### 创建 `ql_object_new(type)`

1. `ql_class_get(type)`：从类表取类描述，未注册直接返回 `NULL`；
2. 沿 `klass->parent` 收集继承链到栈上数组（顺便检查深度 ≤ `QL_CFG_MAX_INHERIT_DEPTH`）；
3. `ql_mem_calloc(instance_size + priv_total)`：一次分配，失败返回 `NULL`；
4. `self = mem + priv_total`，填 `klass / ref_count = 1 / conn_head = 空 / state = ALIVE`；
5. 按 **父 → 子** 依次调用每层 `init(self)`。

### 销毁 `ql_object_unref(self)`（计数归零时）

1. **子对象**：先摘链（`first_child`、`parent`、`next_sibling` 全部清空）再 `unref`，避免边遍历边改链表；
2. **事件**：`ql_disconnect_all` 把所有连接归还静态池；
3. **deinit**：按 **子 → 父** 调用；
4. **与父解绑**：从父的 `first_child` 链表摘除（不改引用计数，父对子的引用已在第 1 步归还）；
5. `ql_mem_free(块首)`，并把 `state` 置为 `DEAD`。

`state` 的作用：析构期间再被 `unref` 会打错误日志而不是重复释放；
`ql_object_ref` 遇到 `DEAD` 也会拒绝。

---

## 4. 类注册与类型 id

### 4.1 为什么不用 `__attribute__((constructor))`

构造函数按**链接顺序**执行，每次改文件、改链接顺序，类型 id 就会变，
二进制资源、语言绑定、序列化全部跟着失效；裸机上还未必有 `.init_array`。

现在的做法：`tools/qlgen` 扫描源码生成固定顺序的 id 枚举与注册表，
`qlgl_init()` 按表顺序注册，并**校验运行期 id 与枚举一致**，不一致直接返回 `QL_ERR_INVAL`。

### 4.2 注册时必须满足

| 约束 | 违反时 |
|---|---|
| 父类先于子类注册 | 返回 `QL_TYPE_NONE`，打 `parent type not registered` |
| 继承深度 ≤ `QL_CFG_MAX_INHERIT_DEPTH`（默认 8） | 返回 `QL_TYPE_NONE` |
| 私有区总和 ≤ 64KB | 返回 `QL_TYPE_NONE` |
| 类表容量 ≤ `QL_CFG_MAX_CLASS`（默认 64） | 返回 `QL_TYPE_NONE` |

### 4.3 新增一个类的 5 步

1. 写属性/事件枚举（没有就写 `QL_PROPERTY_ENUM_EMPTY` / `QL_EVENT_ENUM_EMPTY`）；
2. 头文件里 `QL_DECLARE(QLFoo, ql_foo)`，并定义 `struct QLFoo / struct QLFooClass`；
3. `.c` 里定义 `struct QLFooPrivate`，然后 `QL_DEFINE_WITH_PRIVATE(QL_FOO, QLFoo, ql_foo, ql_parent)`；
4. 实现三个 `static` 钩子：`ql_foo_class_init` / `ql_foo_init` / `ql_foo_deinit`；
5. 运行 `python3 tools/qlgen/qlgen.py`，重新生成 id 表。

> `QL_DEFINE_WITH_PRIVATE` 的第 4 个参数是父类的**函数前缀**，qlgen 靠它推导继承关系。

---

## 5. 属性派发

每个类向父类要一段 id 区间（默认 100 个），区间是**闭区间**：

```
QLObject : 1        .. 100      （START=1，END=100，都是哨兵）
子类 A   : 101      .. 200      （每个子类向父类要 100 个 id）
子类 B   : 201      .. 300
```

- `*_PROPERTY_START` 和 `*_PROPERTY_END` 是**区间哨兵**，不绑定具体属性；
  用户写的第一个属性自动是 `START + 1`；
- 派发：从对象自己的类开始向上找，谁包含这个 id 就由谁的 `set_property/get_property` 处理；
- 找不到（或该类的函数为空）返回 `QL_ERR_INVAL` / `QL_VALUE_NULL`。

```c
ql_object_set_property(obj, QL_MYTHING_PROPERTY_TEXT, ql_value_string("hi"));
```

即使 `obj` 是最派生的类，也会沿着链找到声明这个属性的那一层的 setter。

---

## 6. 事件系统

### 6.1 连接池

事件连接**不分配堆内存**，全部来自静态池：

```c
QLEventConn s_pool[QL_CFG_EVENT_CONN_POOL];   // 默认 256 个槽位
```

- 对象头只存 `conn_head`（16 位池下标）；
- 同一对象的连接用 `next` 串成单链表，新连接插在表头；
- `ql_connect` 返回**单调递增的 32 位 id**（永不复用，避免 ABA 误断）；
- `ql_disconnect` 只标记 `removed`；若当前不在派发中，立刻回收；
  派发过程中标记的，在 `ql_emit` 结束、`emit_depth` 归零时统一回收。

### 6.2 一次 emit 的流程

```
     目标 obj
        │  路径：obj → parent → ... → root（最多 QL_CFG_EVENT_PATH_MAX 层）
        ▼
[1] 取事件描述（沿类链找 events 表）→ 得到传播标志
[2] CAPTURE 标志：root …… 目标的父  依次派发
[3] 目标自身派发
[4] BUBBLE 标志：目标的父 …… root 依次派发
[5] emit_depth 归零 → 回收所有 removed 连接
```

每个节点的派发顺序是：**该节点上的全部回调 → 默认处理**。

| 规则 | 说明 |
|---|---|
| `ql_event_stop_propagation` | 立刻停止当前节点剩余回调，且不再向上传播 |
| `ql_event_prevent_default` | 跳过默认处理；默认处理只在**目标节点**、且事件带 `DEFAULT` 标志时调用 |
| 未登记描述表的事件 | 默认按"可冒泡 + 可停止 + 无默认处理"处理 |
| emit 期间 unref 目标 | 安全：emit 前后自带一次 `ref/unref` 守卫 |

### 6.3 事件参数

参数是**定长数组的值拷贝**，不是栈指针：

```c
QLEventArgs args;                 // count / type[4] / arg[4]
ql_args_reset(&args);
ql_args_int(&args, 42);
ql_args_string(&args, "hi");
ql_emit(obj, QL_EVT_EVENT_PING, &args);      // 或 QL_EMIT_2(...)
```

回调里按索引取，类型不符时返回 fallback，不会崩。

---

## 7. 对象树与引用计数

规则只有一条：**父对象对子对象持有一份引用，子对象不引用父对象。**

| 操作 | 引用计数变化 |
|---|---|
| `ql_object_new` | 1（调用方持有） |
| `ql_object_set_parent(child, parent)` | 父 +1；如果原来有父，旧父 -1 |
| `ql_object_add_child(parent, child)` | 等价于 `set_parent` 后再把调用方那份 -1（常用） |
| `set_parent(child, NULL)` | 旧父 -1，可能因此销毁 child |
| `ql_object_unref(parent)` | 递归销毁所有子对象 |

> 用 `add_child` 之后不要再 `unref(child)`，否则会把父对象持有的那份也放掉。

成环会被拒绝：`set_parent` 会检查 `parent` 是不是自己或自己的后代，返回 `QL_ERR_INVAL`。

---

## 8. 常见坑（都是真实踩过的）

| 坑 | 现象 | 正确做法 |
|---|---|---|
| 宏参数 `name` 会替换字段 `.name` | 编译报 `.ql_foo` 不是成员 | 宏内部用 `QL_CLASS_FIELD_NAME` 间接展开，**改宏时注意** |
| 属性枚举最后一个值后面加了逗号 | `expected expression` | 最后一项不写逗号（`QL_PROPERTY_ENUM_END` 自带前导逗号） |
| 调用宏加了分号 | `empty declaration` 警告 | `QL_DECLARE` / `QL_DEFINE_WITH_PRIVATE` **自带分号**，调用处不要加 |
| `struct QLFooPrivate` 没定义就写宏 | `incomplete type` | 私有结构体必须在宏之前定义 |
| 三个 static 钩子没实现 | 链接/编译报未定义 | 必须实现 `class_init / init / deinit`，空实现也要写 |
| 把 `*_PROPERTY_START` 当成第一个属性 | set 返回 `QL_ERR_INVAL` | 第一个属性是 `START + 1` |
| `unref` 用 `free(self)` | ASan 报堆破坏 | 只释放 `mem = self - priv_total` |
| 类注册在 `qlgl_init` 之前 | `ql_object_new` 返回 NULL | 先 `qlgl_init()`，再 `ql_class_register()` |

---

## 9. 调试手段

| 需求 | 手段 |
|---|---|
| 看注册结果 | `QL_CFG_LOG_LEVEL=4`，注册时会打印 type / instance / private / total |
| 排查泄漏 | `ql_mem_stats(&s)`，看 `bytes_live` 与 `alloc_count/free_count` |
| 排查连接泄漏 | `ql_event_pool_usage()` 对比 `ql_event_pool_capacity()` |
| 确认某属性归谁 | `ql_object_property_owner(obj, id, &type)` |
| 对象身份 | `ql_object_type_name(obj)`、`ql_object_is(obj, QL_TYPE_X)` |

日志一律输出英文（便于 grep 与跨环境显示），注释与文档用中文。

---

## 10. 建议的阅读/验证顺序

1. 跑 `ctest --test-dir build --output-on-failure`，6 个用例（5 个单元测试 + 1 个示例程序）全绿再读代码；
2. 按 §1 的文件表读头文件（不用急着看 `.c`）；
3. 读 `test/object/test_object_inherit.c`——它把私有区公式、初始化顺序、属性派发都验证了一遍；
4. 读 `test/object/test_object_event.c`——传播规则的可执行说明；
5. 最后读 `src/core/qlobject.c` 与 `src/core/qlevent.c`。
