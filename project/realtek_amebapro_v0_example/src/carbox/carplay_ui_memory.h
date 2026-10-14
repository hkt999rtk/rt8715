#ifndef CARPLAY_UI_MEMORY_H
#define CARPLAY_UI_MEMORY_H
#include <stdint.h>
#include <stddef.h>
void RGB2YUV(uint8_t **rgb, uint8_t **yuv, int bpp, int width, int height);
int carbox_ui_inplace(uint8_t *image, int bpp, int width, int height);
typedef struct {
    size_t bytes[3], stride[3], offset[3], total;
} carbox_ui_layout;
int carbox_ui_layout_init(carbox_ui_layout *l, int width, int height);
void carbox_ui_pad(uint8_t *image, const carbox_ui_layout *l, int width, int height);
typedef struct {
    unsigned sps_id, pps_id, frame_bits, poc_bits, mb_count;
    unsigned width, height, have_sps, have_pps, cabac;
} carbox_ui_skip;
int carbox_ui_read_headers(carbox_ui_skip *s, const uint8_t *nal, size_t size);
size_t carbox_ui_skip_cabac(const carbox_ui_skip *s, unsigned frame,
                           uint8_t *dst, size_t capacity);
#endif
