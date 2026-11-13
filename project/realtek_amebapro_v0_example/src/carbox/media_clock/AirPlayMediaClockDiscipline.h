#ifndef AIRPLAY_MEDIA_CLOCK_DISCIPLINE_H
#define AIRPLAY_MEDIA_CLOCK_DISCIPLINE_H
#include <stdint.h>
#define MEDIA_SYNC_VALID 1U
#define MEDIA_PHASE_STEP 2U
#define MEDIA_SOURCE_RESET 4U
#define MEDIA_SOURCE_SETTLING 8U
#define MEDIA_WINDOW_POINTS 32U
enum media_clock_state { MEDIA_LOCAL, MEDIA_ACQUIRE, MEDIA_TRACK, MEDIA_HOLDOVER };
/* 纯状态模块：调用方串行化；不含锁、系统时钟、动态分配或 I/O。
 * 速率单位 ppb（1 ppm=1000 ppb），时间单位 us，源绝对偏移不进入输出。
 */
struct media_discipline {
    uint64_t anchor_local, anchor_media, fraction, last_update, last_sync;
    int64_t target_ppb, applied_ppb, estimated_ppb;
    uint64_t local[MEDIA_WINDOW_POINTS], source[MEDIA_WINDOW_POINTS];
    uint32_t generation, count, state, rejected, out_of_range, steps;
    uint32_t have_sync, residual_us;
};
void MediaDiscipline_Init(struct media_discipline *, uint64_t, uint64_t);
void MediaDiscipline_SourceReset(struct media_discipline *, uint32_t);
void MediaDiscipline_Observe(struct media_discipline *, uint32_t, uint64_t, uint64_t, uint64_t, uint32_t);
void MediaDiscipline_Update(struct media_discipline *, uint64_t, int);
uint64_t MediaDiscipline_Now(const struct media_discipline *, uint64_t);
uint64_t MediaDiscipline_LocalDeadline(const struct media_discipline *, uint64_t);
#endif
