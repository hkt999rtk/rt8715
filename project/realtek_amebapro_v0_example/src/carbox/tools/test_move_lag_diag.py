#!/usr/bin/env python3
"""Exercise bounded telemetry, frame lifetime, errno and transparent I/O."""
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'move_lag_diag.c'

class MoveLagTest(unittest.TestCase):
    def test_runtime(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d/'lwip').mkdir()
            (d/'FreeRTOS.h').write_text('''#include <stdint.h>
extern unsigned critical;
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() (--critical)
''')
            (d/'task.h').write_text('''typedef void *TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
const char *pcTaskGetName(TaskHandle_t task);
''')
            (d/'hal_timer.h').write_text('#include <stdint.h>\nuint32_t hal_read_curtime_us(void);\n')
            (d/'diag.h').write_text('int rt_printf(const char *, ...);\n')
            (d/'lwip/sockets.h').write_text('#include <stddef.h>\n#include <poll.h>\n')
            (d/'lwip/errno.h').write_text('extern int lwip_err;\n')
            harness = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
unsigned critical;
static unsigned now, sender_calls, write_calls, probe_calls;
static int io_result, io_error, io_revents, consume;
static const char *task_name = "ScreenThread";
int lwip_err;
void carbox_screen_setup_report(uint32_t n){(void)n;}
void carbox_screen_flow_report(uint32_t n){(void)n;}
void carbox_screen_flow_frame(unsigned s,const void *p,size_t n){assert(!critical);(void)s;(void)p;(void)n;}
#define CONFIG_MOVE_LAG_PROFILE 1
#define CONFIG_AIRPLAY_HID_HTTP_BYPASS 1
#define CONFIG_TOUCH_MOVE_SAMPLE_HZ 0
#include "SOURCE"
int car_rx_begin(int api,int fd) { (void)api;(void)fd;return -1; }
void car_rx_end(int token,int result,int error) { (void)token;(void)result;(void)error; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)1; }
const char *pcTaskGetName(TaskHandle_t t) { (void)t; return task_name; }
uint32_t hal_read_curtime_us(void) { return now; }
int rt_printf(const char *f, ...) { (void)f; assert(!critical); return 0; }
int __real_lwip_recv(int s, void *b, size_t n, int f)
{ (void)s;(void)b;(void)n;(void)f;now+=70000;lwip_err=io_error;return io_result; }
int __real_lwip_write(int s, const void *b, size_t n)
{ (void)s;(void)b;(void)n;write_calls++;now+=60000;lwip_err=io_error;return io_result; }
int carbox_screen_tx_probe_write(int s, const void *b, size_t n)
{ probe_calls++; return __real_lwip_write(s,b,n); }
int __real_lwip_poll(struct pollfd *f, nfds_t n, int t)
{ (void)t;now+=250000;if(n)f[0].revents=io_revents;lwip_err=io_error;return io_result; }
void AirPlayScreen_SendScreenFrame(const void *p, int n)
{ (void)p;(void)n;sender_calls++;now+=300; }
void __real_AirPlayScreen_SendVideo(const void *p, int n)
{ if(consume)carbox_move_screen_frame(p,n); }
int main(void)
{
    unsigned i;
    char *p=malloc(8);
    struct pollfd f={.fd=3,.events=POLLOUT};
    /* wraparound duration and a bounded full ring */
    for(i=0;i<40;i++)carbox_move_lag_record(MOVE_HID_DRAIN,i,0xfffffff0U,0x20U-0xfffffff0U,0,0);
    assert(metrics[MOVE_HID_DRAIN].maximum==48 && event_count==0);
    for(i=0;i<40;i++)carbox_move_lag_record(MOVE_HID_DRAIN,i,0,60000,0,0);
    assert(event_count==32 && event_lost==8);
    carbox_move_lag_report(1);assert(event_count==24 && !critical);
    /* queue/consume pointer matching and producer drop cleanup */
    now=100;__wrap_AirPlayScreen_SendVideo(p,8);
    now=100100;carbox_move_screen_frame(p,8);
    assert(sender_calls==1 && metrics[MOVE_VIDEO_QUEUE].maximum==100000);
    assert(frame_task==NULL && pending[0].pointer==NULL);
    __wrap_AirPlayScreen_SendVideo(p,8);carbox_move_screen_free(p);
    assert(pending[0].pointer==NULL && unmatched==1);
    /* immediate consumer racing producer return is valid */
    p=malloc(8);consume=1;__wrap_AirPlayScreen_SendVideo(p,8);
    assert(sender_calls==2 && !pending[0].pointer);free(p);
    /* preserve return and lwIP errno; partial/zero/error are distinct */
    io_result=2;io_error=77;assert(__wrap_lwip_write(3,"abcd",4)==2);
    assert(lwip_err==77 && write_partial==1);
    io_result=0;assert(__wrap_lwip_write(3,"x",1)==0 && write_zero==1);
    io_result=-1;io_error=EAGAIN;
    assert(__wrap_lwip_write(3,"x",1)==-1 && lwip_err==EAGAIN && write_again==1);
    io_result=0;io_revents=0;
    assert(__wrap_lwip_poll(&f,1,250)==0 && poll_timeout==1);
    io_result=1;io_revents=POLLHUP;
    assert(__wrap_lwip_poll(&f,1,250)==1 && poll_error==1);
    io_revents=POLLOUT;assert(__wrap_lwip_poll(&f,1,250)==1 && poll_ready==1);
    task_name="AirPlayScreenReceiver";io_result=123;io_error=55;
    assert(__wrap_lwip_recv(3,NULL,200,0)==123 && lwip_err==55);
    assert(metrics[MOVE_VIDEO_RECV].count==1);
    task_name="other";io_result=-1;io_error=EIO;
    assert(__wrap_lwip_write(3,"x",1)==-1 && lwip_err==EIO);
    assert(metrics[MOVE_VIDEO_WRITE].count==3);
    assert(!critical);
    assert(write_calls==4);
    assert(probe_calls==(CONFIG_SCREEN_BLOCK_PROFILE ? 3U : 0U));
    return 0;
}
'''.replace('SOURCE', str(SOURCE))
            (d/'harness.c').write_text(harness)
            for enabled in (0, 1):
                subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror',f'-DCONFIG_SCREEN_BLOCK_PROFILE={enabled}',f'-DCONFIG_SCREEN_SETUP_EVIDENCE={enabled}',f'-I{d}',str(d/'harness.c'),'-o',str(d/'test')],check=True)
                subprocess.run([str(d/'test')],check=True)

if __name__ == '__main__': unittest.main()
