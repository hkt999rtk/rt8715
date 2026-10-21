#include <stddef.h>
#include <stdint.h>
#include <string.h>
void * __wrap_memcpy(void *d,const void *s,size_t n) {return memcpy(d,s,n);}
void * __wrap_memset(void *d,int c,size_t n) {return memset(d,c,n);}
void carbox_screen_rx_crypto_init(void) {}
void carbox_screen_rx_crypto_aad(size_t n) {(void)n;}
void carbox_screen_rx_crypto_decrypt(size_t a,size_t b) {(void)a;(void)b;}
void carbox_screen_rx_crypto_verify(size_t a,const int32_t *b) {(void)a;(void)b;}
void carbox_screen_rx_crypto_final(size_t n) {(void)n;}
