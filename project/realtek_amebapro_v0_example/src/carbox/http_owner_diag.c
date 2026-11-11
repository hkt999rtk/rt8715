/* HTTP 归属观测，简体中文说明：
 * 只旁观已返回的明文字节，不额外读 socket、不改变回覆、调度或超时。
 * 同一连接共用一个流解析器，处理分段、合并、Content-Length 和 chunked。
 * 请求按首次调用 writer 的顺序关联（不是入队顺序）；CSeq 存在时交叉检查。
 * 原厂 read 可能预读多个 response：另存完成 FIFO，在原厂 parser 返回成功时
 * 才核对真正交付的 request。仅“读到”与“解析交付”是不同证据。
 * 有界表溢出、未知 framing、连接失去同步时停止可信配对，不猜测丢包。
 * 不打印 body、header 原文、URL 参数；只打印方法/路径分类和数字。
 */
#include "http_owner_diag.h"
#if defined(CONFIG_MOVE_LAG_PROFILE) && CONFIG_MOVE_LAG_PROFILE
#include "FreeRTOS.h"
#include "task.h"
#include "hal_timer.h"
#include "diag.h"
#include <string.h>
#include <stddef.h>
#include <limits.h>
#define CONNS 16
#define REQUESTS 128
#define EVENTS 256
#define DELIVERIES 64
#define LINE 192
/* 当前 v17 ABI，与 touch_path_profiler 的已验证偏移相同。 */
#define HEADER 0x10U
#define HEADER_LEN 0x410U
#define BODY_LEN 0x4a0U
#define NOSEQ UINT32_MAX
#define BP 1U
#define VENDOR 2U

enum { ENQUEUE=1, WRITE_BEGIN, WRITE_END, READ_BYTES, RESPONSE, DELIVER,
       GUARD, INVALID, CLOSE_CONN, SEND_ERROR };
enum { HEAD=0, BODY, CHUNK_SIZE, CHUNK_BODY, CHUNK_CR, CHUNK_LF, TRAILER };
/* flags: 1=读者/发送路径不同(仅嫌疑), 2=CSeq不同, 4=实际交付错配,
 * 8=混合读取, 16=失去同步, 32=没有已发送请求, 64=无CSeq(按FIFO推定)。 */
typedef struct { uint32_t id, cseq, flags; } delivery;
typedef struct {
    unsigned state, used, lines, header_bytes, owners, status, chunked, has_len;
    uint32_t remain, cseq, reader_id;
    char line[LINE];
} parser;
typedef struct {
    void *client, *context;
    uint32_t id, order, reads, bytes, responses, guards, queued, written;
    unsigned active, hid, invalid, dq_head, dq_count;
    parser p;
    delivery dq[DELIVERIES];
} connection;
typedef struct {
    void *message;
    uint32_t id, conn, order, cseq, bytes, caller;
    unsigned path, method, target, sent, answered;
    char command[32];
} request;
typedef struct {
    uint32_t time, conn, id, peer, a, b, flags;
    int32_t result;
    unsigned kind, path;
    char command[32];
} event;
static connection cs[CONNS] __attribute__((section(".lpddr.bss.http_owner")));
static request rs[REQUESTS] __attribute__((section(".lpddr.bss.http_owner")));
static event es[EVENTS] __attribute__((section(".lpddr.bss.http_owner")));
static unsigned er, en;
/* 正常请求只汇总并保存最近 32 个事件。交叠/异常时复制历史，再捕获后续 64 个，
 * 避免每次触摸打印五行而改变时序。事件表满只丢观测，lost 明确累计。 */
static event history[32] __attribute__((section(".lpddr.bss.http_owner")));
static unsigned hr, hn, capture_left;
static uint32_t captures;
static uint32_t next_conn, next_req, lost, full, parse_errors;
static uint32_t suspect, mismatch, deliveries, guard_total;
static const char *const names[] = { "?", "enqueue", "write_begin", "write_end",
    "read", "response", "deliver", "guard", "invalid", "close", "send_error" };
extern int32_t __real_HTTPMessageReadMessage(void *, void *, void *);
extern void __real_HTTPClientInvalidate(void *);
static uint32_t load_u32(const void *p) { uint32_t n; memcpy(&n,p,4); return n; }
static void reset_parser(parser *p) { memset(p,0,sizeof(*p)); p->cseq=NOSEQ; }
static connection *by_id(uint32_t id) {
    unsigned i; for(i=0;i<CONNS;i++) if(cs[i].active && cs[i].id==id) return &cs[i];
    return NULL;
}
static connection *by_client(void *client) {
    unsigned i; for(i=0;i<CONNS;i++) if(cs[i].active && cs[i].client==client) return &cs[i];
    return NULL;
}
static request *by_message(void *m) {
    unsigned i; for(i=0;i<REQUESTS;i++) if(rs[i].id && rs[i].message==m) return &rs[i];
    return NULL;
}
static void push_event(const event *e) {
    if(en==EVENTS) { lost++; return; }
    es[(er+en++)%EVENTS]=*e;
}
static void emit(connection *c, unsigned kind, unsigned path, uint32_t id,
                 uint32_t peer, uint32_t a, uint32_t b, int32_t result, uint32_t flags) {
    event e; unsigned i, trigger;
    memset(&e,0,sizeof(e));
    if(!c || !c->hid) return;
    e.time=hal_read_curtime_us(); e.conn=c->id; e.kind=kind; e.path=path;
    e.id=id; e.peer=peer; e.a=a; e.b=b; e.result=result; e.flags=flags;
    if(kind==ENQUEUE) for(i=0;i<REQUESTS;i++) if(rs[i].id==id) {
        memcpy(e.command,rs[i].command,sizeof(e.command)); break;
    }
    trigger=(kind==ENQUEUE && path==VENDOR) || kind==INVALID ||
            ((kind==RESPONSE || kind==DELIVER) && (flags & 15U));
    if(trigger && !capture_left) {
        captures++;
        for(i=0;i<hn;i++) push_event(&history[(hr+i)%32]);
        capture_left=64;
    }
    if(capture_left) { push_event(&e); capture_left--; }
    if(hn==32) { hr=(hr+1)%32; hn--; }
    history[(hr+hn++)%32]=e;
}
static void invalid(connection *c, unsigned reason) {
    if(!c->invalid) { c->invalid=reason; parse_errors++;
        emit(c,INVALID,0,0,0,reason,c->p.state,0,16); }
}
static int prefix(const char *s, const char *p) {
    while(*p) { unsigned a=(unsigned char)*s++, b=(unsigned char)*p++;
        if(a>='A' && a<='Z') a+=32;
        if(b>='A' && b<='Z') b+=32;
        if(a!=b) return 0;
    } return 1;
}
static int number(const char *s, unsigned base, uint32_t *out) {
    uint32_t n=0; unsigned digits=0,d;
    while(*s==' ' || *s=='\t') s++;
    while(*s) {
        if(*s>='0' && *s<='9') d=(unsigned)(*s-'0');
        else if(base==16 && *s>='a' && *s<='f') d=(unsigned)(*s-'a'+10);
        else if(base==16 && *s>='A' && *s<='F') d=(unsigned)(*s-'A'+10);
        else break;
        if(d>=base || n>(UINT32_MAX-d)/base) return 0;
        n=n*base+d; digits++; s++;
    }
    while(*s==' ' || *s=='\t') s++;
    if(!digits || (*s && !(base==16 && *s==';'))) return 0;
    *out=n; return 1;
}
/* 只读取 bplist00 顶层字典的 ASCII "type"，不反序列化/分配其他数据。
 * 所有 offset/ref 均校验范围；不支持的编码返回 unknown，不影响原业务。 */
static int be32(const unsigned char *s,unsigned n,uint32_t *v) {
    uint32_t r=0; unsigned i;
    if(!n || n>8) return 0;
    for(i=0;i<n;i++) { if(r>0xffffffU) return 0; r=(r<<8)|s[i]; }
    *v=r; return 1;
}
typedef struct { const unsigned char *b; uint32_t table,count; unsigned os,refs; } plist;
static int object(plist *p,uint32_t ref,uint32_t *off) {
    if(ref>=p->count || !be32(p->b+p->table+ref*p->os,p->os,off)) return 0;
    return *off>=8 && *off<p->table;
}
static int objlen(plist *p,uint32_t *off,unsigned marker,uint32_t *n) {
    unsigned width;
    *n=marker&15; (*off)++;
    if(*n!=15) return 1;
    if(*off>=p->table || (p->b[*off]>>4)!=1 || (p->b[*off]&15)>3) return 0;
    width=1U<<(p->b[(*off)++]&15);
    if(width>p->table-*off || !be32(p->b+*off,width,n)) return 0;
    *off+=width; return 1;
}
static void command_type(const unsigned char *b,uint32_t len,char out[32]) {
    plist p; uint32_t top,off,n,i,k,v,ko,vo,kn,vn,j; unsigned marker;
    strcpy(out,"unknown");
    if(!b || len<40 || len>65536 || memcmp(b,"bplist00",8)) return;
    p.b=b; p.os=b[len-26]; p.refs=b[len-25];
    if(!p.os || p.os>8 || !p.refs || p.refs>8 ||
       !be32(b+len-24,8,&p.count) || !be32(b+len-16,8,&top) ||
       !be32(b+len-8,8,&p.table) || p.table<8 || p.table>len-32 ||
       p.count>(len-32-p.table)/p.os || !object(&p,top,&off)) return;
    marker=b[off];
    if((marker>>4)!=13 || !objlen(&p,&off,marker,&n) || n>64 ||
       n>(p.table-off)/(2*p.refs)) return;
    for(i=0;i<n;i++) {
        if(!be32(b+off+i*p.refs,p.refs,&k) || !object(&p,k,&ko)) return;
        marker=b[ko];
        if((marker>>4)!=5 || !objlen(&p,&ko,marker,&kn) || kn>p.table-ko) return;
        if(kn!=4 || memcmp(b+ko,"type",4)) continue;
        if(!be32(b+off+(n+i)*p.refs,p.refs,&v) || !object(&p,v,&vo)) return;
        marker=b[vo];
        if((marker>>4)!=5 || !objlen(&p,&vo,marker,&vn) || !vn || vn>=32 || vn>p.table-vo) return;
        for(j=0;j<vn;j++) {
            unsigned ch=b[vo+j];
            if(!((ch>='a'&&ch<='z') || (ch>='A'&&ch<='Z') ||
                 (ch>='0'&&ch<='9') || ch=='_' || ch=='-')) return;
        }
        memcpy(out,b+vo,vn); out[vn]=0; return;
    }
}
/* 仅分类，不保留 URL、认证信息或 body。 */
static void request_header(request *r, void *m) {
    const char *s=(const char *)m+HEADER; uint32_t n=load_u32((char *)m+HEADER_LEN),i=0;
    char line[LINE]; unsigned j;
    r->cseq=NOSEQ; r->bytes=load_u32((char *)m+BODY_LEN);
    if(n>1024) return;
    while(i<n) {
        j=0;
        while(i<n && s[i]!='\n') { if(j+1<LINE && s[i]!='\r') line[j++]=s[i]; i++; }
        if(i<n) i++;
        line[j]=0;
        if(prefix(line,"CSeq:")) { uint32_t v; if(number(line+5,10,&v)) r->cseq=v; }
        if(!r->method) {
            const char *p=strchr(line,' ');
            r->method=prefix(line,"POST ")?1:prefix(line,"GET ")?2:prefix(line,"PUT ")?3:4;
            if(p) r->target=prefix(p+1,"/command ")?1:prefix(p+1,"/event ")?2:3;
        }
    }
}
void http_owner_enqueue(void *client, void *message, int bypass, uintptr_t caller) {
    connection *c; request *r=NULL; unsigned i;
    taskENTER_CRITICAL();
    c=by_client(client);
    if(!c) for(i=0;i<CONNS;i++) if(!cs[i].active) {
        c=&cs[i]; memset(c,0,sizeof(*c)); c->active=1; c->id=++next_conn;
        c->client=client; reset_parser(&c->p); break;
    }
    if(!c) { full++; taskEXIT_CRITICAL(); return; }
    if(bypass) c->hid=1;
    if(by_message(message)) { invalid(c,1); taskEXIT_CRITICAL(); return; }
    for(i=0;i<REQUESTS;i++) if(!rs[i].id) { r=&rs[i]; break; }
    if(!r) { full++; invalid(c,2); taskEXIT_CRITICAL(); return; }
    memset(r,0,sizeof(*r)); r->id=++next_req; r->message=message; r->conn=c->id;
    c->queued++;
    r->path=bypass?BP:VENDOR; r->caller=(uint32_t)caller; request_header(r,message);
    {
        const unsigned char *body;
        memcpy(&body,(char *)message+0x49cU,sizeof(body));
        command_type(body,r->bytes,r->command);
    }
    emit(c,ENQUEUE,r->path,r->id,r->caller,r->cseq,
         (r->method<<16)|r->target,(int32_t)r->bytes,c->invalid?16:0);
    taskEXIT_CRITICAL();
}
void http_owner_enqueue_result(void *message,int32_t result) {
    request *r; connection *c;
    if(!result) return;
    taskENTER_CRITICAL(); r=by_message(message);
    if(r) { c=by_id(r->conn); emit(c,SEND_ERROR,r->path,r->id,0,0,0,result,0);
        if(c && r->order) invalid(c,3);
        memset(r,0,sizeof(*r)); }
    taskEXIT_CRITICAL();
}
void http_owner_write_begin(void *message,void *context) {
    request *r; connection *c;
    taskENTER_CRITICAL(); r=by_message(message); c=r?by_id(r->conn):NULL;
    if(c && !r->order) {
        if(c->context && c->context!=context) invalid(c,4);
        c->context=context;
        {
            unsigned i;
            for(i=0;i<REQUESTS;i++) if(rs[i].id && rs[i].conn==c->id &&
                rs[i].order && !rs[i].sent && !rs[i].answered) {
                invalid(c,23); break; /* 尚未完整写完的请求被另一请求穿插，顺序不可信。 */
            }
        }
        r->order=++c->order;
        /* 原厂可能在入队后才生成 header，首次 write 再读取 CSeq。 */
        request_header(r,message);
        emit(c,WRITE_BEGIN,r->path,r->id,0,r->cseq,r->order,0,0);
    } taskEXIT_CRITICAL();
}
void http_owner_write_end(void *message,int32_t result) {
    request *r; connection *c;
    taskENTER_CRITICAL(); r=by_message(message); c=r?by_id(r->conn):NULL;
    if(c) {
        emit(c,WRITE_END,r->path,r->id,0,r->cseq,r->order,result,0);
        if(result==0) { if(!r->sent) c->written++; r->sent=1; if(r->path==BP) r->message=NULL; }
        else if(result!=11) invalid(c,5);
    } taskEXIT_CRITICAL();
}
static request *oldest(connection *c) {
    request *r=NULL; unsigned i;
    for(i=0;i<REQUESTS;i++) if(rs[i].id && rs[i].conn==c->id && rs[i].order && !rs[i].answered)
        if(!r || rs[i].order<r->order) r=&rs[i];
    return r;
}
static void response(connection *c) {
    parser *p=&c->p; request *r=oldest(c); uint32_t f=0, id=r?r->id:0;
    if(p->status<200) { /* 1xx 不消耗请求；101 是协议切换，停止推断。 */
        emit(c,RESPONSE,p->owners,id,p->reader_id,p->cseq,0,(int32_t)p->status,0);
        if(p->status==101) invalid(c,6);
        reset_parser(p); return;
    }
    if(!r) f|=32;
    else {
        if(p->owners!=r->path) { f|=1; suspect++; }
        if(p->cseq!=NOSEQ && r->cseq!=NOSEQ) {
            if(p->cseq!=r->cseq) { f|=2; mismatch++; }
        } else f|=64;
    }
    if(p->owners==3) f|=8;
    emit(c,RESPONSE,p->owners,id,p->reader_id,p->cseq,r?r->cseq:NOSEQ,(int32_t)p->status,f);
    c->responses++;
    if(p->owners==VENDOR) {
        if(c->dq_count==DELIVERIES) invalid(c,7);
        else { delivery *d=&c->dq[(c->dq_head+c->dq_count++)%DELIVERIES];
            d->id=id; d->cseq=p->cseq; d->flags=f; }
    } else if(p->owners==3) invalid(c,8);
    if(r) { r->answered=1; if(r->path==BP) memset(r,0,sizeof(*r)); }
    if(!id) invalid(c,9);
    reset_parser(p);
}
static void line_done(connection *c) {
    parser *p=&c->p; uint32_t v; char *s=p->line;
    if(p->state==CHUNK_SIZE) {
        if(!number(s,16,&v)) { invalid(c,10); return; }
        p->remain=v; p->state=v?CHUNK_BODY:TRAILER; return;
    }
    if(p->state==TRAILER) { if(!*s) response(c); return; }
    if(p->lines++==0) {
        if((!prefix(s,"HTTP/1.1 ") && !prefix(s,"HTTP/1.0 ")) || strlen(s)<12 ||
           s[9]<'1' || s[9]>'5' || s[10]<'0' || s[10]>'9' || s[11]<'0' || s[11]>'9') {
            invalid(c,11); return;
        }
        p->status=(unsigned)(s[9]-'0')*100+(unsigned)(s[10]-'0')*10+(unsigned)(s[11]-'0');
    } else if(!*s) {
        if(p->status<200 || p->status==204 || p->status==304) response(c);
        else if(p->chunked) { if(p->has_len) invalid(c,12); else p->state=CHUNK_SIZE; }
        else if(p->has_len) { if(p->remain) p->state=BODY; else response(c); }
        else invalid(c,13); /* close-delimited 不猜测结束位置。 */
    } else if(prefix(s,"Content-Length:")) {
        if(!number(s+15,10,&v) || (p->has_len && p->remain!=v)) invalid(c,14);
        else { p->remain=v; p->has_len=1; }
    } else if(prefix(s,"CSeq:")) {
        if(!number(s+5,10,&v) || (p->cseq!=NOSEQ && p->cseq!=v)) invalid(c,15);
        else p->cseq=v;
    } else if(prefix(s,"Transfer-Encoding:")) {
        s+=18; while(*s==' ' || *s=='\t') s++;
        if(prefix(s,"chunked") && strlen(s)==7) p->chunked=1;
        else invalid(c,16);
    }
}
static void feed(connection *c,const void *data,uint32_t len,unsigned owner,uint32_t reader) {
    const unsigned char *s=data; parser *p=&c->p; uint32_t n;
    while(len && !c->invalid) {
        p->owners|=owner;
        if(!p->reader_id) p->reader_id=reader;
        if(p->state==BODY || p->state==CHUNK_BODY) {
            n=len<p->remain?len:p->remain; p->remain-=n; s+=n; len-=n;
            if(!p->remain) { if(p->state==BODY) response(c); else p->state=CHUNK_CR; }
            continue;
        }
        if(p->state==CHUNK_CR || p->state==CHUNK_LF) {
            if(*s++ != (p->state==CHUNK_CR?'\r':'\n')) { invalid(c,17); return; }
            len--; p->state=p->state==CHUNK_CR?CHUNK_LF:CHUNK_SIZE; continue;
        }
        if(++p->header_bytes>8192) { invalid(c,18); return; }
        if(*s=='\n') {
            if(!p->used || p->line[p->used-1]!='\r') { invalid(c,19); return; }
            p->line[p->used-1]=0; line_done(c); p->used=0;
        } else {
            if(p->used+1>=LINE) { invalid(c,20); return; }
            p->line[p->used++]=(char)*s;
        }
        s++; len--;
    }
}
void http_owner_drain(void *client,const void *data,uint32_t len,int32_t result) {
    connection *c;
    taskENTER_CRITICAL(); c=by_client(client);
    if(c) { c->reads++; c->bytes+=len;
        emit(c,READ_BYTES,BP,0,0,len,0,result,c->invalid?16:0);
        if(len) feed(c,data,len,BP,0);
    } taskEXIT_CRITICAL();
}
void http_owner_guard(void *client) {
    connection *c; taskENTER_CRITICAL(); c=by_client(client);
    if(c) { c->guards++; guard_total++; emit(c,GUARD,BP,0,0,0,0,0,0); }
    taskEXIT_CRITICAL();
}
typedef int32_t (*read_fn)(void *,uint32_t,uint32_t *,void *);
typedef struct { read_fn fn; void *context; uint32_t conn, request; } read_proxy;
static int32_t observe_read(void *buf,uint32_t len,uint32_t *out,void *ctx) {
    read_proxy *q=ctx; connection *c; uint32_t n;
    int32_t result=q->fn(buf,len,out,q->context);
    /* 原厂 reader 在非零返回时不消费 out，错误路径的 out 可能未定义。 */
    n=result==0?*out:0;
    taskENTER_CRITICAL(); c=by_id(q->conn);
    if(c) { c->reads++; c->bytes+=n;
        emit(c,READ_BYTES,VENDOR,q->request,0,n,0,result,c->invalid?16:0);
        if(n<=len) { if(n) feed(c,buf,n,VENDOR,q->request); }
        else invalid(c,21);
    } taskEXIT_CRITICAL(); return result;
}
int32_t http_owner_vendor_read(void *message,void *fn,void *context) {
    read_proxy q; request *r; connection *c; int32_t result; uint32_t id=0;
    taskENTER_CRITICAL(); r=by_message(message); c=r?by_id(r->conn):NULL;
    if(c && c->context==context) { id=c->id; q.request=r->id; }
    taskEXIT_CRITICAL();
    if(!id) return __real_HTTPMessageReadMessage(message,fn,context);
    q.fn=(read_fn)fn; q.context=context; q.conn=id;
    /* HTTPMessageReadMessage 同步调用 callback，不持有此栈上 context。 */
    result=__real_HTTPMessageReadMessage(message,(void *)observe_read,&q);
    taskENTER_CRITICAL(); c=by_id(id); r=by_message(message);
    if(c && result==0) {
        uint32_t flags=c->invalid?16:0, peer=0, seq=NOSEQ;
        if(c->dq_count) {
            delivery *d=&c->dq[c->dq_head]; peer=d->id; seq=d->cseq; flags|=d->flags;
            c->dq_head=(c->dq_head+1)%DELIVERIES; c->dq_count--;
            if(!c->invalid && peer && peer!=q.request) { flags|=4; mismatch++; }
        } else { flags|=16; invalid(c,22); }
        emit(c,DELIVER,VENDOR,q.request,peer,seq,0,0,flags); deliveries++;
        if(r) {
            /* 交付错误时请求自身仍未收到回覆，保留 FIFO 位置，不再绑定会复用的 message。 */
            if(r->answered) memset(r,0,sizeof(*r)); else r->message=NULL;
        }
    }
    taskEXIT_CRITICAL(); return result;
}
void __wrap_HTTPClientInvalidate(void *client) {
    connection *c; unsigned i;
    /* 先记录，原函数可能释放/重用相关对象。 */
    taskENTER_CRITICAL(); c=by_client(client);
    if(c) { emit(c,CLOSE_CONN,0,0,0,c->responses,c->invalid,0,0);
        for(i=0;i<REQUESTS;i++) if(rs[i].conn==c->id) memset(&rs[i],0,sizeof(rs[i]));
        memset(c,0,sizeof(*c)); }
    taskEXIT_CRITICAL(); __real_HTTPClientInvalidate(client);
}
void http_owner_report(uint32_t sequence) {
    unsigned i, pending; uint32_t l,f,p,s,m,d,g,cap;
    taskENTER_CRITICAL(); l=lost; f=full; p=parse_errors; s=suspect; m=mismatch;
    d=deliveries; g=guard_total; pending=en; cap=captures; taskEXIT_CRITICAL();
    rt_printf("[HTTP_OWN][%lu] v=1 cumulative=1 lost/full/invalid=%lu/%lu/%lu suspect/mismatch=%lu/%lu delivered/guard=%lu/%lu captures=%lu pending=%u\r\n",
        (unsigned long)sequence,(unsigned long)l,(unsigned long)f,(unsigned long)p,
        (unsigned long)s,(unsigned long)m,(unsigned long)d,(unsigned long)g,(unsigned long)cap,pending);
    for(i=0;i<CONNS;i++) {
        uint32_t cid=0,queued=0,written=0,responses=0,bytes=0,reads=0,bad=0,out=0;
        unsigned j;
        taskENTER_CRITICAL();
        if(cs[i].active && cs[i].hid) {
            cid=cs[i].id; queued=cs[i].queued; written=cs[i].written;
            responses=cs[i].responses; reads=cs[i].reads; bytes=cs[i].bytes; bad=cs[i].invalid;
            for(j=0;j<REQUESTS;j++) if(rs[j].id && rs[j].conn==cid && !rs[j].answered) out++;
        }
        taskEXIT_CRITICAL();
        if(cid) rt_printf("[HTTP_CONN] c=%lu queued/written/responses=%lu/%lu/%lu reads/bytes=%lu/%lu outstanding=%lu invalid=%lu\r\n",
            (unsigned long)cid,(unsigned long)queued,(unsigned long)written,(unsigned long)responses,
            (unsigned long)reads,(unsigned long)bytes,(unsigned long)out,(unsigned long)bad);
    }
    for(i=0;i<64;i++) {
        event e;
        taskENTER_CRITICAL();
        if(!en) { taskEXIT_CRITICAL(); break; }
        e=es[er]; er=(er+1)%EVENTS; en--; taskEXIT_CRITICAL();
        rt_printf("[HTTP_EVT] t=%lu c=%lu %s path=%u id=%lu peer=%lu a=%lu b=%lu ret=%ld flags=0x%lx cmd=%s\r\n",
            (unsigned long)e.time,(unsigned long)e.conn,names[e.kind],e.path,
            (unsigned long)e.id,(unsigned long)e.peer,(unsigned long)e.a,(unsigned long)e.b,
            (long)e.result,(unsigned long)e.flags,e.command[0]?e.command:"-");
    }
}
#endif
