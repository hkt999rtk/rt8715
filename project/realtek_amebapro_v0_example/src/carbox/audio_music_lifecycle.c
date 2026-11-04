/* Control-plane evidence only: no payload inspection, allocation or printing
 * in the caller. The existing low-priority reporter drains this bounded ring.
 * IDs and saved-register layout are defined by the hash-pinned patch script.
 * A return event's a0..a2 are native result registers, not incoming arguments.
 * Match entry/return by task + caller. Stream-type events expose native parsed
 * values, so incoming type 102 must not be confused with outgoing type 100. */
#include <stdint.h>
#include <stddef.h>
#include "FreeRTOS.h"
#include "task.h"
#include "audio_diag_log.h"
#define LIFE_SLOTS 64U
struct life_event {
    uint32_t serial, ms, task, caller, session, a0, a1, a2;
    uint8_t id, phase;
};
/* Low-frequency retained control events use the existing zero-initialized
 * external-RAM section, preserving the internal SRAM safety margin. */
static struct life_event events[LIFE_SLOTS]
    __attribute__((section(".lpddr.bss.music_lifecycle")));
static uint32_t next_serial, read_index, write_index, count, lost, reported_lost, reported_skip;
static const char *life_name(uint32_t id)
{
    switch(id) {
    case 1:return "connection_close";
    case 2:return "incoming_flush";
    case 3:return "incoming_setup";
    case 4:return "incoming_teardown";
    case 5:return "setup_failed_teardown";
    case 6:return "setup_stream_type";
    case 7:return "teardown_stream_type";
    case 20:return "output_start";
    case 21:return "output_stop";
    case 22:return "outgoing_teardown";
    case 23:return "pcm_exit";
    default:return "unknown";
    }
}
void carbox_music_life_record(uint32_t id,uint32_t phase,const uint32_t *r)
{
    struct life_event e;
    /* Parsed type probes emit only after the native int64-returning function. */
    if((id==6U || id==7U) && !phase)return;
    e.ms=(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS);
    e.task=(uint32_t)(uintptr_t)xTaskGetCurrentTaskHandle();
    e.caller=r[13];e.session=0;
    if(id==6U || id==7U)e.session=r[4];
    else if(id<=5U && !phase)e.session=r[0];
    e.a0=r[0];e.a1=r[1];e.a2=r[2];e.id=(uint8_t)id;e.phase=(uint8_t)phase;
    taskENTER_CRITICAL();
    e.serial=++next_serial;
    if(count==LIFE_SLOTS)lost++;
    else { events[write_index]=e;write_index=(write_index+1U)%LIFE_SLOTS;count++; }
    taskEXIT_CRITICAL();
}
void carbox_music_lifecycle_report(void)
{
    unsigned n;
    uint32_t dropped, skipped;
    /* Bounded drain: caller never waits for space and never emits UART. */
    for(n=0;n<LIFE_SLOTS;n++) {
        struct life_event e;
        taskENTER_CRITICAL();
        if(!count) {taskEXIT_CRITICAL();break;}
        e=events[read_index];read_index=(read_index+1U)%LIFE_SLOTS;count--;
        taskEXIT_CRITICAL();
        carbox_audio_diag_log("[MUSIC_LIFE] id=%lu ms=%lu event=%s phase=%u task=%08lx caller=%08lx\n",
            (unsigned long)e.serial,(unsigned long)e.ms,life_name(e.id),(unsigned)e.phase,
            (unsigned long)e.task,(unsigned long)e.caller);
        carbox_audio_diag_log("[MUSIC_ARGS] id=%lu session=%08lx a0/a1/a2=%08lx/%08lx/%08lx\n",
            (unsigned long)e.serial,(unsigned long)e.session,
            (unsigned long)e.a0,(unsigned long)e.a1,(unsigned long)e.a2);
    }
    taskENTER_CRITICAL();dropped=lost;taskEXIT_CRITICAL();
    skipped=carbox_audio_diag_log_skipped();
    if(dropped!=reported_lost || skipped!=reported_skip) {
        carbox_audio_diag_log("[MUSIC_LIFE_LOST] id=0 events=%lu log_skip=%lu capacity=%u\n",
            (unsigned long)dropped,(unsigned long)skipped,LIFE_SLOTS);
        reported_lost=dropped;reported_skip=skipped;
    }
}
