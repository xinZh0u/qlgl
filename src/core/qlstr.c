/**
 * @file    qlstr.c
 * @brief   字符串的两种容量策略实现。
 */
#include <string.h>

#include <qlgl/qlgl.h>

void ql_str_init(QLStr *str)
{
    if (str == NULL)
        return;
    memset(str, 0, sizeof(*str));
    str->policy = QL_STR_COMPACT;
}

void ql_str_free(QLStr *str)
{
    if (str == NULL)
        return;
    if (str->own && str->data != NULL)
        ql_mem_free(str->data);
    memset(str, 0, sizeof(*str));
    str->policy = QL_STR_COMPACT;
}

/* 源指针是否落在 str 自己的缓冲区里：扩容会释放旧块，必须换算成偏移 */
static int source_offset(const QLStr *str, const char *text, uint32_t *offset)
{
    if (str->own && str->data != NULL && text != NULL) {
        const uintptr_t base = (uintptr_t)str->data;
        const uintptr_t p = (uintptr_t)text;
        if (p >= base && p < base + (uintptr_t)str->capacity) {
            *offset = (uint32_t)(p - base);
            return 1;
        }
    }
    return 0;
}

/* 确保容量能放下 need 字节（含结尾 '\0'）；成功返回 QL_OK */
static ql_err_t ensure_capacity(QLStr *str, uint32_t need)
{
    if (str->own && str->data != NULL) {
        /* compact 要求"容量正好"，amortized 只要"够用" */
        if (str->policy == QL_STR_AMORTIZED && str->capacity >= need)
            return QL_OK;
        if (str->policy == QL_STR_COMPACT && str->capacity == need)
            return QL_OK;
    }

    uint32_t capacity;
    if (str->policy == QL_STR_AMORTIZED) {
        capacity = (str->capacity > 0) ? str->capacity : 16;
        while (capacity < need) {
            if (capacity > UINT32_MAX / 2u) {      /* 再翻倍会回绕，直接取 need */
                capacity = need;
                break;
            }
            capacity *= 2;
        }
        if (capacity < need)                       /* 溢出保护 */
            capacity = need;
    } else {
        capacity = need;
    }

    char *fresh;
    if (str->own && str->data != NULL) {
        fresh = (char *)ql_mem_realloc(str->data, capacity);
        if (fresh == NULL)
            return QL_ERR_NOMEM;
    } else {
        /* 只读字面量或空串：新分配并拷贝现有内容，不递归 */
        fresh = (char *)ql_mem_alloc(capacity);
        if (fresh == NULL)
            return QL_ERR_NOMEM;
        /* 只读视图的 len 可能大于新容量（例如 set_n 缩小），按容量截断 */
        uint32_t copy = (str->len < capacity) ? str->len : capacity - 1u;
        if (copy > 0 && str->data != NULL)
            memcpy(fresh, str->data, copy);
        fresh[copy] = '\0';
    }

    str->data = fresh;
    str->capacity = capacity;
    str->own = 1;
    return QL_OK;
}

void ql_str_set_policy(QLStr *str, QLStrPolicy policy)
{
    if (str != NULL)
        str->policy = (uint8_t)policy;
}

QLStrPolicy ql_str_policy(const QLStr *str)
{
    return (str != NULL) ? (QLStrPolicy)str->policy : QL_STR_COMPACT;
}

ql_err_t ql_str_set_n(QLStr *str, const char *text, uint32_t len)
{
    if (str == NULL)
        return QL_ERR_INVAL;

    if (text == NULL)
        return ql_str_clear(str);

    if (len == UINT32_MAX)                 /* len + 1 回绕 */
        return QL_ERR_INVAL;

    uint32_t need = len + 1u;

    /*
     * 源可能指向自身缓冲区：缩容时 ql_mem_realloc 会丢掉尾巴，
     * 所以先把源搬到缓冲区开头，之后的扩容/缩容都不会再有别名问题。
     */
    uint32_t src_off = 0;
    if (source_offset(str, text, &src_off)) {
        memmove(str->data, str->data + src_off, len);
        text = NULL;                        /* 已经就位，无需再拷 */
    }

    if (ensure_capacity(str, need) != QL_OK)
        return QL_ERR_NOMEM;

    if (text != NULL && len > 0)
        memmove(str->data, text, len);
    str->data[len] = '\0';
    str->len = len;
    return QL_OK;
}

ql_err_t ql_str_set(QLStr *str, const char *text)
{
    if (text == NULL)
        return ql_str_clear(str);
    return ql_str_set_n(str, text, (uint32_t)strlen(text));
}

ql_err_t ql_str_set_static(QLStr *str, const char *text)
{
    if (str == NULL)
        return QL_ERR_INVAL;

    uint32_t off = 0;
    if (text != NULL && source_offset(str, text, &off)) {
        /* 指向自身缓冲区：不能先 free 再登记，退化成拷贝语义 */
        return ql_str_set_n(str, text, (uint32_t)strlen(text));
    }

    if (str->own && str->data != NULL)
        ql_mem_free(str->data);

    str->data = (char *)text;
    str->len = (text != NULL) ? (uint32_t)strlen(text) : 0;
    str->capacity = 0;
    str->own = 0;
    return QL_OK;
}

ql_err_t ql_str_append(QLStr *str, const char *text)
{
    if (str == NULL || text == NULL)
        return QL_ERR_INVAL;

    uint32_t extra = (uint32_t)strlen(text);
    if (extra == 0)
        return QL_OK;

    uint32_t old_len = str->len;
    if (extra > UINT32_MAX - old_len - 1u)     /* old_len + extra + 1 会回绕 */
        return QL_ERR_NOMEM;

    uint32_t src_off = 0;
    const int aliased = source_offset(str, text, &src_off);

    if (ensure_capacity(str, old_len + extra + 1u) != QL_OK)
        return QL_ERR_NOMEM;

    if (aliased)
        text = str->data + src_off;

    memmove(str->data + old_len, text, extra);
    str->len = old_len + extra;
    str->data[str->len] = '\0';
    return QL_OK;
}

ql_err_t ql_str_clear(QLStr *str)
{
    if (str == NULL)
        return QL_ERR_INVAL;

    if (str->own && str->data != NULL) {
        if (str->policy == QL_STR_AMORTIZED && str->capacity > 0) {
            str->data[0] = '\0';
            str->len = 0;
            return QL_OK;
        }
        ql_mem_free(str->data);
    }
    str->data = NULL;
    str->len = 0;
    str->capacity = 0;
    str->own = 0;
    return QL_OK;
}

const char *ql_str_data(const QLStr *str)
{
    return (str != NULL && str->data != NULL) ? str->data : "";
}

uint32_t ql_str_len(const QLStr *str)
{
    return (str != NULL) ? str->len : 0;
}

int ql_str_equals(const QLStr *str, const char *text)
{
    if (str == NULL || text == NULL)
        return 0;
    if (str->data == NULL)
        return text[0] == '\0';
    return strcmp(str->data, text) == 0;
}
