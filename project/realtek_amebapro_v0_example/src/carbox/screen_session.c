/* 客户 v17 发送器的生命周期补全。只用于经过校验的派生 archive。
 * 原始加密、4KB 提交、250ms poll 和 TCP RTO 策略保持不变。
 * 旧 sender 已退出且 join 完成后才关闭 fd，避免 fd 重用导致跨 session 写入。
 */
#include "screen_session.h"
#include "screen_setup_evidence.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "lwip/errno.h"
#include <errno.h>
#include <stdlib.h>
extern volatile unsigned char carbox_screen_sending;
extern int carbox_screen_sock;
extern void *carbox_screen_frames;
extern unsigned char carbox_screen_mutex[];
extern int lwip_err;
/* pthread 对象由 SDK 管理。这里只透传指针，不复制其私有结构。
 * v17 的 pthread_t 为指针，detached=0、joinable=1（与 Linux 数值不同）。 */
extern int pthread_create(void **,const void *,void *(*)(void *),void *);
extern int pthread_attr_setdetachstate(void *,int);
extern int pthread_join(void *,void **);
extern void *pthread_self(void);
extern void pthread_exit(void *);
extern int pthread_mutex_lock(void *),pthread_mutex_unlock(void *);
extern int CVector_size(void *);
extern void CVector_push_back(void *,const void *);
extern void carbox_vendor_screen_start(int,int);
extern void AirPlayScreen_DestroyScreenSockConnect(void);
extern void AirPlayClient_SendMessageTearDown(int);
extern void *GetAirPlayInfo(void);
#if CONFIG_MOVE_LAG_PROFILE
extern void carbox_move_screen_free(void *);
#define release_frame carbox_move_screen_free
#else
#define release_frame free
#endif
static StaticSemaphore_t lifecycle_storage;
static SemaphoreHandle_t lifecycle;
static void *sender;
static volatile unsigned finished;
static volatile uint32_t generation;
static void lock_lifecycle(void)
{
    taskENTER_CRITICAL();
    if(!lifecycle) lifecycle=xSemaphoreCreateMutexStatic(&lifecycle_storage);
    taskEXIT_CRITICAL();
    xSemaphoreTake(lifecycle,portMAX_DELAY);
}
static void event(unsigned action,int result)
{
    carbox_screen_session_event(action,generation,carbox_screen_sock,result,
        (carbox_screen_sending?1U:0U)|(sender?2U:0U)|(finished?4U:0U));
}
static int fail_write(int error)
{
    carbox_screen_sending=0;
    event(5,error);
    lwip_err=error;
    return -1;
}
int carbox_screen_write(int fd,const void *data,size_t length)
{
    int n,error;
    if(!carbox_screen_sending || fd<0 || fd!=carbox_screen_sock) {
        lwip_err=ECANCELED;return -1;
    }
    n=lwip_write(fd,data,length);
    error=n<0?lwip_errno():0;
    if(n==0 && length) return fail_write(EPIPE);
    if(n<0 && error!=EAGAIN && error!=EWOULDBLOCK && error!=EINTR)
        return fail_write(error);
    /* 客户 normal-frame 循环不识别 EINTR，映射成原有的可重试分支。 */
    if(n<0 && error==EINTR) lwip_err=EAGAIN;
    return n;
}
int carbox_screen_poll(struct pollfd *fds,unsigned count,int timeout)
{
    int result,error;
    if(!carbox_screen_sending) {lwip_err=ECANCELED;return -1;}
    result=lwip_poll(fds,count,timeout);
    error=result<0?lwip_errno():0;
    if((result<0 && error!=EINTR) ||
       (result>0 && count && (fds[0].revents&(POLLERR|POLLHUP|POLLNVAL))))
        fail_write(error?error:EPIPE);
    return result;
}
int carbox_screen_detach(void *attr,int state)
{
    (void)state;
    return pthread_attr_setdetachstate(attr,1);
}
int carbox_screen_create(void **thread,const void *attr,void *(*entry)(void *),void *arg)
{
    int result;
    finished=0;
    if(carbox_screen_sock<0) {carbox_screen_sending=0;return ENOTCONN;}
    result=pthread_create(thread,attr,entry,arg);
    if(result) carbox_screen_sending=0;
    else sender=*thread;
    event(2,result);
    return result;
}
void carbox_screen_thread_exit(void *result)
{
    /* 客户线程已释放队列、打印 exit；join 仍会等待 pthread 的真正退出。 */
    finished=1;
    event(6,0);
    pthread_exit(result);
}
static int stop_locked(void)
{
    unsigned i;
    int result,had_resources=(sender!=NULL || carbox_screen_sock>=0 || carbox_screen_frames!=NULL);
    carbox_screen_sending=0;
    if(!had_resources) return 1;
    event(3,0);
    /* 原生 FreeRTOS 任务的 pthread_self() 可以为 NULL，不能把无 sender
     * 的创建失败清理误判成线程等待自己。 */
    if(sender && sender==pthread_self()) {event(7,EDEADLK);return 0;}
    if(sender) {
        /* 正常停止最迟在 250ms poll 后被 write 察觉。保护性等待有上限，
         * 超时只 shutdown 旧 socket；不能 close 后让新 session 复用 fd。 */
        for(i=0;i<200 && !finished;i++) vTaskDelay(pdMS_TO_TICKS(10));
        if(!finished && carbox_screen_sock>=0) lwip_shutdown(carbox_screen_sock,SHUT_RDWR);
        for(i=0;i<200 && !finished;i++) vTaskDelay(pdMS_TO_TICKS(10));
        if(!finished) {event(7,ETIMEDOUT);return 0;}
        result=pthread_join(sender,NULL);
        if(result) {event(7,result);return 0;}
        sender=NULL;
    }
    if(carbox_screen_frames) {event(7,EBUSY);return 0;}
    if(carbox_screen_sock>=0) AirPlayScreen_DestroyScreenSockConnect();
    event(4,0);
    /* 此字段偏移由 patch 工具固定 v17 StopVideo 指令校验；不能套到未知 lib。
     * 控制通道已断开仍完成本地清理，仅跳过无法发送的 TearDown。 */
    if(((const unsigned char *)GetAirPlayInfo())[0x6c4])
        AirPlayClient_SendMessageTearDown(110);
    return 1;
}
void AirPlayScreen_StopVideo(void)
{
    lock_lifecycle();stop_locked();xSemaphoreGive(lifecycle);
}
void AirPlayScreen_StartVideo(int width,int height)
{
    lock_lifecycle();
    if(!carbox_screen_sending && stop_locked()) {
        ++generation;event(1,0);
        carbox_vendor_screen_start(width,height);
        if(!carbox_screen_sending) stop_locked();
    }
    xSemaphoreGive(lifecycle);
}
void AirPlayScreen_SendScreenData(char *data,int length)
{
    int done=0;
    while(done<length && carbox_screen_sending) {
        int n=carbox_screen_write(carbox_screen_sock,data+done,
                                 (size_t)(length-done>4096?4096:length-done));
        if(n>0) {done+=n;continue;}
        if(n<0 && (lwip_errno()==EAGAIN || lwip_errno()==EWOULDBLOCK)) {
            struct pollfd p={carbox_screen_sock,POLLOUT,0};
            carbox_screen_poll(&p,1,250);continue;
        }
        break;
    }
}
void AirPlayScreen_SendVideo(uint8_t *data,int length)
{
    struct {uint8_t *data;int length;} frame;
    uint32_t mine=generation;
    if(!data || length<=0) {release_frame(data);return;}
    for(;;) {
        pthread_mutex_lock(carbox_screen_mutex);
        if(!carbox_screen_sending || mine!=generation || !carbox_screen_frames) {
            pthread_mutex_unlock(carbox_screen_mutex);release_frame(data);return;
        }
        if(CVector_size(carbox_screen_frames)>0) {
            pthread_mutex_unlock(carbox_screen_mutex);vTaskDelay(pdMS_TO_TICKS(1));continue;
        }
        frame.data=data;frame.length=length;
        CVector_push_back(carbox_screen_frames,&frame);
        pthread_mutex_unlock(carbox_screen_mutex);return;
    }
}
