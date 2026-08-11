#ifndef CARBOX_FLASH_LAYOUT_H
#define CARBOX_FLASH_LAYOUT_H

#define CARBOX_FLASH_SECTOR_SIZE        0x1000

/* FW1 0x00040000..0x003AFFFF, FW2 0x003C0000..0x0072FFFF.
 * Keep FW2 on a 256 KiB boundary: C-cut SCE interprets xip_pg_sz=2 as
 * 256 KiB, so the remapped XIP physical base must have that alignment. */
#define CARBOX_FW_SLOT_SIZE             0x00370000

/* 8 MiB A/B layout: preserve LittleFS at its legacy address so firmware
 * updates do not discard pairing/settings.  FatFS occupies the final 256 KiB. */
#define CARBOX_FATFS_BASE               0x007C0000
#define CARBOX_FATFS_SIZE               0x00040000
#define CARBOX_FATFS_LOGICAL_SECTOR_SIZE 512
#define CARBOX_FATFS_SECTOR_COUNT       (CARBOX_FATFS_SIZE / CARBOX_FATFS_LOGICAL_SECTOR_SIZE)
#define CARBOX_FATFS_READ_ONLY          1

#define CARBOX_LITTLEFS_BASE            0x00740000
#define CARBOX_LITTLEFS_SIZE            0x00080000

#endif
