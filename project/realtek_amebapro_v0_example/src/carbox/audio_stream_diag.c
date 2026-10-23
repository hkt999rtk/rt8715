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
	uint32_t first_us;
	uint32_t last_us;
	uint32_t udp_first_us;
	uint32_t udp_last_us;
	uint32_t udp_max_gap_us;
	uint32_t udp_packets;
	uint32_t udp_forward_gaps;
	uint32_t udp_missing_seq;
	uint32_t udp_reordered;
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
		capacity <= AUDIO_RTP_MAX_CAPACITY && task_name != NULL &&
		(strncmp(task_name, "MainAltAudio", sizeof("MainAltAudio") - 1U) == 0 ||
		 strncmp(task_name, "MainAudio", sizeof("MainAudio") - 1U) == 0 ||
		 strncmp(task_name, "MainHighAudio", sizeof("MainHighAudio") - 1U) == 0);
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
		taskEXIT_CRITICAL();
		if (slot == NULL) {
			local_port = audio_diag_socket_port(fd);
			taskENTER_CRITICAL();
			for (i = 0U; i < AUDIO_DIAG_SLOTS; ++i) {
				if (audio_diag_slots[i].task == NULL) {
					slot = &audio_diag_slots[i];
					slot->task = task;
					slot->fd = fd;
					slot->local_port = local_port;
					break;
				}
			}
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
}
