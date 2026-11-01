/* Temporary UI heap accounting for the customer x264 path.
 * No dynamic allocations or UART output occur in the successful hot path.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"

#define UI_TRACE_SLOTS 512U
#define UI_TRACE_GROUPS 128U
#define UI_SURFACE_BYTES 7372800U /* 2560 * 1440 * RGB565 */
#define UI_FAILED_LOWRES_BYTES 4214807U

typedef struct {
    void *ptr;
    void *task;
    uintptr_t caller;
    size_t bytes;
    unsigned char x264;
    char task_name[configMAX_TASK_NAME_LEN];
} ui_trace_block;

typedef struct {
    void *task;
    uintptr_t caller;
    size_t bytes;
    unsigned count;
    unsigned char x264;
    char task_name[configMAX_TASK_NAME_LEN];
} ui_trace_group;

static ui_trace_block blocks[UI_TRACE_SLOTS]
    __attribute__((section(".lpddr.bss.ui_heap_trace")));
static ui_trace_group groups[UI_TRACE_GROUPS]
    __attribute__((section(".lpddr.bss.ui_heap_trace")));
static unsigned active, frozen, lost, allocs, frees;
static size_t start_free, end_free;

extern void *__real_pvPortMalloc(size_t bytes);
extern void *__real_pvPortMallocExt(size_t bytes, int idx);
extern void __real_vPortFree(void *ptr);
extern void *__real_x264_malloc(int bytes);
extern size_t xPortGetFreeHeapSize(void);

static void record_alloc(void *ptr, size_t bytes, uintptr_t caller, int heap0)
{
    unsigned i, n;
    const char *name;
    if (!heap0 || frozen || !ptr)
        return;
    if (!active) {
        if (bytes != UI_SURFACE_BYTES || !ptr)
            return;
        start_free = xPortGetFreeHeapSize() + bytes;
        active = 1;
        printf("[UI_HEAP] begin surface=%u heap0_before_approx=%u task=%p\r\n",
               (unsigned)bytes, (unsigned)start_free,
               xTaskGetCurrentTaskHandle());
    }
    taskENTER_CRITICAL();
    for (i = 0; i < UI_TRACE_SLOTS && blocks[i].ptr; ++i) {}
    if (i == UI_TRACE_SLOTS) {
        ++lost;
    } else {
        blocks[i].ptr = ptr;
        blocks[i].task = xTaskGetCurrentTaskHandle();
        blocks[i].caller = caller & ~(uintptr_t)1U;
        blocks[i].bytes = bytes;
        blocks[i].x264 = 0;
        name = pcTaskGetName(NULL);
        for (n = 0; n + 1U < configMAX_TASK_NAME_LEN && name[n]; ++n)
            blocks[i].task_name[n] = name[n];
        blocks[i].task_name[n] = 0;
        ++allocs;
    }
    taskEXIT_CRITICAL();
}

static void record_free(void *ptr)
{
    unsigned i;
    if (!active || frozen || !ptr)
        return;
    taskENTER_CRITICAL();
    for (i = 0; i < UI_TRACE_SLOTS; ++i) {
        if (blocks[i].ptr == ptr) {
            blocks[i].ptr = NULL;
            ++frees;
            break;
        }
    }
    taskEXIT_CRITICAL();
}

void carbox_ui_heap_trace_retag(void *ptr, void *caller)
{
    unsigned i;
    if (!active || frozen || !ptr)
        return;
    taskENTER_CRITICAL();
    for (i = 0; i < UI_TRACE_SLOTS; ++i) {
        if (blocks[i].ptr == ptr) {
            blocks[i].caller = (uintptr_t)caller & ~(uintptr_t)1U;
            break;
        }
    }
    taskEXIT_CRITICAL();
}

/* Called by x264's public allocator wrapper. Its returned aligned pointer
 * stores the underlying FreeRTOS allocation immediately before the header. */
static void retag_x264(void *aligned, uintptr_t caller)
{
    void *raw;
    unsigned i;
    if (!active || frozen || !aligned)
        return;
    raw = ((void **)aligned)[-1];
    taskENTER_CRITICAL();
    for (i = 0; i < UI_TRACE_SLOTS; ++i) {
        if (blocks[i].ptr == raw) {
            blocks[i].caller = caller & ~(uintptr_t)1U;
            blocks[i].x264 = 1;
            break;
        }
    }
    taskEXIT_CRITICAL();
}

static void dump_failure(size_t request)
{
    unsigned i, j, used = 0;
    size_t tracked = 0;
    if (!active || frozen || request != UI_FAILED_LOWRES_BYTES)
        return;
    taskENTER_CRITICAL();
    frozen = 1;
    end_free = xPortGetFreeHeapSize();
    for (i = 0; i < UI_TRACE_SLOTS; ++i) {
        if (!blocks[i].ptr)
            continue;
        tracked += blocks[i].bytes;
        for (j = 0; j < used; ++j)
            if (groups[j].task == blocks[i].task &&
                groups[j].caller == blocks[i].caller &&
                groups[j].x264 == blocks[i].x264)
                break;
        if (j == used && used < UI_TRACE_GROUPS) {
            unsigned n;
            groups[used].task = blocks[i].task;
            groups[used].caller = blocks[i].caller;
            groups[used].x264 = blocks[i].x264;
            groups[used].bytes = 0;
            groups[used].count = 0;
            for (n = 0; n < configMAX_TASK_NAME_LEN; ++n)
                groups[used].task_name[n] = blocks[i].task_name[n];
            ++used;
        }
        if (j < used) {
            groups[j].bytes += blocks[i].bytes;
            ++groups[j].count;
        } else {
            ++lost;
        }
    }
    taskEXIT_CRITICAL();
    printf("[UI_HEAP] OOM request=%u start_free_approx=%u now_free=%u net_heap_used_approx=%u tracked_live=%u allocs=%u frees=%u lost=%u\r\n",
           (unsigned)request, (unsigned)start_free, (unsigned)end_free,
           (unsigned)(start_free - end_free), (unsigned)tracked,
           allocs, frees, lost);
    for (i = 0; i < used; ++i)
        printf("[UI_HEAP] live task=%s/%p caller=%p source=%s count=%u bytes=%u\r\n",
               groups[i].task_name, groups[i].task, (void *)groups[i].caller,
               groups[i].x264 ? "x264" : "other", groups[i].count,
               (unsigned)groups[i].bytes);
}

void *__wrap_pvPortMalloc(size_t bytes)
{
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    void *ptr = __real_pvPortMalloc(bytes);
    record_alloc(ptr, bytes, caller, 1);
    if (!ptr) dump_failure(bytes);
    return ptr;
}

void *__wrap_pvPortMallocExt(size_t bytes, int idx)
{
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    void *ptr = __real_pvPortMallocExt(bytes, idx);
    record_alloc(ptr, bytes, caller, idx == 0);
    if (!ptr && idx == 0) dump_failure(bytes);
    return ptr;
}

void __wrap_vPortFree(void *ptr)
{
    record_free(ptr);
    __real_vPortFree(ptr);
}

void *__wrap_x264_malloc(int bytes)
{
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    void *ptr = __real_x264_malloc(bytes);
    retag_x264(ptr, caller);
    return ptr;
}
