/* 接收入口旁观统计：不读取 payload，不增加 socket read，不改变超时/重试。
 * read/recv 覆盖所有调用线程，以 fd/线程/API 区分；不能把所有行都算作车机。
 * CONNECT/SELECT 只观察 TCPClient（车机 event）。HTTP/TYPE 不依赖 HID 匹配。
 * 固定表不回收，full 非零表示覆盖不足；fd 重用可合并累计计数，ports 是报告
 * 当时快照，不是连接历史 ID。所有输出由现有统计线程执行。 */
#include "car_rx_diag.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "diag.h"
#include "lwip/sockets.h"
#include "lwip/errno.h"
#include <stdint.h>
#include <string.h>
#include <errno.h>
#define RX_ROWS 96
#define RX_NAME 24

typedef struct {
    TaskHandle_t task;
    int fd, api;
    char name[RX_NAME];
    uint32_t calls, done, positive, bytes, zero, again, errors;
    uint32_t started, active, max_us, last_us, hid, other, unknown;
    int last_result, last_error;
    char type[32];
    uint32_t dirty;
} rx_row;
static rx_row rows[RX_ROWS] __attribute__((section(".lpddr.bss.car_rx_diag")));
static uint32_t full, nested_recv;
static const char *const api_names[] = {"read", "recv", "connect", "select", "http", "type", "recvfrom"};
static void copy_name(char *out, const char *in, unsigned cap)
{
    unsigned i=0;
    if(in) for(; i+1<cap && in[i]; ++i) out[i]=in[i];
    out[i]=0;
}
/* 调用者已持锁。一个 task 内同步 read -> recv 不重复计数。 */
static int find_row(int api, int fd, TaskHandle_t task, const char *name)
{
    int i, empty=-1;
    for(i=0;i<RX_ROWS;i++) {
        if(rows[i].task==task && rows[i].fd==fd && rows[i].api==api) return i;
        if(!rows[i].task && empty<0) empty=i;
    }
    if(empty<0) { full++; return -1; }
    rows[empty].task=task; rows[empty].fd=fd; rows[empty].api=api;
    copy_name(rows[empty].name,name,RX_NAME);
    return empty;
}
int car_rx_begin(int api, int fd)
{
    TaskHandle_t task=xTaskGetCurrentTaskHandle();
    const char *name=pcTaskGetName(task);
    uint32_t now=hal_read_curtime_us();
    int i;
    taskENTER_CRITICAL();
    if(api==CAR_RX_RECV || api==CAR_RX_RECVFROM) for(i=0;i<RX_ROWS;i++)
        if(rows[i].task==task && rows[i].active &&
           (rows[i].api==CAR_RX_READ || (api==CAR_RX_RECVFROM && rows[i].api==CAR_RX_RECV))) {
            nested_recv++; taskEXIT_CRITICAL(); return -1;
        }
    i=find_row(api,fd,task,name);
    if(i>=0) { rows[i].calls++; rows[i].active=1; rows[i].started=now; rows[i].dirty=1; }
    taskEXIT_CRITICAL();
    return i;
}
void car_rx_end(int token, int result, int error)
{
    rx_row *r;
    uint32_t now=hal_read_curtime_us(), elapsed;
    if(token<0 || token>=RX_ROWS) return;
    taskENTER_CRITICAL();
    r=&rows[token]; elapsed=now-r->started;
    r->active=0; r->done++; r->dirty=1; r->last_us=now;
    r->last_result=result; r->last_error=error;
    if(elapsed>r->max_us) r->max_us=elapsed;
    if(r->api==CAR_RX_HTTP) {
        /* HTTPMessageReadMessage: 0=完整消息，正数 11/35=would-block。 */
        if(result==0) r->positive++;
        else if(result==EAGAIN || result==EWOULDBLOCK || result==35) r->again++;
        else r->errors++;
    } else if(result>0) {
        r->positive++;
        if(r->api==CAR_RX_READ || r->api==CAR_RX_RECV || r->api==CAR_RX_RECVFROM) r->bytes+=(uint32_t)result;
    } else if(result==0) r->zero++;
    else if(error==EAGAIN || error==EWOULDBLOCK || error==EINPROGRESS) r->again++;
    else r->errors++;
    taskEXIT_CRITICAL();
}
void car_rx_type(const char *type, uint32_t bytes, int result)
{
    TaskHandle_t task=xTaskGetCurrentTaskHandle();
    const char *name=pcTaskGetName(task);
    int i;
    taskENTER_CRITICAL();
    i=find_row(CAR_RX_TYPE,-1,task,name);
    if(i>=0) {
        rx_row *r=&rows[i];
        r->calls++; r->done++; r->bytes+=bytes; r->last_result=result;
        r->last_us=hal_read_curtime_us(); r->dirty=1;
        /* 客户 GetInfoType 成功返回 1，失败返回 0；失败时不可读输出字符串。 */
        if(result<=0 || !type || !type[0]) { r->unknown++; r->type[0]=0; }
        else {
            if(!strcmp(type,"hidSendReport")) r->hid++;
            else r->other++;
            copy_name(r->type,type,sizeof(r->type));
        }
    }
    taskEXIT_CRITICAL();
}
static int event_task(void)
{
    const char *n=pcTaskGetName(xTaskGetCurrentTaskHandle());
    return n && !strcmp(n,"TCPClient");
}
extern int __real_lwip_read(int, void *, size_t);
/* IPHONE_HTTP_RX_PROFILE 的已有 wrapper 在启用时负责相同 begin/end。 */
#if !CONFIG_IPHONE_HTTP_RX_PROFILE
int __wrap_lwip_read(int fd, void *buffer, size_t length)
{
    int t=car_rx_begin(CAR_RX_READ,fd);
    int result=__real_lwip_read(fd,buffer,length), error=lwip_err;
    car_rx_end(t,result,result<0?error:0); lwip_err=error;
    return result;
}
#endif
extern int __real_lwip_recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
int __wrap_lwip_recvfrom(int fd, void *buffer, size_t length, int flags,
                        struct sockaddr *from, socklen_t *fromlen)
{
    int t=car_rx_begin(CAR_RX_RECVFROM,fd);
    int result=__real_lwip_recvfrom(fd,buffer,length,flags,from,fromlen), error=lwip_err;
    car_rx_end(t,result,result<0?error:0); lwip_err=error;
    return result;
}
extern int __real_lwip_connect(int, const struct sockaddr *, socklen_t);
int __wrap_lwip_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    int t=event_task()?car_rx_begin(CAR_RX_CONNECT,fd):-1;
    int result=__real_lwip_connect(fd,addr,len), error=lwip_err;
    car_rx_end(t,result,result<0?error:0); lwip_err=error;
    return result;
}
extern int __real_lwip_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int __wrap_lwip_select(int count, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *timeout)
{
    int fd=-1, t=-1, result, error, i;
    if(event_task()) {
        /* 单 fd event select；多 fd 不猜测归属，用 -1 表示。 */
        for(i=0;i<count && i<FD_SETSIZE;i++) if(rd && FD_ISSET(i,rd)) {
            if(fd>=0) { fd=-1; break; } fd=i;
        }
        t=car_rx_begin(CAR_RX_SELECT,fd);
    }
    result=__real_lwip_select(count,rd,wr,ex,timeout); error=lwip_err;
    car_rx_end(t,result,result<0?error:0); lwip_err=error;
    return result;
}
void car_rx_report(uint32_t sequence)
{
    rx_row r;
    unsigned i, used=0;
    uint32_t lost, nested, now=hal_read_curtime_us();
    for(i=0;i<RX_ROWS;i++) {
        taskENTER_CRITICAL(); r=rows[i]; rows[i].dirty=0; taskEXIT_CRITICAL();
        now=hal_read_curtime_us();
        if(!r.task) continue;
        used++;
        /* 保留 TCPClient 心跳，区分等待 select 和未启动；其他空闲行不刷屏。 */
        if(!r.dirty && !r.active && strcmp(r.name,"TCPClient")) continue;
        if(r.api==CAR_RX_TYPE) {
            rt_printf("[CARRX_TYPE][%lu] task=%s calls/body_bytes=%lu/%lu hid/other/unknown=%lu/%lu/%lu last_ret=%d last_type=%s\r\n",
                (unsigned long)sequence,r.name,(unsigned long)r.calls,(unsigned long)r.bytes,
                (unsigned long)r.hid,(unsigned long)r.other,(unsigned long)r.unknown,r.last_result,r.type);
        } else {
            struct sockaddr_storage local, peer;
            socklen_t len=sizeof(local);
            int local_ok=-1, peer_ok=-1, family=0;
            unsigned lp=0, pp=0;
            if(r.fd>=0) {
                local_ok=lwip_getsockname(r.fd,(struct sockaddr *)&local,&len);
                len=sizeof(peer); peer_ok=lwip_getpeername(r.fd,(struct sockaddr *)&peer,&len);
                if(local_ok==0) { family=local.ss_family;
                    if(family==AF_INET) lp=ntohs(((struct sockaddr_in *)&local)->sin_port);
                    else if(family==AF_INET6) lp=ntohs(((struct sockaddr_in6 *)&local)->sin6_port);
                }
                if(peer_ok==0) {
                    if(peer.ss_family==AF_INET) pp=ntohs(((struct sockaddr_in *)&peer)->sin_port);
                    else if(peer.ss_family==AF_INET6) pp=ntohs(((struct sockaddr_in6 *)&peer)->sin6_port);
                }
            }
            rt_printf("[CARRX][%lu] task=%s fd=%d api=%s ports=%u/%u af=%d addr_ret=%d/%d calls/done/data/bytes=%lu/%lu/%lu/%lu zero/again/error=%lu/%lu/%lu active/age_us=%lu/%lu max_us=%lu last_ret/errno=%d/%d last_age_us=%lu\r\n",
                (unsigned long)sequence,r.name,r.fd,api_names[r.api],lp,pp,family,local_ok,peer_ok,
                (unsigned long)r.calls,(unsigned long)r.done,(unsigned long)r.positive,(unsigned long)r.bytes,
                (unsigned long)r.zero,(unsigned long)r.again,(unsigned long)r.errors,
                (unsigned long)r.active,(unsigned long)(r.active?now-r.started:0),
                (unsigned long)r.max_us,r.last_result,r.last_error,(unsigned long)(r.done?now-r.last_us:0));
        }
    }
    taskENTER_CRITICAL(); lost=full; nested=nested_recv; taskEXIT_CRITICAL();
    rt_printf("[CARRX_SUM][%lu] cumulative=1 rows=%u/%u full=%lu nested_recv_suppressed=%lu\r\n",
        (unsigned long)sequence,used,RX_ROWS,(unsigned long)lost,(unsigned long)nested);
}
