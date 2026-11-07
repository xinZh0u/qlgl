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
        while (capacity < need)
            capacity *= 2;
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
        if (str->len > 0 && str->data != NULL)
            memcpy(fresh, str->data, str->len);
        fresh[str->len] = '\0';
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

    uint32_t need = len + 1;
    if (ensure_capacity(str, need) != QL_OK)
        return QL_ERR_NOMEM;

    if (len > 0)
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
    if (ensure_capacity(str, old_len + extra + 1) != QL_OK)
        return QL_ERR_NOMEM;

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
