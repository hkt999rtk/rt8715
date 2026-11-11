#!/usr/bin/env python3
"""Host tests for fragmented/coalesced HTTP evidence and transparent read hooks."""
import plistlib
import subprocess
import tempfile
from pathlib import Path
SRC = Path(__file__).resolve().parents[1] / 'http_owner_diag.c'
with tempfile.TemporaryDirectory() as tmp:
    d=Path(tmp)
    (d/'FreeRTOS.h').write_text('extern unsigned critical;\n#define taskENTER_CRITICAL() (++critical)\n#define taskEXIT_CRITICAL() (--critical)\n')
    (d/'task.h').write_text('')
    (d/'hal_timer.h').write_text('#include <stdint.h>\nuint32_t hal_read_curtime_us(void);\n')
    (d/'diag.h').write_text('int rt_printf(const char *, ...);\n')
    code=r'''
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
unsigned critical;
#define CONFIG_MOVE_LAG_PROFILE 1
#include "SOURCE"
static uint32_t now;
uint32_t hal_read_curtime_us(void) { return ++now; }
int rt_printf(const char *fmt,...) { (void)fmt; assert(!critical); return 0; }
void __real_HTTPClientInvalidate(void *p) { (void)p; assert(!critical); }
static const char *input;
static int calls, parse_result;
static int32_t reader(void *buf,uint32_t size,uint32_t *out,void *ctx) {
    assert(!critical && ctx==(void *)0x99); calls++;
    *out=(uint32_t)strlen(input); assert(*out<=size); memcpy(buf,input,*out); return 0;
}
int32_t __real_HTTPMessageReadMessage(void *m,void *fn,void *ctx) {
    char buf[512]; uint32_t n=0; (void)m;
    if(input) assert(((read_fn)fn)(buf,sizeof(buf),&n,ctx)==0);
    return parse_result;
}
static unsigned char messages[8][4096];
static void setup(void) {
    memset(cs,0,sizeof(cs)); memset(rs,0,sizeof(rs)); memset(es,0,sizeof(es));
    memset(messages,0,sizeof(messages));
    er=en=hr=hn=capture_left=0; next_conn=next_req=lost=full=parse_errors=0;
    suspect=mismatch=deliveries=guard_total=captures=0; input=NULL; calls=0; parse_result=0;
}
static void send(unsigned idx,int bp,unsigned seq) {
    char header[128]; uint32_t n;
    snprintf(header,sizeof(header),"POST /command HTTP/1.1\r\nCSeq: %u\r\nContent-Length: 0\r\n\r\n",seq);
    n=strlen(header); memcpy(messages[idx]+HEADER,header,n); memcpy(messages[idx]+HEADER_LEN,&n,4);
    http_owner_enqueue((void *)1,messages[idx],bp,0x70000000);
    http_owner_write_begin(messages[idx],(void *)0x99);
    http_owner_write_end(messages[idx],0);
}
static void drain(const char *s) { http_owner_drain((void *)1,s,(uint32_t)strlen(s),0); }
int main(void) {
    unsigned i; connection *c;
    /* 一字节一段，body 中的伪 HTTP 不应成为新响应。 */
    setup(); send(0,1,1);
    input="HTTP/1.1 200 OK\r\nCSeq: 1\r\nContent-Length: 4\r\n\r\nHTTP";
    for(i=0;i<strlen(input);i++) http_owner_drain((void *)1,input+i,1,0);
    c=by_client((void *)1); assert(c && c->responses==1 && !c->invalid && !oldest(c));
    /* 一次 read 两个响应，chunked 含扩展和 trailer。 */
    send(1,1,2); send(2,1,3);
    drain("HTTP/1.1 200 OK\r\nCSeq: 2\r\nTransfer-Encoding: chunked\r\n\r\n2;x=y\r\nab\r\n0\r\nX-T: 1\r\n\r\nHTTP/1.1 204 No Content\r\nCSeq: 3\r\n\r\n");
    assert(c->responses==3 && !c->invalid && mismatch==0);
    /* 100 continue 不消耗请求。 */
    send(3,1,4); drain("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nCSeq: 4\r\nContent-Length: 0\r\n\r\n");
    assert(c->responses==4 && !oldest(c));
    /* 真正交付错误：vendor parser 收到先前 HID 的 response。 */
    setup(); send(0,1,10); send(1,0,11);
    input="HTTP/1.1 200 OK\r\nCSeq: 10\r\nContent-Length: 0\r\n\r\n";
    assert(http_owner_vendor_read(messages[1],(void *)reader,(void *)0x99)==0);
    assert(calls==1 && suspect==1 && mismatch==1 && deliveries==1);
    assert(en && captures);
    /* 预读两响应：读取时不同 id 不是交付错配，解析完成 FIFO 应正确。 */
    setup(); send(0,0,20); send(1,0,21);
    input="HTTP/1.1 200 OK\r\nCSeq: 20\r\nContent-Length: 0\r\n\r\nHTTP/1.1 200 OK\r\nCSeq: 21\r\nContent-Length: 0\r\n\r\n";
    assert(http_owner_vendor_read(messages[0],(void *)reader,(void *)0x99)==0);
    input=NULL; assert(http_owner_vendor_read(messages[1],(void *)reader,(void *)0x99)==0);
    assert(calls==1 && mismatch==0 && deliveries==2);
    /* 返回 would-block，下一次继续同一 response；不提前交付。 */
    setup(); send(0,0,30); parse_result=11;
    input="HTTP/1.1 200 OK\r\nCSeq: 30\r\nContent-Len";
    assert(http_owner_vendor_read(messages[0],(void *)reader,(void *)0x99)==11);
    assert(deliveries==0); parse_result=0; input="gth: 0\r\n\r\n";
    assert(http_owner_vendor_read(messages[0],(void *)reader,(void *)0x99)==0);
    assert(deliveries==1 && mismatch==0);
    /* 缺失长度、超长行、CSeq 不匹配、表满不能伪造成功。 */
    setup(); send(0,1,1); drain("HTTP/1.1 200 OK\r\n\r\n"); assert(parse_errors==1);
    setup(); send(0,1,1); drain("HTTP/1.1 200 OK\r\nCSeq: 2\r\nContent-Length: 0\r\n\r\n"); assert(mismatch==1);
    setup(); send(0,1,1); drain("HTTP/1.1 200 OK\r\nX: "); for(i=0;i<300;i++) drain("x"); assert(parse_errors==1);
    /* mixed reader 明确标成不可信。 */
    setup(); send(0,1,1); send(1,0,2); drain("HTTP/1.1 200 OK\r\n");
    input="CSeq: 1\r\nContent-Length: 0\r\n\r\n";
    http_owner_vendor_read(messages[1],(void *)reader,(void *)0x99); assert(parse_errors==1);
    /* 固定 ring 溢出只丢观测；UART 不在 critical section 内。 */
    c=by_client((void *)1); for(i=0;i<1000;i++)emit(c,ENQUEUE,VENDOR,i,0,0,0,0,0);
    assert(en==EVENTS && lost); http_owner_report(1); assert(en==EVENTS-64);
    __wrap_HTTPClientInvalidate((void *)1); assert(!by_client((void *)1) && !critical);
    puts("HTTP owner tests PASS"); return 0;
}
'''.replace('SOURCE',str(SRC))
    blob=plistlib.dumps({'type':'requestUI','other':b'not logged'},fmt=plistlib.FMT_BINARY)
    extra='''{ unsigned char b[]={BLOB}; char out[32]; unsigned z;
        command_type(b,sizeof(b),out); assert(!strcmp(out,"requestUI"));
        for(z=0;z<sizeof(b);z++) { command_type(b,z,out); assert(!strcmp(out,"unknown")); }
        b[sizeof(b)-6]=255; command_type(b,sizeof(b),out); assert(!strcmp(out,"unknown"));
    }
    setup();
    for(i=0;i<REQUESTS+1;i++) send(0,1,i+1);
    assert(full==1 && parse_errors==1);
    '''.replace('BLOB',','.join(str(x) for x in blob))
    code=code.replace('puts("HTTP owner tests PASS");',extra+'puts("HTTP owner tests PASS");')
    (d/'test.c').write_text(code)
    subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-I'+str(d),str(d/'test.c'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
