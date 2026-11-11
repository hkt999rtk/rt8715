/* 车机 timing 使用媒体时钟。保留原厂 NTP epoch/包格式计算。
 * iAP2 仍可更新系统日期，只有 AirPlayTimer.o 的时间入口被替换。
 */
#include "airplay_timer_clock.h"
#include "screen_hw_timestamp.h"
#include <stddef.h>
#include "FreeRTOS.h"
#include "task.h"

/* 独立于诊断开关，必须与原厂 Start 保存的 baseline 完全一致。 */
static uint32_t media_baseline_s, media_generation;
#if CONFIG_AIRPLAY_TIMER_CLOCK_PROFILE
static uint32_t video_before_start, video_underflow;
#endif

uint64_t carbox_media_video_us(void)
{
	uint64_t us, base;
	/* 与 Start 的更新互斥；内部只读计时器，无等待和 I/O。允许嵌套临界区。 */
	taskENTER_CRITICAL();
	us = carbox_media_monotonic_us();
	base = (uint64_t)media_baseline_s * 1000000ULL;
#if CONFIG_AIRPLAY_TIMER_CLOCK_PROFILE
	if (!media_generation) video_before_start++;
	if (us < base) video_underflow++;
#endif
	if (us < base) us = base;
	taskEXIT_CRITICAL();
	return us - base;
}

#ifndef CONFIG_SCREEN_CURRENT_TIMESTAMP
#define CONFIG_SCREEN_CURRENT_TIMESTAMP 0
#endif

#if CONFIG_AIRPLAY_TIMER_CLOCK_PROFILE
#include "diag.h"

struct clock_evidence {
	uint32_t calls, packets, sent, errors, videos;
	int fd;
	uint64_t clock_us, packet_us, video_us, video_ntp;
	uint32_t rx_s, rx_f, tx_s, tx_f;
	uint32_t send_max_us, video_back, video_repeat, video_old, video_future;
	uint32_t gap_max_us, age_max_us, age_last_us;
	uint64_t send_total_us;
	uint32_t starts, baseline_s, configs, config_flags;
	uint32_t packet_baseline_s, video_generation;
	uint64_t start_us, config_us, config_ntp;
};
static struct clock_evidence evidence;

static uint32_t bounded_us(uint64_t us)
{
	return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

static uint64_t ntp_to_us(uint64_t ntp)
{
	/* 向上取整，消除生成 32.32 时向下舍入产生的 1us 假年龄。 */
	return (ntp >> 32) * 1000000ULL +
		(((uint64_t)(uint32_t)ntp * 1000000ULL + 0xffffffffULL) >> 32);
}

static uint32_t read_be32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		((uint32_t)p[2] << 8) | p[3];
}
#endif

int carbox_airplay_timer_gettimeofday(struct timeval *tv, void *tz)
{
	uint64_t us;
	(void)tz; /* 原厂所有调用均传 NULL。 */
	if (!tv) return -1;
	us = carbox_media_monotonic_us();
	tv->tv_sec = us / 1000000ULL;
	tv->tv_usec = us % 1000000ULL;
#if CONFIG_AIRPLAY_TIMER_CLOCK_PROFILE
	taskENTER_CRITICAL();
	evidence.calls++;
	evidence.clock_us = us;
	taskEXIT_CRITICAL();
#endif
	return 0;
}

int carbox_airplay_timer_start_time(struct timeval *tv, void *tz)
{
	int result = carbox_airplay_timer_gettimeofday(tv, tz);
	if (result) return result;
#if CONFIG_AIRPLAY_TIMER_CLOCK_PROFILE
	uint64_t captured_us = (uint64_t)tv->tv_sec * 1000000ULL + tv->tv_usec;
#endif
	/* 原厂 Start 只保存 tv_sec；SetTime 会从每次采样减去该 baseline。
	 * 默认保留原厂 baseline，video 单独减去同一整数秒；
	 * SHARED_ORIGIN=1 仅供与上一版全 uptime 起点做 A/B。
	 * 不能全局返回 0，也不能减固定 2 秒：启动时刻和重连时刻都不固定。
	 * tv_usec 不参与 baseline，保留原采样值；此入口不能替代普通采时。
	 */
#if CONFIG_AIRPLAY_TIMER_SHARED_ORIGIN
	tv->tv_sec = 0;
#endif
	taskENTER_CRITICAL();
	media_baseline_s = (uint32_t)tv->tv_sec;
	media_generation++;
#if CONFIG_AIRPLAY_TIMER_CLOCK_PROFILE
	evidence.starts++;
	evidence.start_us = captured_us;
	evidence.baseline_s = (uint32_t)tv->tv_sec;
#endif
	taskEXIT_CRITICAL();
	return 0;
}

#if CONFIG_AIRPLAY_TIMER_CLOCK_PROFILE
/* 只观察 Configure 的发送入口，保持原厂发送及 session wrapper 语义。 */
extern void AirPlayScreen_SendScreenData(char *data, int size);
void carbox_airplay_clock_config_send(char *data, int size)
{
	const unsigned char *p = (const unsigned char *)data;
	if (p && size >= 128 && p[4] == 1) {
		uint64_t ntp = 0, us;
		unsigned i;
		for (i = 0; i < 8; i++) ntp |= (uint64_t)p[8 + i] << (8 * i);
		us = carbox_media_monotonic_us();
		taskENTER_CRITICAL();
		evidence.configs++;
		evidence.config_flags = p[6];
		evidence.config_us = us; evidence.config_ntp = ntp;
		taskEXIT_CRITICAL();
	}
	AirPlayScreen_SendScreenData(data, size);
}

ssize_t carbox_airplay_timer_sendto(int fd, const void *data, size_t size,
		int flags, const struct sockaddr *to, socklen_t tolen)
{
	const unsigned char *p = data;
	uint32_t rx_s = 0, rx_f = 0, tx_s = 0, tx_f = 0, baseline = 0;
	uint64_t us = 0;
	int valid = p && size == 32 && p[1] == 0xd3;
	ssize_t result;
	/* 只读实际 timing 回复。一次调用，原样转发；不重试、不改 errno。 */
	if (valid) {
		rx_s = read_be32(p + 16); rx_f = read_be32(p + 20);
		tx_s = read_be32(p + 24); tx_f = read_be32(p + 28);
		taskENTER_CRITICAL();
		us = carbox_media_monotonic_us();
		baseline = media_baseline_s;
		taskEXIT_CRITICAL();
	}
	result = lwip_sendto(fd, data, size, flags, to, tolen);
	if (valid) {
		/* 这里只读 HW/RTOS 时钟，不调用 socket/libc，不触碰 SDK 的全局
		 * lwip_err。不能保存再写回该全局变量，以免覆盖其它线程的错误。 */
		uint64_t elapsed = carbox_media_monotonic_us() - us;
		taskENTER_CRITICAL();
		evidence.packets++;
		if (result == (ssize_t)size) evidence.sent++; else evidence.errors++;
		evidence.fd = fd; evidence.packet_us = us; evidence.packet_baseline_s = baseline;
		evidence.rx_s = rx_s; evidence.rx_f = rx_f;
		evidence.tx_s = tx_s; evidence.tx_f = tx_f;
		evidence.send_total_us += elapsed;
		if (elapsed > evidence.send_max_us) evidence.send_max_us = bounded_us(elapsed);
		taskEXIT_CRITICAL();
	}
	return result;
}

void carbox_airplay_clock_video(const void *header)
{
	const unsigned char *p = header;
	uint64_t ntp = 0, us, stamp_us, gap;
	unsigned i;
	if (!p) return;
	/* 原厂 128-byte screen header：偏移 8 的 params[0] 为 LE 32.32。
	 * 只读取加密前真正写入 header 的值，不能用观测时间冒充 wire 值。
	 */
	for (i = 0; i < 8; i++) ntp |= (uint64_t)p[8 + i] << (8 * i);
	taskENTER_CRITICAL();
#if CONFIG_SCREEN_TIMER_ORIGIN
	us = carbox_media_video_us();
#else
	us = carbox_media_monotonic_us();
#endif
	stamp_us = ntp_to_us(ntp);
	if (evidence.videos && evidence.video_generation == media_generation) {
		if (ntp < evidence.video_ntp) evidence.video_back++;
		else if (ntp == evidence.video_ntp) evidence.video_repeat++;
		else {
			gap = stamp_us - ntp_to_us(evidence.video_ntp);
			if (gap > evidence.gap_max_us) evidence.gap_max_us = bounded_us(gap);
		}
	}
	evidence.age_last_us = stamp_us <= us ? bounded_us(us - stamp_us) : 0;
	if (stamp_us > us) evidence.video_future++;
	if (evidence.age_last_us > evidence.age_max_us) evidence.age_max_us = evidence.age_last_us;
	if (evidence.age_last_us > 20000U) evidence.video_old++;
	evidence.video_generation = media_generation;
	evidence.videos++;
	evidence.video_ntp = ntp;
	evidence.video_us = us + (CONFIG_SCREEN_TIMER_ORIGIN ? (uint64_t)media_baseline_s * 1000000ULL : 0);
	taskEXIT_CRITICAL();
}

void carbox_airplay_clock_report(uint32_t sequence)
{
	struct clock_evidence s;
	struct timeval wall;
	uint32_t early, underflow;
	static uint32_t previous_calls, previous_packets, previous_videos, previous_configs;
	taskENTER_CRITICAL();
	s = evidence; early = video_before_start; underflow = video_underflow;
	taskEXIT_CRITICAL();
	if (s.calls == previous_calls && s.packets == previous_packets &&
	    s.videos == previous_videos && s.configs == previous_configs) return;
	previous_calls = s.calls; previous_packets = s.packets; previous_videos = s.videos;
	previous_configs = s.configs;
	/* 只在现有统计线程读真实日期/打印；时间和发送热路径不输出 UART。 */
	gettimeofday(&wall, NULL);
	rt_printf("[MEDIA_CLOCK][%lu] source=monotonic cumulative=1 wall_now_s=%ld calls=%lu last_clock=%lu.%06lu timing_try/sent/error=%lu/%lu/%lu fd=%d sample=%lu.%06lu rx=%08lx:%08lx tx=%08lx:%08lx send_avg/max_us=%lu/%lu\r\n",
		(unsigned long)sequence, (long)wall.tv_sec, (unsigned long)s.calls,
		(unsigned long)(s.clock_us / 1000000ULL), (unsigned long)(s.clock_us % 1000000ULL),
		(unsigned long)s.packets, (unsigned long)s.sent, (unsigned long)s.errors, s.fd,
		(unsigned long)(s.packet_us / 1000000ULL), (unsigned long)(s.packet_us % 1000000ULL),
		(unsigned long)s.rx_s, (unsigned long)s.rx_f, (unsigned long)s.tx_s, (unsigned long)s.tx_f,
		(unsigned long)(s.packets ? s.send_total_us/s.packets : 0), (unsigned long)s.send_max_us);
	if (s.starts) rt_printf("[MEDIA_ORIGIN][%lu] shared_origin=%u starts=%lu captured=%lu.%06lu baseline_s=%lu video_align=%u early/underflow=%lu/%lu\r\n",
		(unsigned long)sequence, (unsigned)CONFIG_AIRPLAY_TIMER_SHARED_ORIGIN,
		(unsigned long)s.starts, (unsigned long)(s.start_us / 1000000ULL),
		(unsigned long)(s.start_us % 1000000ULL), (unsigned long)s.baseline_s,
		(unsigned)CONFIG_SCREEN_TIMER_ORIGIN, (unsigned long)early, (unsigned long)underflow);
	if (s.packets) {
		/* 使用同次发送前的 uptime，不能把不同时刻的 video/timing 直接相减。
		 * 正常结果略小于 0（填包到发送的执行时间）；旧模式额外少 baseline 秒。
		 * uint32 减法同时处理 NTP 秒的自然回卷。超出打印范围明确标记。
		 */
		uint64_t ntp = ((uint64_t)(uint32_t)(s.tx_s - 0x83aa7e80U) << 32) | s.tx_f;
		int64_t delta = (int64_t)ntp_to_us(ntp) - (int64_t)s.packet_us;
		int clamped = delta > INT32_MAX || delta < INT32_MIN;
		int64_t aligned = delta + (int64_t)s.packet_baseline_s * 1000000LL;
		long aligned_print = aligned > INT32_MAX ? INT32_MAX : aligned < INT32_MIN ? INT32_MIN : (int32_t)aligned;
		long printable = delta > INT32_MAX ? INT32_MAX : delta < INT32_MIN ? INT32_MIN : (int32_t)delta;
		rt_printf("[MEDIA_BASIS][%lu] tx_minus_uptime_us=%ld clamped=%d tx_minus_session_us=%ld baseline_s=%lu stage=before_send\r\n",
			(unsigned long)sequence, printable, clamped, aligned_print, (unsigned long)s.packet_baseline_s);
	}
	if (s.configs) rt_printf("[MEDIA_CONFIG][%lu] count=%lu flags1=0x%02lx use_ts=%lu header_ntp=%08lx:%08lx sample=%lu.%06lu stage=before_send\r\n",
		(unsigned long)sequence, (unsigned long)s.configs, (unsigned long)s.config_flags,
		(unsigned long)((s.config_flags >> 1) & 1), (unsigned long)(s.config_ntp >> 32),
		(unsigned long)(uint32_t)s.config_ntp, (unsigned long)(s.config_us / 1000000ULL),
		(unsigned long)(s.config_us % 1000000ULL));
	if (s.videos) rt_printf("[MEDIA_VIDEO][%lu] current_ts=%u cumulative=1 frames=%lu header_ntp=%08lx:%08lx sample=%lu.%06lu stage=pre_encrypt age_last/max_us=%lu/%lu old20ms=%lu future=%lu back/repeat=%lu/%lu gap_max_us=%lu\r\n",
		(unsigned long)sequence, (unsigned)CONFIG_SCREEN_CURRENT_TIMESTAMP, (unsigned long)s.videos,
		(unsigned long)(s.video_ntp >> 32), (unsigned long)(uint32_t)s.video_ntp,
		(unsigned long)(s.video_us / 1000000ULL), (unsigned long)(s.video_us % 1000000ULL),
		(unsigned long)s.age_last_us, (unsigned long)s.age_max_us,
		(unsigned long)s.video_old, (unsigned long)s.video_future,
		(unsigned long)s.video_back, (unsigned long)s.video_repeat, (unsigned long)s.gap_max_us);
}
#endif
