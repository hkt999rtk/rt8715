/* Count PCM losses between the customer audio callback and its UDP output.
 * Only three validated AirPlayAudio.o calls are redirected to these helpers.
 * The audio path never prints; the existing AAC diagnostic window reports it.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "lwip/sockets.h"

#define AUDIO_PCM_TX_SLOTS 16U

typedef struct {
	void *ring;
	TaskHandle_t output_task;
	int fd;
	uint32_t last_activity_us;
	uint32_t push_ok;
	uint32_t push_fail;
	uint32_t push_bytes;
	uint32_t push_drop_bytes;
	uint32_t pop_ok;
	uint32_t pop_empty;
	uint32_t pop_bytes;
	uint32_t send_ok;
	uint32_t send_fail;
	uint32_t send_short;
	uint32_t send_bytes;
	uint32_t send_drop_bytes;
	uint32_t send_max_us;
	int last_errno;
} audio_pcm_tx_slot_t;

static audio_pcm_tx_slot_t tx_slots[AUDIO_PCM_TX_SLOTS];
static uint32_t tx_slot_overflow;

extern int RingBuffer_push(void *ring, const void *data, uint32_t length);
extern int RingBuffer_pop(void *ring, void *data, uint32_t length);
extern int lwip_errno(void);

static audio_pcm_tx_slot_t *tx_find_ring_locked(void *ring)
{
	unsigned i;
	for (i = 0U; i < AUDIO_PCM_TX_SLOTS; ++i)
		if (tx_slots[i].ring == ring) return &tx_slots[i];
	for (i = 0U; i < AUDIO_PCM_TX_SLOTS; ++i) {
		if (tx_slots[i].ring == NULL) {
			tx_slots[i].ring = ring;
			tx_slots[i].fd = -1;
			return &tx_slots[i];
		}
	}
	tx_slot_overflow++;
	return NULL;
}

int carbox_audio_pcm_tx_push(void *ring, const void *data, uint32_t length)
{
	int result = RingBuffer_push(ring, data, length);
	audio_pcm_tx_slot_t *slot;
	taskENTER_CRITICAL();
	slot = tx_find_ring_locked(ring);
	if (slot != NULL) {
		slot->last_activity_us = hal_read_curtime_us();
		if (result != 0) {
			slot->push_ok++;
			slot->push_bytes += length;
		} else {
			slot->push_fail++;
			slot->push_drop_bytes += length;
		}
	}
	taskEXIT_CRITICAL();
	return result;
}

int carbox_audio_pcm_tx_pop(void *ring, void *data, uint32_t length)
{
	int result = RingBuffer_pop(ring, data, length);
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	audio_pcm_tx_slot_t *slot;
	unsigned i;
	taskENTER_CRITICAL();
	slot = tx_find_ring_locked(ring);
	if (slot != NULL) {
		/* A recycled output task handle must never identify an older ring. */
		for (i = 0U; i < AUDIO_PCM_TX_SLOTS; ++i)
			if (&tx_slots[i] != slot && tx_slots[i].output_task == task)
				tx_slots[i].output_task = NULL;
		slot->output_task = task;
		slot->last_activity_us = hal_read_curtime_us();
		if (result != 0) {
			slot->pop_ok++;
			slot->pop_bytes += length;
		} else {
			slot->pop_empty++;
		}
	}
	taskEXIT_CRITICAL();
	return result;
}

ssize_t carbox_audio_pcm_tx_sendto(int fd, const void *data, size_t length,
	int flags, const struct sockaddr *to, socklen_t tolen)
{
	uint32_t start_us = hal_read_curtime_us();
	ssize_t result = lwip_sendto(fd, data, length, flags, to, tolen);
	uint32_t elapsed_us = hal_read_curtime_us() - start_us;
	int saved_errno = result < 0 ? lwip_errno() : 0;
	TaskHandle_t task = xTaskGetCurrentTaskHandle();
	unsigned i;
	taskENTER_CRITICAL();
	for (i = 0U; i < AUDIO_PCM_TX_SLOTS; ++i) {
		audio_pcm_tx_slot_t *slot = &tx_slots[i];
		if (slot->ring == NULL || slot->output_task != task) continue;
		slot->fd = fd;
		slot->last_activity_us = start_us + elapsed_us;
		if (elapsed_us > slot->send_max_us)
			slot->send_max_us = elapsed_us;
		if (result == (ssize_t)length) {
			slot->send_ok++;
			slot->send_bytes += (uint32_t)length;
		} else {
			if (result < 0) {
				slot->send_fail++;
				slot->last_errno = saved_errno;
				slot->send_drop_bytes += (uint32_t)length;
			} else {
				slot->send_short++;
				slot->send_bytes += (uint32_t)result;
				slot->send_drop_bytes += (uint32_t)(length - (size_t)result);
			}
		}
		break;
	}
	taskEXIT_CRITICAL();
	return result;
}

void carbox_audio_pcm_tx_report(void)
{
	unsigned i;
	uint32_t now_us = hal_read_curtime_us();
	for (i = 0U; i < AUDIO_PCM_TX_SLOTS; ++i) {
		audio_pcm_tx_slot_t snapshot;
		taskENTER_CRITICAL();
		snapshot = tx_slots[i];
		if (snapshot.ring != NULL &&
		    (uint32_t)(now_us - snapshot.last_activity_us) >= 10000000U)
			memset(&tx_slots[i], 0, sizeof(tx_slots[i]));
		else
			memset(&tx_slots[i].push_ok, 0,
				sizeof(tx_slots[i]) - offsetof(audio_pcm_tx_slot_t, push_ok));
		taskEXIT_CRITICAL();
		if (snapshot.ring == NULL ||
		    (snapshot.push_ok == 0U && snapshot.push_fail == 0U &&
		     snapshot.pop_ok == 0U && snapshot.pop_empty == 0U &&
		     snapshot.send_ok == 0U && snapshot.send_fail == 0U &&
		     snapshot.send_short == 0U)) continue;
		printf("[AUDIO_PCM_TX] ring=%p stage=in push=%lu fail=%lu bytes=%lu drop_bytes=%lu\n",
			snapshot.ring,
			(unsigned long)snapshot.push_ok,
			(unsigned long)snapshot.push_fail,
			(unsigned long)snapshot.push_bytes,
			(unsigned long)snapshot.push_drop_bytes);
		printf("[AUDIO_PCM_TX] ring=%p stage=pop ok=%lu empty=%lu bytes=%lu\n",
			snapshot.ring,
			(unsigned long)snapshot.pop_ok,
			(unsigned long)snapshot.pop_empty,
			(unsigned long)snapshot.pop_bytes);
		printf("[AUDIO_PCM_TX] ring=%p stage=udp fd=%d ok=%lu fail=%lu "
			"short=%lu bytes=%lu drop=%lu max_us=%lu errno=%d\n",
			snapshot.ring, snapshot.fd,
			(unsigned long)snapshot.send_ok,
			(unsigned long)snapshot.send_fail,
			(unsigned long)snapshot.send_short,
			(unsigned long)snapshot.send_bytes,
			(unsigned long)snapshot.send_drop_bytes,
			(unsigned long)snapshot.send_max_us, snapshot.last_errno);
	}
	if (tx_slot_overflow != 0U) {
		uint32_t overflow;
		taskENTER_CRITICAL();
		overflow = tx_slot_overflow;
		tx_slot_overflow = 0U;
		taskEXIT_CRITICAL();
		printf("[AUDIO_PCM_TX] slot_overflow=%lu\n", (unsigned long)overflow);
	}
}
