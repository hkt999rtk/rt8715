#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifi_action_protocol.h"
static unsigned char packet[512];
static size_t add(size_t n, int type, const char *s) {
    size_t k = strlen(s)+1; packet[n++]=type; packet[n++]=k;
    memcpy(packet+n,s,k); return n+k;
}
int main(void) {
    struct action_request *r=malloc(action_request_bytes(sizeof(packet)));
    assert(r);
    struct action_url u;
    size_t n=39, end, i;
    packet[0]=0xd0; packet[1]=1;
    memset(packet+4,255,6); memset(packet+16,255,6);
    memcpy(packet+24,"\xc8OTA\xdd\x09litiantai",15);
    n=add(n,0x10,"sukn"); n=add(n,0x11,"suknlovken");
    n=add(n,0x12,"http://192.168.3.10:8080/20260523161414_2DFE_ota_all.bin");
    n=add(n,0x13,"20260523161414_2DFE"); end=n;
    assert(action_parse_frame(packet,n,r)==1);
    assert(r->ap_count==1 && !strcmp(r->ap[0].ssid,"sukn"));
    assert(!strcmp(r->ap[0].password,"suknlovken") && r->ap[0].secured);
    assert(!strcmp(r->version,"20260523161414_2DFE"));
    assert(action_parse_url(r->url,&u)==0 && u.port==8080);
    assert(!strcmp(u.host,"192.168.3.10"));
    assert(!strcmp(u.path,"/20260523161414_2DFE_ota_all.bin"));
    memset(packet+n,0,16); assert(action_parse_frame(packet,n+16,r)==1);
    for(i=0;i<39;i++) assert(action_parse_frame(packet,i,r)!=1);
    assert(action_parse_frame(packet,end-1,r)==-1);
    packet[24]=4; assert(action_parse_frame(packet,end,r)==0); packet[24]=0xc8;
    packet[30]='X'; assert(action_parse_frame(packet,end,r)==0); packet[30]='l';
    packet[40]=255; assert(action_parse_frame(packet,end,r)==-1);
    assert(r->parse_offset==39 && r->parse_type==0x10 && r->parse_length==255); packet[40]=5;
    packet[45]='X'; assert(action_parse_frame(packet,end,r)==-1); packet[45]=0;
    n=39; n=add(n,0x10,"first"); n=add(n,0x10,"second"); n=add(n,0x11,"12345678");
    assert(action_parse_frame(packet,n,r)==1 && r->ap_count==2);
    /* Smart inserts at list head: last AP has random index zero. */
    assert(!strcmp(r->ap[0].ssid,"second") && r->ap[0].secured && !r->ap[1].secured);
    assert(action_parse_url("http://192.168.3.10/a",&u)==0 && u.port==80);
    assert(action_parse_url("https://192.168.3.10/a",&u)<0);
    assert(action_parse_url("http://x:99999/a",&u)<0);
    assert(action_parse_url("http://x/a\r\nInjected: yes",&u)<0);
    n=39; for(i=0;i<33;i++) n=add(n,0x10,"ap");
    assert(action_parse_frame(packet,n,r)==1 && r->ap_count==33);
    free(r);
    puts("action protocol: PASS");
}
