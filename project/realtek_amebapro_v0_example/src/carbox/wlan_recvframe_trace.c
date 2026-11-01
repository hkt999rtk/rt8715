/* Observe vendor recvframe ownership. Exact native call sites are build-verified.
 * No queue policy changes, payload copies, allocation or UART in these hooks.
 * Reporter snapshots only registered pool counters under the critical section;
 * it never walks vendor lists or dereferences retained event pointers. */
#include "rx_handover_diag.h"
#if CONFIG_RX_HANDOVER_DIAG
#include <stddef.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

#define RF_POOLS 4U
#define RF_COUNT 32U
#define RF_STRIDE 192U
#define RF_OLD_MS 100U
#define RF_EVENTS 16U
#define RF_EVENTS_PER_REPORT 2U
#define RF_UNKNOWN UINT32_MAX

/* Addresses only; these native functions are not invoked through these types. */
extern void _rtw_init_recv_priv(void), _rtw_free_recv_priv(void);
extern void _rtw_alloc_recvframe(void), rtw_free_recvframe(void);
extern void enqueue_reorder_recvframe(void), recv_indicatepkts_in_order(void);
extern void recvframe_chk_defrag(void), _rtw_enqueue_recvframe(void);
extern void amsdu_to_msdu(void);
extern void __real_rtw_list_insert_tail(void *, void *);
extern void __real_rtw_list_delete(void *);
extern void __real_rtw_vmfree(void *, uint32_t);
extern int __real_rtw_recv_indicatepkt(void *, void *);
extern int __real_rtw_recv_entry(void *);
#define RF_PC(fn, off) (((uintptr_t)(fn) & ~(uintptr_t)1U) + (off))

enum rf_state { RF_FREE, RF_ALLOC, RF_REORDER, RF_QUEUE, RF_DRAIN, RF_DETACHED,
                RF_INDICATE, RF_DEFRAG, RF_DECRYPT, RF_STATES };
struct rf_frame {
    uint32_t loan_tick, stage_tick;
    uintptr_t caller, owner, control, task;
    uint16_t seq, len;
    uint8_t state, amsdu, stop_flags, attrs;
    uint8_t frag, morefrag;
    uintptr_t borrow_queue, alloc_adapter, entry_adapter, entry_caller;
};
struct rf_pool {
    uintptr_t queue, base, raw;
    uint32_t active, epoch, allocs, returns, double_alloc, double_return;
    uint32_t foreign_return, unowned_event;
    uint32_t amsdu_child, amsdu_adapter_mismatch;
    struct rf_frame frames[RF_COUNT];
};
static struct rf_pool pools[RF_POOLS];
static struct rf_pool report_pool; /* sole reporter; keeps its task stack small */
static uint32_t next_epoch, missed_pools;

enum rf_event_kind { RF_FOREIGN_RETURN, RF_FOREIGN_BORROW, RF_HOME_RETURN };
struct rf_event {
    uint32_t id, tick, epoch, queue_epoch, slot, kind;
    uintptr_t frame, home, queue, free_caller, previous_caller;
    uint32_t previous_state, home_count_at_hook, queue_count_at_hook;
    uintptr_t adapter, buddy;
    uint32_t adapter_type, promisc;
    struct rf_frame loan;
};
static struct rf_event events[RF_EVENTS], report_event;
static uint32_t event_read, event_count, event_id, event_lost;

/* Caller holds the critical section. Unknown destinations are kept as opaque
 * addresses, never dereferenced. Native free counts may legitimately exceed 32
 * if another pool's frames were put here; do not clamp away that evidence. */
static uint32_t pool_count(uintptr_t queue)
{
    unsigned i;
    for (i = 0; i < RF_POOLS; ++i)
        if (pools[i].active && pools[i].queue == queue)
            return *(const volatile uint32_t *)(queue + 0x2cU);
    return RF_UNKNOWN;
}

static void adapter_info(uintptr_t adapter, uintptr_t *buddy,
                         uint32_t *type, uint32_t *promisc)
{
    unsigned i;
    *buddy = 0U; *type = *promisc = RF_UNKNOWN;
    for (i = 0; i < RF_POOLS; ++i) {
        if (pools[i].active && pools[i].queue == adapter + 0xc44U) {
            *buddy = *(const volatile uint32_t *)(adapter + 0x3378U);
            *type = *(const volatile uint8_t *)(adapter + 0x338aU);
            *promisc = *(const volatile uint8_t *)(adapter + 0xd84U);
            return;
        }
    }
}

uint32_t carbox_recvframe_trace_epoch(const void *queue)
{
    unsigned i;
    for (i = 0; i < RF_POOLS; ++i)
        if (pools[i].active && pools[i].queue == (uintptr_t)queue) return pools[i].epoch;
    return 0U;
}

static uint32_t read_u32(const void *p, unsigned offset)
{
    return *(const volatile uint32_t *)((uintptr_t)p + offset);
}

static struct rf_pool *find_pool(uintptr_t frame, unsigned *index)
{
    unsigned i;
    for (i = 0; i < RF_POOLS; ++i) {
        uintptr_t offset = frame - pools[i].base;
        if (pools[i].active && offset < RF_COUNT * RF_STRIDE && offset % RF_STRIDE == 0U) {
            *index = (unsigned)(offset / RF_STRIDE);
            return &pools[i];
        }
    }
    return NULL;
}

static void register_pool(void *frame, void *queue)
{
    uintptr_t base = read_u32(queue, 0x28U);
    uintptr_t raw = read_u32(queue, 0x24U);
    struct rf_pool *p = NULL;
    unsigned i;
    /* _rtw_init_recv_priv sets these pointers before linking all 32 frames. */
    if ((uintptr_t)frame != base || (base & 255U) != 0U || raw == 0U)
        return;
    taskENTER_CRITICAL();
    for (i = 0; i < RF_POOLS; ++i) {
        if (pools[i].queue == (uintptr_t)queue) { p = &pools[i]; break; }
        if (!pools[i].active && p == NULL) p = &pools[i];
    }
    if (p != NULL) {
        memset(p, 0, sizeof(*p));
        p->queue = (uintptr_t)queue; p->base = base; p->raw = raw;
        p->epoch = ++next_epoch; p->active = 1U;
        for (i = 0; i < RF_COUNT; ++i) p->frames[i].owner = (uintptr_t)queue;
    } else ++missed_pools;
    taskEXIT_CRITICAL();
}

static void capture_attrs(struct rf_frame *f, void *frame, unsigned state)
{
    uint32_t len = read_u32(frame, 160U);
    f->len = len > UINT16_MAX ? UINT16_MAX : (uint16_t)len;
    /* A-MSDU child allocation fills Ethernet data but does not initialize the
     * 802.11 metadata. Never label the frame's previous contents as valid. */
    if (state == RF_INDICATE && f->caller == RF_PC(amsdu_to_msdu, 0x1feU)) {
        f->attrs = f->amsdu = f->frag = f->morefrag = 0U;
        f->seq = 0U;
        return;
    }
    f->seq = *(const volatile uint16_t *)((uintptr_t)frame + 40U);
    f->amsdu = *(const volatile uint8_t *)((uintptr_t)frame + 35U);
    f->frag = *(const volatile uint8_t *)((uintptr_t)frame + 42U);
    f->morefrag = *(const volatile uint8_t *)((uintptr_t)frame + 43U);
    f->attrs = 1U;
    if (state == RF_REORDER) f->control = read_u32(frame, 188U);
    if (state == RF_DRAIN) {
        uintptr_t adapter = read_u32(frame, 16U);
        /* The native drain dereferences this adapter immediately afterwards. */
        if (adapter != 0U)
            f->stop_flags = (read_u32((void *)adapter, 0x32c4U) != 0U) |
                           ((read_u32((void *)adapter, 0x32c8U) != 0U) << 1);
    }
}

/* Rare events only. All pointers and counters are captured now, while the
 * objects are live; the output task uses these copies after teardown as well. */
static void record_flow(struct rf_pool *p, unsigned index, void *frame,
                        uintptr_t queue, uintptr_t free_caller, unsigned kind,
                        unsigned previous_state, uintptr_t previous_caller)
{
    struct rf_event *e;
    uint32_t id = ++event_id;
    if (event_count == RF_EVENTS) { ++event_lost; return; }
    e = &events[(event_read + event_count) % RF_EVENTS];
    e->id = id; e->tick = xTaskGetTickCount(); e->epoch = p->epoch;
    e->queue_epoch = carbox_recvframe_trace_epoch((void *)queue);
    e->slot = index; e->kind = kind; e->frame = (uintptr_t)frame;
    e->home = p->queue; e->queue = queue; e->free_caller = free_caller;
    e->previous_state = previous_state; e->previous_caller = previous_caller;
    e->loan = p->frames[index];
    e->adapter = read_u32(frame, 16U);
    adapter_info(e->adapter, &e->buddy, &e->adapter_type, &e->promisc);
    e->home_count_at_hook = pool_count(p->queue);
    e->queue_count_at_hook = pool_count(queue);
    ++event_count;
}

static void observe_return(void *frame, uintptr_t owner, uintptr_t caller,
                           unsigned state, uintptr_t free_caller)
{
    struct rf_pool *p;
    struct rf_frame *f;
    unsigned index;
    uint32_t now;
    /* Common unrelated list operations need no critical section. Recheck
     * after entering: pool destruction/reinitialization may have preempted us. */
    if (find_pool((uintptr_t)frame, &index) == NULL) return;
    taskENTER_CRITICAL();
    p = find_pool((uintptr_t)frame, &index);
    if (p == NULL) { taskEXIT_CRITICAL(); return; }
    f = &p->frames[index];
    now = xTaskGetTickCount();
    /* The generic enqueue helper can name any registered free queue. */
    if (state == RF_QUEUE && carbox_recvframe_trace_epoch((void *)owner) != 0U)
        state = RF_FREE;
    /* The same vendor allocator dequeues pending decrypt frames. This resumes
     * an existing loan; it is not another allocation from the free pool. */
    if (state == RF_ALLOC && (f->state == RF_DECRYPT || f->state == RF_QUEUE))
        state = RF_DETACHED;
    if (state == RF_ALLOC) {
        uintptr_t borrow_queue = f->owner;
        unsigned was_free = f->state == RF_FREE;
        unsigned previous_state = f->state;
        uintptr_t previous_caller = f->caller;
        if (f->state != RF_FREE) ++p->double_alloc;
        memset(f, 0, sizeof(*f));
        f->loan_tick = now;
        f->task = (uintptr_t)xTaskGetCurrentTaskHandle();
        /* Last observed free destination, not the possibly stale adapter in
         * the frame. An invalid transition has no established borrow queue. */
        f->borrow_queue = was_free ? borrow_queue : 0U;
        f->alloc_adapter = read_u32(frame, 16U);
        ++p->allocs;
        if (was_free && borrow_queue != p->queue)
            record_flow(p, index, frame, borrow_queue, 0U, RF_FOREIGN_BORROW,
                        previous_state, previous_caller);
    } else if (state == RF_FREE) {
        if (f->state == RF_FREE) ++p->double_return;
        if (owner != p->queue) ++p->foreign_return;
        ++p->returns;
        if (owner != p->queue || (f->borrow_queue != 0U && f->borrow_queue != p->queue))
            record_flow(p, index, frame, owner, free_caller,
                        owner != p->queue ? RF_FOREIGN_RETURN : RF_HOME_RETURN,
                        f->state, f->caller);
    } else if (f->state == RF_FREE) {
        ++p->unowned_event;
        taskEXIT_CRITICAL();
        return;
    }
    /* Count actual child indications, inside the existing critical section.
     * No extra hook, queue traversal, formatting or output on the RX path. */
    if (state == RF_INDICATE && caller == RF_PC(amsdu_to_msdu, 0x1feU)) {
        ++p->amsdu_child;
        if ((uintptr_t)read_u32(frame, 16U) != owner) ++p->amsdu_adapter_mismatch;
    }
    f->state = (uint8_t)state; f->stage_tick = now;
    f->caller = caller; f->owner = owner;
    if (state == RF_REORDER || state == RF_DRAIN || state == RF_INDICATE ||
        state == RF_QUEUE || state == RF_DEFRAG || state == RF_DECRYPT)
        capture_attrs(f, frame, state);
    taskEXIT_CRITICAL();
}

static void observe(void *frame, uintptr_t owner, uintptr_t caller, unsigned state)
{
    observe_return(frame, owner, caller, state, 0U);
}

/* Called by the assembly shim, which preserves native caller LR without a
 * general stack walk. Native member hash and final shim ABI are build-checked. */
void carbox_recvframe_insert(void *frame, void *head, uintptr_t pc, uintptr_t free_caller)
{
    unsigned state;
    pc &= ~(uintptr_t)1U;
    free_caller &= ~(uintptr_t)1U;
    __real_rtw_list_insert_tail(frame, head);
    if (pc == RF_PC(_rtw_init_recv_priv, 0x66U)) {
        register_pool(frame, head);
        return;
    }
    state = pc == RF_PC(rtw_free_recvframe, 0x46U) ? RF_FREE :
            pc == RF_PC(enqueue_reorder_recvframe, 0x36U) ? RF_REORDER :
            (pc == RF_PC(recvframe_chk_defrag, 0x60U) ||
             pc == RF_PC(recvframe_chk_defrag, 0xe0U)) ? RF_DEFRAG : RF_QUEUE;
    if (pc == RF_PC(_rtw_enqueue_recvframe, 0x14U) &&
        (uintptr_t)head == (uintptr_t)read_u32(frame, 16U) + 0xc5cU)
        state = RF_DECRYPT;
    /* For sorted reorder insertion, head is the successor, not necessarily
     * the queue head. The copied control pointer identifies that queue. */
    observe_return(frame, (uintptr_t)head, pc, state, free_caller);
}

int __wrap_rtw_recv_entry(void *frame)
{
    struct rf_pool *p;
    unsigned index;
    uintptr_t caller = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1U;
    taskENTER_CRITICAL();
    p = find_pool((uintptr_t)frame, &index);
    if (p != NULL && p->frames[index].state != RF_FREE) {
        struct rf_frame *f = &p->frames[index];
        f->entry_adapter = read_u32(frame, 16U);
        f->entry_caller = caller;
        capture_attrs(f, frame, RF_ALLOC);
    }
    taskEXIT_CRITICAL();
    /* Native entry may consume/free frame even on failure. Do not touch after. */
    return __real_rtw_recv_entry(frame);
}

void __wrap_rtw_list_delete(void *frame)
{
    uintptr_t pc = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1U;
    __real_rtw_list_delete(frame);
    /* free's own delete is transitional and must not hide duplicate returns. */
    if (pc == RF_PC(rtw_free_recvframe, 0x38U)) return;
    observe(frame, 0U, pc,
            pc == RF_PC(_rtw_alloc_recvframe, 0x14U) ? RF_ALLOC :
            pc == RF_PC(recv_indicatepkts_in_order, 0x90U) ? RF_DRAIN : RF_DETACHED);
}

int __wrap_rtw_recv_indicatepkt(void *adapter, void *frame)
{
    uintptr_t pc = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1U;
    observe(frame, (uintptr_t)adapter, pc, RF_INDICATE);
    /* The native function returns the frame itself. Never inspect it after
     * this call: another task may already have borrowed it again. */
    return __real_rtw_recv_indicatepkt(adapter, frame);
}

void __wrap_rtw_vmfree(void *ptr, uint32_t size)
{
    uintptr_t pc = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1U;
    unsigned i;
    if (pc == RF_PC(_rtw_free_recv_priv, 0x2aU)) {
        taskENTER_CRITICAL();
        for (i = 0; i < RF_POOLS; ++i)
            if (pools[i].active && pools[i].raw == (uintptr_t)ptr) pools[i].active = 0U;
        taskEXIT_CRITICAL();
    }
    __real_rtw_vmfree(ptr, size);
}

static unsigned long age_ms(uint32_t now, uint32_t then)
{
    return (unsigned long)((uint64_t)(uint32_t)(now - then) * 1000U / configTICK_RATE_HZ);
}

static void report_flows(void)
{
    static const char *const kinds[] = { "foreign_return", "foreign_borrow", "home_return" };
    static uint32_t previous_lost;
    unsigned i;
    for (i = 0; i < RF_EVENTS_PER_REPORT; ++i) {
        struct rf_event *e = &report_event;
        taskENTER_CRITICAL();
        if (event_count == 0U) { taskEXIT_CRITICAL(); break; }
        *e = events[event_read];
        event_read = (event_read + 1U) % RF_EVENTS; --event_count;
        taskEXIT_CRITICAL();
        /* id is the repeated continuation-line key for a multi-line event. */
        carbox_rxhd_log("[WLAN_FRAME_FLOW] id=%lu tick=%lu event=%s epoch=%lu slot=%lu frame=%p home=%p queue=%p queue_epoch=%lu borrow_queue=%p alloc_adapter=%p entry_adapter=%p entry_pc=%p adapter_at_hook=%p buddy=%p adapter_type=%lu promisc=%lu free_pc=%p prev_state=%lu prev_pc=%p loan_ms=%lu attrs=%u seq80211=%u amsdu=%u frag/more=%u/%u home_count_at_hook=%lu queue_count_at_hook=%lu\n",
            (unsigned long)e->id, (unsigned long)e->tick, kinds[e->kind],
            (unsigned long)e->epoch, (unsigned long)e->slot, (void *)e->frame,
            (void *)e->home, (void *)e->queue, (unsigned long)e->queue_epoch,
            (void *)e->loan.borrow_queue, (void *)e->loan.alloc_adapter,
            (void *)e->loan.entry_adapter, (void *)e->loan.entry_caller,
            (void *)e->adapter, (void *)e->buddy, (unsigned long)e->adapter_type,
            (unsigned long)e->promisc, (void *)e->free_caller,
            (unsigned long)e->previous_state, (void *)e->previous_caller,
            age_ms(e->tick, e->loan.loan_tick), e->loan.attrs, e->loan.seq,
            e->loan.amsdu, e->loan.frag, e->loan.morefrag,
            (unsigned long)e->home_count_at_hook, (unsigned long)e->queue_count_at_hook);
    }
    {
        uint32_t lost, queued, now;
        taskENTER_CRITICAL();
        lost = event_lost; queued = event_count; now = xTaskGetTickCount();
        taskEXIT_CRITICAL();
        if (lost != previous_lost || queued != 0U) {
            carbox_rxhd_log("[WLAN_FRAME_FLOW_Q] tick=%lu queued=%lu lost=%lu capacity=16 per_report=2\n",
                (unsigned long)now, (unsigned long)queued, (unsigned long)lost);
            previous_lost = lost;
        }
    }
}

void carbox_recvframe_trace_report(void)
{
    static uint32_t epochs[RF_POOLS], print_tick[RF_POOLS], detail_tick[RF_POOLS];
    static const char *const names[] = { "free", "alloc", "reorder", "queue", "drain", "detached", "indicate", "defrag", "decrypt" };
    unsigned slot, i;
    report_flows();
    for (slot = 0; slot < RF_POOLS; ++slot) {
        struct rf_pool *p = &report_pool;
        uint32_t now, live = 0U, reorder_mask = 0U, chosen = 0U;
        unsigned counts[RF_STATES] = { 0U }, held = 0U, fresh;
        unsigned long oldest = 0U;
        unsigned home_free = 0U, foreign_free = 0U;
        uint32_t foreign_mask = 0U, native_free, type, promisc;
        uintptr_t buddy;
        unsigned long foreign_oldest = 0U;
        taskENTER_CRITICAL();
        *p = pools[slot];
        now = xTaskGetTickCount();
        native_free = p->active ? pool_count(p->queue) : RF_UNKNOWN;
        adapter_info(p->active ? p->queue - 0xc44U : 0U, &buddy, &type, &promisc);
        taskEXIT_CRITICAL();
        if (!p->active) continue;
        fresh = epochs[slot] != p->epoch;
        for (i = 0; i < RF_COUNT; ++i) {
            struct rf_frame *f = &p->frames[i];
            ++counts[f->state];
            if (f->state != RF_FREE) {
                unsigned long age = age_ms(now, f->loan_tick);
                ++held; live |= 1UL << i;
                if (f->state == RF_REORDER) reorder_mask |= 1UL << i;
                if (age > oldest) oldest = age;
            } else if (f->owner == p->queue) ++home_free;
            else {
                unsigned long age = age_ms(now, f->stage_tick);
                ++foreign_free; foreign_mask |= 1UL << i;
                if (age > foreign_oldest) foreign_oldest = age;
            }
        }
        if (!fresh && held < 24U && oldest < RF_OLD_MS && age_ms(now, print_tick[slot]) < 10000U)
            continue;
        carbox_rxhd_log("[WLAN_FRAME_POOL] tick=%lu epoch=%lu queue=%p base=%p held=%u alloc=%u reorder=%u other_queue=%u drain=%u detached=%u indicate=%u defrag=%u decrypt=%u oldest_ms=%lu live_mask=%08lx reorder_mask=%08lx totals_alloc/return=%lu/%lu double_alloc/return=%lu/%lu foreign=%lu unowned=%lu missed_pools=%lu\n",
            (unsigned long)now, (unsigned long)p->epoch, (void *)p->queue, (void *)p->base,
            held, counts[RF_ALLOC], counts[RF_REORDER], counts[RF_QUEUE], counts[RF_DRAIN],
            counts[RF_DETACHED], counts[RF_INDICATE], counts[RF_DEFRAG], counts[RF_DECRYPT], oldest, (unsigned long)live,
            (unsigned long)reorder_mask, (unsigned long)p->allocs, (unsigned long)p->returns,
            (unsigned long)p->double_alloc, (unsigned long)p->double_return,
            (unsigned long)p->foreign_return, (unsigned long)p->unowned_event, (unsigned long)missed_pools);
        carbox_rxhd_log("[WLAN_POOL_BALANCE] tick=%lu epoch=%lu queue=%p native_free=%lu home_free=%u foreign_free=%u held=%u foreign_mask=%08lx foreign_oldest_ms=%lu adapter=%p buddy=%p adapter_type=%lu promisc=%lu amsdu_child/mismatch=%lu/%lu\n",
            (unsigned long)now, (unsigned long)p->epoch, (void *)p->queue,
            (unsigned long)native_free, home_free, foreign_free, held,
            (unsigned long)foreign_mask, foreign_oldest, (void *)(p->queue - 0xc44U),
            (void *)buddy, (unsigned long)type, (unsigned long)promisc,
            (unsigned long)p->amsdu_child, (unsigned long)p->amsdu_adapter_mismatch);
        print_tick[slot] = now; epochs[slot] = p->epoch;
        if (oldest < RF_OLD_MS || (!fresh && detail_tick[slot] != 0U && age_ms(now, detail_tick[slot]) < 10000U))
            continue;
        /* Four oldest records, at most once per 10 seconds per pool. Masks
         * identify all live/reorder slots without printing every packet. */
        for (i = 0; i < 4U; ++i) {
            unsigned n, pick = RF_COUNT;
            uint32_t longest = 0U;
            for (n = 0; n < RF_COUNT; ++n) {
                uint32_t age = now - p->frames[n].loan_tick;
                if ((live & ~chosen & (1UL << n)) && (pick == RF_COUNT || age > longest)) {
                    pick = n; longest = age;
                }
            }
            if (pick == RF_COUNT) break;
            {
                struct rf_frame *f = &p->frames[pick];
                carbox_rxhd_log("[WLAN_FRAME] tick=%lu epoch=%lu slot=%u frame=%p state=%s age_ms=%lu stage_ms=%lu caller=%p owner=%p reorder=%p task=%p attrs=%u seq80211=%u len=%u amsdu=%u frag/more=%u/%u stop_flags=%u\n",
                    (unsigned long)now, (unsigned long)p->epoch, pick,
                    (void *)(p->base + pick * RF_STRIDE), names[f->state],
                    age_ms(now, f->loan_tick), age_ms(now, f->stage_tick),
                    (void *)f->caller, (void *)f->owner, (void *)f->control,
                    (void *)f->task, f->attrs, f->seq, f->len, f->amsdu, f->frag, f->morefrag, f->stop_flags);
            }
            chosen |= 1UL << pick;
        }
        detail_tick[slot] = now;
    }
}
#endif
