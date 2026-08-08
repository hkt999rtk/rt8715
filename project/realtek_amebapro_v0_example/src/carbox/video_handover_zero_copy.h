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

/* Called before the generic GDMA/M33 memcpy paths.  Returns non-zero only for
 * the exact source==destination handover transaction. */
int carbox_video_handover_memcpy_is_elided(void *destination,
					   const void *source, size_t length);

void carbox_video_handover_report(uint32_t sequence);

#endif /* CARBOX_VIDEO_HANDOVER_ZERO_COPY_H */
