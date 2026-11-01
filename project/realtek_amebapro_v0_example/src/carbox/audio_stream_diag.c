/* Event and receive-side audio diagnostics for the closed CarPlay objects.
 * Keep the RTP hot path free of printf; report only at navigation-tone events.
 * The SocketRecvFrom ABI follows the existing rx_crypto_buffers.c adapter. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"

extern void carbox_audio_pcm_tx_report(void);

#define AUDIO_DIAG_SLOTS 4U
#define AUDIO_RTP_MIN_CAPACITY 1024U
#define AUDIO_RTP_MAX_CAPACITY 4096U

typedef struct {
	TaskHandle_t task;
	int fd;
	uint16_t local_port;
	uint16_t first_seq;
	uint16_t udp_first_seq;
	uint16_t udp_last_seq;
	uint16_t wlan_first_seq;
	uint16_t wlan_last_seq;
	uint32_t first_us;
	uint32_t last_us;
	uint32_t udp_first_us;
	uint32_t udp_last_us;
	uint32_t udp_max_gap_us;
	uint32_t udp_packets;
	uint32_t udp_forward_gaps;
	uint32_t udp_missing_seq;
	uint32_t udp_reordered;
	uint32_t wlan_first_us;
	uint32_t wlan_last_us;
	uint32_t wlan_packets;
	uint32_t wlan_missing_seq;
	uint32_t wlan_forward_gaps;
	uint32_t wlan_reordered;
	uint32_t wlan_max_gap_us;
	uint32_t wlan_input_rejects;
	uint16_t wlan_jump_before;
	uint16_t wlan_jump_after;
	uint16_t wlan_max_jump;
	uint8_t wlan_have_seq;
	uint16_t udp_jump_before;
	uint16_t udp_jump_after;
	uint16_t udp_max_jump;
	uint32_t last_call_end_us;
	uint32_t max_gap_us;
	uint32_t max_recv_us;
	uint32_t max_outside_recv_us;
	uint32_t packets;
	uint32_t bytes;
	uint32_t forward_gaps;
	uint32_t missing_seq;
	uint32_t reordered;
	uint32_t recv_errors;
	uint32_t recv_calls;
	uint32_t node_get_calls;
	uint32_t node_get_failures;
	uint32_t node_empty_start_us;
	uint32_t node_empty_max_us;
	uint32_t node_empty_current_us;
	uint32_t busy_put_calls;
	uint32_t busy_put_failures;
	uint16_t jump_before;
	uint16_t jump_after;
	uint16_t max_jump;
	uint16_t last_seq;
	uint8_t have_seq;
	uint8_t udp_have_seq;
	uint8_t have_call;
} audio_diag_slot_t;

static audio_diag_slot_t audio_diag_slots[AUDIO_DIAG_SLOTS];
static uint32_t audio_diag_slot_overflow;
static uint32_t audio_lag_touch_calls;
static uint32_t audio_lag_touch_max_us;
static uint32_t audio_lag_touch_last_us;

static int audio_diag_is_rx_task(const char *name)
{
	return name != NULL &&
		(strncmp(name, "MainAltAudio", sizeof("MainAltAudio") - 1U) == 0 ||
		 strncmp(name, "MainAudio", sizeof("MainAudio") - 1U) == 0 ||
		 strncmp(name, "MainHighAudio", sizeof("MainHighAudio") - 1U) == 0);
}

/* The first GetFreeNode precedes the first SocketRecvFrom. Reserve a slot by
 * task now; SocketRecvFrom fills in its fd and local port on first use. */
static audio_diag_slot_t *audio_diag_find_task_locked(TaskHandle_t task)
{
	unsigned i;
	for (i = 0U; i < AUDIO_DIAG_SLOTS; ++i)
		if (audio_diag_slots[i].task == task)
			return &audio_diag_slots[i];
	return NULL;
}

extern int __real_RTPJitterBufferGetFreeNode(void *jitter, void **node);
int __wrap_RTPJitterBufferGetFreeNode(void *jitter, void **node)
{
	int result = __real_RTPJitterBufferGetFreeNode(jitter, node);
	const char *name = pcTaskGetName(NULL);
	audio_diag_slot_t *slot;
	uint32_t now_us;
	unsigned i;
	if (!audio_diag_is_rx_task(name)) return result;
	now_us = hal_read_curtime_us();
	taskENTER_CRITICAL();
	slot = audio_diag_find_task_locked(xTaskGetCurrentTaskHandle());
	if (slot == NULL) {
		for (i = 0U; i < AUDIO_DIAG_SLOTS; ++i)
			if (audio_diag_slots[i].task == NULL) {
				slot = &audio_diag_slots[i];
				slot->task = xTaskGetCurrentTaskHandle();
				slot->fd = -1;
				break;
			}
		if (slot == NULL) audio_diag_slot_overflow++;
	}
	if (slot != NULL) {
		slot->node_get_calls++;
		if (result != 0 || node == NULL || *node == NULL) {
			slot->node_get_failures++;
			if (slot->node_empty_start_us == 0U)
				slot->node_empty_start_us = now_us ? now_us : 1U;
			slot->node_empty_current_us = now_us - slot->node_empty_start_us;
			if (slot->node_empty_current_us > slot->node_empty_max_us)
				slot->node_empty_max_us = slot->node_empty_current_us;
		} else {
			slot->node_empty_start_us = 0U;
			slot->node_empty_current_us = 0U;
		}
	}
	taskEXIT_CRITICAL();
	/* A readable socket makes the next select return immediately. When the
	 * jitter buffer has no free node, retrying here without a pause spins and
	 * competes with the decoder that must return a node. Only this error path
	 * yields one RTOS tick; successful packet reception is unchanged. */
	if (result != 0) vTaskDelay(1U);
	return result;
}

extern int __real_RTPJitterBufferPutBusyNode(void *jitter, void *node);
#if CONFIG_AUDIO_JITTER_ADAPTIVE
extern void carbox_jitter_note_packet(void *jitter, uint32_t rtp_timestamp,
	uint32_t arrival_us);
#endif
int __wrap_RTPJitterBufferPutBusyNode(void *jitter, void *node)
{
	#if CONFIG_AUDIO_JITTER_ADAPTIVE
	uint32_t rtp_timestamp = 0U;
	uint32_t arrival_us = hal_read_curtime_us();
	/* AirPlayUtils.o reads the RTP sample timestamp from node + 12. Capture
	 * it before PutBusyNode can recycle the node during excess purging. */
	if (node != NULL) memcpy(&rtp_timestamp, (const uint8_t *)node + 12U,
		sizeof(rtp_timestamp));
	#endif
	int result = __real_RTPJitterBufferPutBusyNode(jitter, node);
	audio_diag_slot_t *slot;
	#if CONFIG_AUDIO_JITTER_ADAPTIVE
	if (result == 0 && node != NULL)
		carbox_jitter_note_packet(jitter, rtp_timestamp, arrival_us);
	#endif
	if (!audio_diag_is_rx_task(pcTaskGetName(NULL))) return result;
	taskENTER_CRITICAL();
	slot = audio_diag_find_task_locked(xTaskGetCurrentTaskHandle());
	if (slot != NULL) {
		slot->busy_put_calls++;
		if (result != 0) slot->busy_put_failures++;
	}
	taskEXIT_CRITICAL();
	return result;
}

void carbox_audio_lag_touch_record(uint32_t elapsed_us, uint32_t start_us)
{
	taskENTER_CRITICAL();
	audio_lag_touch_calls++;
	if (elapsed_us > audio_lag_touch_max_us)
		audio_lag_touch_max_us = elapsed_us;
	audio_lag_touch_last_us = start_us;
	taskEXIT_CRITICAL();
}

/* Called once per AAC-ELD diagnostic window, never from the touch or RX path. */
void carbox_audio_lag_report(uint32_t now_us)
{
	uint32_t calls, max_us, last_us;
	unsigned i;

	taskENTER_CRITICAL();
	calls = audio_lag_touch_calls;
	max_us = audio_lag_touch_max_us;
	last_us = audio_lag_touch_last_us;
	audio_lag_touch_calls = 0U;
	audio_lag_touch_max_us = 0U;
	taskEXIT_CRITICAL();
	printf("[AUDIO_LAG] hid_touch_calls=%lu hid_touch_max_us=%lu hid_touch_last_age_ms=%ld\n",
		(unsigned long)calls, (unsigned long)max_us,
		last_us ? (long)((now_us - last_us) / 1000U) : -1L);
	for (i = 0U; i < AUDIO_DIAG_SLOTS; ++i) {
		audio_diag_slot_t slot;
		taskENTER_CRITICAL();
		slot = audio_diag_slots[i];
		taskEXIT_CRITICAL();
		if (slot.task == NULL || (slot.packets == 0U && slot.udp_packets == 0U &&
		    slot.node_get_calls == 0U))
			continue;
		printf("[AUDIO_LAG] port=%u udp=%lu/%lu missing=%lu/%lu "
			"gap_ms=%lu/%lu outside_recv_ms=%lu socket_ms=%lu\n",
			slot.local_port, (unsigned long)slot.udp_packets,
			(unsigned long)slot.packets,
			(unsigned long)slot.udp_missing_seq,
			(unsigned long)slot.missing_seq,
			(unsigned long)(slot.udp_max_gap_us / 1000U),
			(unsigned long)(slot.max_gap_us / 1000U),
			(unsigned long)(slot.max_outside_recv_us / 1000U),
			(unsigned long)(slot.max_recv_us / 1000U));
		printf("[AUDIO_LAG_WLAN] port=%u wlan/udp/socket=%lu/%lu/%lu "
			"missing=%lu/%lu/%lu gap_ms=%lu/%lu/%lu input_reject=%lu\n",
			slot.local_port, (unsigned long)slot.wlan_packets,
			(unsigned long)slot.udp_packets, (unsigned long)slot.packets,
			(unsigned long)slot.wlan_missing_seq,
			(unsigned long)slot.udp_missing_seq,
			(unsigned long)slot.missing_seq,
			(unsigned long)(slot.wlan_max_gap_us / 1000U),
			(unsigned long)(slot.udp_max_gap_us / 1000U),
			(unsigned long)(slot.max_gap_us / 1000U),
			(unsigned long)slot.wlan_input_rejects);
		printf("[AUDIO_RX_LOOP] port=%u recv_calls=%lu node_get=%lu node_fail=%lu "
			"node_empty_max_ms=%lu node_empty_now_ms=%lu busy_put=%lu busy_fail=%lu\n",
			slot.local_port, (unsigned long)slot.recv_calls,
			(unsigned long)slot.node_get_calls,
			(unsigned long)slot.node_get_failures,
			(unsigned long)(slot.node_empty_max_us / 1000U),
			(unsigned long)(slot.node_empty_current_us / 1000U),
			(unsigned long)slot.busy_put_calls,
			(unsigned long)slot.busy_put_failures);
	}
}

static uint16_t audio_diag_socket_port(int fd)
{
	struct sockaddr_storage address;
	socklen_t length = sizeof(address);

	memset(&address, 0, sizeof(address));
	if (lwip_getsockname(fd, (struct sockaddr *)&address, &length) != 0)
		return 0U;
	if (address.ss_family == AF_INET)
		return lwip_ntohs(((const struct sockaddr_in *)&address)->sin_port);
	if (address.ss_family == AF_INET6)
		return lwip_ntohs(((const struct sockaddr_in6 *)&address)->sin6_port);
	return 0U;
}

/* Sample the Ethernet frame before ethernetif_recv hands it to lwIP. Only
 * ordinary IPv4/IPv6 UDP frames with a contiguous RTP header are counted;
 * VLAN tags are accepted, while IP fragments/extensions are ignored. */
int carbox_audio_diag_wlan_packet(const struct pbuf *p)
{
	const uint8_t *frame;
	uint32_t len, ip, udp, rtp;
	uint16_t ethertype, port, seq;
	unsigned i;
	int slot_index = -1;

	if (p == NULL || p->payload == NULL || p->len < 54U)
		return -1;
	frame = (const uint8_t *)p->payload;
	len = p->len;
	ip = 14U;
	ethertype = ((uint16_t)frame[12] << 8) | frame[13];
	if (ethertype == 0x8100U || ethertype == 0x88a8U) {
		if (len < 18U) return -1;
		ethertype = ((uint16_t)frame[16] << 8) | frame[17];
		ip += 4U;
	}
	if (ethertype == 0x0800U) {
		uint32_t ihl;
		if (len < ip + 20U || (frame[ip] >> 4) != 4U ||
		    frame[ip + 9U] != 17U ||
		    (frame[ip + 6U] & 0x1fU) != 0U || frame[ip + 7U] != 0U)
			return -1;
		ihl = (frame[ip] & 0x0fU) * 4U;
		if (ihl < 20U) return -1;
		udp = ip + ihl;
	} else if (ethertype == 0x86ddU) {
		if (len < ip + 40U || (frame[ip] >> 4) != 6U ||
		    frame[ip + 6U] != 17U)
			return -1;
		udp = ip + 40U;
	} else {
		return -1;
	}
	rtp = udp + 8U;
	if (len < rtp + 4U || (frame[rtp] & 0xc0U) != 0x80U)
		return -1;
	port = ((uint16_t)frame[udp + 2U] << 8) | frame[udp + 3U];
	seq = ((uint16_t)frame[rtp + 2U] << 8) | frame[rtp + 3U];
	taskENTER_CRITICAL();
	for (i = 0U; i < AUDIO_DIAG_SLOTS; ++i) {
		audio_diag_slot_t *slot = &audio_diag_slots[i];
		uint32_t now_us;
		uint16_t delta;
		if (port == 0U || slot->task == NULL || slot->local_port != port)
			continue;
		now_us = hal_read_curtime_us();
		if (slot->wlan_packets == 0U) {
			slot->wlan_first_us = now_us;
			slot->wlan_first_seq = seq;
		} else {
			uint32_t gap_us = now_us - slot->wlan_last_us;
			if (gap_us > slot->wlan_max_gap_us)
				slot->wlan_max_gap_us = gap_us;
		}
		if (slot->wlan_have_seq) {
			delta = (uint16_t)(seq - slot->wlan_last_seq);
			if (delta > 1U && delta < 0x8000U) {
				slot->wlan_forward_gaps++;
				slot->wlan_missing_seq += delta - 1U;
				if (delta > slot->wlan_max_jump) {
					slot->wlan_max_jump = delta;
					slot->wlan_jump_before = slot->wlan_last_seq;
					slot->wlan_jump_after = seq;
				}
			} else if (delta == 0U || delta >= 0x8000U) {
				slot->wlan_reordered++;
			}
		}
		slot->wlan_last_seq = seq;
		slot->wlan_last_us = now_us;
		slot->wlan_have_seq = 1U;
		slot->wlan_packets++;
		slot_index = (int)i;
		break;
	}
	taskEXIT_CRITICAL();
	return slot_index;
}

void carbox_audio_diag_wlan_input_reject(int slot_index)
{
	if (slot_index < 0 || slot_index >= (int)AUDIO_DIAG_SLOTS) return;
	taskENTER_CRITICAL();
	audio_diag_slots[slot_index].wlan_input_rejects++;
	taskEXIT_CRITICAL();
}

/* udp_input starts with the UDP header. This probe runs before lwIP's
 * checksum and PCB checks; it shows whether a packet reached the UDP layer,
 * not whether the socket accepted it. Keep the hot path free of printf. */
extern void __real_udp_input(struct pbuf *p, struct netif *inp);
void __wrap_udp_input(struct pbuf *p, struct netif *inp)
{
	const uint8_t *header = p != NULL ? (const uint8_t *)p->payload : NULL;
	uint16_t port;
	uint16_t seq;
	uint32_t now_us;
	unsigned i;

	if (p != NULL && p->len >= 20U &&
	    (header[8] & 0xc0U) == 0x80U) {
		port = ((uint16_t)header[2] << 8) | header[3];
		seq = ((uint16_t)header[10] << 8) | header[11];
		taskENTER_CRITICAL();
		for (i = 0U; i < AUDIO_DIAG_SLOTS; ++i) {
			audio_diag_slot_t *slot = &audio_diag_slots[i];
			uint16_t delta;

			if (port == 0U || slot->local_port != port || slot->task == NULL)
				continue;
			now_us = hal_read_curtime_us();
			if (slot->udp_packets == 0U) {
				slot->udp_first_us = now_us;
				slot->udp_first_seq = seq;
			} else {
				uint32_t gap_us = now_us - slot->udp_last_us;
				if (gap_us > slot->udp_max_gap_us)
					slot->udp_max_gap_us = gap_us;
			}
			if (slot->udp_have_seq) {
				delta = (uint16_t)(seq - slot->udp_last_seq);
				if (delta > 1U && delta < 0x8000U) {
					slot->udp_forward_gaps++;
					slot->udp_missing_seq += delta - 1U;
					if (delta > slot->udp_max_jump) {
						slot->udp_max_jump = delta;
						slot->udp_jump_before = slot->udp_last_seq;
						slot->udp_jump_after = seq;
					}
				} else if (delta == 0U || delta >= 0x8000U) {
					slot->udp_reordered++;
				}
			}
			slot->udp_last_seq = seq;
			slot->udp_have_seq = 1U;
			slot->udp_last_us = now_us;
			slot->udp_packets++;
		}
		taskEXIT_CRITICAL();
	}
	__real_udp_input(p, inp);
}

extern int __real_SocketRecvFrom(int fd, void *buffer, size_t capacity,
	size_t *received, uintptr_t s0, uintptr_t s1, uintptr_t s2,
	uintptr_t s3, uintptr_t s4, uintptr_t s5);

int __wrap_SocketRecvFrom(int fd, void *buffer, size_t capacity,
	size_t *received, uintptr_t s0, uintptr_t s1, uintptr_t s2,
	uintptr_t s3, uintptr_t s4, uintptr_t s5)
{
	const char *task_name = pcTaskGetName(NULL);
	int is_audio = capacity >= AUDIO_RTP_MIN_CAPACITY &&
		capacity <= AUDIO_RTP_MAX_CAPACITY && audio_diag_is_rx_task(task_name);
	uint32_t start_us;
	uint32_t end_us;
	audio_diag_slot_t *slot = NULL;
	TaskHandle_t task;
	uint16_t local_port = 0U;
	uint16_t seq = 0U;
	int has_rtp = 0;
	int result;
	unsigned i;

	/* Register the destination port before blocking in recv, so the first
	 * incoming RTP packet can be counted at udp_input as well. */
	if (is_audio) {
		task = xTaskGetCurrentTaskHandle();
		taskENTER_CRITICAL();
		for (i = 0U; i < AUDIO_DIAG_SLOTS; ++i) {
			if (audio_diag_slots[i].task == task && audio_diag_slots[i].fd == fd) {
				slot = &audio_diag_slots[i];
				local_port = slot->local_port;
				break;
			}
		}
		if (slot == NULL) {
			slot = audio_diag_find_task_locked(task);
			if (slot != NULL && slot->fd == -1) slot->fd = fd;
			if (slot != NULL) local_port = slot->local_port;
		}
		taskEXIT_CRITICAL();
		if (slot == NULL || local_port == 0U) {
			local_port = audio_diag_socket_port(fd);
			taskENTER_CRITICAL();
			if (slot == NULL)
				for (i = 0U; i < AUDIO_DIAG_SLOTS; ++i)
					if (audio_diag_slots[i].task == NULL) {
						slot = &audio_diag_slots[i];
						slot->task = task;
						slot->fd = fd;
						break;
					}
			if (slot != NULL) slot->local_port = local_port;
			if (slot == NULL) audio_diag_slot_overflow++;
			taskEXIT_CRITICAL();
		}
	}
	start_us = is_audio ? hal_read_curtime_us() : 0U;
	result = __real_SocketRecvFrom(fd, buffer, capacity, received,
		s0, s1, s2, s3, s4, s5);

	if (!is_audio) {
		return result;
	}
	end_us = hal_read_curtime_us();
	task = xTaskGetCurrentTaskHandle();
	if (result == 0 && received != NULL && *received >= 12U &&
		buffer != NULL && (((const uint8_t *)buffer)[0] & 0xc0U) == 0x80U) {
		const uint8_t *packet = (const uint8_t *)buffer;
		seq = ((uint16_t)packet[2] << 8) | packet[3];
		has_rtp = 1;
	}

	taskENTER_CRITICAL();
	slot = NULL;
	for (i = 0U; i < AUDIO_DIAG_SLOTS; ++i) {
		if (audio_diag_slots[i].task == task && audio_diag_slots[i].fd == fd) {
			slot = &audio_diag_slots[i];
			break;
		}
	}
	if (slot == NULL) {
		for (i = 0U; i < AUDIO_DIAG_SLOTS; ++i) {
			if (audio_diag_slots[i].task == NULL) {
				slot = &audio_diag_slots[i];
				slot->task = task;
				slot->fd = fd;
				slot->local_port = local_port;
				break;
			}
		}
	}
	if (slot != NULL) {
		uint32_t call_us = end_us - start_us;
		slot->recv_calls++;
		if (slot->have_call) {
			uint32_t outside_us = start_us - slot->last_call_end_us;
			if (outside_us > slot->max_outside_recv_us)
				slot->max_outside_recv_us = outside_us;
		}
		slot->last_call_end_us = end_us;
		slot->have_call = 1U;
		if (call_us > slot->max_recv_us) slot->max_recv_us = call_us;
		if (result != 0) {
			slot->recv_errors++;
		} else if (has_rtp) {
			uint16_t delta;

			if (slot->packets == 0U) {
				slot->first_us = end_us;
				slot->first_seq = seq;
			} else {
				uint32_t gap_us = end_us - slot->last_us;
				if (gap_us > slot->max_gap_us) slot->max_gap_us = gap_us;
			}
			if (slot->have_seq) {
				delta = (uint16_t)(seq - slot->last_seq);
				if (delta > 1U && delta < 0x8000U) {
					slot->forward_gaps++;
					slot->missing_seq += delta - 1U;
					if (delta > slot->max_jump) {
						slot->max_jump = delta;
						slot->jump_before = slot->last_seq;
						slot->jump_after = seq;
					}
				} else if (delta == 0U || delta >= 0x8000U) {
					slot->reordered++;
				}
			}
			slot->last_seq = seq;
			slot->have_seq = 1U;
			slot->last_us = end_us;
			slot->packets++;
			slot->bytes += (uint32_t)*received;
		}
	}
	taskEXIT_CRITICAL();
	return result;
}

extern void __real_CAccessory_StatusNotify(int status);
extern int CLinkInterface_GetActiveLink(void);
extern int CLinkInterface_IsNaviTone(int link);

/* In this LinkInterface.o, status 20/21 sets/clears the byte returned by
 * CLinkInterface_IsNaviTone. StatusNotify runs before that byte is changed.
 * Also snapshot phone transitions (10/11) to isolate stream restarts. */
void __wrap_CAccessory_StatusNotify(int status)
{
	int link;
	int previous;
	uint32_t now_us;
	unsigned i;
	unsigned reported = 0U;

	if (status != 10 && status != 11 && status != 20 && status != 21) {
		__real_CAccessory_StatusNotify(status);
		return;
	}
	link = CLinkInterface_GetActiveLink();
	previous = (status == 20 || status == 21) ?
		CLinkInterface_IsNaviTone(link) : -1;
	__real_CAccessory_StatusNotify(status);
	now_us = hal_read_curtime_us();
	printf("[AUDIO_EVT] link=%d status=%d event=%s previous_nav=%d\n",
		link, status, status == 20 ? "nav_start" :
		status == 21 ? "nav_stop" :
		status == 10 ? "phone_start" : "phone_stop", previous);

	/* A short snapshot at each state transition covers rapid start/stop cycles.
	 * Reset after copying so later events report only the intervening traffic. */
	for (i = 0U; i < AUDIO_DIAG_SLOTS; ++i) {
		audio_diag_slot_t snapshot;
		taskENTER_CRITICAL();
		snapshot = audio_diag_slots[i];
		memset(&audio_diag_slots[i], 0, sizeof(audio_diag_slots[i]));
		taskEXIT_CRITICAL();
		if (snapshot.task == NULL) continue;
		reported++;
		printf("[AUDIO_RX] status=%d task=%p fd=%d port=%u "
			"socket=%lu seq=%u..%u bytes=%lu "
			"seq_gaps=%lu missing=%lu max_jump=%u(%u..%u) "
			"reorder=%lu recv_errors=%lu "
			"span_ms=%lu max_gap_ms=%lu max_socket_ms=%lu "
			"max_outside_ms=%lu last_age_ms=%lu\n",
			status, (void *)snapshot.task, snapshot.fd, snapshot.local_port,
			(unsigned long)snapshot.packets, snapshot.first_seq,
			snapshot.last_seq, (unsigned long)snapshot.bytes,
			(unsigned long)snapshot.forward_gaps,
			(unsigned long)snapshot.missing_seq,
			snapshot.max_jump, snapshot.jump_before,
			snapshot.jump_after,
			(unsigned long)snapshot.reordered,
			(unsigned long)snapshot.recv_errors,
			(unsigned long)(snapshot.packets ?
				(snapshot.last_us - snapshot.first_us) / 1000U : 0U),
			(unsigned long)(snapshot.max_gap_us / 1000U),
			(unsigned long)(snapshot.max_recv_us / 1000U),
			(unsigned long)(snapshot.max_outside_recv_us / 1000U),
			(unsigned long)(snapshot.packets ?
				(now_us - snapshot.last_us) / 1000U : 0U));
		printf("[AUDIO_UDP_PRE] status=%d port=%u packets=%lu seq=%u..%u "
			"seq_gaps=%lu missing=%lu max_jump=%u(%u..%u) "
			"reorder=%lu max_gap_ms=%lu "
			"last_age_ms=%lu\n",
			status, snapshot.local_port,
			(unsigned long)snapshot.udp_packets, snapshot.udp_first_seq,
			snapshot.udp_last_seq,
			(unsigned long)snapshot.udp_forward_gaps,
			(unsigned long)snapshot.udp_missing_seq,
			snapshot.udp_max_jump, snapshot.udp_jump_before,
			snapshot.udp_jump_after,
			(unsigned long)snapshot.udp_reordered,
			(unsigned long)(snapshot.udp_max_gap_us / 1000U),
			(unsigned long)(snapshot.udp_packets ?
				(now_us - snapshot.udp_last_us) / 1000U : 0U));
		printf("[AUDIO_WLAN_PRE] status=%d port=%u packets=%lu seq=%u..%u "
			"seq_gaps=%lu missing=%lu max_jump=%u(%u..%u) "
			"reorder=%lu max_gap_ms=%lu input_reject=%lu\n",
			status, snapshot.local_port,
			(unsigned long)snapshot.wlan_packets,
			snapshot.wlan_first_seq, snapshot.wlan_last_seq,
			(unsigned long)snapshot.wlan_forward_gaps,
			(unsigned long)snapshot.wlan_missing_seq,
			snapshot.wlan_max_jump, snapshot.wlan_jump_before,
			snapshot.wlan_jump_after,
			(unsigned long)snapshot.wlan_reordered,
			(unsigned long)(snapshot.wlan_max_gap_us / 1000U),
			(unsigned long)snapshot.wlan_input_rejects);
		printf("[AUDIO_RX_LOOP] status=%d port=%u recv_calls=%lu node_get=%lu "
			"node_fail=%lu node_empty_max_ms=%lu busy_put=%lu busy_fail=%lu\n",
			status, snapshot.local_port,
			(unsigned long)snapshot.recv_calls,
			(unsigned long)snapshot.node_get_calls,
			(unsigned long)snapshot.node_get_failures,
			(unsigned long)(snapshot.node_empty_max_us / 1000U),
			(unsigned long)snapshot.busy_put_calls,
			(unsigned long)snapshot.busy_put_failures);
	}
	if (reported == 0U) {
		printf("[AUDIO_RX] status=%d slots=0\n", status);
	}
	if (audio_diag_slot_overflow != 0U) {
		uint32_t overflow;
		taskENTER_CRITICAL();
		overflow = audio_diag_slot_overflow;
		audio_diag_slot_overflow = 0U;
		taskEXIT_CRITICAL();
		printf("[AUDIO_RX] status=%d slot_overflow=%lu\n", status,
			(unsigned long)overflow);
	}
	carbox_audio_pcm_tx_report();
}
