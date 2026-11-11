#include "stubs.h"
#include "airplay_timer_clock.h"
#include "screen_hw_timestamp.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

static uint64_t now_us;
static long wall_sec;
static int depth, vendor_time_calls;
static char printed[8192];
static size_t printed_len;
static const void *expected_packet;
static ssize_t send_result;
static int send_calls;
static uint32_t send_delay_us;
static int config_calls;
void AirPlayScreen_SendScreenData(char *data, int size) {
    assert(depth==0 && data==expected_packet && size==128);
    config_calls++;
}

void enter_critical(void) { depth++; }
void exit_critical(void) { assert(depth>0); depth--; }
void vTaskSetTimeOutState(TimeOut_t *s) {
    uint64_t ticks=now_us/1000U;
    s->xTimeOnEntering=(uint32_t)ticks;
    s->xOverflowCount=(int32_t)(ticks>>32);
}
uint32_t xTaskGetTickCount(void) { return (uint32_t)(now_us/1000U); }
uint32_t hal_read_curtime_us(void) { return (uint32_t)now_us; }
void sntp_set_lasttime(long sec,long usec,unsigned int tick) {
    (void)usec; (void)tick; wall_sec=sec;
}
int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz; tv->tv_sec=wall_sec; tv->tv_usec=0; return 0;
}
void __real_UpdateDeviceTime(int y,int m,int d,int h,int min,int s) {
    (void)y;(void)m;(void)d;(void)h;(void)min;(void)s;vendor_time_calls++;
}
void carbox_screen_timestamp_profile_sample(uint64_t t,uint64_t n,uint32_t s) {
    (void)t;(void)n;(void)s;
}
int rt_printf(const char *fmt, ...) {
    va_list ap; int n; assert(depth==0);
    va_start(ap,fmt); n=vsnprintf(printed+printed_len,sizeof(printed)-printed_len,fmt,ap); va_end(ap);
    assert(n>=0 && (size_t)n<sizeof(printed)-printed_len); printed_len+=(size_t)n; return n;
}
ssize_t lwip_sendto(int fd,const void *data,size_t len,int flags,const struct sockaddr *to,socklen_t size) {
    assert(depth==0); assert(fd==19 && data==expected_packet && len==32 && flags==7 && to==NULL && size==0);
    send_calls++; now_us+=send_delay_us; errno=EWOULDBLOCK; return send_result;
}
extern uint64_t __wrap_UpTicksToNTP(uint64_t);
extern void __wrap_UpdateDeviceTime(int,int,int,int,int,int);

int main(void) {
    struct timeval tv, start;
    now_us=2000000; wall_sec=2;
    assert(carbox_airplay_timer_gettimeofday(&start,NULL)==0);
    assert(start.tv_sec==2 && start.tv_usec==0);
    /* Before Timer_Start, conversion is deterministic uptime (no underflow). */
    assert(__wrap_UpTicksToNTP(0)==(2ULL<<32));
    assert(carbox_airplay_timer_start_time(&start,NULL)==0);
    assert(start.tv_sec==(CONFIG_AIRPLAY_TIMER_SHARED_ORIGIN ? 0 : 2));
    assert(carbox_airplay_timer_start_time(NULL,NULL)==-1);
    now_us=33500123;
    assert(carbox_airplay_timer_gettimeofday(&tv,NULL)==0);
    assert(tv.tv_sec==33 && tv.tv_usec==500123);
    __wrap_UpdateDeviceTime(2026,11,10,11,51,12);
    assert(wall_sec==1794311472 && vendor_time_calls==1);
    assert(carbox_airplay_timer_gettimeofday(&tv,NULL)==0);
    assert(tv.tv_sec==33 && tv.tv_usec==500123);
    /* Real video conversion and timing read must share the same uptime. */
    now_us=34000000;
    assert(__wrap_UpTicksToNTP(34000)==((CONFIG_SCREEN_TIMER_ORIGIN ? (34ULL-start.tv_sec) : 34ULL)<<32));
    assert(carbox_airplay_timer_gettimeofday(&tv,NULL)==0);
    assert(tv.tv_sec-start.tv_sec==(CONFIG_AIRPLAY_TIMER_SHARED_ORIGIN ? 34 : 32));
    wall_sec=1; /* Another writer steps realtime backwards. */
    now_us=34250000;
    assert(__wrap_UpTicksToNTP(34250)==(((CONFIG_SCREEN_TIMER_ORIGIN ? (34ULL-start.tv_sec) : 34ULL)<<32)|0x40000000ULL));
    assert(carbox_airplay_timer_gettimeofday(&tv,NULL)==0 && tv.tv_sec==34 && tv.tv_usec==250000);
    /* A restarted timer can take a new baseline without resetting the shared clock. */
    assert(carbox_airplay_timer_start_time(&start,NULL)==0);
    /* Baseline is integer seconds, not the full fractional restart time. */
    assert(__wrap_UpTicksToNTP(0)==(((CONFIG_SCREEN_TIMER_ORIGIN ? (34ULL-start.tv_sec) : 34ULL)<<32)|0x40000000ULL));
    now_us=45250000;
    assert(carbox_airplay_timer_gettimeofday(&tv,NULL)==0);
    assert(tv.tv_sec-start.tv_sec==(CONFIG_AIRPLAY_TIMER_SHARED_ORIGIN ? 45 : 11));
#if CONFIG_AIRPLAY_TIMER_CLOCK_PROFILE
    {
        unsigned char p[32]={0x80,0xd3};
        uint32_t sec=0x83aa7e80U+(uint32_t)(tv.tv_sec-start.tv_sec);
        unsigned i;
        for(i=0;i<4;i++) p[24+i]=(unsigned char)(sec>>(24-8*i));
        p[28]=0x40; /* 45.25 秒，与本次采时一致；旧模式少 baseline 秒。 */
        expected_packet=p; send_result=32;
        assert(carbox_airplay_timer_sendto(19,p,32,7,NULL,0)==32);
        carbox_airplay_clock_report(0);
        assert(strstr(printed,CONFIG_AIRPLAY_TIMER_SHARED_ORIGIN ?
            "tx_minus_uptime_us=0" : "tx_minus_uptime_us=-34000000"));
        assert(strstr(printed,"starts=2"));
        assert(strstr(printed,"tx_minus_session_us=0"));
        printed_len=0; printed[0]=0; send_calls=0;
    }
#endif
    now_us=4294967200ULL;
    assert(carbox_media_monotonic_us()==4294967200ULL);
    now_us=4294967400ULL;
    assert(carbox_media_monotonic_us()==4294967400ULL);
    /* Long idle spanning multiple hardware timer wraps and the RTOS tick wrap. */
    now_us=4294967296500ULL;
    assert(carbox_media_monotonic_us()==4294967296500ULL);
    assert(carbox_airplay_timer_gettimeofday(&tv,NULL)==0 && tv.tv_sec==4294967 && tv.tv_usec==296500);
    assert(carbox_airplay_timer_gettimeofday(NULL,NULL)==-1);
#if CONFIG_AIRPLAY_TIMER_CLOCK_PROFILE
    {
        unsigned char packet[32]={0x80,0xd3}, before[32], header[128]={0};
        packet[16]=0x83;packet[17]=0xaa;packet[18]=0x7e;packet[19]=0xa2;
        packet[24]=0x83;packet[25]=0xaa;packet[26]=0x7e;packet[27]=0xa2;
        packet[28]=0x40; memcpy(before,packet,32);
        /* 连续帧、空闲后恢复、旧时间戳、重复及倒退，header 必须只读。 */
        {
            uint64_t n; unsigned i;
            n=__wrap_UpTicksToNTP(0);
            for(i=0;i<8;i++) header[8+i]=(unsigned char)(n>>(8*i));
            header[4]=1; header[6]=0x16;
            expected_packet=header;
            carbox_airplay_clock_config_send((char *)header,128);
            assert(config_calls==1 && header[6]==0x16);
            now_us+=250; carbox_airplay_clock_video(header);
            now_us+=5000000; n=__wrap_UpTicksToNTP(0);
            for(i=0;i<8;i++) header[8+i]=(unsigned char)(n>>(8*i));
            carbox_airplay_clock_video(header); /* Current timestamp after idle. */
            now_us+=30000; carbox_airplay_clock_video(header); /* Duplicate/old. */
            n-=1ULL<<32;
            for(i=0;i<8;i++) header[8+i]=(unsigned char)(n>>(8*i));
            carbox_airplay_clock_video(header); /* Backwards by one second. */
            assert(header[12]==(unsigned char)(n>>32));
        }
        expected_packet=packet;send_result=32;
        send_delay_us=40000;
        assert(carbox_airplay_timer_sendto(19,packet,32,7,NULL,0)==32);
        assert(memcmp(packet,before,32)==0 && errno==EWOULDBLOCK);
        send_result=-1;
        assert(carbox_airplay_timer_sendto(19,packet,32,7,NULL,0)==-1);
        assert(memcmp(packet,before,32)==0 && errno==EWOULDBLOCK && send_calls==2);
        carbox_airplay_clock_report(1);
        assert(strstr(printed,"[MEDIA_CLOCK]") && strstr(printed,"83aa7ea2") && strstr(printed,"40000000"));
        assert(strstr(printed,"send_avg/max_us=26666/40000"));
        assert(strstr(printed,"[MEDIA_CONFIG]") && strstr(printed,"use_ts=1"));
        assert(strstr(printed,"back/repeat=1/1"));
        assert(strstr(printed,"old20ms=2"));
        assert(strstr(printed,"gap_max_us=5000250"));
        printed_len=0;printed[0]=0;
        carbox_airplay_clock_report(2);
        assert(!printed_len); /* No activity: no repeated lines. */
        /* Reconnect resets the session axis; it must not count as a backwards frame. */
        assert(carbox_airplay_timer_start_time(&start,NULL)==0);
        {
            uint64_t n=__wrap_UpTicksToNTP(0); unsigned i;
            for(i=0;i<8;i++) header[8+i]=(unsigned char)(n>>(8*i));
            carbox_airplay_clock_video(header);
            carbox_airplay_clock_report(3);
            assert(strstr(printed,"back/repeat=1/1"));
            assert(strstr(printed,"starts=3"));
            assert(strstr(printed,"age_last/max_us=0/1030000"));
        }
    }
#else
    (void)send_calls;(void)send_result;(void)expected_packet;(void)send_delay_us;
#endif
    assert(depth==0);
    puts("clock: wall-step, shared video clock, rollover, idle/restart and send observation PASS");
    return 0;
}
