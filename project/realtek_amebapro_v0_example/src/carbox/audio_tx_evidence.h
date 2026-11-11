#ifndef CARBOX_AUDIO_TX_EVIDENCE_H
#define CARBOX_AUDIO_TX_EVIDENCE_H
#include <stdint.h>
#ifndef CONFIG_AUDIO_TX_EVIDENCE
#define CONFIG_AUDIO_TX_EVIDENCE 0
#endif
#if CONFIG_AUDIO_TX_EVIDENCE
void carbox_audio_tx_evidence_report(uint32_t sequence);
#else
static inline void carbox_audio_tx_evidence_report(uint32_t sequence) { (void)sequence; }
#endif
#endif
