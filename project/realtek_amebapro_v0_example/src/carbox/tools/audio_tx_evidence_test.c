#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../audio_tx_evidence.c"

struct audio_native_prefix mainAudioStream,altAudioStream;
static uint32_t now_us;
static int critical,ring_result=1,send_result,send_calls,push_calls,pop_calls,packet_calls,exit_calls,reports;
static int send_errno,reset_during_send;
static int ring_fill;
static void *current_thread=(void *)1;
static void *expected_ring,*expected_data;
static int expected_size;
static const struct sockaddr *expected_addr;
void enter_critical(void) { critical++; }
void exit_critical(void) { assert(critical>0); critical--; }
void *pthread_self(void) { return current_thread; }
void pthread_exit(void *result) { assert(result==(void *)99); assert(!critical); exit_calls++; }
uint32_t hal_read_curtime_us(void) { return now_us; }
int rt_printf(const char *format,...) { (void)format; assert(!critical); reports++; return 0; }
int RingBuffer_init(void *ring,int size) { assert(!critical); assert(ring==expected_ring); assert(size==100000); ring_fill=0; return 1; }
int RingBuffer_size(void *ring) { assert(!critical && ring==expected_ring); return ring_fill; }
int RingBuffer_push(void *ring,char *data,int size) { assert(!critical); assert(ring==expected_ring && data==expected_data && size==expected_size); push_calls++; if(ring_result)ring_fill+=size; return ring_result; }
int RingBuffer_pop(void *ring,char *data,int size) { assert(!critical); assert(ring==expected_ring && data==expected_data && size==expected_size); pop_calls++; if(ring_result)ring_fill-=size; return ring_result; }
static void put32(unsigned char *p,uint32_t n) { p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n; }
int AirPlayAudio_SetAudioStreamPacket(void *ctx,void *packet,size_t *len,void *data,size_t size)
{
    struct audio_native_prefix *c=ctx;
    unsigned char *p=packet;
    assert(!critical && data==expected_data && (int)size==expected_size);
    p[0]=0x80;p[1]=c->stream_type;p[2]=c->seq>>8;p[3]=c->seq;
    put32(p+4,c->timestamp);put32(p+8,0);
    memcpy(p+12,data,size);
    c->seq++;c->timestamp+=size/c->frame_bytes;*len=size+12;packet_calls++;
    return 1;
}
ssize_t lwip_sendto(int fd,const void *data,size_t size,int flags,const struct sockaddr *to,socklen_t tolen)
{
    assert(!critical && fd==7 && data && size>=12 && flags==3 && to==expected_addr && tolen==sizeof(*to));
    send_calls++; send_errno=123;
    now_us+=500;
    if(reset_during_send) { reset_during_send=0; carbox_audio_tx_ring_init(expected_ring,100000); }
    return send_result;
}
static void emit(unsigned char *packet,unsigned char *pcm,int bytes)
{
    size_t len;
    unsigned char copy[64];
    expected_data=pcm;expected_size=bytes;
    assert(carbox_audio_tx_packet(&mainAudioStream,packet,&len,pcm,(size_t)bytes)==1);
    assert(len==(size_t)bytes+12 && !memcmp(pcm,packet+12,bytes));
    memcpy(copy,packet,len);send_result=(int)len;
    assert(carbox_audio_tx_sendto(7,packet,len,3,expected_addr,sizeof(*expected_addr))==(ssize_t)len);
    assert(!memcmp(copy,packet,len));
    assert(send_errno==123);
}
int main(void)
{
    unsigned char packet[64]={0},pcm[32];
    struct sockaddr addr={0};
    size_t len;
    memset(pcm,0x5a,sizeof(pcm));expected_addr=&addr;
    mainAudioStream.ring=(void *)10;mainAudioStream.output_fd=7;
    mainAudioStream.frame_bytes=4;mainAudioStream.sample_rate=48000;mainAudioStream.stream_type=100;
    mainAudioStream.seq=65535;mainAudioStream.timestamp=UINT32_MAX-1;
    expected_ring=mainAudioStream.ring;
    assert(carbox_audio_tx_ring_init(expected_ring,100000)==1);
    expected_data=pcm;expected_size=8;ring_result=0;
    assert(carbox_audio_tx_ring_push(expected_ring,(char *)pcm,8)==0);
    assert(carbox_audio_tx_ring_pop(expected_ring,(char *)pcm,8)==0);
    ring_result=1;
    assert(carbox_audio_tx_ring_push(expected_ring,(char *)pcm,8)==1);
    assert(carbox_audio_tx_ring_pop(expected_ring,(char *)pcm,8)==1);
    assert(push_calls==2 && pop_calls==2 && audio_stats[0].push_fail==1 && audio_stats[0].pop_empty==1);
    assert(audio_stats[0].push_bytes==8 && audio_stats[0].pop_bytes==8);
    assert(audio_stats[0].push_rejected_bytes==8 && audio_stats[0].capacity==100000);
    assert(audio_stats[0].fill_min==0 && audio_stats[0].fill_max==8 && audio_stats[0].fill_last==0);
    assert(audio_stats[0].push_before==0 && audio_stats[0].push_after==8);
    assert(audio_stats[0].pop_before==8 && audio_stats[0].pop_after==0);
    now_us=UINT32_MAX-1000;emit(packet,pcm,8);
    now_us+=7500;emit(packet,pcm,16);
    assert(audio_stats[0].seq_bad==0 && audio_stats[0].ts_bad==0);
    assert(audio_stats[0].gap_max_us==8000 && audio_stats[0].send_max_us==500);
    assert(audio_stats[0].last_ts==0 && audio_stats[0].last_seq==0);
    mainAudioStream.timestamp+=3;now_us+=30000;emit(packet,pcm,8);
    assert(audio_stats[0].ts_bad==1 && audio_stats[0].gap_over20ms==1);
    expected_size=8;
    assert(carbox_audio_tx_packet(&mainAudioStream,packet,&len,pcm,8)==1);
    send_result=-1;
    assert(carbox_audio_tx_sendto(7,packet,len,3,&addr,sizeof(addr))==-1);
    assert(audio_stats[0].send_error==1 && send_errno==123);
    assert(carbox_audio_tx_packet(&mainAudioStream,packet,&len,pcm,8)==1);
    packet[7]^=1;send_result=3;
    assert(carbox_audio_tx_sendto(7,packet,len,3,&addr,sizeof(addr))==3);
    assert(audio_stats[0].wire_bad==1 && audio_stats[0].send_short==1);
    assert(audio_stats[0].ts_bad==1 && audio_stats[0].seq_bad==0);
    current_thread=(void *)2;
    assert(carbox_audio_tx_sendto(7,packet,len,3,&addr,sizeof(addr))==3);
    assert(untracked_sends==1 && audio_stats[0].sends==5);
    current_thread=(void *)1;
    carbox_audio_tx_thread_exit((void *)99);
    assert(exit_calls==1 && !audio_stats[0].active);
    carbox_audio_tx_evidence_report(1);assert(reports==3);
    carbox_audio_tx_evidence_report(2);assert(reports==3);
    /* 同 fd、同 ring 重启必须重置包序列比较和计数。 */
    mainAudioStream.seq=1;mainAudioStream.timestamp=0;
    carbox_audio_tx_ring_init(expected_ring,100000);
    emit(packet,pcm,8);
    assert(audio_stats[0].generation==2 && audio_stats[0].packets==1 && !audio_stats[0].seq_bad && !audio_stats[0].ts_bad);
    /* 模拟发送尚未返回时状态换代，不把旧结果写入新一代。 */
    reset_during_send=1;send_result=-1;
    assert(carbox_audio_tx_sendto(7,packet,20,3,&addr,sizeof(addr))==-1);
    assert(audio_stats[0].generation==3 && audio_stats[0].sends==0 && audio_stats[0].send_error==0);
    assert(packet_calls==6 && send_calls==8 && !critical);
    puts("audio evidence forwarding, counter rollover, failure and generation checks passed");
    return 0;
}
