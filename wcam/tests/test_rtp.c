#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wcam_rtp.h"
static uint8_t packets[64][WCAM_RTP_MTU];
static size_t lengths[64], count, delivered, output_size;
static uint8_t frame[16384], output[16384];
static int capture(void *arg,const uint8_t *p,size_t n) {
    (void)arg; assert(count<64 && n<=WCAM_RTP_MTU);
    memcpy(packets[count],p,n); lengths[count++]=n; return 0;
}
static uint8_t *ready(void *arg,uint8_t *p,size_t n,uint32_t ts) {
    (void)arg; (void)ts; assert(n<=sizeof output); memcpy(output,p,n);
    output_size=n; ++delivered; return p;
}
static void make_au(uint8_t *p,size_t n,int idr) {
    memset(p,0x55,n);
    const uint8_t head[]={0,0,0,1,0x67,0x42,0xE0,0x28,0,0,0,1,0x68,0xCE,0x38,0x80,0,0,0,1,0x65};
    memcpy(p,head,sizeof head); p[20]=idr?0x65:0x41;
}
int main(void) {
    struct wcam_rtp_tx tx; struct wcam_rtp_rx *rx=calloc(1,sizeof(*rx));
    uint8_t au[6000], control[128]; size_t n; unsigned i;
    assert(rx); assert(wcam_rtp_tx_init(&tx,0x12345678,65533,capture,NULL)==0);
    assert(wcam_rtp_rx_init(rx,frame,sizeof frame,ready,NULL)==0);
    make_au(au,sizeof au,1);
    assert(wcam_rtp_send(&tx,au,sizeof au,9000)==0); assert(count>4);
    assert(packets[0][0]==0x80 && packets[0][1]==96);
    assert(packets[2][12]==0x7c && (packets[2][13]&0x80));
    assert(packets[count-1][1]==(96|128));
    /* FU-A reorder and sequence wrap must preserve the complete access unit. */
    wcam_rtp_receive(rx,packets[0],lengths[0],0);
    uint32_t feedback_before=rx->feedback_epoch;
    wcam_rtp_receive(rx,packets[2],lengths[2],1);
    /* A forward sequence gap requests FAST_UPDATE immediately, while the
       reordering window still permits a valid frame to be recovered. */
    assert(rx->feedback_epoch==feedback_before+1);
    wcam_rtp_receive(rx,packets[1],lengths[1],2);
    for(i=3;i<count;i++) wcam_rtp_receive(rx,packets[i],lengths[i],i);
    assert(delivered==1 && output_size==sizeof au && !memcmp(au,output,sizeof au));
    assert(!rx->need_idr);
    /* Missing middle fragment cannot escape as a corrupt frame. */
    count=0; assert(!wcam_rtp_send(&tx,au,sizeof au,12000));
    for(i=0;i<count;i++) if(i!=3) wcam_rtp_receive(rx,packets[i],lengths[i],40+i);
    wcam_rtp_poll(rx,100); assert(delivered==1 && rx->need_idr && rx->lost>0);
    assert(rx->feedback_epoch>feedback_before+1);
    count=0; make_au(au,sizeof au,0); assert(!wcam_rtp_send(&tx,au,sizeof au,15000));
    for(i=0;i<count;i++) wcam_rtp_receive(rx,packets[i],lengths[i],110+i);
    assert(delivered==1);
    count=0; make_au(au,sizeof au,1); assert(!wcam_rtp_send(&tx,au,sizeof au,18000));
    for(i=0;i<count;i++) wcam_rtp_receive(rx,packets[i],lengths[i],150+i);
    assert(delivered==2 && !rx->need_idr);
    /* A marker on an unfinished FU must not publish a partial access unit,
       even when a complete VCL NAL preceded that fragment. */
    count=0; assert(!wcam_rtp_send(&tx,au,sizeof au,19000));
    wcam_rtp_receive(rx,packets[0],lengths[0],170);
    wcam_rtp_receive(rx,packets[1],lengths[1],171);
    uint8_t small[14]; memcpy(small,packets[2],12); small[1]=96;
    small[12]=0x65;small[13]=0x55;
    wcam_rtp_receive(rx,small,sizeof small,172);
    packets[3][1]|=128; packets[3][13]=0x85;
    wcam_rtp_receive(rx,packets[3],lengths[3],173);
    assert(delivered==2 && rx->need_idr);
    count=0; assert(!wcam_rtp_send(&tx,au,sizeof au,19500));
    for(i=0;i<count;i++) wcam_rtp_receive(rx,packets[i],lengths[i],180+i);
    wcam_rtp_poll(rx,220);
    assert(delivered==3 && !rx->need_idr);
    /* Duplicate must not create another output or corrupt the next frame. */
    wcam_rtp_receive(rx,packets[count-1],lengths[count-1],230); assert(delivered==3);
    /* Compound RR+SDES+PLI, only addressed PLI triggers IDR. */
    n=wcam_rtcp_report(control,sizeof control,0x87654321,rx,1,200);
    assert(n>12); assert(wcam_rtcp_feedback(control,n,tx.ssrc)==1);
    assert(wcam_rtcp_feedback(control,n,0x11223344)==0);
    for(i=0;i<n;i++) assert(wcam_rtcp_feedback(control,i,tx.ssrc)<=0);
    /* malformed RTP and insufficient AU capacity never corrupt memory. */
    for(i=0;i<12;i++) wcam_rtp_receive(rx,packets[0],i,210);
    wcam_rtp_rx_destroy(rx);
    assert(!wcam_rtp_rx_init(rx,frame,32,ready,NULL)); count=0;
    assert(!wcam_rtp_send(&tx,au,sizeof au,21000));
    for(i=0;i<count;i++) wcam_rtp_receive(rx,packets[i],lengths[i],250+i);
    assert(delivered==3 && rx->need_idr);
    wcam_rtp_rx_destroy(rx);
    assert(!wcam_rtp_rx_init(rx,frame,sizeof frame,ready,NULL));
    /* A legal trailing filler NAL still terminates the same access unit. */
    uint8_t tail[64]; make_au(tail,58,1);
    memcpy(tail+58,"\0\0\0\1\x0c\x80",6);count=0;
    assert(!wcam_rtp_send(&tx,tail,sizeof tail,24000));
    assert(packets[count-1][1]&128);
    for(i=0;i<count;i++)wcam_rtp_receive(rx,packets[i],lengths[i],300+i);
    assert(delivered==4 && output_size==sizeof tail && !memcmp(tail,output,sizeof tail));
    /* A long idle link can resume beyond the signed 16-bit sequence window. */
    count=0;assert(!wcam_rtp_send(&tx,tail,sizeof tail,27000));
    for(i=0;i<count;i++) {
        uint16_t seq=(uint16_t)(((unsigned)packets[i][2]<<8|packets[i][3])+40000);
        packets[i][2]=seq>>8;packets[i][3]=(uint8_t)seq;
        wcam_rtp_receive(rx,packets[i],lengths[i],5000+i);
    }
    assert(delivered==5 && !rx->need_idr);
    /* SR/RR reports carry interval loss and valid DLSR, not always zero. */
    rx->highest=109;rx->base_seq=100;rx->received=8;rx->expected_prior=rx->received_prior=0;
    n=wcam_rtcp_report(control,sizeof control,0x87654321,rx,0,5200);
    assert(n==52 && control[12]==51 && control[15]==2);
    n=wcam_rtcp_report(control,sizeof control,0x87654321,rx,0,5300);
    assert(control[12]==0);
    /* A missing FU start with a marker must invalidate an otherwise
       decodable prefix; never publish only the first slice of that AU. */
    wcam_rtp_resync(rx); count=0; make_au(au,sizeof au,1);
    assert(!wcam_rtp_send(&tx,au,sizeof au,29000));
    wcam_rtp_receive(rx,packets[0],lengths[0],5500);
    wcam_rtp_receive(rx,packets[1],lengths[1],5501);
    memcpy(small,packets[2],12);small[1]=96;small[12]=0x65;small[13]=0x55;
    wcam_rtp_receive(rx,small,sizeof small,5502);
    packets[3][1]|=128;packets[3][13]=0x45;
    wcam_rtp_receive(rx,packets[3],lengths[3],5503);
    assert(delivered==5 && rx->need_idr);
    /* Extreme valid timestamp deltas cannot overflow signed jitter math. */
    wcam_rtp_resync(rx);
    uint8_t extreme[]={0x80,0xe0,0,1,0,0,0,0,0x12,0x34,0x56,0x78,0x41,0x55};
    wcam_rtp_receive(rx,extreme,sizeof extreme,6000);
    extreme[3]=2;extreme[4]=0x80;wcam_rtp_receive(rx,extreme,sizeof extreme,6000);
    wcam_rtp_rx_destroy(rx); wcam_rtp_tx_destroy(&tx); free(rx);
    puts("RTP: Annex B/FU-A/wrap/reorder/loss/recovery/PLI/bounds PASS");
    return 0;
}
