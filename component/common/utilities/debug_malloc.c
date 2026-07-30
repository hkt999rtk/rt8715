/**
 * debug_malloc.c — 带 __FUNCTION__ / __LINE__ 追踪的内存分配器
 *
 * 维护一个固定大小的 alloc table，记录每次 MALLOC / FREE 的调用点。
 * 底层委托给 FreeRTOS 的 pvPortMalloc() / vPortFree()。
 *
 * 用法（替换 malloc/free）：
 *   #include "debug_malloc.h"
 *   void *buf = MALLOC(128);    // 自动记录 __FUNCTION__, __LINE__
 *   FREE(buf);                  // 自动移除记录
 *   debug_malloc_dump();       // 列出所有未释放的分配
 */

#include "debug_malloc.h"
#include "FreeRTOS.h"       /* pvPortMalloc, vPortFree */
#include <stdio.h>
#include <string.h>

/* ---- alloc table entry ---- */
typedef struct {
    void       *ptr;        /* allocated pointer, NULL = free slot     */
    size_t      size;       /* requested size                          */
    const char *func;       /* __FUNCTION__ from caller                */
    int         line;       /* __LINE__ from caller                    */
} alloc_entry_t;

static alloc_entry_t alloc_table[DEBUG_MALLOC_MAX_ENTRIES];
static int           alloc_table_inited = 0;

/* ---- internal helpers ---- */

static void alloc_table_init(void)
{
    memset(alloc_table, 0, sizeof(alloc_table));
    alloc_table_inited = 1;
}

/** Find a free slot. Returns index, or -1 if table is full. */
static int alloc_find_free(void)
{
    int i;
    if (!alloc_table_inited) alloc_table_init();

    for (i = 0; i < DEBUG_MALLOC_MAX_ENTRIES; i++) {
        if (alloc_table[i].ptr == NULL)
            return i;
    }
    return -1;
}

/** Find an entry by ptr. Returns index, or -1 if not found. */
static int alloc_find_ptr(void *ptr)
{
    int i;
    if (!alloc_table_inited || ptr == NULL) return -1;

    for (i = 0; i < DEBUG_MALLOC_MAX_ENTRIES; i++) {
        if (alloc_table[i].ptr == ptr)
            return i;
    }
    return -1;
}

/* ---- public functions ---- */

void *debug_malloc(size_t size, const char *func, int line)
{
    void *ptr;
    int   idx;

    if (!alloc_table_inited) alloc_table_init();

    ptr = pvPortMalloc(size);
    if (ptr == NULL) {
        printf("[ALLOC] FAIL  func=%s line=%d size=%u -> NULL\n",
               func, line, (unsigned)size);
        return NULL;
    }

    idx = alloc_find_free();
    if (idx >= 0) {
        alloc_table[idx].ptr  = ptr;
        alloc_table[idx].size = size;
        alloc_table[idx].func = func;
        alloc_table[idx].line = line;
    } else {
        printf("[ALLOC] WARN  table full! func=%s line=%d size=%u ptr=%p\n",
               func, line, (unsigned)size, ptr);
    }

    return ptr;
}

void debug_free(void *ptr, const char *func, int line)
{
    int idx;

    if (ptr == NULL) return;
    if (!alloc_table_inited) alloc_table_init();

    idx = alloc_find_ptr(ptr);
    if (idx >= 0) {
        alloc_table[idx].ptr = NULL;   /* release slot */
    } else {
        printf("[FREE]  WARN  untracked ptr=%p from %s:%d\n",
               ptr, func, line);
    }

    vPortFree(ptr);
}

void debug_malloc_dump(void)
{
    int i;
    int count = 0;
    unsigned total = 0;

    if (!alloc_table_inited) {
        printf("[ALLOC] table not initialised\n");
        return;
    }

    printf("\n========== MALLOC DUMP (active allocations) ==========\n");
    for (i = 0; i < DEBUG_MALLOC_MAX_ENTRIES; i++) {
        if (alloc_table[i].ptr != NULL) {
            printf("  [%3d] ptr=%p  size=%-6u  %s:%d\n",
                   i,
                   alloc_table[i].ptr,
                   (unsigned)alloc_table[i].size,
                   alloc_table[i].func,
                   alloc_table[i].line);
            total += (unsigned)alloc_table[i].size;
            count++;
        }
    }
    printf("---------- %d allocations, %u bytes total ----------\n\n",
           count, total);
}

int debug_malloc_count(void)
{
    int i;
    int count = 0;

    if (!alloc_table_inited) return 0;

    for (i = 0; i < DEBUG_MALLOC_MAX_ENTRIES; i++) {
        if (alloc_table[i].ptr != NULL)
            count++;
    }
    return count;
}
