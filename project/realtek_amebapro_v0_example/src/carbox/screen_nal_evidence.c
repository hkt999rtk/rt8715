/* 只读 Annex B 参数观测。禁止修改 NAL、删除 loading.video 或改变发送状态。
 * stage=forward 仅表示进入客户 SendScreenFrame，不等于车机收到/解码成功。
 * 每块最多扫描4096字节、32个NAL；参数最多保存512字节。截断时不声称精确相等。
 * UART只在既有统计线程输出，热路径仅解析和复制到固定队列；无动态内存。
 */
#include "screen_nal_evidence.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "diag.h"
#include <string.h>
#define NAL_SCAN 4096U
#define NAL_RAW 512U
#define NAL_EVENTS 32U
#define NAL_IDRS 8U
struct parameter {unsigned length,valid;unsigned char raw[NAL_RAW];};
struct nal_event {
    uint32_t at,frame,hash;
    unsigned stage,type,length,copied,complete,valid;
    int sps_id,pps_id,profile,level,previous_equal,forward_equal;
    char task[20];
    unsigned char raw[NAL_RAW];
};
static struct {
    struct parameter previous[3][2],forward[2];
    struct nal_event queue[NAL_EVENTS];
    unsigned head,count,lost,scan_limited,idrs[3],frames[3];
} nal __attribute__((section(".lpddr.bss.screen_nal_evidence")));
/* 顺序读取RBSP，跨字节处理emulation_prevention_three_byte，UE最多31个前导0。
 * 仅解析SPS前三字节及ID、PPS两个ID、slice头三个UE，不依赖完整图像。 */
struct bits {const unsigned char *p;unsigned n,pos,zeros,left,byte;int bad;};
static unsigned bit(struct bits *b)
{
    if(!b->left){
        if(b->pos>=b->n){b->bad=1;return 0;}
        b->byte=b->p[b->pos++];
        if(b->zeros>=2 && b->byte==3){
            b->zeros=0;
            if(b->pos>=b->n){b->bad=1;return 0;}
            b->byte=b->p[b->pos++];
        }
        b->zeros=b->byte==0?b->zeros+1:0;b->left=8;
    }
    return (b->byte>>(--b->left))&1U;
}
static unsigned bits(struct bits *b,unsigned n)
{unsigned v=0;while(n--)v=(v<<1)|bit(b);return v;}
static int ue(struct bits *b)
{
    unsigned zeros=0,value;
    while(!bit(b) && !b->bad){if(++zeros>=31){b->bad=1;return -1;}}
    if(b->bad)return -1;
    value=((1U<<zeros)-1U)+bits(b,zeros);
    return b->bad?-1:(int)value;
}
static void parse(struct nal_event *e,const unsigned char *p,unsigned n)
{
    struct bits b={p+1,n>0?n-1:0,0,0,0,0,0};int dummy;
    e->sps_id=e->pps_id=e->profile=e->level=-1;
    if(e->type==7){e->profile=(int)bits(&b,8);bits(&b,8);e->level=(int)bits(&b,8);e->sps_id=ue(&b);}
    else if(e->type==8){e->pps_id=ue(&b);e->sps_id=ue(&b);}
    else {dummy=ue(&b);(void)dummy;dummy=ue(&b);(void)dummy;e->pps_id=ue(&b);}
    e->valid=!b.bad && !(p[0]&0x80) && e->sps_id<32 && e->pps_id<256;
}
static int equal(const struct parameter *p,const struct nal_event *e)
{
    if(!p->valid || !e->complete || e->length>NAL_RAW)return -1;
    return p->length==e->length && !memcmp(p->raw,e->raw,e->length);
}
static void remember(struct parameter *p,const struct nal_event *e)
{
    p->valid=e->complete && e->length<=NAL_RAW;
    p->length=e->length;
    if(p->valid)memcpy(p->raw,e->raw,e->length);
}
static void record(unsigned stage,uint32_t frame,const unsigned char *p,unsigned n,int complete)
{
    struct nal_event e;unsigned i,index,type=p[0]&31U;
    const char *task;
    if(type!=7 && type!=8 && type!=5)return;
    /* IDR只记录每个观测阶段前8个；参数集则持续记录，队列满明确计数。 */
    if(type==5){taskENTER_CRITICAL();i=nal.idrs[stage]++;taskEXIT_CRITICAL();if(i>=NAL_IDRS)return;}
    memset(&e,0,sizeof e);e.stage=stage;e.frame=frame;e.type=type;e.length=n;e.complete=complete;
    e.at=hal_read_curtime_us();e.previous_equal=e.forward_equal=-1;
    e.copied=n<NAL_RAW?n:NAL_RAW;
    /* IDR只保留头部，避免把画面payload写入日志。 */
    if(type==5 && e.copied>16)e.copied=16;
    memcpy(e.raw,p,e.copied);parse(&e,p,e.copied);
    e.hash=2166136261U;for(i=0;i<e.copied;i++)e.hash=(e.hash^e.raw[i])*16777619U;
    task=pcTaskGetName(xTaskGetCurrentTaskHandle());
    if(task){for(i=0;i<sizeof(e.task)-1 && task[i];i++)e.task[i]=task[i];}
    taskENTER_CRITICAL();
    if(type!=5){
        index=type-7;
        e.previous_equal=equal(&nal.previous[stage][index],&e);
        e.forward_equal=equal(&nal.forward[index],&e);
        remember(&nal.previous[stage][index],&e);
        if(stage==1)remember(&nal.forward[index],&e);
    }
    if(nal.count<NAL_EVENTS){nal.queue[(nal.head+nal.count)%NAL_EVENTS]=e;nal.count++;}
    else nal.lost++;
    taskEXIT_CRITICAL();
}
static unsigned startcode(const unsigned char *p,unsigned n,unsigned at,unsigned *prefix)
{
    unsigned i;
    for(i=at;i+2<n;i++)if(p[i]==0 && p[i+1]==0){
        if(p[i+2]==1){*prefix=3;return i;}
        if(i+3<n && p[i+2]==0 && p[i+3]==1){*prefix=4;return i;}
    }
    *prefix=0;return n;
}
void carbox_screen_nal_observe(unsigned stage,const void *data,size_t bytes)
{
    const unsigned char *p=data;unsigned n,at,prefix,next,next_prefix,count=0,frame;
    if(stage>=3 || !p || !bytes)return;
    n=bytes>NAL_SCAN?NAL_SCAN:(unsigned)bytes;
    taskENTER_CRITICAL();frame=++nal.frames[stage];taskEXIT_CRITICAL();
    at=startcode(p,n,0,&prefix);
    while(prefix && count++<32){
        unsigned begin=at+prefix,end;int complete;
        next=startcode(p,n,begin,&next_prefix);end=next;
        complete=next_prefix!=0 || bytes<=NAL_SCAN;
        /* Annex B trailing_zero_8bits不属于NAL，避免3/4-byte前缀差异误报。 */
        if(complete)while(end>begin && p[end-1]==0)end--;
        if(end>begin)record(stage,frame,p+begin,end-begin,complete);
        at=next;prefix=next_prefix;
    }
    if(bytes>NAL_SCAN || prefix){taskENTER_CRITICAL();nal.scan_limited++;taskEXIT_CRITICAL();}
}
void carbox_screen_nal_report(uint32_t seq)
{
    unsigned i,j,off,lost,limited,pending;struct nal_event e;char hex[129];
    static const char *names[]={"ingress","forward","unforwarded"};
    /* 每次最多8条，避免统计线程一次输出大量内容。输出时不持有临界区。 */
    for(i=0;i<8;i++){
        taskENTER_CRITICAL();
        if(!nal.count){taskEXIT_CRITICAL();break;}
        e=nal.queue[nal.head];nal.head=(nal.head+1)%NAL_EVENTS;nal.count--;
        taskEXIT_CRITICAL();
        rt_printf("[SCREEN_NAL][%lu] stage=%s task=%s stage_frame=%lu at_us=%lu type=%u bytes=%u complete=%u saved=%u hash=%08lx valid=%u sps_id=%d pps_id=%d profile=%d level=%d previous_equal=%d forward_equal=%d\r\n",
          (unsigned long)seq,names[e.stage],e.task,(unsigned long)e.frame,(unsigned long)e.at,e.type,e.length,e.complete,e.copied,(unsigned long)e.hash,e.valid,e.sps_id,e.pps_id,e.profile,e.level,e.previous_equal,e.forward_equal);
        /* 分行输出，避免原厂printf单行缓冲区截断参数字节。 */
        for(off=0;off<e.copied;off+=64){
            unsigned count=e.copied-off<64?e.copied-off:64;
            for(j=0;j<count;j++){hex[j*2]="0123456789abcdef"[e.raw[off+j]>>4];hex[j*2+1]="0123456789abcdef"[e.raw[off+j]&15];}
            hex[count*2]=0;
            rt_printf("[SCREEN_NAL_RAW][%lu] stage=%s stage_frame=%lu type=%u offset=%u hex=%s\r\n",
              (unsigned long)seq,names[e.stage],(unsigned long)e.frame,e.type,off,hex);
        }
    }
    taskENTER_CRITICAL();lost=nal.lost;limited=nal.scan_limited;pending=nal.count;taskEXIT_CRITICAL();
    rt_printf("[SCREEN_NAL_LIMIT][%lu] cumulative=1 lost=%u scan_limited=%u pending=%u scan_bytes=%u raw_bytes=%u idr_limit_per_stage=%u equal_unknown=-1 forward_is_entry=1\r\n",
      (unsigned long)seq,lost,limited,pending,NAL_SCAN,NAL_RAW,NAL_IDRS);
}
