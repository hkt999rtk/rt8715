#include "device.h"

#define EFUSE_LOGICAL_MAP_SIZE 512U

extern int efuse_logical_read(u16 laddr, u16 size, u8 *pbuf);
extern int efuse_logical_write(u16 addr, u16 cnts, u8 *data);

u32 OTP_LogicalMap_Read(u8 *pbuf, u32 addr, u32 len)
{
	if ((addr + len) > EFUSE_LOGICAL_MAP_SIZE) {
		return _FAIL;
	}

	if (efuse_logical_read((u16)addr, (u16)len, pbuf) < 0) {
		return _FAIL;
	}

	return _SUCCESS;
}

u32 OTP_LogicalMap_Write(u32 addr, u32 cnts, u8 *data)
{
	if ((addr + cnts) > EFUSE_LOGICAL_MAP_SIZE) {
		return _FAIL;
	}

	if (efuse_logical_write((u16)addr, (u16)cnts, data) < 0) {
		return _FAIL;
	}

	return _SUCCESS;
}
