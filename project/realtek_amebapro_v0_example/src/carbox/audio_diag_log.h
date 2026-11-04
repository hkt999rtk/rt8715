/* UART output is independent of the hooks used by navigation policy.
 * Keep reporter housekeeping running when quiet: completed output epochs
 * must become reusable even when no diagnostic record is printed.
 */
#ifndef CARBOX_AUDIO_DIAG_LOG_H
#define CARBOX_AUDIO_DIAG_LOG_H

#include "rx_handover_diag.h"

#ifndef CONFIG_AUDIO_DIAG_LOG
#define CONFIG_AUDIO_DIAG_LOG 0
#endif

#if CONFIG_AUDIO_DIAG_LOG
#define carbox_audio_diag_log carbox_rxhd_log
#define carbox_audio_diag_log_skipped carbox_diag_log_skipped
#else
/* An inline sink preserves format checking and variable-use diagnostics,
 * while optimized builds remove the call and its format string entirely. */
static inline void __attribute__((format(printf, 1, 2)))
carbox_audio_diag_log(const char *fmt, ...) { (void)fmt; }
static inline uint32_t carbox_audio_diag_log_skipped(void) { return 0U; }
#endif

#endif
