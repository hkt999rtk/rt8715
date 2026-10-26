/* HTTP transport for Smart OTA packages. Flash layout/validation stays in compat. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <errno.h>
#include "carplay_ota_http.h"
#include "wifi_action_protocol.h"
#ifdef ACTION_HOST_TEST
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <time.h>
#define OTA_SOCKET_ERRNO errno
#define pvPortMalloc malloc
#define vPortFree free
#else
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/errno.h"
#define OTA_SOCKET_ERRNO lwip_err
#endif

static uint32_t http_now_ms(void)
{
#ifdef ACTION_HOST_TEST
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint32_t)((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000);
#else
    return (uint32_t)xTaskGetTickCount()*portTICK_PERIOD_MS;
#endif
}

int carbox_ota_tcp_connect(const char *host, int port)
{
    struct sockaddr_in sa;
    struct hostent *he;
    int fd, ret;
    uint32_t started=http_now_ms();
#if LWIP_SO_SNDRCVTIMEO_NONSTANDARD
    int timeout=15000;
#else
    struct timeval timeout={15,0};
#endif
    if(!host || port<1 || port>65535) return -1;
    memset(&sa,0,sizeof(sa)); sa.sin_family=AF_INET; sa.sin_port=htons(port);
    printf("[ACTION_OTA][TCP] resolve/connect host=%s port=%d start_ms=%lu\n",host,port,(unsigned long)started);
    he=gethostbyname(host);
    if(!he || he->h_addrtype!=AF_INET || he->h_length!=4) {
        printf("[ACTION_OTA][TCP] DNS failed host=%s errno=%d\n",host,OTA_SOCKET_ERRNO); return -1;
    }
    memcpy(&sa.sin_addr,he->h_addr_list[0],4);
    fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0) {printf("[ACTION_OTA][TCP] socket failed errno=%d\n",OTA_SOCKET_ERRNO); return -1;}
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    ret=connect(fd,(struct sockaddr *)&sa,sizeof(sa));
    printf("[ACTION_OTA][TCP] connect result=%d errno=%d elapsed_ms=%lu\n",
           ret,ret<0?OTA_SOCKET_ERRNO:0,(unsigned long)(http_now_ms()-started));
    if(ret<0) { close(fd); return -1; }
    return fd;
}

int carbox_ota_send_all(int fd, const void *data, size_t len)
{
    const char *p=data;
    while(len) {
        int n=send(fd,p,len,0);
        if(n<=0) {printf("[ACTION_OTA][TCP] send failed ret=%d errno=%d remaining=%u\n",n,OTA_SOCKET_ERRNO,(unsigned)len);return -1;}
        p+=n; len-=(size_t)n;
    }
    return 0;
}

static int content_length(char *header, uint32_t *length)
{
    char *p=strstr(header,"\r\n");
    int found=0;
    if((strncmp(header,"HTTP/1.0 200 ",13) && strncmp(header,"HTTP/1.1 200 ",13)) || !p) return -1;
    p+=2;
    while(*p) {
        char *end=strstr(p,"\r\n"), *value;
        uint32_t n=0;
        if(!end) return -1;
        if(end==p) break;
        *end=0;
        if(!strncasecmp(p,"Transfer-Encoding:",18)) return -1;
        if(!strncasecmp(p,"Content-Encoding:",17)) return -1;
        if(!strncasecmp(p,"Content-Length:",15)) {
            if(found++) return -1;
            value=p+15; while(*value==' ' || *value=='\t') value++;
            if(*value<'0' || *value>'9') return -1;
            while(*value>='0' && *value<='9') {
                unsigned digit=(unsigned)(*value++-'0');
                if(n>(UINT32_MAX-digit)/10) return -1;
                n=n*10+digit;
            }
            while(*value==' ' || *value=='\t') value++;
            if(*value || !n) return -1;
            *length=n;
        }
        p=end+2;
    }
    return found?0:-1;
}

/* Return 1 for a valid absolute HTTP 302 Location, 0 for other responses. */
static int redirect_target(char *header, struct action_url *target)
{
    char *p;
    if(strncmp(header,"HTTP/1.0 302 ",13) && strncmp(header,"HTTP/1.1 302 ",13)) return 0;
    p=strstr(header,"\r\n");
    if(!p) return -1;
    p+=2;
    while(*p) {
        char *end=strstr(p,"\r\n"), *value;
        if(!end || end==p) break;
        if(!strncasecmp(p,"Location:",9)) {
            *end=0; value=p+9;
            while(*value==' ' || *value=='\t') value++;
            while(end>value && (end[-1]==' ' || end[-1]=='\t')) *--end=0;
            return action_parse_url(value,target)?-1:1;
        }
        p=end+2;
    }
    return -1;
}

int carbox_ota_http_get(const char *host, int port, const char *path,
    int (*begin)(void *,uint32_t), int (*data)(void *,const uint8_t *,uint32_t), void *ctx)
{
    char *buf=pvPortMalloc(4097), *body;
    uint32_t total=0, received=0;
    size_t used=0, body_bytes;
    int fd=-1, ret=-1, n, redirected=0;
    struct action_url target;
    const char *phase="arguments";
    uint32_t started=http_now_ms();
    unsigned progress=0;
    int io_result=0, io_error=0, header_ret;
    if(!buf) {printf("[ACTION_OTA][HTTP] buffer allocation failed\n");return -1;}
    if(!host || !path || strpbrk(host,"\r\n") || strpbrk(path,"\r\n")) goto out;
request:
    used=0;
    n=snprintf(buf,4097,"GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n",path,host,port);
    if(n<0 || n>4096) goto out;
    phase="connect";
    fd=carbox_ota_tcp_connect(host,port);
    if(fd<0) goto out;
    phase="request_send";
    if(carbox_ota_send_all(fd,buf,(size_t)n)) goto out;
    phase="response_header";
    for(;;) {
        if(used==4096) goto out;
        n=recv(fd,buf+used,4096-used,0);
        if(n<=0) {io_result=n;io_error=n<0?OTA_SOCKET_ERRNO:0;goto out;}
        used+=(size_t)n; buf[used]=0;
        body=strstr(buf,"\r\n\r\n");
        if(body) break;
    }
    printf("[ACTION_OTA][HTTP] status_line=%.*s header_bytes=%u\n",
           (int)((strstr(buf,"\r\n")-buf)>72?72:(strstr(buf,"\r\n")-buf)),buf,
           (unsigned)(body+4-buf));
    phase="redirect";
    n=redirect_target(buf,&target);
    if(n<0) goto out;
    if(n>0) {
        if(redirected++) goto out;
        close(fd); fd=-1;
        host=target.host; port=target.port; path=target.path;
        printf("[ACTION_OTA] HTTP redirect host=%s port=%d path=%s\n",host,port,path);
        goto request;
    }
    body+=4; body_bytes=used-(size_t)(body-buf);
    /* Parse headers only; never treat firmware bytes as header fields. */
    phase="header_validation";
    header_ret=content_length(buf,&total);
    if(header_ret || body_bytes>total) {
        printf("[ACTION_OTA][HTTP] invalid header ret=%d declared=%lu initial_body=%u\n",
               header_ret,(unsigned long)total,(unsigned)body_bytes);
        goto out;
    }
    phase="package_begin";
    if(begin(ctx,total)) goto out;
    printf("[ACTION_OTA] HTTP status=200 content_length=%lu\n",(unsigned long)total);
    phase="package_write";
    if(body_bytes && data(ctx,(uint8_t *)body,(uint32_t)body_bytes)) goto out;
    received=(uint32_t)body_bytes;
    while(received<total) {
        uint32_t wanted=total-received;
        if(wanted>4096) wanted=4096;
        phase="body_receive";
        n=recv(fd,buf,wanted,0);
        if(n<=0) {io_result=n;io_error=n<0?OTA_SOCKET_ERRNO:0;goto out;}
        phase="package_write";
        if(data(ctx,(uint8_t *)buf,(uint32_t)n)) goto out;
        received+=(uint32_t)n;
        if((uint64_t)received*100/total>=progress+10) {
            progress=(unsigned)((uint64_t)received*100/total);
            printf("[ACTION_OTA][HTTP] progress=%u%% bytes=%lu/%lu elapsed_ms=%lu\n",
                   progress,(unsigned long)received,(unsigned long)total,
                   (unsigned long)(http_now_ms()-started));
        }
    }
    ret=0; phase="complete";
out:
    if(fd>=0) close(fd);
    vPortFree(buf);
    printf("[ACTION_OTA][HTTP] result=%d phase=%s io_result=%d errno=%d "
           "received=%lu expected=%lu elapsed_ms=%lu\n",ret,phase,io_result,io_error,
           (unsigned long)received,(unsigned long)total,(unsigned long)(http_now_ms()-started));
    return ret;
}
