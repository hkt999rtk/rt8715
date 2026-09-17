/* OpenSSL vectors plus real RX core/adapters. Handover and socket boundaries
 * are modeled here; DMA/cache/IRQ behavior requires a board test. */
#include "ChaCha20Poly1305.h"
#include "screen_rx_poly_buffer.h"
#include "task.h"
#include <assert.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Compile adapter implementation into this test to exercise private lifetime
 * tables as well as the exact public entry points used by scoped relocations. */
#include "../../../../src/carbox/rx_crypto_buffers.c"

TaskHandle_t rx_poly_test_task = (void *)(uintptr_t)1;
void mock_rtl_reset_stats(void);
void mock_rtl_fail_chacha_on(unsigned);
void mock_rtl_fail_chacha_after_write_on(unsigned);
void mock_rtl_fail_poly1305_on(unsigned);
void mock_rtl_fail_aad_snapshot_once(void);
unsigned mock_rtl_chacha_operations(void);
unsigned mock_rtl_poly1305_operations(void);
unsigned mock_rtl_transaction_active(void);
const void *mock_rtl_last_chacha_input(void);
void *mock_rtl_last_chacha_output(void);
void mock_rtl_recovery_disabled(unsigned);
void mock_rtl_fail_combined_after_write(unsigned);

static uint8_t key[32]={1}, nonce[8]={2}, aad[128];
static const uint8_t *expected_plain;
static size_t expected_len;
static void *queued;
static int fail_alloc, consume_early, queue_error;
static unsigned producer_frees;
static unsigned fail_next_malloc;
void *__real_malloc(size_t n);
void *__wrap_malloc(size_t n) {
    if (fail_next_malloc) { --fail_next_malloc; return NULL; }
    return __real_malloc(n);
}

void *carbox_video_handover_source_malloc(size_t n) {
    if (fail_alloc) return NULL;
    return carbox_screen_rx_poly_alloc(n);
}
void carbox_video_handover_producer_free(void *p) {
    ++producer_frees;
    if (p != queued) carbox_screen_rx_poly_free(p);
}
int ScreenStreamProcessData(void *stream, const void *p, size_t n, uintptr_t r3,
                            uintptr_t s0, uintptr_t s1, uintptr_t s2,
                            uintptr_t s3, uintptr_t s4) {
    assert(stream == (void *)99 && r3 == 3 && s0 == 4 && s1 == 5 &&
           s2 == 6 && s3 == 7 && s4 == 8);
    assert(n == expected_len && !memcmp(p,expected_plain,n));
    if (!consume_early && !queue_error) queued = (void *)p;
    return queue_error ? -1 : 0;
}
static void vector_aad(uint8_t *cipher, uint8_t *plain, size_t n, const void *ad, size_t alen) {
    uint8_t iv[12]={0};
    int out,tail;
    EVP_CIPHER_CTX *ctx=EVP_CIPHER_CTX_new();
    assert(ctx);
    for(size_t i=0;i<n;++i) plain[i]=(uint8_t)(i*37+11);
    memcpy(iv+4,nonce,8);
    assert(EVP_EncryptInit_ex(ctx,EVP_chacha20_poly1305(),NULL,key,iv)==1);
    assert(EVP_EncryptUpdate(ctx,NULL,&out,ad,(int)alen)==1);
    assert(EVP_EncryptUpdate(ctx,cipher,&out,plain,(int)n)==1);
    assert(EVP_EncryptFinal_ex(ctx,cipher+out,&tail)==1);
    assert(EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_AEAD_GET_TAG,16,cipher+n)==1);
    EVP_CIPHER_CTX_free(ctx);
}
static void vector(uint8_t *cipher, uint8_t *plain, size_t n, size_t alen) {
    vector_aad(cipher,plain,n,aad,alen);
}
static void fault(unsigned f) {
    mock_rtl_reset_stats();
    if(f==1) mock_rtl_fail_chacha_on(1); /* Poly key */
    if(f==2) mock_rtl_fail_poly1305_on(1);
    if(f==3) mock_rtl_fail_chacha_after_write_on(2); /* payload */
    if(f==4) {mock_rtl_recovery_disabled(1); mock_rtl_fail_chacha_after_write_on(2);}
    if(f==5) mock_rtl_fail_combined_after_write(1);
}
static void direct_case(size_t n,size_t alen,unsigned offset,unsigned f,int bad,int managed) {
    uint8_t *plain=malloc(n+1), *a0, *b0, *a,*b,*saved=malloc(n+16);
    chacha20_poly1305_state state;
    int32_t error;
    if(managed) {
        a0=a=carbox_screen_rx_poly_alloc(n+16);
        b0=b=carbox_screen_rx_poly_alloc(n+16);
    } else {
        a0=malloc(n+64); b0=malloc(n+64); a=a0+offset;b=b0+offset;
    }
    assert(plain&&a&&b&&saved);
    memset(b,0xa5,n+16);
    vector(a,plain,n,alen);
    if(bad) a[n+3]^=1;
    memcpy(saved,a,n+16);
    fault(f);
    chacha20_poly1305_init_64x64(&state,key,nonce);
    if (f==6) mock_rtl_fail_aad_snapshot_once();
    chacha20_poly1305_add_aad(&state,aad,alen);
    if (alen==8) assert(carbox_audio_general_decrypt(&state,a,n,b)==n);
    else assert(chacha20_poly1305_decrypt_rx(&state,a,n,b,0,1)==n);
    assert(b[0]==0xa5); /* registration must not stage a payload */
    if (alen==8) assert(carbox_audio_general_verify(&state,b+n,a+n,&error)==0);
    else assert(chacha20_poly1305_verify(&state,b+n,a+n,&error)==0);
    assert((error!=0)==bad);
    assert(!memcmp(a,saved,n+16)); /* including tag after partial DMA failure */
    if(!bad) assert(!memcmp(b,plain,n));
    assert(!mock_rtl_transaction_active());
    assert(!chacha20_poly1305_take_rx_nonce_resync(&state));
    if(managed && n==5265 && !f) {
        assert(mock_rtl_chacha_operations()==2);
        assert(mock_rtl_poly1305_operations()==1);
        assert(mock_rtl_last_chacha_input()==a);
        assert(mock_rtl_last_chacha_output()==b);
    }
    if(n<1024) assert(!mock_rtl_chacha_operations()&&!mock_rtl_poly1305_operations());
    if(!managed) {assert(b[n]==0xa5);free(a0);free(b0);}
    else {carbox_screen_rx_poly_free(a0);carbox_screen_rx_poly_free(b0);}
    free(saved);free(plain);
}
static void screen_case(unsigned f,int bad,int allocation_fail,int early,int qerr) {
    size_t n=5265;
    uint8_t *a=carbox_screen_rx_poly_alloc(n+16),*plain=malloc(n),*saved=malloc(n+16);
    chacha20_poly1305_state state;
    int32_t error;
    unsigned frees=producer_frees;
    vector(a,plain,n,128);
    if(bad) a[n]^=1;
    memcpy(saved,a,n+16);
    fault(f);fail_alloc=allocation_fail;consume_early=early;queue_error=qerr;
    chacha20_poly1305_init_64x64(&state,key,nonce);
    chacha20_poly1305_add_aad(&state,aad,128);
    assert(carbox_screen_rx_decrypt(&state,a,n,a)==n);
    assert(carbox_screen_rx_verify(&state,a+n,a+n,&error)==0);
    assert((error!=0)==bad);
    if(!allocation_fail) assert(!memcmp(a,saved,n+16));
    expected_plain=plain;expected_len=n;
    if(!bad) assert(carbox_screen_rx_process((void*)99,a,n,3,4,5,6,7,8)==(qerr?-1:0));
    carbox_screen_rx_free(a);
    assert(producer_frees-frees==(allocation_fail?1:2));
    if(queued) {assert(!memcmp(queued,plain,n));carbox_screen_rx_poly_free(queued);queued=NULL;}
    for(unsigned i=0;i<RX_SLOTS;++i) assert(!screens[i].a);
    free(saved);free(plain);fail_alloc=0;queue_error=0;consume_early=0;
}
static uint8_t node[1536], context[2048], packet[1472];
static size_t packet_len;
static unsigned busy_count,free_count;
int RTPJitterBufferGetFreeNode(void *jitter,void **p) {assert(jitter==context+48);*p=node;return 0;}
void RTPJitterBufferPutFreeNode(void *jitter,void *p) {assert(jitter==context+48 && p==node);++free_count;}
int RTPJitterBufferPutBusyNode(void *jitter,void *p) {
    assert(jitter==context+48 && p==node);
    assert(!memcmp(node+20,expected_plain,expected_len));++busy_count;return queue_error?-1:0;
}
int SocketRecvFrom(int fd,void *buf,size_t cap,size_t *n,uintptr_t s0,uintptr_t s1,
                   uintptr_t s2,uintptr_t s3,uintptr_t s4,uintptr_t s5) {
    assert(fd==7 && cap==1472 && s0==10 && s1==11 && s2==12 && s3==13 && s4==14 && s5==15);
    memcpy(buf,packet,packet_len);*n=packet_len;return 0;
}
static void audio_case(size_t n,size_t alen,unsigned f,int bad,int truncated) {
    void *p;size_t received;
    uint8_t plain[1436];chacha20_poly1305_state state;int32_t error;
    context[1432]=1; memset(node,0xa5,sizeof(node));
    vector_aad(packet+12,plain,n,aad+12-alen,alen);memcpy(packet,aad,12);memcpy(packet+12+n+16,nonce,8);
    packet_len=n+36; if(bad) packet[n+12]^=1;
    if(truncated) packet_len=20;
    fault(f);
    assert(!carbox_audio_get_node(context+48,&p));
    if(f==7) fail_next_malloc=1; /* allocation A fails before socket receive */
    int recv=carbox_audio_recv(7,node+8,1472,&received,10,11,12,13,14,15);
    if(truncated) {assert(recv!=0);carbox_audio_put_free(context+48,p);return;}
    assert(!recv);
    assert(!memcmp(node+8,packet,12));
    if(f!=7) assert(node[20]==0xa5); /* no payload staged in B */
    chacha20_poly1305_init_64x64(&state,key,node+8+received-8);
    chacha20_poly1305_add_aad(&state,node+8+(12-alen),alen);
    /* For 8-byte AAD use the last 8 RTP header bytes in vector too. */
    assert(carbox_audio_main_decrypt(&state,node+20,n,node+20)==n);
    assert(carbox_audio_main_verify(&state,node+20+n,node+20+n,&error)==0);
    assert((error!=0)==bad);
    if(f==7) assert(!mock_rtl_chacha_operations() && !mock_rtl_poly1305_operations());
    if(!bad) {
        expected_plain=plain;expected_len=n;queue_error=(f==8);
        int result=carbox_audio_put_busy(context+48,p);
        if(f==8) {assert(result!=0);carbox_audio_put_free(context+48,p);}
        else assert(!result);
        queue_error=0;
    }
    else carbox_audio_put_free(context+48,p);
    for(unsigned i=0;i<RX_SLOTS;++i) assert(!audios[i].node);
}
static void interleaved(void) {
    uint8_t *a[9], *plain=malloc(5265);chacha20_poly1305_state state[9];int32_t error;
    for(unsigned i=0;i<9;++i) {
        rx_poly_test_task=(void*)(uintptr_t)(i+1);a[i]=carbox_screen_rx_poly_alloc(5281);
        vector(a[i],plain,5265,128);
        chacha20_poly1305_init_64x64(&state[i],key,nonce);
        chacha20_poly1305_add_aad(&state[i],aad,128);
        carbox_screen_rx_decrypt(&state[i],a[i],5265,a[i]);
    }
    for(unsigned i=0;i<9;++i) {
        rx_poly_test_task=(void*)(uintptr_t)(i+1);
        carbox_screen_rx_verify(&state[i],a[i]+5265,a[i]+5265,&error);assert(!error);
        carbox_screen_rx_free(a[i]);
    }
    rx_poly_test_task=(void*)1;free(plain);
}
static void alias_case(void) {
    uint8_t a[1120], b[1101], plain[1101];
    chacha20_poly1305_state state;
    uint8_t *persistent_key=(uint8_t *)&state+264;
    uint8_t *persistent_nonce=(uint8_t *)&state+200;
    int32_t error;
    for(unsigned record=0;record<2;++record) {
        vector(a,plain,sizeof(b),8);
        memcpy(persistent_key,key,32);memcpy(persistent_nonce,nonce,8);
        fault(record==0?3:0);
        chacha20_poly1305_init_64x64(&state,persistent_key,persistent_nonce);
        chacha20_poly1305_add_aad(&state,aad,8);
        carbox_audio_general_decrypt(&state,a,sizeof(b),b);
        carbox_audio_general_verify(&state,b+sizeof(b),a+sizeof(b),&error);
        assert(!error && !memcmp(b,plain,sizeof(b)));
        assert(!memcmp(persistent_key,key,32));assert(!memcmp(persistent_nonce,nonce,8));
        ++nonce[0]; /* caller increments once, after successful verify */
    }
}
int main(void) {
    for(unsigned i=0;i<sizeof(aad);++i) aad[i]=(uint8_t)(i*13+7);
    size_t lengths[]={0,1,63,1023,1024,1025,1436,4096,4413,5265,65391,65409,65536,65537};
    for(unsigned k=0;k<sizeof(lengths)/sizeof(*lengths);++k)
        for(unsigned offset=0;offset<16;++offset)
            direct_case(lengths[k],offset%2?8:128,offset,0,0,0);
    for(unsigned f=0;f<=4;++f) for(int bad=0;bad<2;++bad) {
        direct_case(5265,128,0,f,bad,1);
        direct_case(1101,8,1,f,bad,0);
        screen_case(f,bad,0,0,0);
        audio_case(1101,8,f,bad,0);
        audio_case(1101,12,f,bad,0);
        ++nonce[0]; /* following record uses exactly the caller's next nonce */
        direct_case(5265,128,0,0,0,1);
    }
    audio_case(1101,8,7,0,0);audio_case(1101,12,7,1,0);
    audio_case(1101,8,8,0,0);
    direct_case(200,8,0,0,1,0);
    alias_case();
    direct_case(5265,128,0,6,0,0);
    direct_case(4096,128,0,5,0,0);direct_case(4096,128,0,5,1,0);
    screen_case(0,0,1,1,0);screen_case(3,0,0,1,0);screen_case(3,0,0,0,1);
    audio_case(200,8,0,0,0);audio_case(200,12,0,0,1);
    mock_rtl_reset_stats();interleaved();
    carbox_screen_rx_free(malloc(72)); /* unrelated context passthrough */
    puts("RX recovery core + screen/general/main-alt adapters: PASS");
    return 0;
}
