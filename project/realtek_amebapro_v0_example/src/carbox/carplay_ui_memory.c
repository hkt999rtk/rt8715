/* RGB565/BGR888 conversion, kept byte compatible with the vendor UI. */
#include "carplay_ui_memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static inline void ui_read_rgb(const uint8_t *src, unsigned int bpp,
			       int *r, int *g, int *b)
{
	if (bpp == 2U) {
		uint16_t pixel = (uint16_t)src[0] | ((uint16_t)src[1] << 8);
		unsigned int red = (pixel >> 11) & 31U;
		unsigned int green = (pixel >> 5) & 63U;
		unsigned int blue = pixel & 31U;
		*r = (int)((red << 3) | (red >> 2));
		*g = (int)((green << 2) | (green >> 4));
		*b = (int)((blue << 3) | (blue >> 2));
	} else {
		*b = src[0];
		*g = src[1];
		*r = src[2];
	}
}

/* Accessory.o uses BT.601 limited range and samples chroma at each 2x2
 * block's top-left pixel.  Keep those coefficients and sampling positions. */
void RGB2YUV(uint8_t **rgb_buffer, uint8_t **yuv_buffer,
	     int bytes_per_pixel, int width, int height)
{
	uint8_t *y;
	uint8_t *u;
	uint8_t *v;
	const uint8_t *rgb;
	size_t stride;
	size_t luma;

	if (rgb_buffer == NULL || yuv_buffer == NULL ||
	    *rgb_buffer == NULL || *yuv_buffer == NULL ||
	    (bytes_per_pixel != 2 && bytes_per_pixel != 3) ||
	    width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
		printf("[UI565] invalid RGB2YUV args bpp=%d size=%dx%d\r\n",
		       bytes_per_pixel, width, height);
		return;
	}
	/* The vendor Accessory.o converter reads RGB rows at CSurface pitch. */
	stride = ((size_t)(width + 15) & ~(size_t)15) * (size_t)bytes_per_pixel;
	luma = (size_t)width * (size_t)height;
	rgb = *rgb_buffer;
	y = *yuv_buffer;
	u = y + luma;
	v = u + luma / 4U;
	for (int row = 0; row < height; row += 2) {
		const uint8_t *top = rgb + (size_t)row * stride;
		const uint8_t *bottom = top + stride;
		for (int col = 0; col < width; col += 2) {
			int r, g, b;
			size_t y_pos = (size_t)row * (size_t)width + (size_t)col;
			size_t uv_pos = (size_t)(row / 2) * (size_t)(width / 2)
					+ (size_t)(col / 2);
			const uint8_t *p = top + (size_t)col * bytes_per_pixel;
			ui_read_rgb(p, (unsigned int)bytes_per_pixel, &r, &g, &b);
			y[y_pos] = (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
			u[uv_pos] = (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
			v[uv_pos] = (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
			p += bytes_per_pixel;
			ui_read_rgb(p, (unsigned int)bytes_per_pixel, &r, &g, &b);
			y[y_pos + 1U] = (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
			p = bottom + (size_t)col * bytes_per_pixel;
			ui_read_rgb(p, (unsigned int)bytes_per_pixel, &r, &g, &b);
			y[y_pos + (size_t)width] = (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
			p += bytes_per_pixel;
			ui_read_rgb(p, (unsigned int)bytes_per_pixel, &r, &g, &b);
			y[y_pos + (size_t)width + 1U] =
				(uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
		}
	}
}

/* Compact each two-row stripe first, then cycle half-row chunks to gather
 * [all Y][all U][all V]. Only two RGB rows and a visited bitmap are needed. */
int carbox_ui_inplace(uint8_t *image, int bpp, int w, int h)
{
    size_t rows, source_stride, stripes, chunks, chunk, marks;
    uint8_t *scratch, *seen;
    if (!image || (bpp != 2 && bpp != 3) || w <= 0 || h <= 0 ||
        w > 4096 || h > 4096 || (w & 1) || (h & 1)) return -1;
    /* Align one RGB row before doubling it. RGB2YUV uses this pitch. */
    source_stride = ((size_t)(w + 15) & ~(size_t)15) * (size_t)bpp;
    rows = 2 * source_stride;
    stripes = h / 2; chunks = stripes * 6; chunk = w / 2;
    marks = (chunks + 7) / 8;
    scratch = malloc(rows + marks);
    if (!scratch) return -1; /* No input changes on allocation failure. */
    seen = scratch + rows;
    memset(seen, 0, marks);
    for (size_t r = 0; r < stripes; ++r) {
        uint8_t *source = scratch, *output = image + r * 3 * w;
        memcpy(scratch, image + r * rows, rows);
        RGB2YUV(&source, &output, bpp, w, 2);
    }
    for (size_t first = 0; first < chunks; ++first) {
        size_t current = first;
        if (seen[first / 8] & (1U << (first % 8))) continue;
        memcpy(scratch, image + first * chunk, chunk);
        do {
            size_t r = current / 6, k = current % 6;
            size_t next = k < 4 ? r * 4 + k : k * stripes + r;
            uint8_t *out = image + next * chunk;
            for (size_t j = 0; j < chunk; ++j) {
                uint8_t old = out[j]; out[j] = scratch[j]; scratch[j] = old;
            }
            seen[current / 8] |= 1U << (current % 8);
            current = next;
        } while (current != first);
    }
    free(scratch);
    return 0;
}

int carbox_ui_layout_init(carbox_ui_layout *l, int w, int h)
{
    if (!l || w <= 0 || h <= 0 || w > 4096 || h > 4096 ||
        (w & 15) || (h & 15)) return -1;
    l->stride[0] = w + 64;
    l->stride[1] = l->stride[2] = ((w / 2 + 32 + 15) & ~15);
    l->bytes[0] = l->stride[0] * (h + 64);
    l->bytes[1] = l->bytes[2] = l->stride[1] * (h / 2 + 64);
    l->offset[0] = l->stride[0] * 32 + 32;
    l->offset[1] = l->bytes[0] + (l->stride[1] * 32 + 32) / 2;
    l->offset[2] = l->offset[1] + l->bytes[1];
    l->total = l->bytes[0] + l->bytes[1] * 2;
    return 0;
}

void carbox_ui_pad(uint8_t *image, const carbox_ui_layout *l, int w, int h)
{
    size_t src[3] = {0, (size_t)w*h, (size_t)w*h*5/4};
    /* Expand backwards before writing any padding (which overlaps compact UV). */
    for (int p = 2; p >= 0; --p) {
        int rows = p ? h/2 : h, cols = p ? w/2 : w;
        for (int r = rows - 1; r >= 0; --r)
            memmove(image + l->offset[p] + r*l->stride[p],
                    image + src[p] + (size_t)r*cols, cols);
    }
    for (int p = 0; p < 3; ++p) {
        size_t rows = p ? h/2 : h, cols = p ? w/2 : w;
        size_t pad = p ? 16 : 32, stride = l->stride[p];
        uint8_t *active = image + l->offset[p];
        for (size_t r = 0; r < rows; ++r) {
            uint8_t *row = active + r*stride;
            memset(row - pad, row[0], pad);
            memset(row + cols, row[cols-1], stride - cols - pad);
        }
        for (size_t r = 1; r <= pad; ++r)
            memcpy(active - pad - r*stride, active - pad, stride);
        /* Chroma allocator reserves more bottom padding than top padding. */
        size_t base = p ? l->bytes[0] + (p-1)*l->bytes[1] : 0;
        size_t used = l->offset[p] - base - pad + rows*stride;
        for (; used + stride <= l->bytes[p]; used += stride)
            memcpy(image + base + used, active - pad + (rows-1)*stride, stride);
    }
}

/* Parse the emitted SPS/PPS before handing the encoder's own skip NALs to
 * the UI file. Reject unknown layouts and any departure from CABAC. */
typedef struct { const uint8_t *p; size_t bits, pos; int bad; } reader;
static unsigned getbits(reader *r, unsigned n)
{
    unsigned v = 0;
    if (n > 32 || r->pos + n > r->bits) { r->bad = 1; return 0; }
    while (n--) { v = (v << 1) | ((r->p[r->pos/8] >> (7-r->pos%8)) & 1); ++r->pos; }
    return v;
}
static unsigned getue(reader *r)
{
    unsigned n = 0;
    while (!r->bad && !getbits(r, 1)) if (++n > 30) { r->bad = 1; break; }
    return r->bad ? 0 : ((1U << n) - 1) + getbits(r, n);
}
int carbox_ui_read_headers(carbox_ui_skip *s, const uint8_t *nal, size_t size)
{
    uint8_t raw[256]; size_t count = 0, i = 0; unsigned zeros = 0, type;
    while (i < size && !nal[i]) ++i;
    if (i < 2 || i + 1 >= size || nal[i++] != 1) return -1;
    type = nal[i++] & 31;
    if (type != 7 && type != 8) return 0;
    for (; i < size; ++i) {
        unsigned b = nal[i];
        if (zeros == 2 && b == 3) { zeros = 0; continue; }
        if (count == sizeof(raw)) return -1;
        raw[count++] = b; zeros = b == 0 ? zeros + 1 : 0;
    }
    reader r = {raw, count*8, 0, 0};
    if (type == 7) {
        unsigned profile = getbits(&r, 8);
        getbits(&r, 16); s->sps_id = getue(&r);
        if (profile == 100) {
            if (getue(&r) != 1 || getue(&r) != 0 || getue(&r) != 0 ||
                getbits(&r, 1) || getbits(&r, 1)) return -1;
        } else if (profile != 66 && profile != 77) return -1;
        s->frame_bits = getue(&r) + 4;
        if (getue(&r) != 0) return -1; /* POC type 0 */
        s->poc_bits = getue(&r) + 4;
        unsigned refs = getue(&r);
        if (!refs || refs > 16 || getbits(&r, 1)) return -1;
        unsigned mw = getue(&r) + 1, mh = getue(&r) + 1;
        if (!getbits(&r, 1)) return -1;
        getbits(&r, 1);
        unsigned left = 0, right = 0, top = 0, bottom = 0;
        if (getbits(&r, 1)) {
            left = getue(&r); right = getue(&r); top = getue(&r); bottom = getue(&r);
        }
        if (r.bad || mw > 256 || mh > 256 || left+right >= mw*8 ||
            top+bottom >= mh*8 || s->frame_bits < 5 || s->frame_bits > 16 ||
            s->poc_bits < 6 || s->poc_bits > 16) return -1;
        s->mb_count = mw*mh;
        s->width = mw*16 - 2*(left+right); s->height = mh*16 - 2*(top+bottom);
        s->have_sps = 1;
    } else {
        s->pps_id = getue(&r);
        if (!s->have_sps || getue(&r) != s->sps_id ||
            (s->cabac = getbits(&r, 1)) != 1 ||
            getbits(&r, 1) || getue(&r) || getue(&r) || getue(&r) ||
            getbits(&r, 1) || getbits(&r, 2)) return -1;
        getue(&r); getue(&r); getue(&r); /* signed QP/chroma offsets; not used */
        if (!getbits(&r, 1)) return -1;
        getbits(&r, 1); /* constrained intra prediction */
        if (getbits(&r, 1) || r.bad) return -1;
        s->have_pps = 1;
    }
    return 0;
}
