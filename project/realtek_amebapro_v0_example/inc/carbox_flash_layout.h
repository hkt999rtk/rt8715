#ifndef CARBOX_FLASH_LAYOUT_H
#define CARBOX_FLASH_LAYOUT_H

#define CARBOX_FLASH_SECTOR_SIZE        0x1000

/*
 * Immutable-recovery OTA layout for the 8 MiB NOR flash.
 * Both XIP image bases are 256 KiB aligned as required by the IS linker.
 * FW1 is factory/recovery-only; field OTA is allowed to modify FW2 only.
 * FATFS keeps its original 1 MiB allocation.  LittleFS stores only mutable
 * configuration state in this product, so it uses the final 128 KiB.
 */
#define CARBOX_RECOVERY_FW_BASE         0x00040000
#define CARBOX_RECOVERY_FW_SIZE         0x00140000
#define CARBOX_MAIN_FW_BASE             0x00180000
#define CARBOX_MAIN_FW_SIZE             0x00560000
#define CARBOX_FIRMWARE_REGION_END      0x006E0000

#define CARBOX_FATFS_BASE               0x006E0000
#define CARBOX_FATFS_SIZE               0x00100000
#define CARBOX_FATFS_LOGICAL_SECTOR_SIZE 512
#define CARBOX_FATFS_SECTOR_COUNT       (CARBOX_FATFS_SIZE / CARBOX_FATFS_LOGICAL_SECTOR_SIZE)
#define CARBOX_FATFS_READ_ONLY          1

#define CARBOX_LITTLEFS_BASE            0x007E0000
#define CARBOX_LITTLEFS_SIZE            0x00020000

#define CARBOX_NOR_FLASH_SIZE           0x00800000

#endif
