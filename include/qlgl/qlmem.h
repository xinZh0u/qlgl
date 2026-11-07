/**
 * @file    qlmem.h
 * @brief   内存分配钩子与统计。对象系统全部经由这里分配，便于换掉 malloc。
 */
#ifndef QLGL_MEM_H
#define QLGL_MEM_H

#include "qlgl.h"

typedef struct {
    void *(*alloc)(size_t size);
    void *(*realloc)(void *ptr, size_t size);
    void  (*free)(void *ptr);
} QLAllocator;

typedef struct {
    size_t alloc_count;   /* 累计分配次数 */
    size_t free_count;    /* 累计释放次数 */
    size_t bytes_live;    /* 当前存活字节数 */
    size_t bytes_peak;    /* 峰值存活字节数 */
} QLMemStats;

/* 必须在 qlgl_init() 之前调用；传 NULL 恢复默认 malloc 族 */
void ql_mem_set_allocator(const QLAllocator *alloc);

void *ql_mem_alloc(size_t size);
void *ql_mem_calloc(size_t count, size_t size);
void *ql_mem_realloc(void *ptr, size_t size);
void  ql_mem_free(void *ptr);

/* QL_CFG_MEM_TRACK=0 时输出全 0（除 bytes_live 由调用方自行维护） */
void ql_mem_stats(QLMemStats *out);
void ql_mem_reset_stats(void);

#endif /* QLGL_MEM_H */
