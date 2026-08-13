/*
 * Smart OTA compatibility for the standalone CarPlay link probe.
 *
 * Smart lib_usbdev.a exposes an HTTPD OTA path that feeds firmware chunks into
 * ota_update_init/download_fw_program/ota_update_deinit. Pro1 does not export
 * those Smart names, but it does provide alternate-slot discovery and flash
 * programming primitives. This wrapper keeps the Smart ABI names and routes the
 * chunk-writing path to Pro1 flash APIs for dependency discovery.
 *
 * Supports single-image (ota_app.bin / ota_fatfs.bin) and multi-image
 * (ota_all.bin with FW + FATFS) Smart OTA packages.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "carbox_flash_layout.h"

#define CARPLAY_OTA_RET_ERR (-1)
#define CARPLAY_OTA_RET_OK 0
#define CARPLAY_OTA_RET_FINISH 1
#define CARPLAY_OTA_SECTOR_SIZE 0x1000U
#define CARPLAY_OTA_DEFAULT_FW_SLOT_SIZE CARBOX_FW_SLOT_SIZE  /* 0x370000 = 3.5 MiB */
#define CARPLAY_OTA_SIGNATURE_LEN 32U
#define CARPLAY_OTA_VERIFY_CHUNK 256U
#define RT_DEV_LOCK_FLASH 1U

/* ---- Smart OTA header (mirrors ameba_ota.h) --------------------------- */
#define CARPLAY_SMART_FILE_HDR_LEN  8
#define CARPLAY_SMART_IMG_HDR_LEN   24
#define CARPLAY_SMART_MAX_IMG_NUM   2
#define CARPLAY_SMART_HEADER_MAX    \
	(CARPLAY_SMART_FILE_HDR_LEN + CARPLAY_SMART_MAX_IMG_NUM * CARPLAY_SMART_IMG_HDR_LEN)

#define CARPLAY_SMART_IMGID_APP     1
#define CARPLAY_SMART_IMGID_FATFS   2

typedef struct {
	uint32_t FwVer;
	uint32_t HdrNum;
} carplay_smart_file_hdr_t;

typedef struct {
	uint8_t  Signature[4];
	uint32_t ImgHdrLen;
	uint32_t Checksum;
	uint32_t ImgLen;
	uint32_t Offset;
	uint32_t ImgID;
} carplay_smart_img_hdr_t;
/* ----------------------------------------------------------------------- */

typedef uint32_t RT_DEV_LOCK_E;

typedef struct {
	void *phal_spic_adaptor;
	uint8_t flash_pin_sel;
} flash_t;

extern void *pvPortMalloc(size_t xWantedSize);
extern void vPortFree(void *pv);
extern void device_mutex_lock(RT_DEV_LOCK_E device);
extern void device_mutex_unlock(RT_DEV_LOCK_E device);
extern void flash_erase_sector(flash_t *obj, uint32_t address);
extern int flash_burst_write(flash_t *obj, uint32_t address, uint32_t Length, uint8_t *data);
extern int flash_stream_read(flash_t *obj, uint32_t address, uint32_t len, uint8_t *data);
extern int http_update_ota(char *host, int port, char *resource);
extern uint32_t update_ota_prepare_addr(void);
extern uint32_t update_ota_get_curr_fw_start_offset(void);
extern int update_ota_signature(unsigned char *sig_backup, uint32_t NewFWAddr);

typedef struct {
	char *host;
	int port;
	char *resource;
	int fd;
	uint8_t type;
	void *tls;
	void *otactrl;
	void *redirect;
	void *otaTargetHdr;
	void (*progress_cb)(int percent);
	uint8_t *firmware_buffer;
	uint32_t firmware_buffer_size;
} carplay_ota_context_t;

typedef struct {
	flash_t flash;
	uint32_t curr_fw_addr;
	uint32_t new_fw_addr;
	uint32_t target_fw_size;
	uint32_t write_offset;
	uint32_t erased_until;
	uint32_t received_len;
	uint32_t expected_input_len;
	uint8_t signature[CARPLAY_OTA_SIGNATURE_LEN];
	uint8_t signature_len;
	uint8_t initialized;
	uint8_t buffered_post_mode;
	uint8_t placeholder_written;
	uint8_t same_image_checked;
	uint8_t failed;
	uint8_t finalized;

	/* Smart OTA header parsing */
	uint8_t  smart_header_buf[CARPLAY_SMART_HEADER_MAX];
	uint32_t smart_header_needed;
	uint8_t  smart_hdr_num;
	carplay_smart_img_hdr_t smart_img_hdrs[CARPLAY_SMART_MAX_IMG_NUM];
	uint32_t smart_package_end;
	uint32_t image_received[CARPLAY_SMART_MAX_IMG_NUM];
	uint32_t image_checksum[CARPLAY_SMART_MAX_IMG_NUM];
	uint8_t  image_complete[CARPLAY_SMART_MAX_IMG_NUM];

	/* Multi-image tracking */
	uint8_t  current_img_idx;
	uint8_t  is_fatfs_ota;      /* true when current image is FATFS */
	uint8_t  fw_seen;
	uint8_t  fatfs_seen;
	uint8_t  fw_signature_committed;

	/* FATFS direct-write path */
	uint32_t fatfs_base;
	uint32_t fatfs_size;
	uint32_t fatfs_write_offset;
	uint32_t fatfs_erased_until;
} carplay_pro1_ota_state_t;

static int carplay_flash_percent;


static int carplay_pro1_get_fw_layout(carplay_pro1_ota_state_t *state)
{
	state->new_fw_addr = update_ota_prepare_addr();
	state->curr_fw_addr = update_ota_get_curr_fw_start_offset();

	if (state->curr_fw_addr == UINT32_MAX || state->new_fw_addr == UINT32_MAX ||
		state->curr_fw_addr == 0U || state->new_fw_addr == 0U) {
		printf("[carplay_ota] invalid fw addr curr=0x%08lx new=0x%08lx\n",
				(unsigned long)state->curr_fw_addr, (unsigned long)state->new_fw_addr);
		return CARPLAY_OTA_RET_ERR;
	}

	state->target_fw_size = CARPLAY_OTA_DEFAULT_FW_SLOT_SIZE;
	printf("[carplay_ota] curr=0x%08lx target=0x%08lx size=0x%08lx\n",
			(unsigned long)state->curr_fw_addr, (unsigned long)state->new_fw_addr,
			(unsigned long)state->target_fw_size);

	return CARPLAY_OTA_RET_OK;
}
static int carplay_ota_erase_to(carplay_pro1_ota_state_t *state, uint32_t end_offset)
{
	if (end_offset > state->target_fw_size) {
		printf("[carplay_ota] erase range overflow end=0x%08lx size=0x%08lx\n",
				(unsigned long)end_offset, (unsigned long)state->target_fw_size);
		return CARPLAY_OTA_RET_ERR;
	}

	while (state->erased_until < end_offset) {
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		flash_erase_sector(&state->flash, state->new_fw_addr + state->erased_until);
		device_mutex_unlock(RT_DEV_LOCK_FLASH);
		state->erased_until += CARPLAY_OTA_SECTOR_SIZE;
	}

	return 0;
}

static int carplay_flash_read(carplay_pro1_ota_state_t *state, uint32_t offset,
							  uint8_t *buf, uint32_t len)
{
	int ret;

	if (offset > state->target_fw_size || len > (state->target_fw_size - offset)) {
		return CARPLAY_OTA_RET_ERR;
	}

	device_mutex_lock(RT_DEV_LOCK_FLASH);
	ret = flash_stream_read(&state->flash, state->new_fw_addr + offset, len, buf);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);

	return (ret == 1) ? CARPLAY_OTA_RET_OK : CARPLAY_OTA_RET_ERR;
}

static int carplay_flash_read_abs(carplay_pro1_ota_state_t *state, uint32_t addr,
								  uint8_t *buf, uint32_t len)
{
	int ret;

	device_mutex_lock(RT_DEV_LOCK_FLASH);
	ret = flash_stream_read(&state->flash, addr, len, buf);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);

	return (ret == 1) ? CARPLAY_OTA_RET_OK : CARPLAY_OTA_RET_ERR;
}

static int carplay_flash_write_raw(carplay_pro1_ota_state_t *state, uint32_t offset,
								   const uint8_t *buf, uint32_t len)
{
	int ret;

	if (offset > state->target_fw_size || len > (state->target_fw_size - offset)) {
		printf("[carplay_ota] write overflow offset=0x%08lx len=0x%08lx size=0x%08lx\n",
				(unsigned long)offset, (unsigned long)len,
				(unsigned long)state->target_fw_size);
		return CARPLAY_OTA_RET_ERR;
	}

	device_mutex_lock(RT_DEV_LOCK_FLASH);
	ret = flash_burst_write(&state->flash, state->new_fw_addr + offset, len, (uint8_t *)buf);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);

	return (ret == 1) ? CARPLAY_OTA_RET_OK : CARPLAY_OTA_RET_ERR;
}

static int carplay_flash_verify(carplay_pro1_ota_state_t *state, uint32_t offset,
								const uint8_t *buf, uint32_t len)
{
	uint8_t verify_buf[CARPLAY_OTA_VERIFY_CHUNK];
	uint32_t checked = 0U;

	while (checked < len) {
		uint32_t chunk = len - checked;

		if (chunk > sizeof(verify_buf)) {
			chunk = sizeof(verify_buf);
		}

		if (carplay_flash_read(state, offset + checked, verify_buf, chunk) != CARPLAY_OTA_RET_OK) {
			printf("[carplay_ota] readback failed offset=0x%08lx len=0x%08lx\n",
					(unsigned long)(offset + checked), (unsigned long)chunk);
			return CARPLAY_OTA_RET_ERR;
		}

		if (memcmp(verify_buf, buf + checked, chunk) != 0) {
			printf("[carplay_ota] verify mismatch offset=0x%08lx len=0x%08lx\n",
					(unsigned long)(offset + checked), (unsigned long)chunk);
			return CARPLAY_OTA_RET_ERR;
		}

		checked += chunk;
	}

	return CARPLAY_OTA_RET_OK;
}

static int carplay_flash_write_verify(carplay_pro1_ota_state_t *state, uint32_t offset,
									  const uint8_t *buf, uint32_t len)
{
	if (carplay_ota_erase_to(state, offset + len) != CARPLAY_OTA_RET_OK) {
		return CARPLAY_OTA_RET_ERR;
	}

	if (carplay_flash_write_raw(state, offset, buf, len) != CARPLAY_OTA_RET_OK) {
		printf("[carplay_ota] flash write failed offset=0x%08lx len=0x%08lx\n",
		       (unsigned long)offset, (unsigned long)len);
		return CARPLAY_OTA_RET_ERR;
	}

	return carplay_flash_verify(state, offset, buf, len);
}

/* ---- Smart header parser ---------------------------------------------- */

static int carplay_switch_to_image(carplay_pro1_ota_state_t *state, uint8_t idx)
{
	carplay_smart_img_hdr_t *hdr;

	if (idx >= state->smart_hdr_num) {
		return CARPLAY_OTA_RET_ERR;
	}

	hdr = &state->smart_img_hdrs[idx];
	state->current_img_idx = idx;

	if (hdr->ImgID == CARPLAY_SMART_IMGID_FATFS) {
		state->is_fatfs_ota = 1U;
		printf("[carplay_ota] switch to img[%u] FATFS base=0x%08lx size=0x%08lx\n",
		       (unsigned int)idx,
		       (unsigned long)state->fatfs_base,
		       (unsigned long)state->fatfs_size);
	} else {
		state->is_fatfs_ota = 0U;
		printf("[carplay_ota] switch to img[%u] FW ImgLen=%lu\n",
		       (unsigned int)idx,
		       (unsigned long)hdr->ImgLen);
	}
	return CARPLAY_OTA_RET_OK;
}

static uint32_t carplay_img_data_end(carplay_pro1_ota_state_t *state, uint8_t idx)
{
	carplay_smart_img_hdr_t *hdr = &state->smart_img_hdrs[idx];
	return hdr->Offset + hdr->ImgLen;
}

static uint32_t carplay_checksum_add(uint32_t sum, const uint8_t *buf, uint32_t len)
{
	uint32_t i;

	for (i = 0U; i < len; i++) {
		sum += buf[i];
	}

	return sum;
}

static int carplay_complete_image(carplay_pro1_ota_state_t *state, uint8_t idx)
{
	carplay_smart_img_hdr_t *hdr;

	if (idx >= state->smart_hdr_num) {
		return CARPLAY_OTA_RET_ERR;
	}
	if (state->image_complete[idx] != 0U) {
		return CARPLAY_OTA_RET_OK;
	}

	hdr = &state->smart_img_hdrs[idx];
	if (state->received_len != carplay_img_data_end(state, idx) ||
	    state->image_received[idx] != hdr->ImgLen) {
		printf("[carplay_ota] img[%u] incomplete received=%lu/%lu stream=0x%08lx end=0x%08lx\n",
		       (unsigned int)idx,
		       (unsigned long)state->image_received[idx],
		       (unsigned long)hdr->ImgLen,
		       (unsigned long)state->received_len,
		       (unsigned long)carplay_img_data_end(state, idx));
		return CARPLAY_OTA_RET_ERR;
	}

	if (state->image_checksum[idx] != hdr->Checksum) {
		printf("[carplay_ota] img[%u] checksum mismatch calculated=0x%08lx expected=0x%08lx\n",
		       (unsigned int)idx,
		       (unsigned long)state->image_checksum[idx],
		       (unsigned long)hdr->Checksum);
		return CARPLAY_OTA_RET_ERR;
	}

	if (hdr->ImgID == CARPLAY_SMART_IMGID_APP &&
	    state->write_offset != hdr->ImgLen) {
		printf("[carplay_ota] FW write length mismatch written=0x%08lx expected=0x%08lx\n",
		       (unsigned long)state->write_offset,
		       (unsigned long)hdr->ImgLen);
		return CARPLAY_OTA_RET_ERR;
	}
	if (hdr->ImgID == CARPLAY_SMART_IMGID_FATFS &&
	    state->fatfs_write_offset != hdr->ImgLen) {
		printf("[carplay_ota] FATFS write length mismatch written=0x%08lx expected=0x%08lx\n",
		       (unsigned long)state->fatfs_write_offset,
		       (unsigned long)hdr->ImgLen);
		return CARPLAY_OTA_RET_ERR;
	}

	state->image_complete[idx] = 1U;
	printf("[carplay_ota] img[%u] complete ImgID=%lu len=%lu checksum=0x%08lx\n",
	       (unsigned int)idx,
	       (unsigned long)hdr->ImgID,
	       (unsigned long)hdr->ImgLen,
	       (unsigned long)state->image_checksum[idx]);
	return CARPLAY_OTA_RET_OK;
}

static int carplay_parse_smart_header(carplay_pro1_ota_state_t *state)
{
	carplay_smart_file_hdr_t *file_hdr;
	uint32_t collected;
	uint32_t expected_offset;
	uint8_t i;

	collected = state->received_len;

	if (collected < CARPLAY_SMART_FILE_HDR_LEN) {
		return CARPLAY_OTA_RET_OK;
	}

	file_hdr = (carplay_smart_file_hdr_t *)state->smart_header_buf;

	if (state->smart_header_needed == 0U) {
		if (file_hdr->HdrNum == 0U || file_hdr->HdrNum > CARPLAY_SMART_MAX_IMG_NUM) {
			printf("[carplay_ota] bad Smart HdrNum=%lu\n",
			       (unsigned long)file_hdr->HdrNum);
			return CARPLAY_OTA_RET_ERR;
		}
		state->smart_hdr_num = (uint8_t)file_hdr->HdrNum;
		state->smart_header_needed = CARPLAY_SMART_FILE_HDR_LEN +
			(uint32_t)state->smart_hdr_num * CARPLAY_SMART_IMG_HDR_LEN;
		printf("[carplay_ota] Smart hdr: FwVer=%lu HdrNum=%u need=%lu bytes\n",
		       (unsigned long)file_hdr->FwVer,
		       (unsigned int)state->smart_hdr_num,
		       (unsigned long)state->smart_header_needed);
	}

	if (collected < state->smart_header_needed) {
		return CARPLAY_OTA_RET_OK;
	}

	/* Copy and validate all image headers.  The streaming writer requires a
	 * compact package ordered exactly as the image headers. */
	expected_offset = state->smart_header_needed;
	for (i = 0U; i < state->smart_hdr_num; i++) {
		carplay_smart_img_hdr_t *dst = &state->smart_img_hdrs[i];
		carplay_smart_img_hdr_t *src = (carplay_smart_img_hdr_t *)
			(state->smart_header_buf + CARPLAY_SMART_FILE_HDR_LEN +
			 (uint32_t)i * CARPLAY_SMART_IMG_HDR_LEN);

		memcpy(dst, src, sizeof(*dst));

		if (dst->Signature[0] != 'O' || dst->Signature[1] != 'T' ||
		    dst->Signature[2] != 'A') {
			printf("[carplay_ota] no 'OTA' signature in ImgHdr[%u]\n",
			       (unsigned int)i);
			return CARPLAY_OTA_RET_ERR;
		}
		if (dst->ImgHdrLen != CARPLAY_SMART_IMG_HDR_LEN || dst->ImgLen == 0U ||
		    dst->Offset != expected_offset || dst->ImgLen > (UINT32_MAX - dst->Offset)) {
			printf("[carplay_ota] bad ImgHdr[%u] hdr_len=%lu len=%lu offset=0x%08lx expected=0x%08lx\n",
			       (unsigned int)i,
			       (unsigned long)dst->ImgHdrLen,
			       (unsigned long)dst->ImgLen,
			       (unsigned long)dst->Offset,
			       (unsigned long)expected_offset);
			return CARPLAY_OTA_RET_ERR;
		}

		if (dst->ImgID == CARPLAY_SMART_IMGID_APP) {
			if (state->fw_seen != 0U || dst->ImgLen <= CARPLAY_OTA_SIGNATURE_LEN ||
			    dst->ImgLen > state->target_fw_size) {
				printf("[carplay_ota] invalid/duplicate FW ImgHdr[%u] len=%lu seen=%u\n",
				       (unsigned int)i, (unsigned long)dst->ImgLen, state->fw_seen);
				return CARPLAY_OTA_RET_ERR;
			}
			state->fw_seen = 1U;
		} else if (dst->ImgID == CARPLAY_SMART_IMGID_FATFS) {
			if (state->fatfs_seen != 0U || dst->ImgLen > state->fatfs_size) {
				printf("[carplay_ota] invalid/duplicate FATFS ImgHdr[%u] len=%lu seen=%u\n",
				       (unsigned int)i, (unsigned long)dst->ImgLen, state->fatfs_seen);
				return CARPLAY_OTA_RET_ERR;
			}
			state->fatfs_seen = 1U;
		} else {
			printf("[carplay_ota] unsupported ImgID=%lu in ImgHdr[%u]\n",
			       (unsigned long)dst->ImgID, (unsigned int)i);
			return CARPLAY_OTA_RET_ERR;
		}

		expected_offset = dst->Offset + dst->ImgLen;

		printf("[carplay_ota] ImgHdr[%u] ImgID=%lu ImgLen=%lu Offset=0x%lx Checksum=0x%lx\n",
		       (unsigned int)i,
		       (unsigned long)dst->ImgID,
		       (unsigned long)dst->ImgLen,
		       (unsigned long)dst->Offset,
		       (unsigned long)dst->Checksum);
	}

	state->smart_package_end = expected_offset;
	if (state->buffered_post_mode != 0U && state->expected_input_len > 0U &&
	    state->expected_input_len != state->smart_package_end) {
		printf("[carplay_ota] POST length mismatch content=%lu package=%lu\n",
		       (unsigned long)state->expected_input_len,
		       (unsigned long)state->smart_package_end);
		return CARPLAY_OTA_RET_ERR;
	}

	/* Activate first image */
	if (carplay_switch_to_image(state, 0U) != CARPLAY_OTA_RET_OK) {
		return CARPLAY_OTA_RET_ERR;
	}

	return CARPLAY_OTA_RET_OK;
}

/* ---- FATFS direct-write helpers --------------------------------------- */

static int carplay_fatfs_erase_to(carplay_pro1_ota_state_t *state, uint32_t end_offset)
{
	if (end_offset > state->fatfs_size) {
		printf("[carplay_ota] fatfs erase overflow end=0x%08lx size=0x%08lx\n",
		       (unsigned long)end_offset, (unsigned long)state->fatfs_size);
		return CARPLAY_OTA_RET_ERR;
	}

	while (state->fatfs_erased_until < end_offset) {
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		flash_erase_sector(&state->flash, state->fatfs_base + state->fatfs_erased_until);
		device_mutex_unlock(RT_DEV_LOCK_FLASH);
		state->fatfs_erased_until += CARPLAY_OTA_SECTOR_SIZE;
	}

	return CARPLAY_OTA_RET_OK;
}

static int carplay_fatfs_write_verify(carplay_pro1_ota_state_t *state,
				      uint32_t offset, const uint8_t *buf, uint32_t len)
{
	uint8_t verify_buf[CARPLAY_OTA_VERIFY_CHUNK];
	uint32_t checked;
	int ret;

	if (carplay_fatfs_erase_to(state, offset + len) != CARPLAY_OTA_RET_OK) {
		return CARPLAY_OTA_RET_ERR;
	}

	device_mutex_lock(RT_DEV_LOCK_FLASH);
	ret = flash_burst_write(&state->flash, state->fatfs_base + offset, len, (uint8_t *)buf);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);
	if (ret != 1) {
		printf("[carplay_ota] fatfs write failed offset=0x%08lx len=0x%08lx\n",
		       (unsigned long)offset, (unsigned long)len);
		return CARPLAY_OTA_RET_ERR;
	}

	checked = 0U;
	while (checked < len) {
		uint32_t chunk = len - checked;
		if (chunk > sizeof(verify_buf)) {
			chunk = sizeof(verify_buf);
		}

		device_mutex_lock(RT_DEV_LOCK_FLASH);
		ret = flash_stream_read(&state->flash, state->fatfs_base + offset + checked,
					chunk, verify_buf);
		device_mutex_unlock(RT_DEV_LOCK_FLASH);

		if (ret != 1 || memcmp(verify_buf, buf + checked, chunk) != 0) {
			printf("[carplay_ota] fatfs verify fail offset=0x%08lx\n",
			       (unsigned long)(offset + checked));
			return CARPLAY_OTA_RET_ERR;
		}
		checked += chunk;
	}

	return CARPLAY_OTA_RET_OK;
}

/* ---- Signature / finalize helpers (FW path only) --------------------- */

static int carplay_ota_prepare_after_signature(carplay_pro1_ota_state_t *state)
{
	uint8_t current_sig[CARPLAY_OTA_SIGNATURE_LEN];
	uint8_t erased_sig[CARPLAY_OTA_SIGNATURE_LEN];

	if (state->same_image_checked != 0U) {
		return CARPLAY_OTA_RET_OK;
	}

	if (carplay_flash_read_abs(state, state->curr_fw_addr, current_sig, sizeof(current_sig)) !=
		CARPLAY_OTA_RET_OK) {
		printf("[carplay_ota] read current fw signature failed addr=0x%08lx\n",
		       (unsigned long)state->curr_fw_addr);
		return CARPLAY_OTA_RET_ERR;
	}

	if (memcmp(current_sig, state->signature, sizeof(current_sig)) == 0) {
		printf("[carplay_ota] target firmware matches current firmware, abort\n");
		return CARPLAY_OTA_RET_ERR;
	}

	memset(erased_sig, 0xFF, sizeof(erased_sig));
	if (carplay_flash_write_verify(state, 0U, erased_sig, sizeof(erased_sig)) !=
		CARPLAY_OTA_RET_OK) {
		return CARPLAY_OTA_RET_ERR;
	}

	state->write_offset = sizeof(erased_sig);
	state->placeholder_written = 1U;
	state->same_image_checked = 1U;
	return CARPLAY_OTA_RET_OK;
}

static int carplay_ota_finalize(carplay_pro1_ota_state_t *state)
{
	uint8_t sig_readback[CARPLAY_OTA_SIGNATURE_LEN];
	uint8_t i;

	if (state->failed != 0U || state->finalized != 0U) {
		return (state->finalized != 0U) ? CARPLAY_OTA_RET_FINISH : CARPLAY_OTA_RET_ERR;
	}

	if (state->smart_hdr_num == 0U || state->smart_package_end == 0U ||
	    state->received_len != state->smart_package_end) {
		printf("[carplay_ota] package incomplete received=%lu expected=%lu images=%u\n",
		       (unsigned long)state->received_len,
		       (unsigned long)state->smart_package_end,
		       (unsigned int)state->smart_hdr_num);
		return CARPLAY_OTA_RET_ERR;
	}

	for (i = 0U; i < state->smart_hdr_num; i++) {
		if (state->image_complete[i] == 0U) {
			printf("[carplay_ota] package finalize rejected: img[%u] not complete\n",
			       (unsigned int)i);
			return CARPLAY_OTA_RET_ERR;
		}
	}

	if (state->fw_seen != 0U) {
		if (state->signature_len != CARPLAY_OTA_SIGNATURE_LEN ||
		    state->placeholder_written == 0U ||
		    state->write_offset <= CARPLAY_OTA_SIGNATURE_LEN) {
			printf("[carplay_ota] incomplete FW sig_len=%u placeholder=%u write=0x%08lx\n",
			       state->signature_len, state->placeholder_written,
			       (unsigned long)state->write_offset);
			return CARPLAY_OTA_RET_ERR;
		}

		if (update_ota_signature(state->signature, state->new_fw_addr) == -1) {
			printf("[carplay_ota] signature write failed\n");
			return CARPLAY_OTA_RET_ERR;
		}

		if (carplay_flash_read(state, 0U, sig_readback, sizeof(sig_readback)) != CARPLAY_OTA_RET_OK ||
		    memcmp(sig_readback, state->signature, sizeof(sig_readback)) != 0) {
			printf("[carplay_ota] signature verify failed\n");
			return CARPLAY_OTA_RET_ERR;
		}
		state->fw_signature_committed = 1U;
	}

	state->finalized = 1U;
	carplay_flash_percent = 100;
	printf("[carplay_ota] package finalize done fw=%u fatfs=%u signature=%u "
	       "fw_written=0x%08lx fatfs_written=0x%08lx\n",
	       state->fw_seen, state->fatfs_seen, state->fw_signature_committed,
	       (unsigned long)state->write_offset,
	       (unsigned long)state->fatfs_write_offset);
	return CARPLAY_OTA_RET_FINISH;
}


int get_flash_percentage(void)
{
	return carplay_flash_percent;
}

int ota_update_init(carplay_ota_context_t *ctx, char *host, int port, char *resource, uint8_t type)
{
	carplay_pro1_ota_state_t *state;

	if (ctx == NULL) {
		return CARPLAY_OTA_RET_ERR;
	}

	state = (carplay_pro1_ota_state_t *)pvPortMalloc(sizeof(*state));
	if (state == NULL) {
		return CARPLAY_OTA_RET_ERR;
	}

	memset(state, 0, sizeof(*state));
	if (carplay_pro1_get_fw_layout(state) != CARPLAY_OTA_RET_OK) {
		vPortFree(state);
		return CARPLAY_OTA_RET_ERR;
	}

	state->initialized = 1U;

	if (host != NULL && port > 0 && resource == NULL) {
		state->buffered_post_mode = 1U;
		state->expected_input_len = (uint32_t)port;
	}

	state->fatfs_base  = CARBOX_FATFS_BASE;
	state->fatfs_size  = CARBOX_FATFS_SIZE;

	ctx->host = host;
	ctx->port = port;
	ctx->resource = resource;
	ctx->type = type;
	ctx->fd = -1;
	ctx->otactrl = state;
	carplay_flash_percent = 0;

	printf("[carplay_ota] ota_update_init type=%u; Smart package finalizes "
			"when all declared images are complete", type);
	if (state->buffered_post_mode != 0U) {
		printf(", buffered_post_total=%lu", (unsigned long)state->expected_input_len);
	}
	printf("\n");

	return CARPLAY_OTA_RET_OK;
}

int ota_update_start(carplay_ota_context_t *ctx)
{
	if (ctx == NULL || ctx->host == NULL || ctx->resource == NULL) {
		return CARPLAY_OTA_RET_ERR;
	}

	printf("[carplay_ota] ota_update_start HTTP OTA path is not verified yet; "
			"using native http_update_ota(host=%s, port=%d, resource=%s)\n",
			ctx->host, ctx->port, ctx->resource);

	return (http_update_ota(ctx->host, ctx->port, ctx->resource) == 0) ?
		   CARPLAY_OTA_RET_OK : CARPLAY_OTA_RET_ERR;
}

int ota_update_deinit(carplay_ota_context_t *ctx)
{
	carplay_pro1_ota_state_t *state;
	int finalize_ret = CARPLAY_OTA_RET_OK;

	if (ctx == NULL) {
		return CARPLAY_OTA_RET_OK;
	}

	state = (carplay_pro1_ota_state_t *)ctx->otactrl;
	if (state != NULL) {
		if (state->failed == 0U && state->finalized == 0U) {
			printf("[carplay_ota] ota_update_deinit final consistency check\n");
			finalize_ret = carplay_ota_finalize(state);
			if (finalize_ret == CARPLAY_OTA_RET_ERR) {
				state->failed = 1U;
				printf("[carplay_ota] ota_update_deinit finalize failed\n");
			}
		} else {
			printf("[carplay_ota] ota_update_deinit skip finalize failed=%u finalized=%u "
					"written=0x%08lx\n",
					state->failed, state->finalized,
					(unsigned long)state->write_offset);
		}

		vPortFree(state);
	}

	ctx->otactrl = NULL;
	return (finalize_ret == CARPLAY_OTA_RET_ERR) ? CARPLAY_OTA_RET_ERR : CARPLAY_OTA_RET_OK;
}

int download_fw_program(carplay_ota_context_t *ctx, const uint8_t *buf, uint32_t len)
{
	carplay_pro1_ota_state_t *state;
	uint32_t input_offset = 0;
	int ret;

	if (ctx == NULL || (buf == NULL && len != 0U)) {
		return CARPLAY_OTA_RET_ERR;
	}

	state = (carplay_pro1_ota_state_t *)ctx->otactrl;
	if (state == NULL || state->initialized == 0U) {
		return CARPLAY_OTA_RET_ERR;
	}

	if (state->failed != 0U) {
		return CARPLAY_OTA_RET_ERR;
	}

	if (len == 0U) {
		ret = carplay_ota_finalize(state);
		if (ret == CARPLAY_OTA_RET_ERR) {
			state->failed = 1U;
			return CARPLAY_OTA_RET_ERR;
		}
		return CARPLAY_OTA_RET_OK;
	}

	/* ---- Phase 0: Collect Smart OTA header ---------------------------- */
	while (state->smart_header_needed == 0U ||
	       state->received_len < state->smart_header_needed) {
		uint32_t need;

		if (state->smart_header_needed == 0U) {
			need = CARPLAY_SMART_FILE_HDR_LEN;
		} else {
			need = state->smart_header_needed;
		}

		/* Buffer as many header bytes as available in this call */
		while (state->received_len < need && input_offset < len) {
			uint32_t idx = state->received_len;

			if (idx < CARPLAY_SMART_HEADER_MAX) {
				state->smart_header_buf[idx] = buf[input_offset];
			}
			state->received_len++;
			input_offset++;
		}

		ret = carplay_parse_smart_header(state);
		if (ret == CARPLAY_OTA_RET_ERR) {
			state->failed = 1U;
			return CARPLAY_OTA_RET_ERR;
		}

		/* Still need more header bytes? */
		if (state->smart_header_needed == 0U ||
		    state->received_len < state->smart_header_needed) {
			if (input_offset >= len) {
				/* Out of data — resume next call */
				return CARPLAY_OTA_RET_OK;
			}
			/* More data in this round — loop to keep collecting */
			continue;
		}

		/* Header complete */
		break;
	}

	/* ---- Phase 1: Process image data (possibly multi-image) ----------- */
	while (input_offset < len) {
		uint32_t boundary;
		uint32_t chunk;

		boundary = carplay_img_data_end(state, state->current_img_idx);

		/* Complete the current image before switching to the next one. */
		if (state->received_len == boundary) {
			if (carplay_complete_image(state, state->current_img_idx) != CARPLAY_OTA_RET_OK) {
				state->failed = 1U;
				return CARPLAY_OTA_RET_ERR;
			}
			if (state->current_img_idx + 1U < state->smart_hdr_num) {
				if (carplay_switch_to_image(state,
				    (uint8_t)(state->current_img_idx + 1U)) != CARPLAY_OTA_RET_OK) {
					state->failed = 1U;
					return CARPLAY_OTA_RET_ERR;
				}
				continue;
			}
			printf("[carplay_ota] unexpected trailing data at package offset=0x%08lx\n",
			       (unsigned long)state->received_len);
			state->failed = 1U;
			return CARPLAY_OTA_RET_ERR;
		}
		if (state->received_len > boundary) {
			printf("[carplay_ota] stream passed img[%u] boundary stream=0x%08lx end=0x%08lx\n",
			       (unsigned int)state->current_img_idx,
			       (unsigned long)state->received_len,
			       (unsigned long)boundary);
			state->failed = 1U;
			return CARPLAY_OTA_RET_ERR;
		}

		/* Clamp chunk to image boundary */
		chunk = len - input_offset;
		if (chunk > (boundary - state->received_len)) {
			chunk = boundary - state->received_len;
		}
		if (chunk == 0U) {
			state->failed = 1U;
			return CARPLAY_OTA_RET_ERR;
		}

		/* ---- Route by image type ------------------------------------ */
		if (state->is_fatfs_ota != 0U) {
			if (carplay_fatfs_write_verify(state,
			    state->fatfs_write_offset, buf + input_offset,
			    chunk) != CARPLAY_OTA_RET_OK) {
				state->failed = 1U;
				return CARPLAY_OTA_RET_ERR;
			}

			state->fatfs_write_offset += chunk;
			state->image_checksum[state->current_img_idx] = carplay_checksum_add(
				state->image_checksum[state->current_img_idx], buf + input_offset, chunk);
			state->image_received[state->current_img_idx] += chunk;
			state->received_len += chunk;
			input_offset += chunk;

			if (state->fatfs_size > 0U) {
				int pct = (int)((state->fatfs_write_offset * 99U) /
				    state->fatfs_size);
				if (pct > carplay_flash_percent && pct < 100) {
					carplay_flash_percent = pct;
				}
			}
		} else {
			/* FW path (OTA_IMGID_APP) */

			/* Collect signature (first 32 bytes of FW image data) */
			while (chunk > 0U &&
			       state->signature_len < CARPLAY_OTA_SIGNATURE_LEN) {
				state->image_checksum[state->current_img_idx] += buf[input_offset];
				state->image_received[state->current_img_idx]++;
				state->signature[state->signature_len++] =
					buf[input_offset];
				input_offset++;
				state->received_len++;
				chunk--;
			}

			if (state->signature_len >= CARPLAY_OTA_SIGNATURE_LEN &&
			    state->placeholder_written == 0U) {
				if (carplay_ota_prepare_after_signature(state)
				    != CARPLAY_OTA_RET_OK) {
					state->failed = 1U;
					return CARPLAY_OTA_RET_ERR;
				}
			}

			/* Write remaining chunk */
			if (chunk > 0U) {
				if (state->placeholder_written == 0U ||
				    carplay_flash_write_verify(state,
				    state->write_offset, buf + input_offset,
				    chunk) != CARPLAY_OTA_RET_OK) {
					state->failed = 1U;
					return CARPLAY_OTA_RET_ERR;
				}

				state->write_offset += chunk;
				state->image_checksum[state->current_img_idx] = carplay_checksum_add(
					state->image_checksum[state->current_img_idx], buf + input_offset, chunk);
				state->image_received[state->current_img_idx] += chunk;
				state->received_len += chunk;
				input_offset += chunk;

				if (state->target_fw_size > 0U) {
					int pct = (int)((state->write_offset * 99U) /
					    state->target_fw_size);
					if (pct > carplay_flash_percent && pct < 100) {
						carplay_flash_percent = pct;
					}
				}
			}
		}
	}

	if (state->smart_hdr_num > 0U &&
	    state->received_len == carplay_img_data_end(state, state->current_img_idx) &&
	    carplay_complete_image(state, state->current_img_idx) != CARPLAY_OTA_RET_OK) {
		state->failed = 1U;
		return CARPLAY_OTA_RET_ERR;
	}

	/* Finalize on the last data-bearing call.  The prebuilt Web handler ignores
	 * the later zero-length finalize return, but it does honor errors returned
	 * while consuming a data chunk. */
	if (state->smart_package_end > 0U &&
	    state->received_len == state->smart_package_end &&
	    state->finalized == 0U) {
		printf("[carplay_ota] Smart package complete, received=%lu expected=%lu; finalize\n",
		       (unsigned long)state->received_len,
		       (unsigned long)state->smart_package_end);
		ret = carplay_ota_finalize(state);
		if (ret == CARPLAY_OTA_RET_ERR) {
			state->failed = 1U;
			return CARPLAY_OTA_RET_ERR;
		}
	}

	return CARPLAY_OTA_RET_OK;
}
