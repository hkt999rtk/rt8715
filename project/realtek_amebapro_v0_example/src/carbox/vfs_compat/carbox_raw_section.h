#ifndef CARBOX_RAW_SECTION_H
#define CARBOX_RAW_SECTION_H

#include <stdint.h>

int carbox_raw_section_read(uint32_t offset, void *buf, uint32_t len);
int carbox_raw_section_erase(uint32_t offset, uint32_t len);
int carbox_raw_section_erase_all(void);
int carbox_raw_section_write(uint32_t offset, const void *buf, uint32_t len);

void raw_test(void);

#endif
