/* 只读 v18 video/timing 边界。只累计固定大小状态，UART由既有统计线程输出。
 * 原函数恰好调用一次，保持返回值、PCM、NAL、时间戳及sendto的errno。
 * 全部值为本次启动累计；frame不是网络packet，sent仅表示本机socket接受。
 */
#include "screen_flow_evidence.h"
#include "screen_nal_evidence.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "diag.h"
#include "lwip/sockets.h"
#include "lwip/errno.h"
#include <sys/time.h>
#include <string.h>
struct time_counter {uint64_t first,last;uint32_t count,same,back,at;};
static struct {
    uint32_t frames[3],sps[3],idr[3];
    struct time_counter config,video,receive,transmit,start;
    uint32_t sends,send_ok,send_error,send_short,last_send_at,last_ok_at;
    int last_result,last_error,fd;
    uint64_t wire_originate,wire_receive,wire_transmit;
} flow;
static void observe_time(struct time_counter *c,uint64_t value)
{
    uint32_t at=hal_read_curtime_us();
    taskENTER_CRITICAL();
    if(c->count){c->same+=value==c->last;c->back+=value<c->last;}
    else c->first=value;
    c->count++;c->last=value;c->at=at;
    taskEXIT_CRITICAL();
}
void carbox_screen_flow_frame(unsigned stage,const void *data,size_t length)
{
    const unsigned char *p=data;unsigned type=0;
    if(stage>=3)return;
    /* 对照原厂pData[4]&31分支，但额外验证长度和4字节起始码，避免观测越界。
     * 仅识别首个NAL；不把同一块中后面的IDR/PPS算成独立帧。 */
    if(p && length>=5 && !p[0] && !p[1] && !p[2] && p[3]==1)type=p[4]&31U;
    taskENTER_CRITICAL();flow.frames[stage]++;flow.sps[stage]+=type==7;flow.idr[stage]+=type==5;taskEXIT_CRITICAL();
    carbox_screen_nal_observe(stage,data,length);
}
extern uint64_t AirPlayClock_VideoTimestamp(void);
uint64_t carbox_screen_time_config(void)
{uint64_t v=AirPlayClock_VideoTimestamp();observe_time(&flow.config,v);return v;}
uint64_t carbox_screen_time_video(void)
{uint64_t v=AirPlayClock_VideoTimestamp();observe_time(&flow.video,v);return v;}
extern void AirPlayClock_GetTime(struct timeval *);
extern void AirPlayClock_StartTime(struct timeval *);
static uint64_t tv_us(const struct timeval *t)
{return (uint64_t)t->tv_sec*1000000ULL+(uint32_t)t->tv_usec;}
void carbox_screen_time_start(struct timeval *t)
{AirPlayClock_StartTime(t);observe_time(&flow.start,tv_us(t));}
void carbox_screen_time_receive(struct timeval *t)
{AirPlayClock_GetTime(t);observe_time(&flow.receive,tv_us(t));}
void carbox_screen_time_transmit(struct timeval *t)
{AirPlayClock_GetTime(t);observe_time(&flow.transmit,tv_us(t));}
static uint64_t be64(const unsigned char *p)
{uint64_t v=0;unsigned i;for(i=0;i<8;i++)v=(v<<8)|p[i];return v;}
int carbox_screen_timing_sendto(int fd,const void *buf,size_t n,int flags,const struct sockaddr *a,socklen_t z)
{
    int ret=lwip_sendto(fd,buf,n,flags,a,z),error=lwip_err;
    uint32_t now=hal_read_curtime_us();const unsigned char *p=buf;
    uint64_t origin=0,rx=0,tx=0;
    /* 只读完整32-byte RTCP timing response，网络字节序保留NTP epoch。 */
    if(p && n==32 && p[1]==0xd3){origin=be64(p+8);rx=be64(p+16);tx=be64(p+24);}
    taskENTER_CRITICAL();flow.sends++;flow.last_send_at=now;flow.fd=fd;
    flow.last_result=ret;flow.last_error=ret<0?error:0;
    if(ret<0)flow.send_error++;else if((size_t)ret!=n)flow.send_short++;
    else {flow.send_ok++;flow.last_ok_at=now;flow.wire_originate=origin;flow.wire_receive=rx;flow.wire_transmit=tx;}
    taskEXIT_CRITICAL();lwip_err=error;return ret;
}
static uint32_t age(uint32_t now,uint32_t at,uint32_t count)
{return count?(now-at)/1000U:UINT32_MAX;}
void carbox_screen_flow_report(uint32_t seq)
{
    carbox_screen_nal_report(seq);
    /* 快照与打印分开，禁止在临界区做UART I/O。年龄只用于短期卡顿（<71min）。 */
    __typeof__(flow) s;uint32_t now;
    /* 先取快照再取当前时间，避免任务抢占后事件时间晚于 now。 */
    taskENTER_CRITICAL();s=flow;now=hal_read_curtime_us();taskEXIT_CRITICAL();
    rt_printf("[SCREEN_FLOW][%lu] cumulative=1 ingress/forward/unforwarded=%lu/%lu/%lu first_sps=%lu/%lu/%lu first_idr=%lu/%lu/%lu config/video_clock_calls=%lu/%lu video_age_ms=%lu config_age_ms=%lu\r\n",
      (unsigned long)seq,(unsigned long)s.frames[0],(unsigned long)s.frames[1],(unsigned long)s.frames[2],
      (unsigned long)s.sps[0],(unsigned long)s.sps[1],(unsigned long)s.sps[2],
      (unsigned long)s.idr[0],(unsigned long)s.idr[1],(unsigned long)s.idr[2],
      (unsigned long)s.config.count,(unsigned long)s.video.count,
      (unsigned long)age(now,s.video.at,s.video.count),(unsigned long)age(now,s.config.at,s.config.count));
    rt_printf("[SCREEN_CLOCK][%lu] video_ntp_first/last=%llu/%llu config_ntp_last=%llu video_same/back=%lu/%lu starts=%lu start_us=%llu timing_rx/tx=%lu/%lu timing_rx/tx_us=%llu/%llu timing_tx_age_ms=%lu global_compensation=0\r\n",
      (unsigned long)seq,(unsigned long long)s.video.first,(unsigned long long)s.video.last,(unsigned long long)s.config.last,
      (unsigned long)s.video.same,(unsigned long)s.video.back,(unsigned long)s.start.count,(unsigned long long)s.start.last,
      (unsigned long)s.receive.count,(unsigned long)s.transmit.count,(unsigned long long)s.receive.last,(unsigned long long)s.transmit.last,
      (unsigned long)age(now,s.transmit.at,s.transmit.count));
    rt_printf("[SCREEN_TIMING_TX][%lu] fd=%d calls/ok/error/short=%lu/%lu/%lu/%lu last_ret/err=%d/%d attempt_age_ms=%lu ok_age_ms=%lu wire_orig/rx/tx_ntp=%llu/%llu/%llu socket_accept_only=1\r\n",
      (unsigned long)seq,s.fd,(unsigned long)s.sends,(unsigned long)s.send_ok,(unsigned long)s.send_error,(unsigned long)s.send_short,s.last_result,s.last_error,
      (unsigned long)age(now,s.last_send_at,s.sends),(unsigned long)age(now,s.last_ok_at,s.send_ok),
      (unsigned long long)s.wire_originate,(unsigned long long)s.wire_receive,(unsigned long long)s.wire_transmit);
}
