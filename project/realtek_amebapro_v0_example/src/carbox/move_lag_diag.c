/* 触控/画面延迟观测：只记录时间和计数，不改变发送、排队或重试策略。
 * 所有 UART 输出由已有统计线程完成。时间来自 hal_read_curtime_us，
 * 32 位无符号差值支持计时器回绕；单次区间应小于约 71 分钟。
 */
#include "move_lag_diag.h"
#include "car_rx_diag.h"
#if CONFIG_SCREEN_SETUP_EVIDENCE
#include "screen_setup_evidence.h"
#endif
#if defined(CONFIG_MOVE_LAG_PROFILE) && CONFIG_MOVE_LAG_PROFILE
#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "diag.h"
#include "lwip/sockets.h"
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include "lwip/errno.h"
#define MOVE_EVENTS 32U
#define MOVE_PENDING 16U
#define MOVE_SLOW_US 50000U

typedef struct {
    uint32_t count, maximum, ge20, ge50, ge100, detail_max;
    uint64_t sum;
} move_metric;
typedef struct {
    uint32_t stage, id, start, elapsed, detail;
    int result;
} move_event;
typedef struct {
    const void *pointer;
    uint32_t id, start;
    int length;
} move_pending;
static move_metric metrics[MOVE_STAGE_COUNT];
static move_event events[MOVE_EVENTS];
static uint32_t event_read, event_count, event_lost;
static move_pending pending[MOVE_PENDING];
static uint32_t last_arrival;
static int arrival_valid;
static uint32_t next_id, pending_full, unmatched, pending_peak;
static uint32_t frame_id, frame_start, frame_writes, frame_bytes;
static uint32_t write_partial, write_again, write_zero, write_error;
static uint32_t poll_ready, poll_timeout, poll_error;
static TaskHandle_t frame_task;
static const char *const stage_name[MOVE_STAGE_COUNT] = {
    "hid_drain", "hid_queue", "hid_write", "dispatch_wait", "dispatch_exec",
    "video_recv", "video_handover", "video_queue", "video_frame",
    "video_write", "video_poll", "video_arrival_gap"
};

void carbox_move_lag_record(unsigned stage, uint32_t id, uint32_t start,
                           uint32_t elapsed, uint32_t detail, int result)
{
    move_metric *m;
    if (stage >= MOVE_STAGE_COUNT) return;
    taskENTER_CRITICAL();
    m = &metrics[stage];
    m->count++; m->sum += elapsed;
    if (elapsed > m->maximum) m->maximum = elapsed;
    if (detail > m->detail_max) m->detail_max = detail;
    m->ge20 += elapsed >= 20000U;
    m->ge50 += elapsed >= MOVE_SLOW_US;
    m->ge100 += elapsed >= 100000U;
    /* recv 等数据可能是静止画面，不把它挤入有限的异常事件 ring。 */
    if (elapsed >= MOVE_SLOW_US && stage != MOVE_VIDEO_RECV && stage != MOVE_VIDEO_ARRIVAL) {
        if (event_count < MOVE_EVENTS) {
            move_event *e = &events[(event_read + event_count) % MOVE_EVENTS];
            *e = (move_event){stage, id, start, elapsed, detail, result};
            event_count++;
        } else event_lost++;
    }
    taskEXIT_CRITICAL();
}

extern void __real_AirPlayScreen_SendVideo(const void *, int);
void __wrap_AirPlayScreen_SendVideo(const void *data, int length)
{
    uint32_t start = hal_read_curtime_us(), id, i, live = 0U;
    int free_slot = -1;
    taskENTER_CRITICAL();
    id = ++next_id;
    if (!id) id = ++next_id;
    if (arrival_valid) carbox_move_lag_record(MOVE_VIDEO_ARRIVAL, id,
        last_arrival, start - last_arrival, 0U, 0);
    last_arrival = start; arrival_valid = 1;
    for (i = 0; i < MOVE_PENDING; i++) {
        /* 未发送就释放的配置帧可能留下记录；地址复用时替换并计数，
         * 不读取帧内容、不保留所有权。满表只漏记，绝不影响业务。 */
        if (pending[i].pointer == data && data != NULL) {
            pending[i].pointer = NULL; unmatched++;
        }
        if (pending[i].pointer) live++;
        else if (free_slot < 0) free_slot = (int)i;
    }
    if (data != NULL && length > 0) {
        if (free_slot >= 0) {
            pending[free_slot] = (move_pending){data, id, start, length};
            if (++live > pending_peak) pending_peak = live;
        } else pending_full++;
    }
    taskEXIT_CRITICAL();
    __real_AirPlayScreen_SendVideo(data, length);
    carbox_move_lag_record(MOVE_VIDEO_HANDOVER, id, start,
        hal_read_curtime_us() - start, length > 0 ? (uint32_t)length : 0U, 0);
}

/* 只替换客户 AirPlayScreen.o 的 free 引用；丢弃帧也清除观测记录。 */
void carbox_move_screen_free(void *pointer)
{
    uint32_t i;
    if (pointer != NULL) {
        taskENTER_CRITICAL();
        for (i = 0; i < MOVE_PENDING; i++) if (pending[i].pointer == pointer) {
            pending[i].pointer = NULL; unmatched++;
        }
        taskEXIT_CRITICAL();
    }
    free(pointer);
}

extern void AirPlayScreen_SendScreenFrame(const void *, int);
/* 仅重定位 ScreenThread 的两个调用点；原函数、payload、4KB write 和
 * poll 均保持客户实现。id 关联同一 buffer 的入队入口与消费入口，
 * video_queue 包含生产者等待，不等同于纯队列驻留时间。 */
void carbox_move_screen_frame(const void *data, int length)
{
    uint32_t start = hal_read_curtime_us(), id = 0U, queued = 0U, i;
    taskENTER_CRITICAL();
    for (i = 0; i < MOVE_PENDING; i++) {
        if (data != NULL && pending[i].pointer == data && pending[i].length == length) {
            id = pending[i].id; queued = pending[i].start;
            pending[i].pointer = NULL; break;
        }
    }
    if (!id) unmatched++;
    frame_id = id; frame_start = start; frame_task = xTaskGetCurrentTaskHandle();
    frame_writes = frame_bytes = 0U;
    taskEXIT_CRITICAL();
    if (id) carbox_move_lag_record(MOVE_VIDEO_QUEUE, id, queued,
                                   start - queued, (uint32_t)length, 0);
    AirPlayScreen_SendScreenFrame(data, length);
    carbox_move_lag_record(MOVE_VIDEO_FRAME, id, start,
        hal_read_curtime_us() - start, frame_writes, (int)frame_bytes);
    taskENTER_CRITICAL();
    frame_task = NULL; frame_id = 0U;
    taskEXIT_CRITICAL();
}

static int named_task(const char *expected)
{
    const char *name = pcTaskGetName(xTaskGetCurrentTaskHandle());
    return name != NULL && strcmp(name, expected) == 0;
}
extern int __real_lwip_recv(int, void *, size_t, int);
int __wrap_lwip_recv(int socket, void *buffer, size_t bytes, int flags)
{
    uint32_t start;
    int result, saved_errno, token;
    int video = named_task("AirPlayScreenReceiver");
    token = car_rx_begin(CAR_RX_RECV, socket);
    start = hal_read_curtime_us();
    result = __real_lwip_recv(socket, buffer, bytes, flags);
    saved_errno = lwip_err;
    car_rx_end(token, result, result < 0 ? saved_errno : 0);
    if (video) carbox_move_lag_record(MOVE_VIDEO_RECV, 0U, start,
        hal_read_curtime_us() - start, result > 0 ? (uint32_t)result : 0U, result);
    if (result < 0) lwip_err = saved_errno;
    return result;
}
extern int __real_lwip_write(int, const void *, size_t);
#if CONFIG_SCREEN_BLOCK_PROFILE
extern int carbox_screen_tx_probe_write(int, const void *, size_t);
#endif
int __wrap_lwip_write(int socket, const void *buffer, size_t bytes)
{
    uint32_t start, id;
    int result, saved_errno;
    if (!named_task("ScreenThread")) return __real_lwip_write(socket, buffer, bytes);
    start = hal_read_curtime_us();
    id = frame_id;
#if CONFIG_SCREEN_BLOCK_PROFILE
    result = carbox_screen_tx_probe_write(socket, buffer, bytes);
#else
    result = __real_lwip_write(socket, buffer, bytes);
#endif
    saved_errno = lwip_err;
    taskENTER_CRITICAL();
    if (frame_task == xTaskGetCurrentTaskHandle()) {
        frame_writes++;
        if (result > 0) frame_bytes += (uint32_t)result;
    }
    if (result > 0) write_partial += (size_t)result < bytes;
    else if (result == 0) write_zero++;
    else if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) write_again++;
    else write_error++;
    taskEXIT_CRITICAL();
    carbox_move_lag_record(MOVE_VIDEO_WRITE, id, start,
        hal_read_curtime_us() - start, (uint32_t)bytes, result);
    if (result < 0) lwip_err = saved_errno;
    return result;
}
extern int __real_lwip_poll(struct pollfd *, nfds_t, int);
int __wrap_lwip_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    uint32_t start, id;
    int result, saved_errno;
    if (!named_task("ScreenThread")) return __real_lwip_poll(fds, count, timeout);
    start = hal_read_curtime_us(); id = frame_id;
    result = __real_lwip_poll(fds, count, timeout); saved_errno = lwip_err;
    taskENTER_CRITICAL();
    if (result < 0 || (count && (fds[0].revents & (POLLERR|POLLHUP|POLLNVAL)))) poll_error++;
    else if (!result) poll_timeout++;
    else if (count && (fds[0].revents & POLLOUT)) poll_ready++;
    taskEXIT_CRITICAL();
    carbox_move_lag_record(MOVE_VIDEO_POLL, id, start,
        hal_read_curtime_us() - start, count ? (uint32_t)fds[0].revents : 0U, result);
    if (result < 0) lwip_err = saved_errno;
    return result;
}

uint32_t carbox_move_current_frame_id(void)
{
    return xTaskGetCurrentTaskHandle()==frame_task ? frame_id : 0U;
}

void carbox_move_lag_report(uint32_t sequence)
{
#if CONFIG_SCREEN_SETUP_EVIDENCE
    carbox_screen_setup_report(sequence);
#endif
    static move_metric snapshot[MOVE_STAGE_COUNT]; /* 不增加统计线程的大栈帧。 */
    static int announced;
    uint32_t now = hal_read_curtime_us(), i, live = 0, oldest = 0;
    uint32_t full, misses, peak, active, age, lost, partial, again, zero, errors, ready, timeout, perr;
    taskENTER_CRITICAL();
    memcpy(snapshot, metrics, sizeof(metrics)); memset(metrics, 0, sizeof(metrics));
    for (i = 0; i < MOVE_PENDING; i++) if (pending[i].pointer) {
        uint32_t a = now - pending[i].start;
        live++; if (a > oldest) oldest = a;
    }
    full = pending_full; misses = unmatched; peak = pending_peak;
    active = frame_id; age = frame_task ? now - frame_start : 0U;
    lost = event_lost;
    partial=write_partial; again=write_again; zero=write_zero; errors=write_error;
    ready=poll_ready; timeout=poll_timeout; perr=poll_error;
    write_partial=write_again=write_zero=write_error=poll_ready=poll_timeout=poll_error=0;
    taskEXIT_CRITICAL();
    if (!announced) {
        announced = 1;
        rt_printf("[MOVE_CFG] hid_bypass=%u move_sample_hz=%u period_ms=2000 slow_us=50000 video_native=17 timestamps=local_us counts=window pending_errors=cumulative\r\n",
            (unsigned)CONFIG_AIRPLAY_HID_HTTP_BYPASS, (unsigned)CONFIG_TOUCH_MOVE_SAMPLE_HZ);
    }
    rt_printf("[MOVE][%lu] at_us=%lu video_pending/peak/oldest_us=%lu/%lu/%lu pending_full/unmatched=%lu/%lu active_frame/age_us=%lu/%lu event_lost=%lu\r\n",
        (unsigned long)sequence,(unsigned long)now,(unsigned long)live,(unsigned long)peak,
        (unsigned long)oldest,(unsigned long)full,(unsigned long)misses,
        (unsigned long)active,(unsigned long)age,(unsigned long)lost);
    for (i = 0; i < MOVE_STAGE_COUNT; i++) {
        move_metric *m = &snapshot[i];
        if (!m->count) continue;
        rt_printf("[MOVE_STAGE][%lu] %s n=%lu us_avg/max=%lu/%lu ge20/50/100ms=%lu/%lu/%lu detail_max=%lu\r\n",
            (unsigned long)sequence,stage_name[i],(unsigned long)m->count,
            (unsigned long)(m->sum/m->count),(unsigned long)m->maximum,
            (unsigned long)m->ge20,(unsigned long)m->ge50,(unsigned long)m->ge100,
            (unsigned long)m->detail_max);
    }
    rt_printf("[MOVE_IO][%lu] write_partial/again/zero/error=%lu/%lu/%lu/%lu poll_ready/timeout/error=%lu/%lu/%lu\r\n",
        (unsigned long)sequence,(unsigned long)partial,(unsigned long)again,(unsigned long)zero,
        (unsigned long)errors,(unsigned long)ready,(unsigned long)timeout,(unsigned long)perr);
    for (i = 0; i < 8U; i++) {
        move_event e;
        taskENTER_CRITICAL();
        if (!event_count) { taskEXIT_CRITICAL(); break; }
        e=events[event_read]; event_read=(event_read+1U)%MOVE_EVENTS; event_count--;
        taskEXIT_CRITICAL();
        rt_printf("[MOVE_SLOW] stage=%s id=%lu start_us=%lu duration_us=%lu detail=%lu result=%ld\r\n",
            stage_name[e.stage],(unsigned long)e.id,(unsigned long)e.start,
            (unsigned long)e.elapsed,(unsigned long)e.detail,(long)e.result);
    }
}
#endif
