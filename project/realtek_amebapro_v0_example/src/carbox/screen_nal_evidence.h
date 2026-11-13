#ifndef SCREEN_NAL_EVIDENCE_H
#define SCREEN_NAL_EVIDENCE_H
#include <stdint.h>
#include <stddef.h>
void carbox_screen_nal_observe(unsigned stage,const void *data,size_t bytes);
void carbox_screen_nal_report(uint32_t sequence);
#endif
