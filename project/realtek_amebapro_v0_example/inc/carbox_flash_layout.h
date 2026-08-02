#ifndef CARBOX_FLASH_LAYOUT_H
#define CARBOX_FLASH_LAYOUT_H

#define CARBOX_FLASH_SECTOR_SIZE        0x1000

/*
 * Immutable-recovery OTA layout inside the original 6 MiB firmware area.
 * Both XIP image bases are 256 KiB aligned as required by the IS linker.
 * FW1 is factory/recovery-only; field OTA is allowed to modify FW2 only.
 */
#define CARBOX_RECOVERY_FW_BASE         0x00040000
#define CARBOX_RECOVERY_FW_SIZE         0x00080000
#define CARBOX_MAIN_FW_BASE             0x000C0000
#define CARBOX_MAIN_FW_SIZE             0x00580000
#define CARBOX_FIRMWARE_REGION_END      0x00640000

#define CARBOX_FATFS_BASE               0x00640000
#define CARBOX_FATFS_SIZE               0x00100000
#define CARBOX_FATFS_LOGICAL_SECTOR_SIZE 512
#define CARBOX_FATFS_SECTOR_COUNT       (CARBOX_FATFS_SIZE / CARBOX_FATFS_LOGICAL_SECTOR_SIZE)
#define CARBOX_FATFS_READ_ONLY          1

#define CARBOX_LITTLEFS_BASE            0x00740000
#define CARBOX_LITTLEFS_SIZE            0x00080000

#endif
