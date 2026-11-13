#ifndef AIRPLAY_MEDIA_CLOCK_SOURCE_H
#define AIRPLAY_MEDIA_CLOCK_SOURCE_H
#include "AirPlayMediaClockDiscipline.h"
#define MEDIA_SOURCE_QUEUE 16U
struct media_sample {
    uint64_t local_us, source_us, rtos_us, upticks;
    int64_t adjustment_ns;
    uint32_t generation, flags, pair_span_us, query_us;
};
/* owner 只作身份比较，绝不由异步统计线程解引用。
 * 调用方必须在短临界区内调用本模块接口；所有处理为固定内存和有界操作。 */
struct media_source {
    uintptr_t owner;
    uint32_t generation, head, count, reset_pending;
    uint32_t lost, stale, conflicts;
    struct media_sample queue[MEDIA_SOURCE_QUEUE];
};
void MediaSource_Init(struct media_source *);
uint32_t MediaSource_Begin(struct media_source *,uintptr_t);
void MediaSource_End(struct media_source *,uintptr_t);
void MediaSource_Push(struct media_source *,uintptr_t,const struct media_sample *);
int MediaSource_Pop(struct media_source *,struct media_sample *);
#endif
