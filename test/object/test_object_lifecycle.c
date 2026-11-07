/**
 * @file    test_object_lifecycle.c
 * @brief   示例：对象创建、引用计数、销毁与内存回收。
 */
#include "../ql_test.h"

QL_TEST_BEGIN("test_object_lifecycle")

    QLMemStats before;
    QLMemStats after;

    /* 1. 创建：一次分配，大小 = 实例 + 私有区总和（根类私有区为 0） */
    ql_mem_stats(&before);
    QLObject *obj = ql_object_new(QL_TYPE_OBJECT);
    QL_CHECK(obj != NULL);
    ql_mem_stats(&after);
    QL_CHECK_EQ(after.bytes_live - before.bytes_live, sizeof(QLObject));
    QL_CHECK_EQ(ql_object_ref_count(obj), 1);
    QL_CHECK_EQ(ql_object_type(obj), QL_TYPE_OBJECT);
    QL_CHECK_STR(ql_object_type_name(obj), "QLObject");
    QL_CHECK(ql_object_is(obj, QL_TYPE_OBJECT));

    /* 2. 引用计数：ref 后需要两次 unref 才真正释放 */
    QL_CHECK(ql_object_ref(obj) == obj);
    QL_CHECK_EQ(ql_object_ref_count(obj), 2);
    ql_object_unref(obj);
    QL_CHECK_EQ(ql_object_ref_count(obj), 1);
    ql_mem_stats(&after);
    QL_CHECK_EQ(after.bytes_live - before.bytes_live, sizeof(QLObject));

    ql_object_unref(obj);
    ql_mem_stats(&after);
    QL_CHECK_EQ(after.bytes_live - before.bytes_live, 0);

    /* 3. 未注册的类型不崩、返回 NULL */
    QL_CHECK(ql_object_new(999u) == NULL);

    /* 4. NULL 安全 */
    ql_object_unref(NULL);
    QL_CHECK(ql_object_ref(NULL) == NULL);
    QL_CHECK(ql_object_type(NULL) == QL_TYPE_NONE);

    /* 5. 统计自洽：分配次数 = 释放次数 */
    ql_mem_stats(&after);
    QL_CHECK_EQ(after.alloc_count, after.free_count);

QL_TEST_END()
