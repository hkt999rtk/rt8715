/* Real board Annex B -> RTP -> Annex B, compare every delivered NAL byte. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wcam_rtp.h"
static struct wcam_rtp_rx rx;
static uint8_t frame[512*1024], au[512*1024];
static size_t used, frames, expected_frames;
static uint32_t clock_ms;
static FILE *result;
static int send_packet(void *arg,const uint8_t *p,size_t n) {
    (void)arg;wcam_rtp_receive(&rx,p,n,clock_ms);return 0;
}
static uint8_t *ready(void *arg,uint8_t *p,size_t n,uint32_t ts) {
    (void)arg;(void)ts;assert(n==used && !memcmp(p,au,n));
    assert(fwrite(p,1,n,result)==n);++frames;return p;
}
static size_t start(const uint8_t *p,size_t n,size_t at,size_t *prefix) {
    for(size_t i=at;i+3<n;i++) {
        if(p[i]==0 && p[i+1]==0) {
            if(p[i+2]==1) {*prefix=3;return i;}
            if(p[i+2]==0 && p[i+3]==1) {*prefix=4;return i;}
        }
    }return n;
}
int main(int argc,char **argv) {
    struct wcam_rtp_tx tx;FILE *f;long length;uint8_t *data;size_t at,prefix;int started=0;
    assert(argc==3);f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));length=ftell(f);rewind(f);
    assert(length>0);data=malloc((size_t)length);assert(data);assert(fread(data,1,(size_t)length,f)==(size_t)length);fclose(f);
    result=fopen(argv[2],"wb");assert(result);
    assert(!wcam_rtp_tx_init(&tx,0x12345678,65000,send_packet,NULL));
    assert(!wcam_rtp_rx_init(&rx,frame,sizeof frame,ready,NULL));
    at=start(data,(size_t)length,0,&prefix);
    while(at<(size_t)length) {
        size_t next_prefix=0,next=start(data,(size_t)length,at+prefix,&next_prefix), n=next-at-prefix;
        unsigned type=data[at+prefix]&31;
        assert(used+n+4<=sizeof au);memcpy(au+used,"\0\0\0\1",4);memcpy(au+used+4,data+at+prefix,n);used+=n+4;
        if(type==1 || type==5) {
            if(type==5)started=1;if(started)++expected_frames;
            assert(!wcam_rtp_send(&tx,au,used,clock_ms*90));clock_ms+=33;used=0;
        }
        at=next;prefix=next_prefix;
    }
    assert(frames==expected_frames && frames==270 && used==0);
    printf("board capture: %zu AUs round-tripped byte-for-byte (initial undecodable P frames gated)\n",frames);
    wcam_rtp_rx_destroy(&rx);wcam_rtp_tx_destroy(&tx);fclose(result);free(data);return 0;
}
