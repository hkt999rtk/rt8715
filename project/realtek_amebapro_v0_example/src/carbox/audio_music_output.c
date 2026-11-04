/* PCM output evidence for the pinned customer AirPlayAudio.o ABI.
 * Hooks forward every native argument/result. They neither inspect payloads
 * nor print or change socket flags. Optional navigation pacing allocates its
 * state at init only; all packet/sample operations still use the native calls.
 * Stream/ring pointers are opaque after the init call's live metadata copy.
 */
#include "audio_music_output.h"
#include "audio_diag_log.h"
#include "nav_tx_trace.h"
#if CONFIG_AUDIO_MUSIC_DIAG
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "lwip/sockets.h"
#if CONFIG_AUDIO_NAV_PACING
#include <stdlib.h>
#include "audio_nav_pacing.h"
extern int RingBuffer_size(void *);
#endif

#define MUSIC_OUTPUT_SLOTS 8U
#define MUSIC_OUTPUT_REPORT_MAX 4U
#define MUSIC_OUTPUT_REPORT_MS 5000U

extern int RingBuffer_init(void *, uint32_t);
extern int RingBuffer_push(void *, const void *, uint32_t);
extern int RingBuffer_pop(void *, void *, uint32_t);
extern int lwip_errno(void);
extern void pthread_exit(void *);

struct music_output {
    uintptr_t stream, ring, output_task;
    uint32_t epoch, type, capacity;
    int fd;
    uint32_t start_tick, end_tick, last_tick, report_tick;
    uint32_t closed, reinit, dirty, reported, inflight, thread_live;
    uint32_t push_ok, push_fail, push_pcm, push_drop_pcm;
    uint32_t pop_ok, pop_empty, pop_pcm;
    uint32_t send_ok, send_fail, send_short, send_oversize;
    uint32_t udp_bytes, udp_drop, send_max_us;
    uint32_t last_push_tick, last_pop_tick, last_send_tick, wraps;
    int last_errno;
#if CONFIG_AUDIO_NAV_PACING
    struct nav_pacer *pacer;
#endif
};
static struct music_output outputs[MUSIC_OUTPUT_SLOTS], report_output;
static uint32_t output_epoch, output_overflow, output_unmapped[4];

static void add_count(struct music_output *s, uint32_t *field, uint32_t amount)
{
    uint32_t before = *field;
    *field += amount;
    if (*field < before) ++s->wraps;
}

/* A reference pins this epoch while the native call can block or be preempted.
 * A subsequent init can close it, but cannot recycle its counter storage. */
static struct music_output *begin_call(void *stream, void *ring, unsigned op)
{
    struct music_output *result = NULL;
    uintptr_t task = (uintptr_t)xTaskGetCurrentTaskHandle();
    unsigned i;
    taskENTER_CRITICAL();
    for (i = 0U; i < MUSIC_OUTPUT_SLOTS; ++i) {
        struct music_output *s = &outputs[i];
        if (s->epoch && !s->closed && s->stream == (uintptr_t)stream &&
            (op == 0U || s->output_task == task) &&
            (ring == NULL || s->ring == (uintptr_t)ring)) {
            ++s->inflight;
            result = s;
            break;
        }
    }
    if (result == NULL) ++output_unmapped[op];
    taskEXIT_CRITICAL();
    return result;
}

/* Called with the critical section held. */
static void finish_call(struct music_output *s)
{
    --s->inflight;
    s->last_tick = xTaskGetTickCount();
    s->dirty = 1U;
    s->reported = 0U;
}

int carbox_music_output_init_c(void *ring, uint32_t capacity, void *stream)
{
    /* At this exact callsite R4 is the native stream. The native thread reads
     * fd at +4, type is initialized to 100/101 at +8, and ring is at +0x30. */
    uint32_t type = *(const volatile uint32_t *)((uintptr_t)stream + 8U);
    int fd = *(const volatile int *)((uintptr_t)stream + 4U);
    int result = RingBuffer_init(ring, capacity);
    struct music_output *slot = NULL;
    uint32_t now = xTaskGetTickCount();
    uintptr_t task = (uintptr_t)xTaskGetCurrentTaskHandle();
    unsigned i;
#if CONFIG_AUDIO_NAV_PACING
    struct nav_pacer *fresh = NULL, *old = NULL;
    uint32_t rate = *(const volatile uint32_t *)((uintptr_t)stream + 32U);
    uint32_t frame_bytes = *(const volatile uint32_t *)((uintptr_t)stream + 20U);
    if (type == 101U && rate == 48000U && frame_bytes == 4U) {
        fresh = calloc(1U, sizeof(*fresh));
        if (fresh) nav_pace_init(fresh, rate * frame_bytes, capacity, hal_read_curtime_us());
    }
#endif
    taskENTER_CRITICAL();
    for (i = 0U; i < MUSIC_OUTPUT_SLOTS; ++i) {
        struct music_output *s = &outputs[i];
        /* A reused task handle must not still bind a retained old epoch. */
        if (s->output_task == task) s->thread_live = 0U;
        if (s->epoch && !s->closed && s->stream == (uintptr_t)stream) {
            s->closed = s->reinit = s->dirty = 1U;
            s->reported = 0U;
            s->end_tick = now;
        }
        if (slot == NULL && (s->epoch == 0U ||
            (s->closed && s->reported && s->inflight == 0U))) slot = s;
    }
    if (slot == NULL) ++output_overflow;
    else {
#if CONFIG_AUDIO_NAV_PACING
        old = slot->pacer;
#endif
        memset(slot, 0, sizeof(*slot));
#if CONFIG_AUDIO_NAV_PACING
        slot->pacer = fresh; fresh = NULL;
#endif
        slot->stream = (uintptr_t)stream; slot->ring = (uintptr_t)ring;
        slot->output_task = task; slot->thread_live = 1U;
        /* Zero is reserved for an unused slot, even across epoch wrap. */
        if (++output_epoch == 0U) ++output_epoch;
        slot->epoch = output_epoch; slot->type = type; slot->fd = fd;
        slot->capacity = capacity;
        slot->start_tick = slot->last_tick = slot->report_tick = now;
        slot->dirty = 1U;
    }
    taskEXIT_CRITICAL();
#if CONFIG_AUDIO_NAV_PACING
    free(old); free(fresh);
#endif
    return result;
}

int carbox_music_output_push_c(void *ring, const void *data, uint32_t length,
                              void *stream)
{
    struct music_output *s = begin_call(stream, ring, 0U);
    int result = RingBuffer_push(ring, data, length);
#if CONFIG_AUDIO_NAV_PACING
    int remaining = s && s->pacer ? RingBuffer_size(ring) : 0;
#endif
    if (s != NULL) {
        taskENTER_CRITICAL();
        if (result != 0) {
            add_count(s, &s->push_ok, 1U); add_count(s, &s->push_pcm, length);
        } else {
            add_count(s, &s->push_fail, 1U); add_count(s, &s->push_drop_pcm, length);
        }
#if CONFIG_AUDIO_NAV_PACING
        if (s->pacer && remaining >= 0) nav_pace_observe(s->pacer, (uint32_t)remaining);
#endif
        s->last_push_tick = xTaskGetTickCount();
        finish_call(s);
        taskEXIT_CRITICAL();
    }
    return result;
}

int carbox_music_output_pop_c(void *ring, void *data, uint32_t length, void *stream)
{
    struct music_output *s = begin_call(stream, ring, 1U);
    int result = RingBuffer_pop(ring, data, length);
#if CONFIG_AUDIO_NAV_PACING
    int remaining = s && s->pacer ? RingBuffer_size(ring) : 0;
    uint32_t at_us = hal_read_curtime_us();
#endif
    if (s != NULL) {
        taskENTER_CRITICAL();
        if (result != 0) {
            add_count(s, &s->pop_ok, 1U); add_count(s, &s->pop_pcm, length);
        } else add_count(s, &s->pop_empty, 1U);
#if CONFIG_AUDIO_NAV_PACING
        if (s->pacer && remaining >= 0)
            nav_pace_pop(s->pacer, at_us, (uint32_t)remaining, length, result != 0);
#endif
        s->last_pop_tick = xTaskGetTickCount();
        finish_call(s);
        taskEXIT_CRITICAL();
    }
    return result;
}

ssize_t carbox_music_output_sendto_c(int fd, const void *data, size_t length,
    int flags, const struct sockaddr *to, socklen_t tolen, void *stream)
{
    struct music_output *s = begin_call(stream, NULL, 2U);
    uint32_t start = hal_read_curtime_us();
#if CONFIG_AUDIO_NAV_PACING
    if (s && s->pacer && length >= 12U) {
        const uint8_t *h = data;
        uint32_t seq = (uint32_t)h[2] * 256U + h[3];
        uint32_t ts = ((uint32_t)h[4] << 24) | ((uint32_t)h[5] << 16) |
                      ((uint32_t)h[6] << 8) | h[7];
        taskENTER_CRITICAL();
        nav_pace_send_enter(s->pacer, start, seq, ts);
        taskEXIT_CRITICAL();
    }
#endif
#if CONFIG_AUDIO_NAV_TX_TRACE
    uint32_t trace=0;
    if(s && s->type==101U && s->pacer) {
        uint32_t q,debt;
        taskENTER_CRITICAL();q=s->pacer->q_us;debt=s->pacer->debt_us;taskEXIT_CRITICAL();
        trace=navtx_begin(s->epoch,data,length,q,debt);
    }
#endif
    ssize_t result = lwip_sendto(fd, data, length, flags, to, tolen);
    uint32_t elapsed = hal_read_curtime_us() - start;
    int saved_errno = result < 0 ? lwip_errno() : 0;
#if CONFIG_AUDIO_NAV_TX_TRACE
    navtx_end(trace,result,saved_errno);
#endif
    if (s != NULL) {
        taskENTER_CRITICAL();
#if CONFIG_AUDIO_NAV_PACING
        if (s->pacer) nav_pace_send_exit(s->pacer, start + elapsed);
#endif
        s->fd = fd;
        if (elapsed > s->send_max_us) s->send_max_us = elapsed;
        if (result < 0) {
            add_count(s, &s->send_fail, 1U);
            add_count(s, &s->udp_drop, (uint32_t)length);
            s->last_errno = saved_errno;
        } else {
            add_count(s, &s->udp_bytes, (uint32_t)result);
            if ((size_t)result == length) add_count(s, &s->send_ok, 1U);
            else if ((size_t)result < length) {
                add_count(s, &s->send_short, 1U);
                add_count(s, &s->udp_drop, (uint32_t)(length - (size_t)result));
            } else add_count(s, &s->send_oversize, 1U);
        }
        s->last_send_tick = xTaskGetTickCount();
        finish_call(s);
        taskEXIT_CRITICAL();
    }
    return result;
}

void carbox_music_output_exit_c(void *value, void *stream)
{
    unsigned i, found = 0U;
    uintptr_t task = (uintptr_t)xTaskGetCurrentTaskHandle();
    taskENTER_CRITICAL();
    for (i = 0U; i < MUSIC_OUTPUT_SLOTS; ++i) {
        struct music_output *s = &outputs[i];
        if (s->epoch && s->thread_live && s->stream == (uintptr_t)stream &&
            s->output_task == task) {
            s->closed = s->dirty = 1U;
            s->reported = 0U;
            s->thread_live = 0U;
#if CONFIG_AUDIO_NAV_PACING
            if (s->pacer) nav_pace_finish(s->pacer, hal_read_curtime_us());
#endif
            s->end_tick = xTaskGetTickCount();
            found = 1U;
        }
    }
    if (!found) ++output_unmapped[3];
    taskEXIT_CRITICAL();
    pthread_exit(value);
}

#if CONFIG_AUDIO_NAV_PACING
/* The pinned post-send hook owns R4=stream, R5=payload bytes. Return 1 only
 * when this function has replaced the native pacing block. No lock spans sleep. */
int carbox_nav_output_pace_c(void *ring, void *stream, uint32_t packet_bytes)
{
    struct music_output *s = begin_call(stream, ring, 1U);
    uint32_t deadline = 0;
    int handled = 0;
    if (s && s->pacer && (packet_bytes == 1440U || packet_bytes == 1416U)) {
        volatile uint32_t *stop = (volatile uint32_t *)((uintptr_t)stream + 52U);
        int remaining = RingBuffer_size(ring);
        if (remaining >= 0 && (uint32_t)remaining <= s->capacity) {
            taskENTER_CRITICAL();
            nav_pace_schedule(s->pacer, hal_read_curtime_us(), (uint32_t)remaining, packet_bytes);
            deadline = s->pacer->deadline_us;
            taskEXIT_CRITICAL();
            handled = 1;
            while (!*stop) {
                int32_t left = (int32_t)(deadline - hal_read_curtime_us());
                uint32_t ticks;
                if (left <= 0) break;
                ticks = (uint32_t)((uint64_t)(uint32_t)left * configTICK_RATE_HZ / 1000000U);
                /* At most one tick before rechecking stop; retain fractional
                 * deadline across calls instead of rounding each interval. */
                if (ticks > 1U) ticks = 1U;
                vTaskDelay(ticks ? ticks : 1U);
            }
        }
    }
    if (s) { taskENTER_CRITICAL(); finish_call(s); taskEXIT_CRITICAL(); }
    return handled;
}
#endif

static unsigned long output_ms(uint32_t ticks)
{
    return (unsigned long)((uint64_t)ticks * 1000U / configTICK_RATE_HZ);
}

void carbox_audio_music_output_report(void)
{
    static unsigned cursor;
    static uint32_t previous_overflow, previous_unmapped[4];
    unsigned visited, printed = 0U;
#if CONFIG_AUDIO_NAV_PACING
    struct nav_pacer pace_snapshot;
    uint32_t pace_now = 0;
#endif
    for (visited = 0U; visited < MUSIC_OUTPUT_SLOTS && printed < MUSIC_OUTPUT_REPORT_MAX;
         ++visited) {
        struct music_output *s = &outputs[cursor];
        struct music_output *live = s;
        uint32_t now, skipped;
        unsigned ready;
        cursor = (cursor + 1U) % MUSIC_OUTPUT_SLOTS;
        taskENTER_CRITICAL();
        now = xTaskGetTickCount();
        ready = s->epoch && ((s->closed && s->dirty) ||
            (!s->closed && output_ms(now - s->report_tick) >= MUSIC_OUTPUT_REPORT_MS));
        if (ready) {
            report_output = *s;
#if CONFIG_AUDIO_NAV_PACING
            if (s->pacer) pace_snapshot = *s->pacer;
            pace_now = hal_read_curtime_us();
#endif
            s->report_tick = now;
            s->dirty = 0U;
            s->reported = 0U;
        }
        taskEXIT_CRITICAL();
        if (!ready) continue;
        s = &report_output;
        ++printed;
        skipped = carbox_audio_diag_log_skipped();
        /* The epoch key is repeated by the shared formatter on continuation
         * lines. UDP bytes include RTP/encryption; PCM byte fields do not. */
        carbox_audio_diag_log("[MUSIC_OUT] epoch=%lu tick=%lu stream=%p ring=%p task=%p type=%lu fd=%d closed=%lu reinit=%lu age_ms=%lu end_tick=%lu last_tick=%lu inflight=%lu push_ok=%lu push_fail=%lu push_pcm=%lu push_drop_pcm=%lu pop_ok=%lu pop_empty=%lu pop_pcm=%lu send_ok=%lu send_fail=%lu send_short=%lu send_oversize=%lu udp_bytes=%lu udp_drop=%lu max_us=%lu errno=%d last_push/pop/send=%lu/%lu/%lu wraps=%lu\n",
            (unsigned long)s->epoch, (unsigned long)now, (void *)s->stream,
            (void *)s->ring, (void *)s->output_task, (unsigned long)s->type, s->fd,
            (unsigned long)s->closed, (unsigned long)s->reinit,
            output_ms((s->closed ? s->end_tick : now) - s->start_tick),
            (unsigned long)s->end_tick, (unsigned long)s->last_tick,
            (unsigned long)s->inflight, (unsigned long)s->push_ok,
            (unsigned long)s->push_fail, (unsigned long)s->push_pcm,
            (unsigned long)s->push_drop_pcm, (unsigned long)s->pop_ok,
            (unsigned long)s->pop_empty, (unsigned long)s->pop_pcm,
            (unsigned long)s->send_ok, (unsigned long)s->send_fail,
            (unsigned long)s->send_short, (unsigned long)s->send_oversize,
            (unsigned long)s->udp_bytes, (unsigned long)s->udp_drop,
            (unsigned long)s->send_max_us, s->last_errno,
            (unsigned long)s->last_push_tick, (unsigned long)s->last_pop_tick,
            (unsigned long)s->last_send_tick, (unsigned long)s->wraps);
#if CONFIG_AUDIO_NAV_PACING
        if (s->type == 101U) {
            const struct nav_pacer *p = &pace_snapshot;
            carbox_audio_diag_log("[NAV_PACE] epoch=%lu tick=%lu closed=%lu enabled=%u target_us=50000 cap_ppm=50000 filter_us=80000 mode=bounded-phase-v2\n",
                (unsigned long)s->epoch, (unsigned long)now, (unsigned long)s->closed, s->pacer != NULL);
            if (s->pacer) {
#define PU(x) ((unsigned long)(x))
                carbox_audio_diag_log("[NAV_PACE] epoch=%lu q_us=%lu q_filter_us=%lu q_min/max_us=%lu/%lu boost_ppm=%lu boost_max_ppm=%lu interval_us=%lu rebases=%lu late_max_us=%lu pressure_samples=%lu\n",
                    PU(s->epoch), PU(p->q_us), PU(p->filtered_us), PU(p->q_min_us), PU(p->q_max_us),
                    PU(p->boost_ppm), PU(p->boost_max_ppm), PU(p->interval_us), PU(p->deadline_rebases),
                    PU(p->late_max_us), PU(p->pressure_samples));
                carbox_audio_diag_log("[NAV_PACE] epoch=%lu debt_us=%lu debt_max_us=%lu catchup_waits=%lu min_gap_us=%lu debt_cap_us=40000 catchup_cap_ppm=500000\n",
                    PU(s->epoch), PU(p->debt_us), PU(p->debt_max_us), PU(p->catchup_waits), PU(p->min_gap_us));
                carbox_audio_diag_log("[NAV_PACE] epoch=%lu send_n=%lu entry_gap_avg/max_us=%lu/%lu return_gap_max_us=%lu gap_gt10/20/40ms=%lu/%lu/%lu gap_seq=%lu gap_ts=%lu gap_at_us=%lu\n",
                    PU(s->epoch), PU(p->sends), PU(p->sends > 1U ? p->send_gap_total_us / (p->sends-1U) : 0),
                    PU(p->send_gap_max_us), PU(p->return_gap_max_us), PU(p->gap_gt10ms), PU(p->gap_gt20ms),
                    PU(p->gap_gt40ms), PU(p->gap_seq), PU(p->gap_ts), PU(p->gap_at_us));
                carbox_audio_diag_log("[NAV_ONSET] epoch=%lu first_us=%lu init_to_first_us=%lu window_us=1000000 send_n=%lu first_q_us=%lu q_min/max_us=%lu/%lu\n",
                    PU(s->epoch),PU(p->first_us),PU(p->sends?p->first_us-p->init_us:0),PU(p->onset_n),
                    PU(p->first_q_us),PU(p->onset_n?p->onset_q_min:0),PU(p->onset_q_max));
                carbox_audio_diag_log("[NAV_ONSET] epoch=%lu gap_avg/min/max_us=%lu/%lu/%lu gap_lt6/gt10ms=%lu/%lu send_max_us=%lu boost_max_ppm=%lu debt_max_us=%lu empty_resume=%lu\n",
                    PU(s->epoch),PU(p->onset_n>1?p->onset_gap_total/(p->onset_n-1):0),
                    PU(p->onset_n>1?p->onset_gap_min:0),PU(p->onset_gap_max),PU(p->onset_fast_n),
                    PU(p->onset_slow_n),PU(p->onset_send_max),PU(p->onset_boost_max),
                    PU(p->onset_debt_max),PU(p->onset_empty_n));
                carbox_audio_diag_log("[NAV_PACE] epoch=%lu startup_empty_us=%lu empty_resume_n=%lu empty_resume_avg/max_us=%lu/%lu terminal_empty_us=%lu pending_empty_us=%lu\n",
                    PU(s->epoch), PU(p->startup_empty_us), PU(p->empty_resume_n),
                    PU(p->empty_resume_n ? p->empty_resume_total_us / p->empty_resume_n : 0),
                    PU(p->empty_resume_max_us), PU(p->terminal_empty_us),
                    PU(p->empty_open ? pace_now-p->empty_since_us : 0));
#undef PU
            }
        }
#endif
        taskENTER_CRITICAL();
        if (live->epoch == s->epoch) {
            if (carbox_audio_diag_log_skipped() != skipped) live->dirty = 1U;
            else if (!live->dirty && live->inflight == 0U) live->reported = 1U;
        }
        taskEXIT_CRITICAL();
    }
    {
        uint32_t overflow, unmapped[4], now;
        taskENTER_CRITICAL();
        overflow = output_overflow;
        memcpy(unmapped, output_unmapped, sizeof(unmapped));
        now = xTaskGetTickCount();
        taskEXIT_CRITICAL();
        if (overflow != previous_overflow || memcmp(unmapped, previous_unmapped, sizeof(unmapped))) {
            uint32_t skipped = carbox_audio_diag_log_skipped();
            carbox_audio_diag_log("[MUSIC_OUT_LOSS] tick=%lu slot_overflow=%lu unmapped_push/pop/send/exit=%lu/%lu/%lu/%lu slots=8 per_report=4\n",
                (unsigned long)now, (unsigned long)overflow,
                (unsigned long)unmapped[0], (unsigned long)unmapped[1],
                (unsigned long)unmapped[2], (unsigned long)unmapped[3]);
            if (carbox_audio_diag_log_skipped() == skipped) {
                previous_overflow = overflow;
                memcpy(previous_unmapped, unmapped, sizeof(unmapped));
            }
        }
    }
}
#endif
