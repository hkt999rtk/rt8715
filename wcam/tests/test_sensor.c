/* Exercise the production ISP/queue handoff with one simulated hardware slot. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "wcam_pattern.h"

static void *frame_alloc(size_t size);
static void frame_free(void *frame);
#define malloc frame_alloc
#define free frame_free
#include "../../project/realtek_amebapro_v0_example/src/wcam/wcam_sensor.c"
#undef malloc
#undef free

/* The ISP ABI holds 32-bit DMA addresses. Link this fixture non-PIE so the
 * static buffers have representable addresses even on a 64-bit test host. */
static uint8_t memory[SENSOR_BUFFERS][WCAM_NV12_SIZE + 31];
static int allocated[SENSOR_BUFFERS];
static unsigned queue_count;
static int probe_ok;
enum isp_state { OFF, INITIALIZED, CREATED, APPLIED, STARTED, STOPPED, CANCELLED };
static enum isp_state isp_state;
static isp_stream_t device;
static uint8_t *dma;
struct test_queue {
    unsigned count, capacity;
    void *items[SENSOR_BUFFERS];
};

static void *frame_alloc(size_t size)
{
    assert(size <= sizeof memory[0]);
    for (unsigned i = 0; i < SENSOR_BUFFERS; i++) {
        if (!allocated[i]) {
            allocated[i] = 1;
            return memory[i];
        }
    }
    return NULL;
}

static void frame_free(void *frame)
{
    if (!frame) return;
    assert(isp_state == OFF);
    for (unsigned i = 0; i < SENSOR_BUFFERS; i++) {
        if (frame == memory[i]) {
            assert(allocated[i]);
            allocated[i] = 0;
            return;
        }
    }
    assert(0 && "unknown allocation");
}

QueueHandle_t xQueueCreate(unsigned count, unsigned item_size)
{
    assert(count <= SENSOR_BUFFERS && item_size == sizeof(void *));
    QueueHandle_t q = calloc(1, sizeof *q);
    assert(q);
    q->capacity = count;
    ++queue_count;
    return q;
}

void vQueueDelete(QueueHandle_t q)
{
    assert(isp_state == OFF && queue_count);
    --queue_count;
    free(q);
}

int xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    (void)wait;
    if (q->count == q->capacity) return pdFALSE;
    memcpy(&q->items[q->count++], item, sizeof(void *));
    return pdTRUE;
}

int xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    (void)wait;
    if (!q->count) return pdFALSE;
    memcpy(item, q->items, sizeof(void *));
    memmove(q->items, q->items + 1, --q->count * sizeof(void *));
    return pdTRUE;
}

void vTaskDelay(TickType_t ticks) { (void)ticks; }

void dcache_invalidate_by_addr(uint32_t *addr, unsigned bytes)
{
    assert(addr && ((uintptr_t)addr % 32 == 0) && bytes == WCAM_NV12_SIZE);
}
void dcache_clean_invalidate_by_addr(uint32_t *addr, unsigned bytes)
{
    dcache_invalidate_by_addr(addr, bytes);
}

int video_subsys_init(isp_init_cfg_t *init)
{
    assert(isp_state == OFF && init->pin_idx == ISP_PIN_SEL_S1);
    isp_state = INITIALIZED;
    return 0;
}
int video_subsys_deinit(void *arg)
{
    (void)arg;
    assert(isp_state == INITIALIZED);
    isp_state = OFF;
    return 0;
}
isp_stream_t *isp_stream_create(isp_cfg_t *cfg)
{
    assert(isp_state == INITIALIZED && cfg->hw_slot_num == 1);
    isp_state = CREATED;
    return &device;
}
void isp_stream_apply(isp_stream_t *s) { assert(s == &device && isp_state == CREATED); isp_state = APPLIED; }
void isp_stream_start(isp_stream_t *s) { assert(s == &device && isp_state == APPLIED); isp_state = STARTED; }
void isp_stream_stop(isp_stream_t *s) { assert(s == &device && isp_state == STARTED); isp_state = STOPPED; }
void isp_stream_cancel(isp_stream_t *s) { assert(s == &device && isp_state == STOPPED); isp_state = CANCELLED; }
isp_stream_t *isp_stream_destroy(isp_stream_t *s)
{
    assert(s == &device && isp_state == CANCELLED);
    isp_state = INITIALIZED;
    return NULL;
}
void isp_stream_set_complete_callback(isp_stream_t *s, void (*cb)(void *), void *arg)
{
    s->frame_complete = cb;
    s->frame_complete_arg = arg;
}
int isp_stream_get_status(int id) { return id == 0 && isp_state == STARTED; }
uint8_t isp_i2c_read_byte(int addr) { return probe_ok ? (addr == 0xf0 ? 0x20 : 0x53) : 0; }

void isp_handle_buffer(isp_stream_t *s, isp_buf_t *b, int mode)
{
    assert(s == &device);
    if (mode == MODE_SKIP) return;
    assert(mode == MODE_SETUP || mode == MODE_EXCHANGE);
    uint8_t *completed = dma;
    dma = (uint8_t *)(uintptr_t)b->y_addr;
    if (mode == MODE_EXCHANGE) b->y_addr = (uint32_t)(uintptr_t)completed;
}

static void capture(uint8_t value)
{
    assert(isp_state == STARTED);
    dma[0] = value;
    device.frame_complete(device.frame_complete_arg);
}
static void assert_clean(void)
{
    assert(isp_state == OFF && !queue_count);
    for (unsigned i = 0; i < SENSOR_BUFFERS; i++) assert(!allocated[i]);
}

int main(void)
{
    uint8_t *frame;
    assert((uintptr_t)&memory[SENSOR_BUFFERS - 1] <= UINT32_MAX);
    assert(wcam_sensor_init() < 0);
    assert_clean();
    wcam_sensor_deinit();
    assert_clean();

    probe_ok = 1;
    assert(wcam_sensor_init() == 0);
    capture(1);
    capture(2);
    capture(3); /* Queues full while TX is offline. */
    wcam_sensor_discard_ready();
    assert(wcam_sensor_get_frame(&frame, 0) < 0);
    capture(4);
    assert(wcam_sensor_get_frame(&frame, 0) == 0 && frame[0] == 4);
    wcam_sensor_release_frame(frame);
    wcam_sensor_deinit();
    assert_clean();
    puts("sensor: failed probe, IRQ teardown, offline backlog and fresh frame PASS");
    return 0;
}
