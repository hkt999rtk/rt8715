#include "wcam_rtp.h"
#include "third_party/ireader/rtp-payload-internal.h"
#include <string.h>
#include <limits.h>
extern void wcam_ir_h264_rx_buffer(void *,uint8_t *,int,int);
extern int wcam_ir_h264_rx_pending(void *);
static void *packet_alloc(void *p,int n) {
    struct wcam_rtp_tx *t=p;
    return n>0 && n<=(int)sizeof t->packet ? t->packet : NULL;
}
static void packet_free(void *p,void *b) { (void)p; (void)b; }
static int packet_send(void *p,const void *b,int n,uint32_t ts,int flags) {
    struct wcam_rtp_tx *t=p; int r;
    (void)ts; (void)flags; r=t->send(t->arg,b,(size_t)n);
    if(!r) { ++t->packets; t->octets+=(unsigned)n-12; } return r;
}
int wcam_rtp_tx_init(struct wcam_rtp_tx *t,uint32_t ssrc,uint16_t seq,wcam_packet_fn fn,void *arg) {
    struct rtp_payload_t h={packet_alloc,packet_free,packet_send};
    memset(t,0,sizeof *t); t->ssrc=ssrc; t->send=fn; t->arg=arg;
    t->packer=rtp_h264_encode()->create(WCAM_RTP_MTU,WCAM_RTP_PT,seq,ssrc,&h,t);
    return t->packer?0:-1;
}
void wcam_rtp_tx_destroy(struct wcam_rtp_tx *t) { if(t->packer) rtp_h264_encode()->destroy(t->packer); t->packer=NULL; }
int wcam_rtp_send(struct wcam_rtp_tx *t,const uint8_t *p,size_t n,uint32_t ts) {
    if(!p || n<5 || n>INT_MAX) return -1;
    t->timestamp=ts; return rtp_h264_encode()->input(t->packer,p,(int)n,ts);
}
void wcam_rtp_request_idr(struct wcam_rtp_rx *r) {
    if(!r->need_idr) ++r->feedback_epoch;
    r->need_idr=1;
}
static void abandon(struct wcam_rtp_rx *r) {
    if(r->have_ts) ++r->dropped;
    r->used=0; r->flags=0; r->damaged=1; wcam_rtp_request_idr(r);
    wcam_ir_h264_rx_buffer(r->unpacker,r->frame+4,(int)r->capacity-4,1);
}
void wcam_rtp_resync(struct wcam_rtp_rx *r) {
    abandon(r); r->have_ts=0; r->have_ssrc=0; r->gap=0;
    ++r->feedback_epoch; /* Stream restart needs a fresh decoder reference. */
    memset(r->slots,0,sizeof r->slots);
}
static int nal_ready(void *p,const void *data,int n,uint32_t ts,int flags) {
    struct wcam_rtp_rx *r=p; unsigned type=((const uint8_t*)data)[0]&31;
    (void)ts;
    if(flags || n<=0 || (size_t)n+4>r->capacity-r->used) return -1;
    if(data!=r->frame+r->used+4) memcpy(r->frame+r->used+4,data,(size_t)n);
    memcpy(r->frame+r->used,"\0\0\0\1",4); r->used+=(size_t)n+4;
    if(type==5) r->flags|=1;
    if(type==7) r->flags|=2;
    if(type==8) r->flags|=4;
    if(type>=1 && type<=5) r->flags|=8;
    return 0;
}
int wcam_rtp_rx_init(struct wcam_rtp_rx *r,uint8_t *b,size_t cap,wcam_frame_fn fn,void *arg) {
    struct rtp_payload_t h={0,0,nal_ready};
    if(cap<8 || cap>INT_MAX) return -1;
    memset(r,0,sizeof *r); r->frame=b; r->capacity=cap; r->ready=fn; r->arg=arg; r->need_idr=1;
    r->unpacker=rtp_h264_decode()->create(&h,r);
    if(!r->unpacker) return -1;
    wcam_ir_h264_rx_buffer(r->unpacker,b+4,(int)cap-4,1); return 0;
}
void wcam_rtp_rx_destroy(struct wcam_rtp_rx *r) { if(r->unpacker) rtp_h264_decode()->destroy(r->unpacker); r->unpacker=NULL; }
static void ordered(struct wcam_rtp_rx *r,const uint8_t *data,size_t n,uint32_t now) {
    struct rtp_packet_t p; int result;
    if(rtp_packet_deserialize(&p,data,(int)n)) { abandon(r); return; }
    if(!r->have_ts || p.rtp.timestamp!=r->timestamp) {
        if(r->have_ts) abandon(r); /* missing marker/end */
        r->timestamp=p.rtp.timestamp; r->frame_start=now;
        r->have_ts=1; r->used=0; r->flags=0; r->damaged=0;
        wcam_ir_h264_rx_buffer(r->unpacker,r->frame+4,(int)r->capacity-4,1);
    }
    if(!r->damaged) {
        if(r->capacity-r->used<5) { abandon(r); return; }
        wcam_ir_h264_rx_buffer(r->unpacker,r->frame+r->used+4,(int)(r->capacity-r->used-4),0);
        result=rtp_h264_decode()->input(r->unpacker,data,(int)n);
        if(result<=0) { ++r->invalid; abandon(r); }
    }
    if(p.rtp.m) {
        if(wcam_ir_h264_rx_pending(r->unpacker)) abandon(r);
        if(!r->damaged && r->used && (r->flags&8) && (!r->need_idr || (r->flags&7)==7)) {
            uint8_t *next;
            r->need_idr=0; ++r->frames;
            next=r->ready(r->arg,r->frame,r->used,r->timestamp);
            if(next) r->frame=next; else wcam_rtp_request_idr(r);
        } else { ++r->dropped; }
        r->have_ts=0; r->used=0;
        wcam_ir_h264_rx_buffer(r->unpacker,r->frame+4,(int)r->capacity-4,1);
    }
}
static void drain(struct wcam_rtp_rx *r,uint32_t now) {
    struct wcam_rtp_slot *s;
    while((s=&r->slots[r->next_seq%WCAM_RTP_WINDOW])->len && s->seq==r->next_seq) {
        ordered(r,s->data,s->len,now); s->len=0; ++r->next_seq; r->gap=0;
    }
    for(unsigned i=0;i<WCAM_RTP_WINDOW;i++) if(r->slots[i].len) {
        if(!r->gap) {r->gap=1; r->gap_start=now;} break;
    }
}
void wcam_rtp_receive(struct wcam_rtp_rx *r,const uint8_t *data,size_t n,uint32_t now) {
    struct rtp_packet_t p; struct wcam_rtp_slot *s; int16_t diff;
    if(n>WCAM_RTP_MTU || n<12 || rtp_packet_deserialize(&p,data,(int)n) ||
       p.rtp.pt!=WCAM_RTP_PT || p.payloadlen<1) { ++r->invalid; abandon(r); return; }
    if(!r->have_ssrc || r->ssrc!=p.rtp.ssrc || now-r->last_ms>=1000u) {
        wcam_rtp_resync(r); r->have_ssrc=1; r->ssrc=p.rtp.ssrc;
        r->next_seq=p.rtp.seq; r->highest=r->base_seq=p.rtp.seq;
        r->received=0; r->lost=0; r->last_sr=0; r->have_transit=0; r->jitter=0;
        r->expected_prior=0; r->received_prior=0;
    }
    diff=(int16_t)((uint16_t)p.rtp.seq-r->next_seq);
    if(diff<0) { ++r->late; return; }
    if(diff>0) {
        ++r->out_of_order;
        if(!r->gap) ++r->feedback_epoch; /* Prompt PLI, but allow reorder recovery. */
    }
    if(diff>=(int)WCAM_RTP_WINDOW) {
        r->lost+=(unsigned)diff; abandon(r); memset(r->slots,0,sizeof r->slots);
        r->next_seq=p.rtp.seq; r->gap=0;
    }
    s=&r->slots[p.rtp.seq%WCAM_RTP_WINDOW];
    if(s->len) { ++r->late; return; }
    s->seq=p.rtp.seq; s->len=(uint16_t)n; memcpy(s->data,data,n);
    ++r->received; r->last_ms=now;
    { int16_t ahead=(int16_t)((uint16_t)p.rtp.seq-(uint16_t)r->highest);
      if(ahead>0) r->highest+=(unsigned)ahead;
      uint32_t transit=now*90u-p.rtp.timestamp;
      if(r->have_transit) {
        uint32_t delta=transit-r->transit;
        uint64_t magnitude=delta>INT32_MAX?0x100000000ULL-delta:delta;
        uint64_t next=(uint64_t)r->jitter+magnitude-((uint64_t)r->jitter+8)/16;
        r->jitter=next>UINT32_MAX?UINT32_MAX:(uint32_t)next;
      }
      r->transit=transit; r->have_transit=1;
    }
    drain(r,now); wcam_rtp_poll(r,now);
}
void wcam_rtp_poll(struct wcam_rtp_rx *r,uint32_t now) {
    if(r->gap && now-r->gap_start>=WCAM_RTP_REORDER_MS) {
        unsigned nearest=WCAM_RTP_WINDOW;
        for(unsigned i=0;i<WCAM_RTP_WINDOW;i++) if(r->slots[i].len) {
            unsigned d=(uint16_t)(r->slots[i].seq-r->next_seq); if(d<nearest) nearest=d;
        }
        if(nearest<WCAM_RTP_WINDOW) { r->lost+=nearest; r->next_seq+=(uint16_t)nearest; abandon(r); }
        r->gap=0; drain(r,now);
    }
    if(r->have_ts && now-r->frame_start>=WCAM_RTP_FRAME_MS) { abandon(r); r->have_ts=0; }
}
/* Minimal compound RTCP: RFC3550 SR/RR + SDES CNAME, RFC4585 PLI.
 * Wire encoding follows the ireader rtcp-psfb/header implementation (MIT).
 * Timing/feedback policy belongs to the FreeRTOS adapter. */
static size_t sdes(uint8_t *p,uint32_t ssrc) {
    p[0]=0x81;p[1]=202;nbo_w16(p+2,4);nbo_w32(p+4,ssrc);
    p[8]=1;p[9]=8;
    for(unsigned i=0;i<8;i++) p[10+i]="0123456789abcdef"[(ssrc>>(28-4*i))&15];
    p[18]=p[19]=0;return 20;
}
size_t wcam_rtcp_report(uint8_t *p,size_t cap,uint32_t self,struct wcam_rtp_rx *r,int pli,uint32_t now) {
    size_t n=r->have_ssrc?32:8, total=n+20+(pli&&r->have_ssrc?12:0);
    if(cap<total) return 0;
    memset(p,0,total);p[0]=r->have_ssrc?0x81:0x80;p[1]=201;nbo_w16(p+2,(uint16_t)(n/4-1));nbo_w32(p+4,self);
    if(r->have_ssrc) {
        uint32_t expected=r->highest-r->base_seq+1, lost=expected>r->received?expected-r->received:0;
        nbo_w32(p+8,r->ssrc);if(lost>0x7fffff)lost=0x7fffff;
        nbo_w32(p+12,lost);
        uint32_t interval=expected-r->expected_prior, received=r->received-r->received_prior;
        if(interval && interval>received) p[12]=(uint8_t)(((uint64_t)(interval-received)*256/interval)>255?255:((uint64_t)(interval-received)*256/interval));
        r->expected_prior=expected; r->received_prior=r->received;
        nbo_w32(p+16,r->highest); nbo_w32(p+20,r->jitter/16);nbo_w32(p+24,r->last_sr);
        nbo_w32(p+28,r->last_sr?(uint32_t)((uint64_t)(now-r->last_sr_ms)*65536/1000):0);
    }
    n+=sdes(p+n,self);
    if(pli&&r->have_ssrc) {p[n]=0x81;p[n+1]=206;nbo_w16(p+n+2,2);nbo_w32(p+n+4,self);nbo_w32(p+n+8,r->ssrc);n+=12;}
    return n;
}
size_t wcam_rtcp_sender(uint8_t *p,size_t cap,const struct wcam_rtp_tx *t,uint32_t now) {
    if(cap<48)return 0;
    memset(p,0,48);p[0]=0x80;p[1]=200;nbo_w16(p+2,6);nbo_w32(p+4,t->ssrc);
    nbo_w32(p+8,now/1000);nbo_w32(p+12,(uint32_t)(((uint64_t)(now%1000)<<32)/1000));
    nbo_w32(p+16,now*90u);nbo_w32(p+20,t->packets);nbo_w32(p+24,t->octets);sdes(p+28,t->ssrc);return 48;
}
int wcam_rtcp_feedback(const uint8_t *p,size_t n,uint32_t target) {
    int pli=0;
    while(n) {
        size_t len;if(n<4 || p[0]>>6!=2 || p[0]&0x20)return -1;
        len=((size_t)nbo_r16(p+2)+1)*4;if(len>n || len<4)return -1;
        if(p[1]==206 && (p[0]&31)==1) { if(len!=12)return -1; if(nbo_r32(p+8)==target)pli=1; }
        p+=len;n-=len;
    }return pli;
}
void wcam_rtcp_receive_sr(struct wcam_rtp_rx *r,const uint8_t *p,size_t n,uint32_t now) {
    if(wcam_rtcp_feedback(p,n,0)<0)return;
    while(n>=4) {
        size_t len=((size_t)nbo_r16(p+2)+1)*4;
        if(p[1]==200 && len>=28 && r->have_ssrc && nbo_r32(p+4)==r->ssrc) {
            r->last_sr=(nbo_r32(p+8)<<16)|(nbo_r32(p+12)>>16);r->last_sr_ms=now;
        }p+=len;n-=len;
    }
}
