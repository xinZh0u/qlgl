/**
 * @file    test_str.c
 * @brief   示例：QLStr 的两种容量策略、内容所有权与边界处理。
 */
#include "../ql_test.h"

QL_TEST_BEGIN("test_str")

    QLStr s;
    ql_str_init(&s);
    QL_CHECK(ql_str_data(&s) != NULL);      /* 空串返回 ""，不是 NULL */
    QL_CHECK_STR(ql_str_data(&s), "");
    QL_CHECK_EQ(ql_str_len(&s), 0);
    QL_CHECK_EQ(ql_str_policy(&s), QL_STR_COMPACT);

    /* 1. compact：容量 = 长度 + 1，删除即缩容 */
    QL_CHECK_EQ(ql_str_set(&s, "hello"), QL_OK);
    QL_CHECK_STR(ql_str_data(&s), "hello");
    QL_CHECK_EQ(ql_str_len(&s), 5);
    QL_CHECK_EQ(s.capacity, 6);
    QL_CHECK(ql_str_equals(&s, "hello"));
    QL_CHECK(!ql_str_equals(&s, "hell"));

    QL_CHECK_EQ(ql_str_set(&s, "hi"), QL_OK);
    QL_CHECK_EQ(s.capacity, 3);

    /* 2. 追加：compact 每次恰好够用 */
    QL_CHECK_EQ(ql_str_append(&s, " there"), QL_OK);
    QL_CHECK_STR(ql_str_data(&s), "hi there");
    QL_CHECK_EQ(s.capacity, 9);

    /* 3. amortized：容量按 2 倍增长，缩短内容不缩容 */
    QLStr a;
    ql_str_init(&a);
    ql_str_set_policy(&a, QL_STR_AMORTIZED);
    QL_CHECK_EQ(ql_str_policy(&a), QL_STR_AMORTIZED);

    QL_CHECK_EQ(ql_str_set(&a, "0123456789"), QL_OK);   /* 需要 11 -> 16 */
    QL_CHECK_EQ(a.capacity, 16);
    uint32_t cap_before = a.capacity;
    QL_CHECK_EQ(ql_str_set(&a, "x"), QL_OK);
    QL_CHECK_EQ(a.capacity, cap_before);                 /* 不缩容 */
    QL_CHECK_STR(ql_str_data(&a), "x");

    for (int i = 0; i < 100; i++)
        QL_CHECK_EQ(ql_str_append(&a, "0123456789"), QL_OK);
    QL_CHECK_EQ(ql_str_len(&a), 1001);
    QL_CHECK(a.capacity >= 1002);
    QL_CHECK_EQ(ql_str_len(&a), (uint32_t)strlen(ql_str_data(&a)));

    /* 4. clear */
    QL_CHECK_EQ(ql_str_clear(&a), QL_OK);
    QL_CHECK_EQ(ql_str_len(&a), 0);
    QL_CHECK_STR(ql_str_data(&a), "");
    QL_CHECK(a.capacity > 0);                                     /* amortized 保留容量 */
    QL_CHECK_EQ(ql_str_append(&a, "again"), QL_OK);
    QL_CHECK_STR(ql_str_data(&a), "again");
    ql_str_free(&a);
    QL_CHECK(ql_str_len(&a) == 0);

    /* 5. set_static：不复制、不释放 */
    QLStr lit;
    ql_str_init(&lit);
    QL_CHECK_EQ(ql_str_set_static(&lit, "literal"), QL_OK);
    QL_CHECK_EQ(lit.own, 0);
    QL_CHECK_STR(ql_str_data(&lit), "literal");
    QL_CHECK_EQ(lit.capacity, 0);
    /* 从只读串切到自有内存：会自动转成可写副本 */
    QL_CHECK_EQ(ql_str_append(&lit, "!"), QL_OK);
    QL_CHECK_STR(ql_str_data(&lit), "literal!");
    QL_CHECK_EQ(lit.own, 1);
    ql_str_free(&lit);

    /* 6. 指定长度（内容可含 '\0' 之前的部分） */
    QL_CHECK_EQ(ql_str_set_n(&s, "abcdef", 3), QL_OK);
    QL_CHECK_STR(ql_str_data(&s), "abc");
    QL_CHECK_EQ(ql_str_len(&s), 3);

    /* 7. NULL 与越界 */
    QL_CHECK_EQ(ql_str_set(&s, NULL), QL_OK);
    QL_CHECK_EQ(ql_str_len(&s), 0);
    QL_CHECK_EQ(ql_str_set(NULL, "x"), QL_ERR_INVAL);
    QL_CHECK_EQ(ql_str_append(NULL, "x"), QL_ERR_INVAL);
    QL_CHECK_EQ(ql_str_clear(NULL), QL_ERR_INVAL);
    ql_str_free(NULL);
    QL_CHECK_EQ(ql_str_len(NULL), 0);
    QL_CHECK_STR(ql_str_data(NULL), "");
    QL_CHECK(!ql_str_equals(NULL, "x"));

    ql_str_free(&s);

QL_TEST_END()
