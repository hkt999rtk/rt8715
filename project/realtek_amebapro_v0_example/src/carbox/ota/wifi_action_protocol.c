#include "wifi_action_protocol.h"
#include <string.h>

size_t action_request_bytes(size_t n)
{
    if(n<39 || n>ACTION_FRAME_MAX) return 0;
    /* Smallest nonempty SSID TLV: type, length, one char, NUL. */
    return sizeof(struct action_request)+(n-39)/4*sizeof(struct action_ap);
}

static int copy_text(char *dst, size_t cap, const uint8_t *p, size_t n)
{
    if (!n || n > cap || p[n-1] != 0 || memchr(p,0,n-1)) return -1;
    memcpy(dst,p,n);
    return 0;
}

int action_parse_frame(const uint8_t *p, size_t n, struct action_request *r)
{
    static const uint8_t marker[] = {0xc8,'O','T','A',0xdd,9,
                                   'l','i','t','i','a','n','t','a','i'};
    size_t pos=39;
    int previous=-1;
    if (!p || !r || n<39 || (p[0]&0xfc)!=0xd0 ||
        memcmp(p+24,marker,sizeof(marker))) return 0;
    if(n>ACTION_FRAME_MAX) return -1;
    memset(r,0,sizeof(*r));
    while(pos<n) {
        unsigned type, len;
        /* Sender pads the Action body with zeros. */
        if(!p[pos]) {
            size_t i; for(i=pos;i<n;i++) if(p[i]) break;
            if(i==n) break;
        }
        r->parse_offset=(unsigned)pos; r->parse_type=p[pos]; r->parse_length=0;
        if(n-pos<2) return -1;
        type=p[pos++]; len=p[pos++]; r->parse_length=len;
        if(len>n-pos) return -1;
        if(type==0x10) {
            if(len<=1) return -1;
            memmove(r->ap+1,r->ap,r->ap_count*sizeof(r->ap[0]));
            memset(r->ap,0,sizeof(r->ap[0]));
            if(copy_text(r->ap[0].ssid,sizeof(r->ap[0].ssid),p+pos,len)) return -1;
            r->ap_count++;
        } else if(type==0x11) {
            if(previous!=0x10 || !r->ap_count || len<9 ||
               copy_text(r->ap[0].password,sizeof(r->ap[0].password),p+pos,len)) return -1;
            r->ap[0].secured=1;
        } else if(type==0x12) {
            if(copy_text(r->url,sizeof(r->url),p+pos,len)) return -1;
        } else if(type==0x13) {
            if(copy_text(r->version,sizeof(r->version),p+pos,len)) return -1;
        }
        previous=(int)type;
        pos+=len;
    }
    return 1;
}

int action_parse_url(const char *url, struct action_url *out)
{
    const char *host, *path, *colon, *p;
    size_t len;
    if(!url || !out || strncmp(url,"http://",7)) return -1;
    for(p=url;*p;p++) if((unsigned char)*p<=32 || *p==127) return -1;
    host=url+7; path=strchr(host,'/');
    if(!path || path==host || strlen(path)>=sizeof(out->path)) return -1;
    colon=memchr(host,':',(size_t)(path-host));
    len=(size_t)((colon?colon:path)-host);
    if(!len || len>=sizeof(out->host)) return -1;
    memcpy(out->host,host,len); out->host[len]=0;
    strcpy(out->path,path); out->port=80;
    if(colon) {
        unsigned port=0;
        if(colon+1==path) return -1;
        for(p=colon+1;p<path;p++) {
            if(*p<'0'||*p>'9') return -1;
            port=port*10+(unsigned)(*p-'0'); if(port>65535) return -1;
        }
        if(!port) return -1;
        out->port=(int)port;
    }
    return 0;
}
