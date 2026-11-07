/**
 * @file    qlmem.c
 * @brief   分配钩子 + 统计 + 越界自检。
 */
#include <stdlib.h>
#include <string.h>

#include <qlgl/qlgl.h>

#if QL_CFG_MEM_TRACK

/* 头部 16 字节：既记录大小，也保证 payload 与 16 字节对齐 */
typedef struct {
    uint32_t magic;
    uint32_t size;
    uint64_t reserved;
} QLMemHeader;

#define QL_MEM_MAGIC    0x514C4D45u   /* 'QLME' */

#endif /* QL_CFG_MEM_TRACK */

static QLAllocator s_alloc = { NULL, NULL, NULL };
static QLMemStats s_stats;

static void *default_alloc(size_t size)   { return malloc(size); }
static void *default_realloc(void *p, size_t size) { return realloc(p, size); }
static void  default_free(void *p)        { free(p); }

static void ensure_allocator(void)
{
    if (s_alloc.alloc == NULL) {
        s_alloc.alloc   = default_alloc;
        s_alloc.realloc = default_realloc;
        s_alloc.free    = default_free;
    }
}

void ql_mem_set_allocator(const QLAllocator *alloc)
{
    if (alloc == NULL) {
        s_alloc.alloc   = default_alloc;
        s_alloc.realloc = default_realloc;
        s_alloc.free    = default_free;
        return;
    }
    s_alloc = *alloc;
    if (s_alloc.alloc == NULL)   s_alloc.alloc   = default_alloc;
    if (s_alloc.free == NULL)    s_alloc.free    = default_free;
    if (s_alloc.realloc == NULL) s_alloc.realloc = default_realloc;
}

void *ql_mem_alloc(size_t size)
{
    if (size == 0)
        size = 1;

    ensure_allocator();

#if QL_CFG_MEM_TRACK
    size_t total = sizeof(QLMemHeader) + size;
    QLMemHeader *header = (QLMemHeader *)s_alloc.alloc(total);
    if (header == NULL)
        return NULL;
    header->magic = QL_MEM_MAGIC;
    header->size = (uint32_t)size;
    header->reserved = 0;
    s_stats.alloc_count++;
    s_stats.bytes_live += size;
    if (s_stats.bytes_live > s_stats.bytes_peak)
        s_stats.bytes_peak = s_stats.bytes_live;
    return (void *)(header + 1);
#else
    void *ptr = s_alloc.alloc(size);
    if (ptr != NULL)
        s_stats.alloc_count++;
    return ptr;
#endif
}

void *ql_mem_calloc(size_t count, size_t size)
{
    size_t total = count * size;
    if (count != 0 && total / count != size)
        return NULL;    /* 乘法溢出 */
    void *ptr = ql_mem_alloc(total);
    if (ptr != NULL)
        memset(ptr, 0, total);
    return ptr;
}

void ql_mem_free(void *ptr)
{
    if (ptr == NULL)
        return;

    ensure_allocator();

#if QL_CFG_MEM_TRACK
    QLMemHeader *header = (QLMemHeader *)ptr - 1;
    if (header->magic != QL_MEM_MAGIC) {
        QL_LOGE("ql_mem_free: bad header at %p (double free or buffer overflow?)", ptr);
        return;
    }
    header->magic = 0;
    s_stats.free_count++;
    if (s_stats.bytes_live >= header->size)
        s_stats.bytes_live -= header->size;
    else
        s_stats.bytes_live = 0;
    s_alloc.free(header);
#else
    s_stats.free_count++;
    s_alloc.free(ptr);
#endif
}

void *ql_mem_realloc(void *ptr, size_t size)
{
    if (ptr == NULL)
        return ql_mem_alloc(size);
    if (size == 0) {
        ql_mem_free(ptr);
        return NULL;
    }

#if QL_CFG_MEM_TRACK
    QLMemHeader *header = (QLMemHeader *)ptr - 1;
    if (header->magic != QL_MEM_MAGIC) {
        QL_LOGE("ql_mem_realloc: bad header at %p", ptr);
        return NULL;
    }
    size_t old_size = header->size;
    void *fresh = ql_mem_alloc(size);
    if (fresh == NULL)
        return NULL;
    memcpy(fresh, ptr, old_size < size ? old_size : size);
    ql_mem_free(ptr);
    return fresh;
#else
    size_t old_size = 0;
    void *fresh = s_alloc.realloc(ptr, size);
    QL_UNUSED(old_size);
    return fresh;
#endif
}

void ql_mem_stats(QLMemStats *out)
{
    if (out != NULL)
        *out = s_stats;
}

void ql_mem_reset_stats(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
}
