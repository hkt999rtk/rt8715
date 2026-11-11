#ifndef CARBOX_MOVE_LAG_DIAG_H
#define CARBOX_MOVE_LAG_DIAG_H
#include <stdint.h>
enum move_lag_stage {
    MOVE_HID_DRAIN, MOVE_HID_QUEUE, MOVE_HID_WRITE,
    MOVE_DISPATCH_WAIT, MOVE_DISPATCH_EXEC, MOVE_VIDEO_RECV,
    MOVE_VIDEO_HANDOVER, MOVE_VIDEO_QUEUE, MOVE_VIDEO_FRAME,
    MOVE_VIDEO_WRITE, MOVE_VIDEO_POLL, MOVE_VIDEO_ARRIVAL, MOVE_STAGE_COUNT
};
#if defined(CONFIG_MOVE_LAG_PROFILE) && CONFIG_MOVE_LAG_PROFILE
void carbox_move_lag_record(unsigned stage, uint32_t id, uint32_t start,
                           uint32_t elapsed, uint32_t detail, int result);
void carbox_move_lag_report(uint32_t sequence);
#else
/* 关闭时不计算参数，避免残留计时调用。 */
#define carbox_move_lag_record(...) ((void)0)
static inline void carbox_move_lag_report(uint32_t sequence) { (void)sequence; }
#endif
#endif
