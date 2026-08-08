# Flash image 與分區配置

## Build 產物

目前 workspace 內的主要產物如下：

| 檔案 | 大小 | 用途 |
|---|---:|---|
| `application_is/flash_is_ota1.bin` | `0x66A000` / 6,725,632 bytes | 完整 flash 燒錄映像 |
| `application_is/firmware_is_ota1.bin` | `0x43F9C4` / 4,454,852 bytes | FW1 firmware，不含 partition table/bootloader |
| `application_is/ota_app.bin` | `0x43F9E4` / 4,454,884 bytes | 帶 OTA wrapper 的 application update |
| `application_is/boot.bin` | `0x5E40` / 24,128 bytes | ELF2BIN 轉換後、實際打包的 bootloader |

上述 `application_is` 的完整路徑是：

`project/realtek_amebapro_v0_example/GCC-RELEASE/application_is`

## Flash 分區

分區來源為：

- `project/realtek_amebapro_v0_example/GCC-RELEASE/partition.json`
- `project/realtek_amebapro_v0_example/inc/carbox_flash_layout.h`

| Flash offset | 配置長度 | 類型 | 是否打包進目前完整 image |
|---:|---:|---|---|
| `0x00000000` | partition table 自身格式決定 | PARTAB | 是 |
| `0x00009000` | `0x00010000` | BOOT | 是 |
| `0x00040000` | `0x00600000` | FW1 | 是 |
| `0x00640000` | `0x00100000` | FATFS / USER | 是，但檔案只延伸到實際 FATFS 尾端 |
| `0x00740000` | `0x00080000` | LittleFS / USER | 分區有保留，目前不預填 |

目前 `flash_is_ota1.bin` 的 EOF 是 `0x0066A000`，因此它含有從
`0x00640000` 開始的 `0x2A000` bytes FATFS 資料；LittleFS 在首次使用時
由 runtime 建立，未預先填入燒錄檔。

## Build 如何組合完整映像

`application.is.mk` 的 `manipulate_images` 依序執行：

1. 產生 `partition.json`。
2. 由 VFS 目錄產生 `application_is/fatfs.bin`。
3. 以 `amebapro_bootloader.json` 產生 partition table。
4. 以 `amebapro_firmware_is.serial.json` 產生 firmware。
5. 把 `bootloader.axf` 交給 ELF2BIN，產生打包格式的
   `bootloader/boot.bin`。
6. 複製成 `application_is/boot.bin`。
7. 執行：

   ```text
   ELF2BIN combine application_is/flash_is.bin \
       PTAB=partition.bin \
       BOOT=application_is/boot.bin \
       FW1=application_is/firmware_is.bin
   ```

8. 用 `dd conv=notrunc` 把 FATFS 放到 `0x00640000`。
9. postbuild 產生帶 OTA index 的最終檔名。

## Bootloader 的精確比對

目前 build 的：

`project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/boot.bin`

與：

`flash_is_ota1.bin[0x9000 : 0x9000 + 0x5E40]`

逐 byte 完全相同。

需注意 BSP 原始的：

`component/soc/realtek/8195b/misc/bsp/image/boot.bin`

也是 24,128 bytes，但不是目前最終 full image 中的相同 byte stream。
原因是 build 又以目前的 AXF、JSON 與 key/hash 設定重新執行 ELF2BIN。
分析或交付燒錄內容時，應以 `application_is/boot.bin` 為準。

## 2026-07-31 workspace 快照 SHA-256

```text
d3a1cb407fae0046a6ac0ea3dbf7917bf55b8f22b7830dde93aa35b990aa6c08  flash_is_ota1.bin
ab2a3fae572c6c46a83b2c378282a3b30ba459540b1ddb24f1f73bae9ca6ef07  firmware_is_ota1.bin
94862896d22b4e8e302f30749d60b54090d68489243dfe0b350746c240d0c52e  ota_app.bin
26189cea8c2bf2a64bc317648e71cea2e9945b6503e00b7facf59e4a51c2f028  application_is/boot.bin
```

重新 build 後 hash 可能隨 serial、timestamp 或內容改變。
