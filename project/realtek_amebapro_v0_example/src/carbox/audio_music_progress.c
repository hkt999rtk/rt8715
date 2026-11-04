/* Observation only, hash-pinned GeneralAudio (incoming type 102) call sites.
 * Does not infer that outgoing type 100 equals incoming type 100. No allocation,
 * UART, packet-body scan, mutex acquisition or native state mutation in probes.
 * Counters reset on native GeneralAudio RX thread start, not on output teardown.
 * Local FLUSH handling is correlated separately by session pointer and timestamp.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "audio_diag_log.h"
#include "audio_music_progress.h"

#define MUSIC_SLOTS 4U
#define MUSIC_PERIOD_US 5000000U
struct music_progress {
    void *session;
    uint32_t sid, active, dirty, report_at, start_at, end_at, stream, fd;
    uint32_t recv_calls, received, recv_errors, bytes, invalid, recv_start, recv_max;
    uint32_t buffer, capacity, length_ptr, have_rx, last_rx, gap_max;
    uint32_t have_seq, first_seq, high_seq, last_seq, last_ts, ssrc;
    uint32_t seq_gaps, missing, backward, duplicates, ssrc_changes, large_jump;
    uint32_t select_start, select_max, select_errors, receiving_result_errors;
    uint32_t reads, read_start, read_max, last_read, requested, pcm_copied;
    uint32_t zero_empty, zero_future, last_copy, copy_calls, have_copy;
    uint32_t decrypt_calls, decrypt_errors, decrypt_start, decrypt_max, last_decrypt;
    uint32_t decode_calls, decode_errors, decode_start, decode_max, last_decode;
    uint32_t decoded_bytes, decode_out_ptr, busy, busy_max, native_rx_seq, native_rx_ts;
    uint32_t native_play_seq, native_play_ts, native_flush, native_rx_count, native_play_started;
    int32_t last_recv_error, last_decrypt_error, last_decode_error;
};
static struct music_progress music_slots[MUSIC_SLOTS];
static uint32_t music_next_sid, music_lost;
static uint32_t get32(const void *p, unsigned off)
{ uint32_t v; memcpy(&v,(const uint8_t *)p+off,4); return v; }
static uint16_t get16(const void *p, unsigned off)
{ uint16_t v; memcpy(&v,(const uint8_t *)p+off,2); return v; }
static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static void max32(uint32_t *maximum, uint32_t value)
{ if(value>*maximum)*maximum=value; }
static struct music_progress *find_slot(void *session)
{
    unsigned i;
    for(i=0;i<MUSIC_SLOTS;i++)
        if(music_slots[i].active && music_slots[i].session==session)return &music_slots[i];
    return NULL;
}
/* Called only at audited points while the original session lock is held.
 * No walking native lists, and the reporter never dereferences session. */
static void native_snapshot(struct music_progress *s, void *session)
{
    s->busy=get32(session,7148);max32(&s->busy_max,s->busy);
    s->native_rx_seq=get16(session,7192);s->native_rx_ts=get32(session,7196);
    s->native_play_seq=get16(session,7208);s->native_play_ts=get32(session,7204);
    /* FlushAudio +0x5e sets byte7180; +0x84 resets RX count at608. */
    s->native_flush=((const uint8_t *)session)[7180];
    s->native_rx_count=get32(session,608);
    s->native_play_started=((const uint8_t *)session)[7200];
}
void carbox_music_progress_before(void *session,uint32_t id,const uint32_t *r)
{
    uint32_t now=hal_read_curtime_us();
    struct music_progress *s;
    unsigned i;
    taskENTER_CRITICAL();
    if(id==1){
        /* A closed but unreported lifetime is never overwritten. */
        if(find_slot(session)){music_lost++;taskEXIT_CRITICAL();return;}
        for(i=0;i<MUSIC_SLOTS;i++)if(!music_slots[i].active && !music_slots[i].dirty)break;
        if(i==MUSIC_SLOTS){music_lost++;taskEXIT_CRITICAL();return;}
        s=&music_slots[i];memset(s,0,sizeof(*s));s->session=session;
        s->sid=++music_next_sid;s->active=1;s->dirty=1;s->start_at=now;s->report_at=now;
        s->stream=get32(session,1084);s->fd=get32(session,1108);
        taskEXIT_CRITICAL();return;
    }
    s=find_slot(session);
    if(!s){taskEXIT_CRITICAL();return;}
    switch(id){
    case 2:
        s->recv_calls++;s->recv_start=now;s->buffer=r[1];s->capacity=r[2];s->length_ptr=r[3];break;
    case 3:s->select_start=now;break;
    case 4:
        /* ReadAudio saved caller stack +124 is the requested PCM byte count.
         * Hook saved r[0..12],LR then native caller stack: 14+124/4=45. */
        s->reads++;s->requested+=r[45];s->read_start=now;break;
    case 5:
        max32(&s->read_max,now-s->read_start);s->last_read=now;
        native_snapshot(s,session);break;
    case 6:case 7:s->decrypt_calls++;s->decrypt_start=now;break;
    case 8:case 9:
        s->decode_calls++;s->decode_start=now;s->decode_out_ptr=r[15];break;
    case 10:s->zero_empty+=r[2];break;
    case 11:s->zero_future+=r[2];break;
    case 12:s->pcm_copied+=r[2];s->copy_calls++;s->last_copy=now;s->have_copy=1;break;
    case 13:s->active=0;s->end_at=now;break;
    case 14:
        if((int32_t)r[6]!=0)s->receiving_result_errors++;
        native_snapshot(s,session);break;
    default:break;
    }
    s->dirty=1;taskEXIT_CRITICAL();
}
void carbox_music_progress_after(void *session,uint32_t id,const uint32_t *r)
{
    struct music_progress *s;
    uint32_t now=hal_read_curtime_us(),sid,buffer,capacity,length_ptr,out_ptr;
    uint32_t length=0,valid=0,seq=0,ts=0,ssrc=0,decoded=0;
    int32_t error=(int32_t)r[0];
    /* Ignore sites without an after-result. This avoids a critical section for
     * memset/memcpy/lock/unlock; every native function still executes normally. */
    if(id!=2 && id!=3 && id!=6 && id!=7 && id!=8 && id!=9)return;
    taskENTER_CRITICAL();s=find_slot(session);
    if(!s){taskEXIT_CRITICAL();return;}
    sid=s->sid;buffer=s->buffer;capacity=s->capacity;length_ptr=s->length_ptr;out_ptr=s->decode_out_ptr;
    taskEXIT_CRITICAL();
    if(id==2 && error==0 && length_ptr){
        length=get32((void *)(uintptr_t)length_ptr,0);
        if(buffer && length>=12 && length<=capacity){
            const uint8_t *p=(const uint8_t *)(uintptr_t)buffer;
            valid=(p[0]>>6)==2;seq=(uint32_t)p[2]<<8|p[3];ts=be32(p+4);ssrc=be32(p+8);
        }
    }
    if((id==8 || id==9) && error==0 && out_ptr)decoded=get32((void *)(uintptr_t)out_ptr,0);
    taskENTER_CRITICAL();s=find_slot(session);
    if(!s || s->sid!=sid){taskEXIT_CRITICAL();return;}
    if(id==2){
        max32(&s->recv_max,now-s->recv_start);
        if(error){s->recv_errors++;s->last_recv_error=error;}
        else{
            s->received++;s->bytes+=length;
            if(s->have_rx)max32(&s->gap_max,now-s->last_rx);
            s->last_rx=now;s->have_rx=1;
            if(!valid)s->invalid++;
            else{
                uint16_t delta=(uint16_t)(seq-s->high_seq);
                if(!s->have_seq){s->have_seq=1;s->first_seq=seq;s->high_seq=seq;}
                else if(ssrc!=s->ssrc){s->ssrc_changes++;s->high_seq=seq;}
                else if(!delta)s->duplicates++;
                else if(delta<0x8000U){
                    if(delta>1){s->seq_gaps++;s->missing+=delta-1U;}
                    if(delta>512U)s->large_jump++;
                    s->high_seq=seq;
                }else s->backward++;
                s->last_seq=seq;s->last_ts=ts;s->ssrc=ssrc;
            }
        }
    }else if(id==3){max32(&s->select_max,now-s->select_start);if(error<0)s->select_errors++;}
    else if(id==6 || id==7){
        max32(&s->decrypt_max,now-s->decrypt_start);s->last_decrypt=now;
        if(error){s->decrypt_errors++;s->last_decrypt_error=error;}
    }else if(id==8 || id==9){
        max32(&s->decode_max,now-s->decode_start);s->last_decode=now;
        if(error){s->decode_errors++;s->last_decode_error=error;}else s->decoded_bytes+=decoded;
    }
    s->dirty=1;taskEXIT_CRITICAL();
}
/* Cumulative per RX-thread lifetime. Snapshots are fixed-size, printed only by
 * the existing low priority reporter. Absence of PCM copies != proof of underrun:
 * native stream state and zero counters determine what was actually requested. */
void carbox_music_progress_report(void)
{
    unsigned i;uint32_t now,lost;static uint32_t printed_lost;
    taskENTER_CRITICAL();lost=music_lost;taskEXIT_CRITICAL();
    if(lost!=printed_lost){
        carbox_audio_diag_log("[MUSIC_PROGRESS_LOST] id=0 slots=%lu\n",(unsigned long)lost);printed_lost=lost;
    }
    for(i=0;i<MUSIC_SLOTS;i++){
        struct music_progress s;
        taskENTER_CRITICAL();now=hal_read_curtime_us();
        if(!music_slots[i].dirty || (music_slots[i].active && now-music_slots[i].report_at<MUSIC_PERIOD_US)){
            taskEXIT_CRITICAL();continue;
        }
        s=music_slots[i];music_slots[i].dirty=0;music_slots[i].report_at=now;taskEXIT_CRITICAL();
        now=hal_read_curtime_us(); /* Cannot precede producer timestamps in snapshot. */
#define U(v) ((unsigned long)(v))
#define AGE(v,seen) U((seen)?now-(v):UINT32_MAX)
        carbox_audio_diag_log("[MUSIC_PROGRESS] id=%lu ctx=%p stream=%lu fd=%lu closed=%lu at/start/end_us=%lu/%lu/%lu\n",
            U(s.sid),s.session,U(s.stream),U(s.fd),U(!s.active),U(now),U(s.start_at),U(s.end_at));
        carbox_audio_diag_log("[MUSIC_RX] id=%lu calls/ok/err=%lu/%lu/%lu bytes=%lu age/gap/recv_max_us=%lu/%lu/%lu\n",
            U(s.sid),U(s.recv_calls),U(s.received),U(s.recv_errors),U(s.bytes),AGE(s.last_rx,s.have_rx),U(s.gap_max),U(s.recv_max));
        carbox_audio_diag_log("[MUSIC_SEQ] id=%lu first/last/high=%lu/%lu/%lu ts=%lu ssrc=%08lx gaps/missing=%lu/%lu\n",
            U(s.sid),U(s.first_seq),U(s.last_seq),U(s.high_seq),U(s.last_ts),U(s.ssrc),U(s.seq_gaps),U(s.missing));
        carbox_audio_diag_log("[MUSIC_SEQ_CHECK] id=%lu backward/dup/ssrc_change/jump512/invalid=%lu/%lu/%lu/%lu/%lu\n",
            U(s.sid),U(s.backward),U(s.duplicates),U(s.ssrc_changes),U(s.large_jump),U(s.invalid));
        carbox_audio_diag_log("[MUSIC_CRYPTO] id=%lu calls/err=%lu/%lu max/age_us=%lu/%lu last_err=%ld\n",
            U(s.sid),U(s.decrypt_calls),U(s.decrypt_errors),U(s.decrypt_max),AGE(s.last_decrypt,s.decrypt_calls),(long)s.last_decrypt_error);
        carbox_audio_diag_log("[MUSIC_DECODE] id=%lu calls/err=%lu/%lu bytes=%lu max/age_us=%lu/%lu last_err=%ld\n",
            U(s.sid),U(s.decode_calls),U(s.decode_errors),U(s.decoded_bytes),U(s.decode_max),AGE(s.last_decode,s.decode_calls),(long)s.last_decode_error);
        carbox_audio_diag_log("[MUSIC_PCM] id=%lu reads=%lu request/copy/zero_empty/zero_future=%lu/%lu/%lu/%lu\n",
            U(s.sid),U(s.reads),U(s.requested),U(s.pcm_copied),U(s.zero_empty),U(s.zero_future));
        carbox_audio_diag_log("[MUSIC_TIME] id=%lu read/copy_age_us=%lu/%lu read/select_max_us=%lu/%lu select_err=%lu\n",
            U(s.sid),AGE(s.last_read,s.last_read),AGE(s.last_copy,s.have_copy),U(s.read_max),U(s.select_max),U(s.select_errors));
        carbox_audio_diag_log("[MUSIC_NATIVE] id=%lu busy/max=%lu/%lu rx_seq/ts=%lu/%lu play_seq/ts=%lu/%lu flush=%lu\n",
            U(s.sid),U(s.busy),U(s.busy_max),U(s.native_rx_seq),U(s.native_rx_ts),U(s.native_play_seq),U(s.native_play_ts),U(s.native_flush));
        carbox_audio_diag_log("[MUSIC_STATE] id=%lu rx_count/play_started=%lu/%lu rx_result_err=%lu last_recv_err=%ld\n",
            U(s.sid),U(s.native_rx_count),U(s.native_play_started),U(s.receiving_result_errors),(long)s.last_recv_error);
#undef AGE
#undef U
    }
}
