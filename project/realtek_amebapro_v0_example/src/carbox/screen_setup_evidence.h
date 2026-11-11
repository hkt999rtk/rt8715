#ifndef CARBOX_SCREEN_SETUP_EVIDENCE_H
#define CARBOX_SCREEN_SETUP_EVIDENCE_H
#include <stdint.h>
#include <stddef.h>
struct screen_nal_summary {uint32_t count,bad,prefix3,types,first_length,second_length;};
void screen_nal_scan(const void *,size_t,struct screen_nal_summary *);
void screen_info_http_fields(const char *,size_t,unsigned *,unsigned *);
void carbox_screen_encrypt_observe(void *header,void *data,int length);
void carbox_screen_session_event(unsigned action,unsigned generation,int fd,int result,unsigned state);
void carbox_screen_setup_report(uint32_t sequence);
#endif
