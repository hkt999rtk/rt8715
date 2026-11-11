#!/usr/bin/env python3
"""Compile the production observer with a small lwIP model; no hardware writes."""
import pathlib, subprocess, tempfile, unittest
ROOT = pathlib.Path(__file__).resolve().parents[5]
SRC = ROOT / "component/common/network/lwip/lwip_v2.1.2/src/core/tcp_output_wait.c"
INC = ROOT / "component/common/network/lwip/lwip_v2.1.2/src/include"
STUB = r"""
#include <stdint.h>
#include <string.h>
#include <assert.h>
typedef uint32_t u32_t; typedef uint16_t u16_t; typedef uint8_t u8_t;
#define LWIP_TCP 1
#define LWIP_TCP_RECOVERY 1
#define CONFIG_TCP_RECOVERY_TRACE 1
#define CONFIG_MOVE_LAG_PROFILE 1
#define TCP_SEQ_LT(a,b) ((int32_t)((a)-(b))<0)
#define TCP_SEQ_LEQ(a,b) ((int32_t)((a)-(b))<=0)
#define TCP_SEQ_GT(a,b) ((int32_t)((a)-(b))>0)
#define TCP_SEQ_GEQ(a,b) ((int32_t)((a)-(b))>=0)
#define lwip_ntohl(x) (x)
#define TCP_TCPLEN(s) ((s)->len)
#define LWIP_MAX(a,b) ((a)>(b)?(a):(b))
#define SYS_ARCH_DECL_PROTECT(x) int x=0
#define SYS_ARCH_PROTECT(x) ((void)(x))
#define SYS_ARCH_UNPROTECT(x) ((void)(x))
struct tcp_hdr { u32_t seqno; };
struct tcp_seg { struct tcp_hdr *tcphdr; u16_t len; };
struct tcp_pcb { struct { int usb_screen_rto; struct {u32_t connection;} trace; } recovery;
 u32_t lastack,snd_nxt,cwnd,snd_wnd; u16_t local_port,remote_port,flags; };
static u32_t now;
static u32_t sys_now(void) {return now;}
"""
TEST = r"""
int main(void) {
 struct tcp_pcb p={0}, before;
 struct tcp_hdr h={2440}, old={1000}; struct tcp_seg s={&h,1440}, r={&old,1440};
 struct tcp_wait_row row; struct tcp_wait_event e; u32_t dropped,evicted;
 p.recovery.trace.connection=7; p.lastack=1000;p.snd_nxt=2440;p.cwnd=1440;p.snd_wnd=181440;
 p.local_port=123;p.remote_port=456;p.flags=0x140;
 /* Non-screen traffic has no table entry. */
 tcp_wait_block(&p,&s,TCP_WAIT_CWND); assert(!tcp_wait_snapshot(0,&row,&evicted));
 p.recovery.usb_screen_rto=1;before=p;now=100;
 tcp_wait_rto(&p,99);tcp_wait_block(&p,&s,TCP_WAIT_CWND);
 now=110;tcp_wait_block(&p,&s,TCP_WAIT_CWND);tcp_wait_sent(&p,&r);
 assert(tcp_wait_snapshot(0,&row,&evicted) && row.active && row.started==1 && row.checks==2);
 now=120;tcp_wait_ack(&p);now=150;tcp_wait_ack(&p);
 now=160;tcp_wait_sent(&p,&s);
 assert(tcp_wait_snapshot(0,&row,&evicted) && !row.active && row.resumed==1 && row.max_ms==60);
 assert(tcp_wait_pop(&e,&dropped));assert(e.duration_ms==60 && e.first_ack_ms==40 && e.last_ack_ms==10);
 assert(e.rto_id==99 && e.reason_mask==TCP_WAIT_CWND && e.result==TCP_WAIT_SENT);
 assert(!memcmp(&before,&p,sizeof(p)));
 /* Reason transitions, partial send covers the blocked first byte. */
 now=200;tcp_wait_block(&p,&s,TCP_WAIT_RWND);now=205;tcp_wait_block(&p,&s,TCP_WAIT_NAGLE);
 now=230;s.len=1;tcp_wait_sent(&p,&s);
 assert(tcp_wait_pop(&e,&dropped) && e.reason_mask==(TCP_WAIT_RWND|TCP_WAIT_NAGLE));
 assert(e.first_ack_ms==UINT32_MAX && e.last_ack_ms==UINT32_MAX);
 /* Tick and sequence wrap are valid; earlier retransmit cannot resume. */
 h.seqno=0xfffffff0U;s.len=32;now=0xfffffff0U;tcp_wait_block(&p,&s,TCP_WAIT_CWND);
 now=5;tcp_wait_ack(&p);now=20;tcp_wait_sent(&p,&s);
 assert(tcp_wait_pop(&e,&dropped) && e.duration_ms==36 && e.last_ack_ms==15);
 /* Empty/closed queues end as cancellation, never successful sends. */
 now=40;tcp_wait_block(&p,&s,3);now=70;tcp_wait_cancel(&p,TCP_WAIT_EMPTY);
 assert(tcp_wait_pop(&e,&dropped) && e.result==TCP_WAIT_EMPTY);
 now=80;tcp_wait_block(&p,&s,1);tcp_wait_cancel(&p,TCP_WAIT_CLOSED);
 assert(tcp_wait_snapshot(0,&row,&evicted) && !row.active && row.cancelled==2);
 /* Effective window credit must not be reported as a CWND block. */
 h.seqno=2440;s.len=1440;p.lastack=1000;p.cwnd=1440;p.snd_wnd=181440;
 tcp_wait_snapshot(0,&row,&evicted);unsigned previous=row.started;
 tcp_wait_window(&p,&s,2880);tcp_wait_snapshot(0,&row,&evicted);assert(row.started==previous);
 p.snd_wnd=1000;now=100;tcp_wait_window(&p,&s,1000);
 tcp_wait_snapshot(0,&row,&evicted);assert(row.active && row.current.reason_mask==3 && row.current.need==2880);
 now=130;tcp_wait_sent(&p,&s);assert(tcp_wait_pop(&e,&dropped) && e.reason_mask==3);
 /* Fixed ring saturation reports lost evidence; table reuse reports eviction. */
 for(int i=0;i<40;i++){now+=30;tcp_wait_block(&p,&s,1);now+=30;tcp_wait_sent(&p,&s);}
 assert(tcp_wait_pop(&e,&dropped) && dropped>0);
 for(unsigned i=8;i<8+TCP_WAIT_ROWS;i++){p.recovery.trace.connection=i;tcp_wait_block(&p,&s,1);}
 tcp_wait_snapshot(0,&row,&evicted);assert(evicted>0);
 return 0;
}
"""
class TestTcpOutputWait(unittest.TestCase):
 def test_disabled(self):
  with tempfile.TemporaryDirectory() as temp:
   t=pathlib.Path(temp); (t/"lwip").mkdir()
   (t/"lwip/opt.h").write_text("#define LWIP_TCP 1\n#define LWIP_TCP_RECOVERY 1\n#define CONFIG_TCP_RECOVERY_TRACE 1\n#define CONFIG_MOVE_LAG_PROFILE 0\n")
   # Disabled macros must not evaluate arguments or require PCB layout/types.
   (t/"off.c").write_text('#include "lwip/priv/tcp_output_wait.h"\nint main(void){tcp_wait_block(missing,missing,TCP_WAIT_NAGLE);tcp_wait_window(missing,missing,0);tcp_wait_sent(missing,missing);tcp_wait_ack(missing);tcp_wait_rto(missing,0);tcp_wait_cancel(missing,TCP_WAIT_CLOSED);return 0;}\n')
   subprocess.run(["cc","-std=c99","-Wall","-Wextra","-Werror","-I"+str(t),"-I"+str(INC),str(t/"off.c"),str(SRC),"-o",str(t/"off")],check=True)
   subprocess.run([str(t/"off")],check=True)
 def test_observer(self):
  self.assertTrue(SRC.exists(), "TCP output wait observer has not been implemented")
  with tempfile.TemporaryDirectory() as temp:
   t=pathlib.Path(temp); (t/"lwip/priv").mkdir(parents=True)
   for name in ("opt.h","sys.h","priv/tcp_priv.h"):
    (t/"lwip"/name).write_text("")
   (t/"test.c").write_text(STUB+'\n#include "'+str(SRC)+'"\n'+TEST)
   subprocess.run(["cc","-std=c99","-Wall","-Wextra","-Werror","-fsanitize=undefined","-I"+str(t),"-I"+str(INC),str(t/"test.c"),"-o",str(t/"test")],check=True)
   subprocess.run([str(t/"test")],check=True)
if __name__=="__main__": unittest.main()
