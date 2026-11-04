#ifndef CARBOX_AUDIO_MUSIC_PROGRESS_H
#define CARBOX_AUDIO_MUSIC_PROGRESS_H
#include <stdint.h>
/* CONFIG_AUDIO_MUSIC_DIAG opts the source and audited archive hooks into build. */
void carbox_music_progress_before(void *session, uint32_t id, const uint32_t *saved);
void carbox_music_progress_after(void *session, uint32_t id, const uint32_t *saved);
void carbox_music_progress_report(void);
#endif
