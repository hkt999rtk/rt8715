/*
 *  Routines to associate Flash driver with FatFs
 *
 *  Copyright (c) 2014 Realtek Semiconductor Corp.
 *
 *  This module is a confidential and proprietary property of RealTek and
 *  possession or use of this module requires written permission of RealTek.
 */

#if defined(CONFIG_PLATFORM_8195BHP)
#include "timer_api.h"
#include "task.h"
#endif

#include "integer.h"
#include "stdint.h"
#include "stdio.h"
#include <disk_if/inc/flash_fatfs.h>
#include "device_lock.h"
#include "platform_opts.h"
#include "carbox_flash_layout.h"


#if	FATFS_DISK_FLASH

#include "flash_api.h" // Flash interface

#define FLASH_SECTOR_COUNT	CARBOX_FATFS_SECTOR_COUNT
#define SECTOR_SIZE_FLASH	CARBOX_FATFS_LOGICAL_SECTOR_SIZE
#define FLASH_APP_BASE		CARBOX_FATFS_BASE
#define FLASH_ERASE_BLOCK_SECTORS	(CARBOX_FLASH_SECTOR_SIZE / CARBOX_FATFS_LOGICAL_SECTOR_SIZE)

#if !CARBOX_FATFS_READ_ONLY
#error "flash_fatfs.c currently supports the Carbox FatFS partition as read-only only"
#endif


flash_t		flash;

extern void flash_init(flash_t *obj);

DRESULT interpret_flash_result(int out){
	DRESULT res;
	if(out)
		res = RES_OK;
	else 
		res = RES_ERROR;
	return res;
}

DSTATUS FLASH_disk_status(void){
	DRESULT res;
	res = RES_OK;
	return res;
}

DSTATUS FLASH_disk_initialize(void){
	DRESULT res;
#if defined(CONFIG_PLATFORM_8195BHP)
	//spic_deinit(&flash.hal_spic_adaptor);
	flash_init(&flash);
#endif
	res = RES_OK;
	return res;
}

DSTATUS FLASH_disk_deinitialize(void){
	DRESULT res;
	res = RES_OK;
	return res;
}

/* Read sector(s) --------------------------------------------*/
DRESULT FLASH_disk_read(BYTE *buff, DWORD sector, UINT count){			
	DRESULT res;
	char retry_cnt = 0;

	if ((count == 0) || (sector >= FLASH_SECTOR_COUNT) || (count > (FLASH_SECTOR_COUNT - sector))) {
		return RES_PARERR;
	}

	device_mutex_lock(RT_DEV_LOCK_FLASH);
	do{
		res = interpret_flash_result(flash_stream_read(&flash, FLASH_APP_BASE + sector*SECTOR_SIZE_FLASH, count*SECTOR_SIZE_FLASH, (uint8_t *) buff));
		if(++retry_cnt>=3)
			break;
	}while(res != RES_OK);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);
	return res;
}

/* Write sector(s) --------------------------------------------*/
#if _USE_WRITE == 1
DRESULT FLASH_disk_write(BYTE const *buff, DWORD sector, UINT count){
	(void)buff;
	(void)sector;
	(void)count;
	return RES_WRPRT;
}
#endif

/* IOCTL sector(s) --------------------------------------------*/
#if _USE_IOCTL == 1
DRESULT FLASH_disk_ioctl (BYTE cmd, void* buff){
	DRESULT res = RES_ERROR;

	switch(cmd){
		/* Generic command (used by FatFs) */
		
		/* Make sure that no pending write process in the physical drive */
		case CTRL_SYNC:		/* Flush disk cache (for write functions) */
			res = RES_OK;
			break;
		case GET_SECTOR_COUNT:	/* Get media size (for only f_mkfs()) */
			*(DWORD*)buff = FLASH_SECTOR_COUNT;
			res = RES_OK;
			break;
		/* for case _MAX_SS != _MIN_SS */
		case GET_SECTOR_SIZE:	/* Get sector size (for multiple sector size (_MAX_SS >= 1024)) */
			*(WORD*)buff = SECTOR_SIZE_FLASH;	//4096 or 2048 or 1024
			res = RES_OK;
			break;

		case GET_BLOCK_SIZE:	/* Get erase block size (for only f_mkfs()) */
			*(DWORD*)buff = FLASH_ERASE_BLOCK_SECTORS;
			res = RES_OK;
			break;
		case CTRL_ERASE_SECTOR:/* Force erased a block of sectors (for only _USE_ERASE) */
			res = RES_OK;
			break;
#if 0
		/* MMC/SDC specific ioctl command */

		case MMC_GET_TYPE:	/* Get card type */
			res = RES_OK;
			break;
		case MMC_GET_CSD:	/* Get CSD */
			res = RES_OK;
			break;
		case MMC_GET_CID:	/* Get CID */
			res = RES_OK;
			break;
		case MMC_GET_OCR:	/* Get OCR */
			res = RES_OK;
			break;
		case MMC_GET_SDSTAT:/* Get SD status */
			res = RES_OK;
			break;
#endif
		default:
			res = RES_PARERR;
			break;
	}
	return res;
}
#endif

ll_diskio_drv FLASH_disk_Driver ={
	.disk_initialize = FLASH_disk_initialize,
	.disk_status = FLASH_disk_status,
	.disk_read = FLASH_disk_read,
	.disk_deinitialize = FLASH_disk_deinitialize,
#if _USE_WRITE == 1
	.disk_write = FLASH_disk_write,
#endif
#if _USE_IOCTL == 1
	.disk_ioctl = FLASH_disk_ioctl,
#endif
	.TAG	= (unsigned char *)"FLASH"
};
#endif
