/* 仅观察已校验 v17 AirPlayAudio.o 的七个调用点，不更改 PCM、RTP 或节奏。
 * 统计按输出线程的 RingBuffer_init 分代，退出后失效，防止 fd/缓冲复用串账。
 */
#include "audio_tx_evidence.h"
#if CONFIG_AUDIO_TX_EVIDENCE
#include <stddef.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "diag.h"
#include "lwip/sockets.h"

/* 原厂符号大小 720；这里只声明已由 ARM 指令验证的公共前缀。
 * ring 偏移 48，frame_bytes 偏移 20，sample_rate 偏移 32。
 */
struct audio_native_prefix {
    int data_fd, output_fd;
    uint32_t stream_type;
    uint16_t seq, padding;
    uint32_t timestamp, frame_bytes, last_time;
    int audio_type, sample_rate, channels, bits;
    uint16_t port, padding2;
    void *ring;
};
typedef char audio_ring_offset_check[offsetof(struct audio_native_prefix, ring)==48 ? 1 : -1];
extern struct audio_native_prefix mainAudioStream, altAudioStream;
extern int RingBuffer_init(void *, int);
extern int RingBuffer_push(void *, char *, int);
extern int RingBuffer_pop(void *, char *, int);
extern int RingBuffer_size(void *);
extern int AirPlayAudio_SetAudioStreamPacket(void *, void *, size_t *, void *, size_t);
extern void *pthread_self(void);
extern void pthread_exit(void *);

struct audio_evidence {
    uint32_t generation, active, revision;
    void *ring, *thread, *packet;
    int fd, rate;
    uint32_t frame_bytes, stream_type;
    uint32_t pushes, push_fail, push_bytes, pops, pop_empty, pop_bytes;
    uint32_t capacity, push_rejected_bytes;
    int fill_last, fill_min, fill_max, push_before, push_after, pop_before, pop_after;
    uint32_t packets, seq_bad, ts_bad, frame_bad, header_bad;
    uint32_t last_ts, last_frames, payload_bytes;
    uint16_t last_seq;
    uint32_t sends, send_ok, send_error, send_short, wire_bad;
    uint32_t last_send_us, gap_max_us, gap_over20ms, send_max_us;
    uint64_t send_total_us;
    int last_result;
};
static struct audio_evidence audio_stats[2];
static uint32_t untracked_sends;

static int context_index(const void *ctx)
{
    return ctx==&mainAudioStream ? 0 : ctx==&altAudioStream ? 1 : -1;
}
static struct audio_native_prefix *context_at(int i)
{
    return i==0 ? &mainAudioStream : &altAudioStream;
}
/* 调用者持有临界区；只识别当前活跃的一代。 */
static int ring_index(const void *ring)
{
    int i;
    for(i=0;i<2;i++) if(audio_stats[i].active && audio_stats[i].ring==ring) return i;
    return -1;
}
static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static uint16_t be16(const unsigned char *p) { return (uint16_t)((p[0]<<8)|p[1]); }

int carbox_audio_tx_ring_init(void *ring, int capacity)
{
    int i, result=RingBuffer_init(ring,capacity);
    int fill=RingBuffer_size(ring);
    void *thread=pthread_self();
    for(i=0;i<2;i++) {
        struct audio_native_prefix *ctx=context_at(i);
        if(ctx->ring==ring) {
            struct audio_evidence *s=&audio_stats[i];
            taskENTER_CRITICAL();
            uint32_t generation=s->generation+1;
            memset(s,0,sizeof(*s));
            s->generation=generation; s->active=1; s->revision=1;
            s->ring=ring; s->thread=thread; s->fd=ctx->output_fd;
            s->frame_bytes=ctx->frame_bytes; s->rate=ctx->sample_rate;
            s->stream_type=ctx->stream_type;
            s->capacity=(uint32_t)capacity;
            s->fill_last=s->fill_min=s->fill_max=fill;
            taskEXIT_CRITICAL();
            break;
        }
    }
    return result;
}

static int ring_operation(void *ring, char *data, int size, int push)
{
    int i, result, before, after;
    uint32_t generation=0;
    taskENTER_CRITICAL();
    i=ring_index(ring);
    if(i>=0) generation=audio_stats[i].generation;
    taskEXIT_CRITICAL();
    /* 原厂 size 只有一次计数读取，不锁队列、不改变队列状态。
     * 前后快照可能包含另一线程的操作，属于采样水位，不能当作原子差值。
     */
    before=RingBuffer_size(ring);
    result=push ? RingBuffer_push(ring,data,size) : RingBuffer_pop(ring,data,size);
    after=RingBuffer_size(ring);
    taskENTER_CRITICAL();
    if(i>=0 && audio_stats[i].active && audio_stats[i].generation==generation) {
        struct audio_evidence *s=&audio_stats[i];
        if(push) {
            s->pushes++; s->push_fail+=!result;
            if(size>0) { if(result) s->push_bytes+=(uint32_t)size; else s->push_rejected_bytes+=(uint32_t)size; }
            s->push_before=before; s->push_after=after;
        } else {
            s->pops++; s->pop_empty+=!result;
            if(result && size>0) s->pop_bytes+=(uint32_t)size;
            s->pop_before=before; s->pop_after=after;
        }
        s->fill_last=after;
        if(before<s->fill_min) s->fill_min=before;
        if(after<s->fill_min) s->fill_min=after;
        if(before>s->fill_max) s->fill_max=before;
        if(after>s->fill_max) s->fill_max=after;
        s->revision++;
    }
    taskEXIT_CRITICAL();
    return result;
}
int carbox_audio_tx_ring_push(void *ring,char *data,int size) { return ring_operation(ring,data,size,1); }
int carbox_audio_tx_ring_pop(void *ring,char *data,int size) { return ring_operation(ring,data,size,0); }

int carbox_audio_tx_packet(void *ctx,void *packet,size_t *length,void *data,size_t size)
{
    int i=context_index(ctx), result;
    uint32_t generation=0;
    taskENTER_CRITICAL();
    if(i>=0) generation=audio_stats[i].generation;
    taskEXIT_CRITICAL();
    result=AirPlayAudio_SetAudioStreamPacket(ctx,packet,length,data,size);
    taskENTER_CRITICAL();
    if(i>=0 && audio_stats[i].active && audio_stats[i].generation==generation) {
        struct audio_evidence *s=&audio_stats[i];
        const unsigned char *p=packet;
        uint16_t seq=be16(p+2);
        uint32_t ts=be32(p+4), frames=s->frame_bytes ? size/s->frame_bytes : 0;
        if(!s->frame_bytes || size%s->frame_bytes) s->frame_bad++;
        if(!result || *length!=size+12 || (p[0]>>6)!=2 || (p[1]&127)!=(s->stream_type&127)) s->header_bad++;
        if(s->packets) {
            if((uint16_t)(seq-s->last_seq)!=1) s->seq_bad++;
            if((uint32_t)(ts-s->last_ts)!=s->last_frames) s->ts_bad++;
        }
        s->packets++; s->last_seq=seq; s->last_ts=ts; s->last_frames=frames;
        s->payload_bytes=(uint32_t)size; s->packet=packet; s->revision++;
    }
    taskEXIT_CRITICAL();
    return result;
}

ssize_t carbox_audio_tx_sendto(int fd,const void *data,size_t size,int flags,
                             const struct sockaddr *to,socklen_t tolen)
{
    int i, found=-1;
    void *thread=pthread_self();
    uint32_t generation=0, start=hal_read_curtime_us(), elapsed;
    ssize_t result;
    taskENTER_CRITICAL();
    for(i=0;i<2;i++) {
        struct audio_evidence *s=&audio_stats[i];
        if(s->active && s->thread==thread && s->fd==fd && s->packet==data) {
            const unsigned char *p=data;
            found=i; generation=s->generation;
            if(size<12 || be16(p+2)!=s->last_seq || be32(p+4)!=s->last_ts ||
               (size!=s->payload_bytes+12 && size!=s->payload_bytes+36)) s->wire_bad++;
            if(s->sends) {
                /* 32 位硬件时钟差值按模运算，支持一次自然回绕。 */
                uint32_t gap=start-s->last_send_us;
                if(gap>s->gap_max_us) s->gap_max_us=gap;
                s->gap_over20ms+=gap>20000;
            }
            s->last_send_us=start; s->sends++; s->revision++;
            break;
        }
    }
    if(found<0) untracked_sends++;
    taskEXIT_CRITICAL();
    /* 原样调用一次；之后不调用 socket/libc 错误接口、不重试、不保存回写 errno。 */
    result=lwip_sendto(fd,data,size,flags,to,tolen);
    elapsed=hal_read_curtime_us()-start;
    taskENTER_CRITICAL();
    if(found>=0 && audio_stats[found].active && audio_stats[found].generation==generation) {
        struct audio_evidence *s=&audio_stats[found];
        s->send_ok+=result==(ssize_t)size;
        s->send_error+=result<0;
        s->send_short+=result>=0 && result!=(ssize_t)size;
        s->send_total_us+=elapsed;
        if(elapsed>s->send_max_us) s->send_max_us=elapsed;
        s->last_result=(int)result; s->revision++;
    }
    taskEXIT_CRITICAL();
    return result;
}

void carbox_audio_tx_thread_exit(void *result)
{
    int i;
    void *thread=pthread_self();
    taskENTER_CRITICAL();
    for(i=0;i<2;i++) if(audio_stats[i].active && audio_stats[i].thread==thread) {
        audio_stats[i].active=0; audio_stats[i].packet=NULL; audio_stats[i].revision++;
    }
    taskEXIT_CRITICAL();
    pthread_exit(result);
}

void carbox_audio_tx_evidence_report(uint32_t sequence)
{
    static uint32_t previous_generation[2],previous_revision[2];
    int i;
    for(i=0;i<2;i++) {
        struct audio_evidence s;
        uint32_t untracked;
        taskENTER_CRITICAL(); s=audio_stats[i]; untracked=untracked_sends; taskEXIT_CRITICAL();
        if(!s.generation || (s.generation==previous_generation[i] && s.revision==previous_revision[i])) continue;
        previous_generation[i]=s.generation; previous_revision[i]=s.revision;
        rt_printf("[AUDIO_TX][%lu] stream=%lu gen=%lu active=%lu fd=%d rate=%d framebytes=%lu cumulative=1 push/fail/bytes=%lu/%lu/%lu pop/empty/bytes=%lu/%lu/%lu packets=%lu seq=%u ts=%lu seqbad/tsbad/framebad/headerbad=%lu/%lu/%lu/%lu\r\n",
            (unsigned long)sequence,(unsigned long)s.stream_type,(unsigned long)s.generation,(unsigned long)s.active,s.fd,s.rate,(unsigned long)s.frame_bytes,
            (unsigned long)s.pushes,(unsigned long)s.push_fail,(unsigned long)s.push_bytes,(unsigned long)s.pops,(unsigned long)s.pop_empty,(unsigned long)s.pop_bytes,
            (unsigned long)s.packets,(unsigned)s.last_seq,(unsigned long)s.last_ts,(unsigned long)s.seq_bad,(unsigned long)s.ts_bad,(unsigned long)s.frame_bad,(unsigned long)s.header_bad);
        rt_printf("[AUDIO_TX_NET][%lu] stream=%lu gen=%lu sends/ok/error/short=%lu/%lu/%lu/%lu last=%d wirebad=%lu gap_max_us=%lu gap20ms=%lu send_avg/max_us=%lu/%lu untracked=%lu\r\n",
            (unsigned long)sequence,(unsigned long)s.stream_type,(unsigned long)s.generation,(unsigned long)s.sends,(unsigned long)s.send_ok,(unsigned long)s.send_error,(unsigned long)s.send_short,s.last_result,
            (unsigned long)s.wire_bad,(unsigned long)s.gap_max_us,(unsigned long)s.gap_over20ms,
            (unsigned long)(s.sends?s.send_total_us/s.sends:0),(unsigned long)s.send_max_us,(unsigned long)untracked);
        rt_printf("[AUDIO_TX_RING][%lu] stream=%lu gen=%lu sampled=1 capacity=%lu fill_last/min/max=%d/%d/%d push_before/after=%d/%d pop_before/after=%d/%d rejected_bytes=%lu\r\n",
            (unsigned long)sequence,(unsigned long)s.stream_type,(unsigned long)s.generation,(unsigned long)s.capacity,
            s.fill_last,s.fill_min,s.fill_max,s.push_before,s.push_after,s.pop_before,s.pop_after,(unsigned long)s.push_rejected_bytes);
    }
}
#endif
