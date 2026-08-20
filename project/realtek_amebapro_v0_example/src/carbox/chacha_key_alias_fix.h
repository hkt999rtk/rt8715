#ifndef CARBOX_CHACHA_KEY_ALIAS_FIX_H
#define CARBOX_CHACHA_KEY_ALIAS_FIX_H

#include <stdint.h>

/* Lightweight screen-RX software ChaCha latency, reported on PCPROF's
 * existing 10-second cadence. */
void carbox_chacha_rx_latency_report(uint32_t sequence);

#endif /* CARBOX_CHACHA_KEY_ALIAS_FIX_H */
