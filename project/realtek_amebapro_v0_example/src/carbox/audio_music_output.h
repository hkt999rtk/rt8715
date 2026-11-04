#ifndef CARBOX_AUDIO_MUSIC_OUTPUT_H
#define CARBOX_AUDIO_MUSIC_OUTPUT_H

/* Called only by the common diagnostics task. Active epochs print every 5 s;
 * completed epochs are eligible immediately, at most four records per call. */
#if CONFIG_AUDIO_MUSIC_DIAG
void carbox_audio_music_output_report(void);
#else
static inline void carbox_audio_music_output_report(void) {}
#endif

#endif
