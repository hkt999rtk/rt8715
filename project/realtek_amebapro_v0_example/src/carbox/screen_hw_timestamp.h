#ifndef CARBOX_SCREEN_HW_TIMESTAMP_H
#define CARBOX_SCREEN_HW_TIMESTAMP_H
#include <stdint.h>

/* 媒体共用的开机微秒时钟；不受 iAP2/SNTP 修改系统日期影响。 */
uint64_t carbox_media_monotonic_us(void);
#endif
