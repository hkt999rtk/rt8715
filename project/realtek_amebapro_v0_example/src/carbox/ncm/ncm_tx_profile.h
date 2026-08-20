#ifndef CARBOX_NCM_TX_PROFILE_H
#define CARBOX_NCM_TX_PROFILE_H

/* Low-frequency validation report emitted by the existing 10-second task. */
void carbox_ncm_tx_single_profile_report(unsigned long sequence);
void carbox_ncm_tx_gdma_latency_report(unsigned long sequence);

#endif
