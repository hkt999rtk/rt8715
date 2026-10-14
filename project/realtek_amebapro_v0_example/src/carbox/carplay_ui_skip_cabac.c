/* Standalone CABAC all-skip slice for the audited x264 ABI 76 archive.
 * Only the first IDR uses the full encoder.  These primitives need CABAC
 * context and bit output, not x264_t, fenc, fdec, or reference pixels.
 */
#include "carplay_ui_memory.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if UINTPTR_MAX != 0xffffffffU
#error The closed x264 CABAC ABI requires 32-bit pointers
#endif

extern void x264_cabac_context_init(void *, int, int, int);
extern void x264_cabac_encode_init(void *, uint8_t *, uint8_t *);
extern void x264_cabac_encode_decision_c(void *, int, int);
extern void x264_cabac_encode_terminal(void *);
extern void x264_cabac_encode_flush(const void *, void *);

/* x264_cabac_encode_flush reads only x264_t+0x494 for its terminal marker.
 * The vendor's 20 skip NALs have this word clear.  Keep this layout local
 * to the SHA-audited x264 archive selected by patch_ui_x264_archive.py. */
static const uint32_t flush_state[0x494 / 4 + 1];

typedef struct {
	uint8_t data[16];
	unsigned bits;
	int error;
} bit_writer;

static void write_bits(bit_writer *w, unsigned count, unsigned value)
{
	if (count > 32 || w->bits + count > sizeof(w->data) * 8) {
		w->error = 1;
		return;
	}
	while (count--) {
		w->data[w->bits / 8] |= ((value >> count) & 1U) <<
						     (7 - w->bits % 8);
		++w->bits;
	}
}

static void write_ue(bit_writer *w, unsigned value)
{
	unsigned bits = 0, number = value + 1, scan = number;
	while (scan >>= 1) ++bits;
	write_bits(w, bits, 0);
	write_bits(w, bits + 1, number);
}

size_t carbox_ui_skip_cabac(const carbox_ui_skip *s, unsigned frame,
			    uint8_t *dst, size_t capacity)
{
	bit_writer header = {{0}, 0, 0};
	uint32_t cabac[128] = {0}; /* context states occupy bytes 36..495 */
	uint8_t *body = NULL, *end;
	size_t body_capacity, body_size, output = 0;
	unsigned zeros = 0;

	if (!s || !dst || !s->have_sps || !s->have_pps || !s->cabac ||
	    s->sps_id || s->pps_id || s->frame_bits != 12 ||
	    s->poc_bits != 13 || !s->mb_count || s->mb_count > 65536 ||
	    !frame || frame > 20 || capacity < 16)
		return 0;
	/* Same P-slice header fields and CABAC byte alignment as the vendor
	 * skip helper for this fixed encoder configuration (CQP 23, one ref). */
	write_ue(&header, 0); /* first_mb_in_slice */
	write_ue(&header, 0); /* P slice */
	write_ue(&header, s->pps_id);
	write_bits(&header, s->frame_bits, frame);
	write_bits(&header, s->poc_bits, 2 * frame);
	write_bits(&header, 3, 0); /* ref override, list mod, MMCO */
	write_bits(&header, 3, 7); /* CABAC init 0, QP delta 0, deblock 0 */
	while (header.bits % 8) write_bits(&header, 1, 1);
	if (header.error || header.bits != 40) return 0;

	/* One decision and (except for the last MB) one terminal per MB.  The
	 * byte bound exceeds the number of encoded bins plus flush overhead. */
	body_capacity = 2 * (size_t)s->mb_count + 32;
	body = malloc(body_capacity);
	if (!body) return 0;
	x264_cabac_context_init(cabac, 0, 23, 0);
	x264_cabac_encode_init(cabac, body, body + body_capacity);
	for (unsigned mb = 0; mb < s->mb_count; ++mb) {
		if (mb) x264_cabac_encode_terminal(cabac);
		x264_cabac_encode_decision_c(cabac, 11, 1);
	}
	x264_cabac_encode_flush(flush_state, cabac);
	memcpy(&end, (const uint8_t *)cabac + 20, sizeof(end));
	if (end < body || end > body + body_capacity) goto done;
	body_size = (size_t)(end - body);

	dst[output++] = 0; dst[output++] = 0;
	dst[output++] = 0; dst[output++] = 1;
	dst[output++] = 0x21; /* non-IDR reference P slice */
	for (size_t i = 0; i < header.bits / 8 + body_size; ++i) {
		uint8_t byte;
		if (i < header.bits / 8) byte = header.data[i];
		else byte = body[i - header.bits / 8];
		if (zeros == 2 && byte <= 3) {
			if (output == capacity) { output = 0; goto done; }
			dst[output++] = 3;
			zeros = 0;
		}
		if (output == capacity) { output = 0; goto done; }
		dst[output++] = byte;
		zeros = byte == 0 ? zeros + 1 : 0;
	}
done:
	free(body);
	return output;
}
