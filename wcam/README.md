# wcam 最小 SDK 範例

這是 RTL8195B AmebaPro Pro1 SDK 的最小啟動範例。HP 主程式初始化 console、輸出 `[wcam] minimal SDK sample booted`，然後啟動 FreeRTOS 排程器。此版本尚未加入相機、串流或 Wi-Fi 應用功能。

建置使用 SDK 原生模組與原始 WLAN 函式庫；客戶提供的 CarPlay、Android Auto 與 USB Smart 靜態函式庫已從專案移除。`flash_is.bin` 是完整 flash 映像，包含 SDK bootloader 與 IS 韌體。

## 建置

在 `project/realtek_amebapro_v0_example/GCC-RELEASE` 依序執行：

```sh
PREFIX=/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-
make clean CROSS_COMPILE="$PREFIX"
make -j1 -B ram_lp CROSS_COMPILE="$PREFIX"
make -j8 ram_is CROSS_COMPILE="$PREFIX"
```

產物在 `application_is/flash_is.bin`；本目錄的 `flash_is.bin` 是這次乾淨建置後的交付副本。

SHA-256：`9e5bf03a4ba951a00f6723057cc59ca6ed5d5cd0953a526b28544f5f01ef288f`

已驗證完整 clean build、最終 ELF 沒有客戶函式符號，且連結 map 沒有載入客戶 `.a`。尚未進行實機燒錄或開機驗證。
