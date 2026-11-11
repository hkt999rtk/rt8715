#!/usr/bin/env python3
"""验证接收统计透明性、阻塞状态、分层计数和满表退化。"""
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE=Path(__file__).resolve().parents[1]/'car_rx_diag.c'
class CarRxTest(unittest.TestCase):
    def test_observer(self):
        with tempfile.TemporaryDirectory() as td:
            d=Path(td); (d/'lwip').mkdir()
            (d/'FreeRTOS.h').write_text('extern unsigned critical;\n#define taskENTER_CRITICAL() (++critical)\n#define taskEXIT_CRITICAL() (--critical)\n')
            (d/'task.h').write_text('typedef void *TaskHandle_t;\nTaskHandle_t xTaskGetCurrentTaskHandle(void);\nconst char *pcTaskGetName(TaskHandle_t);\n')
            (d/'hal_timer.h').write_text('#include <stdint.h>\nuint32_t hal_read_curtime_us(void);\n')
            (d/'diag.h').write_text('int rt_printf(const char *, ...);\n')
            (d/'lwip/errno.h').write_text('extern int lwip_err;\n')
            (d/'lwip/sockets.h').write_text('#include <sys/socket.h>\n#include <sys/select.h>\n#include <arpa/inet.h>\nint lwip_getsockname(int,struct sockaddr *,socklen_t *);\nint lwip_getpeername(int,struct sockaddr *,socklen_t *);\n')
            harness=r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
unsigned critical, now, real_calls, connect_calls, select_calls;
int lwip_err, result, error;
void *task=(void *)1;
const char *name="TCPClient";
#define CONFIG_MOVE_LAG_PROFILE 1
#define CONFIG_IPHONE_HTTP_RX_PROFILE 0
#include "SOURCE"
TaskHandle_t xTaskGetCurrentTaskHandle(void){ return task; }
const char *pcTaskGetName(TaskHandle_t t){ (void)t;return name; }
uint32_t hal_read_curtime_us(void){ return now; }
int rt_printf(const char *f,...){ (void)f;assert(!critical);return 0; }
int lwip_getsockname(int fd,struct sockaddr *a,socklen_t *n){(void)fd;(void)a;(void)n;assert(!critical);return -1;}
int lwip_getpeername(int fd,struct sockaddr *a,socklen_t *n){(void)fd;(void)a;(void)n;assert(!critical);return -1;}
int __real_lwip_read(int fd,void *p,size_t n){
 (void)p;(void)n; real_calls++; assert(car_rx_begin(CAR_RX_RECV,fd)==-1);
 now+=12;lwip_err=error;return result;
}
int __real_lwip_recvfrom(int fd,void *b,size_t n,int f,struct sockaddr *a,socklen_t *l){(void)fd;(void)b;(void)n;(void)f;(void)a;(void)l;lwip_err=error;return result;}
int __real_lwip_connect(int fd,const struct sockaddr *a,socklen_t n){(void)fd;(void)a;(void)n;connect_calls++;lwip_err=error;return result;}
int __real_lwip_select(int n,fd_set *r,fd_set *w,fd_set *e,struct timeval *t){(void)n;(void)r;(void)w;(void)e;(void)t;select_calls++;now+=5;lwip_err=error;return result;}
int main(void){
 int t,i; fd_set f;
 result=10;error=77; assert(__wrap_lwip_read(4,0,20)==10 && lwip_err==77);
 assert(rows[0].calls==1 && rows[0].bytes==10 && nested_recv==1 && real_calls==1);
 result=-1;error=EAGAIN;assert(__wrap_lwip_read(4,0,20)==-1 && lwip_err==EAGAIN);
 result=0;assert(__wrap_lwip_read(4,0,20)==0);assert(rows[0].again==1 && rows[0].zero==1);
 t=car_rx_begin(CAR_RX_RECV,5);now+=30;car_rx_report(1);
 assert(rows[t].active && rows[t].done==0);car_rx_end(t,7,0);assert(rows[t].bytes==7);
 result=8;assert(__wrap_lwip_recvfrom(7,0,8,0,0,0)==8);
 t=find_row(CAR_RX_RECVFROM,7,task,name);assert(rows[t].bytes==8);
 t=car_rx_begin(CAR_RX_HTTP,-1);car_rx_end(t,11,0);
 car_rx_begin(CAR_RX_HTTP,-1);car_rx_end(t,0,0);
 car_rx_begin(CAR_RX_HTTP,-1);car_rx_end(t,-42,0);
 assert(rows[t].again==1 && rows[t].positive==1 && rows[t].errors==1);
 car_rx_type("hidSendReport",12,1);car_rx_type("setLimitedUI",30,1);
 car_rx_type((const char *)1,5,0);car_rx_type("",0,1);
 t=find_row(CAR_RX_TYPE,-1,task,name);
 assert(rows[t].hid==1 && rows[t].other==1 && rows[t].unknown==2 && rows[t].bytes==47);
 FD_ZERO(&f);FD_SET(9,&f);result=0;error=88;
 assert(__wrap_lwip_select(10,&f,0,0,0)==0 && lwip_err==88 && select_calls==1);
 t=find_row(CAR_RX_SELECT,9,task,name);assert(rows[t].calls==1 && rows[t].zero==1);
 assert(__wrap_lwip_connect(9,0,0)==0 && connect_calls==1);
 name="other";task=(void *)2;assert(__wrap_lwip_connect(10,0,0)==0 && connect_calls==2);
 for(i=0;i<120;i++){ t=car_rx_begin(CAR_RX_READ,100+i);car_rx_end(t,1,0); }
 assert(full>0);car_rx_report(2);assert(!critical);
 return 0;
}
'''.replace('SOURCE',str(SOURCE))
            (d/'test.c').write_text(harness)
            subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-fsanitize=undefined',f'-I{d}',str(d/'test.c'),'-o',str(d/'test')],check=True)
            subprocess.run([str(d/'test')],check=True)
if __name__=='__main__':unittest.main()
