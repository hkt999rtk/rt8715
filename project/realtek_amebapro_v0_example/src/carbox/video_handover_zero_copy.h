#ifndef CARBOX_VIDEO_HANDOVER_ZERO_COPY_H
#define CARBOX_VIDEO_HANDOVER_ZERO_COPY_H

#include <stddef.h>
#include <stdint.h>

/* Scope established by the AirPlayScreen_SendVideo linker wrapper. */
void carbox_video_handover_begin(const void *source, int frame_length);
void carbox_video_handover_end(void);

/* Object-local allocation hooks installed in derived vendor archives. */
void *carbox_video_handover_source_malloc(size_t length);
void *carbox_video_handover_destination_malloc(size_t length);
void carbox_video_handover_producer_free(void *pointer);
void carbox_video_handover_consumer_free(void *pointer);

/* Called before the generic GDMA/M33 memcpy paths.  The copy is deferred only
 * for the exact temporary-destination/source/frame transaction. */
int carbox_video_handover_memcpy_is_elided(void *destination,
					   const void *source, size_t length);

/* Two-phase queue publication.  prepare() publishes source ownership before
 * the real CVector push can wake a consumer; finish() commits only after the
 * vector size proves that the 8-byte item was copied successfully. */
int carbox_video_handover_prepare_push(void *temporary, int frame_length,
					void **replacement);
void carbox_video_handover_finish_push(void *temporary, int pushed);

void carbox_video_handover_report(uint32_t sequence);

#endif /* CARBOX_VIDEO_HANDOVER_ZERO_COPY_H */
