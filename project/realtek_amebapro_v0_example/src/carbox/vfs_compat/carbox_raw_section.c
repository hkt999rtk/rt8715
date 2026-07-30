#include "carbox_raw_section.h"

#include "carbox_flash_layout.h"
#include "device_lock.h"
#include "flash_api.h"
#include "osdep_service.h"

#include <string.h>

static flash_t raw_flash;
static int raw_flash_inited;

static void carbox_raw_flash_init_once(void)
{
	if (!raw_flash_inited) {
		flash_init(&raw_flash);
		raw_flash_inited = 1;
	}
}

static int carbox_raw_range_valid(uint32_t offset, uint32_t len)
{
	if (offset > CARBOX_RAW_SECTION_SIZE) {
		return 0;
	}
	if (len > (CARBOX_RAW_SECTION_SIZE - offset)) {
		return 0;
	}
	return 1;
}

int carbox_raw_section_read(uint32_t offset, void *buf, uint32_t len)
{
	if (!buf || !carbox_raw_range_valid(offset, len)) {
		return -1;
	}
	if (len == 0) {
		return 0;
	}

	carbox_raw_flash_init_once();
	device_mutex_lock(RT_DEV_LOCK_FLASH);
	flash_stream_read(&raw_flash, CARBOX_RAW_SECTION_BASE + offset, len, (uint8_t *)buf);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);
	return 0;
}

int carbox_raw_section_erase(uint32_t offset, uint32_t len)
{
	uint32_t erase_start;
	uint32_t erase_end;
	uint32_t addr;

	if (!carbox_raw_range_valid(offset, len)) {
		return -1;
	}
	if (len == 0) {
		return 0;
	}

	erase_start = offset & ~(CARBOX_FLASH_SECTOR_SIZE - 1);
	erase_end = (offset + len + CARBOX_FLASH_SECTOR_SIZE - 1) & ~(CARBOX_FLASH_SECTOR_SIZE - 1);
	if (erase_end > CARBOX_RAW_SECTION_SIZE) {
		return -1;
	}

	carbox_raw_flash_init_once();
	device_mutex_lock(RT_DEV_LOCK_FLASH);
	for (addr = erase_start; addr < erase_end; addr += CARBOX_FLASH_SECTOR_SIZE) {
		flash_erase_sector(&raw_flash, CARBOX_RAW_SECTION_BASE + addr);
	}
	device_mutex_unlock(RT_DEV_LOCK_FLASH);
	return 0;
}

int carbox_raw_section_erase_all(void)
{
	return carbox_raw_section_erase(0, CARBOX_RAW_SECTION_SIZE);
}

int carbox_raw_section_write(uint32_t offset, const void *buf, uint32_t len)
{
	int ret;

	if (!buf || !carbox_raw_range_valid(offset, len)) {
		return -1;
	}
	if (len == 0) {
		return 0;
	}

	carbox_raw_flash_init_once();
	device_mutex_lock(RT_DEV_LOCK_FLASH);
	ret = flash_stream_write(&raw_flash, CARBOX_RAW_SECTION_BASE + offset, len, (uint8_t *)buf);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);
	return ret < 0 ? -1 : 0;
}


#define RAW_TEST_RANDOM_SIZE 256

void raw_test(void)
{
	uint8_t wbuf[RAW_TEST_RANDOM_SIZE];
	uint8_t rbuf[RAW_TEST_RANDOM_SIZE];
	int i;

	/* generate random bytes via hardware TRNG */
	memset(wbuf, 0, sizeof(wbuf));
	rtw_get_random_bytes(wbuf, sizeof(wbuf));

	/* NOR flash: erase the 4 KB sector at offset 0 */
	carbox_raw_section_erase(0, CARBOX_FLASH_SECTOR_SIZE);

	/* write random data */
	carbox_raw_section_write(0, wbuf, sizeof(wbuf));

	/* read back and verify */
	memset(rbuf, 0, sizeof(rbuf));
	carbox_raw_section_read(0, rbuf, sizeof(rbuf));

	if (memcmp(wbuf, rbuf, sizeof(wbuf)) == 0) {
		rt_printf("raw_test: wrote %u random bytes, verify OK\r\n",
			  (unsigned int)sizeof(wbuf));
	} else {
		rt_printf("raw_test: verify FAIL\r\n");
		for (i = 0; i < (int)sizeof(wbuf); i++) {
			if (wbuf[i] != rbuf[i]) {
				rt_printf("  [%03d] wrote=0x%02x read=0x%02x\r\n",
					  i, wbuf[i], rbuf[i]);
				break;
			}
		}
	}
}