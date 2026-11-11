#ifndef CARBOX_AIRPLAY_TIMER_CLOCK_H
#define CARBOX_AIRPLAY_TIMER_CLOCK_H
#include <stdint.h>
#include <sys/time.h>

#ifndef CONFIG_AIRPLAY_TIMER_CLOCK_PROFILE
#define CONFIG_AIRPLAY_TIMER_CLOCK_PROFILE 0
#endif
#ifndef CONFIG_AIRPLAY_TIMER_SHARED_ORIGIN
#define CONFIG_AIRPLAY_TIMER_SHARED_ORIGIN 0
#endif

#ifndef CONFIG_SCREEN_TIMER_ORIGIN
#define CONFIG_SCREEN_TIMER_ORIGIN 0
#endif
/* video 使用 Timer_Start 实际保存的整数秒起点；不会改变 audio sample 计数。 */
uint64_t carbox_media_video_us(void);

/* 只供 AirPlayTimer.o 调用，绝不全局替换 gettimeofday。 */
int carbox_airplay_timer_gettimeofday(struct timeval *tv, void *tz);
/* 仅替换 Timer_Start 的 baseline 采样，不能用于回复时间采样。 */
int carbox_airplay_timer_start_time(struct timeval *tv, void *tz);

#if CONFIG_AIRPLAY_TIMER_CLOCK_PROFILE
#include "lwip/sockets.h"
ssize_t carbox_airplay_timer_sendto(int fd, const void *data, size_t size,
		int flags, const struct sockaddr *to, socklen_t tolen);
void carbox_airplay_clock_video(const void *header);
void carbox_airplay_clock_config_send(char *data, int size);
void carbox_airplay_clock_report(uint32_t sequence);
#else
static inline void carbox_airplay_clock_video(const void *header) { (void)header; }
static inline void carbox_airplay_clock_report(uint32_t sequence) { (void)sequence; }
#endif
#endif
