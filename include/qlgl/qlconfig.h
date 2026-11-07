/**
 * @file    qlconfig.h
 * @brief   QLGL 编译期配置。所有容量/裁剪开关集中在这里，目标平台可覆盖。
 */
#ifndef QLGL_CONFIG_H
#define QLGL_CONFIG_H

/* 类表容量（静态数组，不参与堆分配） */
#ifndef QL_CFG_MAX_CLASS
#define QL_CFG_MAX_CLASS            64
#endif

/* 继承链深度上限，注册超深类会被拒绝 */
#ifndef QL_CFG_MAX_INHERIT_DEPTH
#define QL_CFG_MAX_INHERIT_DEPTH    8
#endif

/* 每类属性 id 预留数量（规范要求"大余量"，见代码规范第 11 行） */
#ifndef QL_CFG_PROPERTY_CAPACITY
#define QL_CFG_PROPERTY_CAPACITY    100
#endif

/* 每类事件 id 预留数量 */
#ifndef QL_CFG_EVENT_CAPACITY
#define QL_CFG_EVENT_CAPACITY       100
#endif

/* 单次事件最多携带的参数个数 */
#ifndef QL_CFG_EVENT_ARG_MAX
#define QL_CFG_EVENT_ARG_MAX        4
#endif

/* 全局事件连接池容量（运行期零 malloc 的关键） */
#ifndef QL_CFG_EVENT_CONN_POOL
#define QL_CFG_EVENT_CONN_POOL      256
#endif

/* 事件传播路径上限（当前对象到根的最大层数） */
#ifndef QL_CFG_EVENT_PATH_MAX
#define QL_CFG_EVENT_PATH_MAX       8
#endif

/* 渲染器同时跟踪的脏矩形数量上限；满了会合并成一块 */
#ifndef QL_CFG_DIRTY_RECT_MAX
#define QL_CFG_DIRTY_RECT_MAX       16
#endif

/* 内存统计与越界校验：MCU 发布版可关掉以省 8 字节/次分配 */
#ifndef QL_CFG_MEM_TRACK
#define QL_CFG_MEM_TRACK            1
#endif

/* 日志级别：0=关闭 1=错误 2=警告 3=信息 4=调试 */
#ifndef QL_CFG_LOG_LEVEL
#define QL_CFG_LOG_LEVEL            2
#endif

#endif /* QLGL_CONFIG_H */
