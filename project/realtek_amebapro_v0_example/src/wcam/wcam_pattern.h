#ifndef WCAM_PATTERN_H
#define WCAM_PATTERN_H

#include <stdint.h>

#define WCAM_WIDTH 1280u
#define WCAM_HEIGHT 720u
#define WCAM_Y_SIZE (WCAM_WIDTH * WCAM_HEIGHT)
#define WCAM_NV12_SIZE (WCAM_Y_SIZE * 3u / 2u)
#define WCAM_PATTERN_LOCAL 0
#define WCAM_PATTERN_TX 1
#define WCAM_PATTERN_RX 2

/* Caller supplies WCAM_NV12_SIZE bytes, with encoder-required alignment. */
void wcam_pattern_init(uint8_t *nv12);
void wcam_pattern_counter(uint8_t *nv12, uint32_t counter);
void wcam_pattern_init_role(uint8_t *nv12, int role);
void wcam_pattern_counter_role(uint8_t *nv12, uint32_t counter, int role);

#endif
