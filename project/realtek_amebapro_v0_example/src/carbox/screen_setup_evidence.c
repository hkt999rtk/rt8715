/* 初始化/发送边界证据。原始 payload、CF 对象、返回值不变；不主动重试。
 * 固定 ring 由已有 2 秒统计线程打印，满时只丢观测并累计 lost。
 */
#include "screen_setup_evidence.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "diag.h"
#include <string.h>
#define EVENTS 24U
struct evidence_event {uint32_t time,kind,v[8];};
static struct evidence_event events[EVENTS];
static unsigned head,used;
static uint32_t lost,nal_frames,nal_multi,nal_bad,nal_prefix3,nal_max_us,nal_details;
static uint32_t nal_types;
static void record(unsigned kind,const uint32_t *v)
{
    taskENTER_CRITICAL();
    if(used<EVENTS) {
        struct evidence_event *e=&events[(head+used++)%EVENTS];
        e->time=hal_read_curtime_us();e->kind=kind;memcpy(e->v,v,sizeof(e->v));
    } else lost++;
    taskEXIT_CRITICAL();
}
void carbox_screen_session_event(unsigned action,unsigned generation,int fd,int result,unsigned state)
{
    uint32_t v[8]={action,generation,(uint32_t)fd,(uint32_t)result,state,0,0,0};
    record(1,v);
}
static unsigned prefix(const unsigned char *p,size_t n)
{
    if(n>=4 && p[0]==0 && p[1]==0 && p[2]==0 && p[3]==1) return 4;
    if(n>=3 && p[0]==0 && p[1]==0 && p[2]==1) return 3;
    return 0;
}
void screen_nal_scan(const void *data,size_t length,struct screen_nal_summary *out)
{
    const unsigned char *p=data;
    size_t pos=0,start=0;
    unsigned prev=0;
    memset(out,0,sizeof(*out));
    if(!p || !prefix(p,length)) {out->bad=1;return;}
    while(pos<length) {
        unsigned n=prefix(p+pos,length-pos);
        if(n) {
            if(prev) {
                size_t bytes=pos-start;
                if(!bytes) out->bad++;
                if(out->count==1) out->first_length=(uint32_t)bytes;
                if(out->count==2) out->second_length=(uint32_t)bytes;
            }
            out->count++;out->prefix3+=n==3;
            pos+=n;start=pos;prev=n;
            if(pos<length) {
                unsigned type=p[pos]&31U;
                out->types|=1U<<type;
                if((p[pos]&128U) || !type) out->bad++;
            }
        } else pos++;
    }
    if(prev) {
        size_t bytes=length-start;
        if(!bytes) out->bad++;
        if(out->count==1) out->first_length=(uint32_t)bytes;
        if(out->count==2) out->second_length=(uint32_t)bytes;
    }
}
extern void AirPlayScreen_EncryptData(void *,void *,int);
extern uint32_t carbox_move_current_frame_id(void);
void carbox_screen_encrypt_observe(void *header,void *data,int length)
{
    struct screen_nal_summary s;
    uint32_t start=hal_read_curtime_us(),elapsed,v[8];
    screen_nal_scan(data,length>0?(size_t)length:0,&s);
    elapsed=hal_read_curtime_us()-start;
    taskENTER_CRITICAL();
    nal_frames++;nal_multi+=s.count>1;nal_bad+=s.bad!=0;nal_prefix3+=s.prefix3!=0;nal_types|=s.types;
    if(elapsed>nal_max_us) nal_max_us=elapsed;
    if(nal_details<8 && (nal_frames<=2 || s.count>1 || s.bad || s.prefix3)) {
        nal_details++;
        v[0]=carbox_move_current_frame_id();v[1]=(uint32_t)length;v[2]=s.count;v[3]=s.prefix3;
        v[4]=s.first_length;v[5]=s.second_length;v[6]=s.types;v[7]=s.bad;
        record(6,v);
    }
    taskEXIT_CRITICAL();
    /* 只扫描，不修复/丢弃：先证实当前普通帧究竟包含几个 NAL。 */
    AirPlayScreen_EncryptData(header,data,length);
}
static int equal_ascii(const char *p,const char *s,size_t n)
{
    size_t i;for(i=0;i<n;i++) {
        char a=p[i],b=s[i];if(a>='A'&&a<='Z') a+=(char)32;
        if(b>='A'&&b<='Z') b+=(char)32;
        if(a!=b)return 0;
    }return 1;
}
void screen_info_http_fields(const char *h,size_t length,unsigned *status,unsigned *cseq)
{
    size_t i=0,j;unsigned value;
    *status=0;*cseq=UINT32_MAX;
    if(length>12 && (!memcmp(h,"HTTP/",5)||!memcmp(h,"RTSP/",5))) {
        for(j=0;j<length && j<16 && h[j]!=' ';j++);
        if(j+3<length && h[j+1]>='0'&&h[j+1]<='9' && h[j+2]>='0'&&h[j+2]<='9' && h[j+3]>='0'&&h[j+3]<='9')
            *status=(unsigned)(h[j+1]-'0')*100U+(unsigned)(h[j+2]-'0')*10U+(unsigned)(h[j+3]-'0');
    }
    while(i<length) {
        size_t end=i;
        while(end<length && h[end]!='\n') end++;
        if(end-i>=5 && equal_ascii(h+i,"cseq:",5)) {
            j=i+5;while(j<end && (h[j]==' '||h[j]=='\t'))j++;
            value=0;
            if(j<end && h[j]>='0'&&h[j]<='9') {
                for(;j<end && h[j]>='0'&&h[j]<='9';j++) {
                    unsigned digit=(unsigned)(h[j]-'0');
                    if(value>(UINT32_MAX-digit)/10U) return;
                    value=value*10U+digit;
                }
                *cseq=value;
            }
        }
        i=end+1;
    }
}
static uint32_t load32(const void *p){uint32_t v;memcpy(&v,p,4);return v;}
struct info_request {void *message;uint32_t id,cseq;};
static struct info_request requests[4];
static uint32_t next_request;
extern int HTTPClientSendMessage(void *,void *);
extern void carbox_vendor_info_complete(void *);
int carbox_info_send(void *client,void *message)
{
    unsigned char *m=message;
    unsigned i,status,cseq;uint32_t v[8]={0};
    /* 客户 HTTPMessage v17：header buffer/len=0x10/0x410，kind=0x8fc。
     * 仅 kind=5 (/info)；不记录认证 payload、密钥或其他 HTTP 消息。 */
    if(m && m[0x8fc]==5) {
        uint32_t n=load32(m+0x410);if(n>1024)n=1024;
        screen_info_http_fields((const char *)m+0x10,n,&status,&cseq);
        taskENTER_CRITICAL();
        for(i=0;i<4;i++) if(!requests[i].message || requests[i].message==message) break;
        if(i<4) {requests[i]=(struct info_request){message,++next_request,cseq};v[0]=requests[i].id;}
        else lost++;
        taskEXIT_CRITICAL();
        v[1]=cseq;v[2]=5;v[3]=n;record(2,v);
    }
    return HTTPClientSendMessage(client,message);
}
void carbox_info_complete(void *message)
{
    unsigned char *m=message;
    unsigned i,status,cseq;uint32_t v[8]={0};
    if(m && m[0x8fc]==5) {
        uint32_t n=load32(m+0x410);if(n>1024)n=1024;
        screen_info_http_fields((const char *)m+0x10,n,&status,&cseq);
        v[1]=status;v[2]=cseq;v[3]=load32(m+0x4a0);v[4]=UINT32_MAX;
        taskENTER_CRITICAL();
        for(i=0;i<4;i++) if(requests[i].message==message) {
            v[0]=requests[i].id;v[4]=requests[i].cseq;requests[i].message=NULL;break;
        }
        taskEXIT_CRITICAL();
        /* match=0 未知，1 匹配，2 不匹配；缺少 CSeq 不推断为匹配。 */
        if(v[0] && cseq!=UINT32_MAX && v[4]!=UINT32_MAX) v[5]=cseq==v[4]?1U:2U;
        record(3,v);
    }
    carbox_vendor_info_complete(message);
}
extern const void *CFBinaryPlistV0CreateWithData(const void *,size_t,int32_t *);
extern unsigned long CFGetTypeID(const void *);
extern const void *CFDictionaryGetValue(const void *,const void *);
extern const void *CFDictionaryGetTypedValue(const void *,const void *,unsigned long,int32_t *);
extern long CFArrayGetCount(const void *);
const void *carbox_info_plist(const void *data,size_t length,int32_t *error)
{
    int32_t local=0;
    const void *result=CFBinaryPlistV0CreateWithData(data,length,error?error:&local);
    uint32_t v[8]={result!=NULL,(uint32_t)(error?*error:local),result?(uint32_t)CFGetTypeID(result):0,(uint32_t)length,0,0,0,0};
    record(4,v);return result;
}
const void *carbox_info_hid_lookup(const void *dict,const void *key,unsigned long type,int32_t *error)
{
    int32_t local=0;
    const void *result=CFDictionaryGetTypedValue(dict,key,type,error?error:&local);
    const void *raw=dict?CFDictionaryGetValue(dict,key):NULL;
    uint32_t v[8]={raw!=NULL,raw?(uint32_t)CFGetTypeID(raw):0,(uint32_t)type,
        result?(uint32_t)CFArrayGetCount(result):0,(uint32_t)(error?*error:local),result!=NULL,0,0};
    record(5,v);return result;
}
void carbox_screen_setup_report(uint32_t sequence)
{
    struct evidence_event e;
    static unsigned announced;
    if(!announced) {
        announced=1;
#if CONFIG_SCREEN_SESSION_FIX
        rt_printf("[SCREEN_FIX] v17 session_fix=1 join_before_close=1 write_max=4096 poll_ms=250 packaging_changed=0 info_retry=0\r\n");
#else
        rt_printf("[SCREEN_FIX] v17 session_fix=0 evidence_only=1 vendor_lifecycle=1 packaging_changed=0 info_retry=0\r\n");
#endif
    }
    uint32_t frames,multi,bad,p3,types,max_us,dropped;unsigned i;
    taskENTER_CRITICAL();frames=nal_frames;multi=nal_multi;bad=nal_bad;p3=nal_prefix3;
    types=nal_types;max_us=nal_max_us;dropped=lost;taskEXIT_CRITICAL();
    if(frames || dropped) rt_printf("[SCREEN_NAL][%lu] cumulative=1 frames/multi/bad/prefix3=%lu/%lu/%lu/%lu types=0x%08lx scan_max_us=%lu lost=%lu packaging_changed=0\r\n",
        (unsigned long)sequence,(unsigned long)frames,(unsigned long)multi,(unsigned long)bad,(unsigned long)p3,(unsigned long)types,(unsigned long)max_us,(unsigned long)dropped);
    for(i=0;i<12;i++) {
        taskENTER_CRITICAL();if(!used){taskEXIT_CRITICAL();break;}
        e=events[head];head=(head+1)%EVENTS;used--;taskEXIT_CRITICAL();
        /* kind: 1=session(action/gen/fd/err/state), 2=info_req(id/CSeq/kind/header_len),
         * 3=info_reply(id/status/CSeq/body_len/request_CSeq/match),
         * 4=plist(ok/error/type/len), 5=hid(present/type/expected/count/error/ok),
         * 6=NAL(frame_id/len/count/prefix3/first_len/second_len/type_mask/bad)。 */
        {
        static const char *const labels[]={"?","SESSION action/gen/fd/error/state/reserved",
            "INFO_REQUEST id/cseq/kind/header_len/reserved", "INFO_REPLY id/status/cseq/body_len/request_cseq/match/reserved",
            "INFO_PLIST ok/error/type/len/reserved", "INFO_HID present/type/expected/count/error/ok/reserved",
            "NAL frame/len/count/prefix3/first_len/second_len/type_mask/bad"};
        rt_printf("[SCREEN_EVIDENCE] %s at_us=%lu v=%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu\r\n",
          labels[e.kind<=6?e.kind:0],(unsigned long)e.time,(unsigned long)e.v[0],(unsigned long)e.v[1],
          (unsigned long)e.v[2],(unsigned long)e.v[3],(unsigned long)e.v[4],(unsigned long)e.v[5],
          (unsigned long)e.v[6],(unsigned long)e.v[7]);
        }
    }
}
