#ifndef CARBOX_NCM_TX_BATCH_H
#define CARBOX_NCM_TX_BATCH_H

#include <stddef.h>
#include <stdint.h>

#ifndef CONFIG_NCM_TX_BATCH_MAX
#define CONFIG_NCM_TX_BATCH_MAX 1
#endif
#if CONFIG_NCM_TX_BATCH_MAX < 1 || CONFIG_NCM_TX_BATCH_MAX > 40
#error "CONFIG_NCM_TX_BATCH_MAX must be between 1 and the NCM ABI limit 40"
#endif

#define CARBOX_NCM_TX_BATCH_MAGIC          0x324D434EU /* "NCM2" */
#define CARBOX_NCM_TX_BATCH_MAX_DATAGRAMS  CONFIG_NCM_TX_BATCH_MAX
#define CARBOX_NCM_TX_BATCH_MAX_SEGMENTS  64U
#define CARBOX_NCM_TX_BATCH_RETRY_SINGLE  (-4096)

struct carbox_ncm_tx_segment {
	const void *data;
	size_t len;
};

struct carbox_ncm_tx_frame {
	size_t len;
	uint16_t first_segment;
	uint16_t segment_count;
};

struct carbox_ncm_tx_batch {
	uint32_t magic;
	uint16_t frame_count;
	uint16_t segment_count;
	struct carbox_ncm_tx_frame frame[CARBOX_NCM_TX_BATCH_MAX_DATAGRAMS];
	struct carbox_ncm_tx_segment segment[CARBOX_NCM_TX_BATCH_MAX_SEGMENTS];
};

/*
 * Synchronous: all segment storage remains owned by the caller until return.
 * sent_frames reports the largest FIFO prefix placed in the transmitted NTB.
 */
int carbox_ncm_tx_send_batch(const struct carbox_ncm_tx_batch *batch,
			     uint16_t *sent_frames);

#endif
