# RT8715 (PRO1) USB PHY register 與 wrapper 調查

## 本機 `lib_usbsmart.a` 的設定路徑

`amebapro_usb.o` 匯出一個 12-byte 的 `usb_hal_driver` 函式表，依序是
`chip_init`、`chip_deinit`、`get_cal_data`。其 `chip_init` 先啟動 USB 電源與
clock，再透過 DWC2 `GPVNDCTL` 設定 PHY。實際寫入順序如下：

| PHY page | register | value |
| --- | --- | --- |
| 0 | `E0` | `6C` |
| 0 | `E1` | `81` |
| 0 | `E2` | `62` |
| 0 | `E7` | `41` |
| 1 | `E0` | `91` |

程式以讀 `D4`、寫 `F4[6:5]` 選擇 page，寫完回到 page 0。`get_cal_data`
回傳空指標，所以本版本的 `usb_hal_calibrate()` 沒有額外校準表可套用。
反編譯的 C 行為還原在
[`USB_PHY/evidence/amebapro_usb.recovered.c`](USB_PHY/evidence/amebapro_usb.recovered.c)，
來源是 `lib_usbsmart.a(amebapro_usb.o)`；它是分析資料，不是原廠原始碼。

PRO1 的 [`dwc_otg_regs.h`](USB_PHY/evidence/rtl8195b_dwc_otg_regs.h)
定義 `GPVNDCTL` 在 USB controller offset `0x34`，`NewRegReq`、`VStsBsy`、
`VStsDone` 分別是 bit 25、26、27。這是**存取 PHY 的 controller 寄存器**，
不是 `E0` 等類比 PHY register 的 bit 定義。

## 四檔 RX boost

[`sysreg_sec_v1.0.1.h`](USB_PHY/sources/realtek/sysreg_sec_v1.0.1.h)
在 `SEC_OTP_SYSCFG2` offset `0x108` 定義 `USB_PHY_RX_BOOST[2:1]`，mask
為 `0x6`。該欄位描述的是 USB high-speed 接收放大器的 AC gain：

| level | bit `[2:1]` | 對 32-bit register 的 OR 值 | gain |
| ---: | --- | ---: | ---: |
| 0 | `00` | `0x0` | 0 dB |
| 1 | `01` | `0x2` | 3.3 dB |
| 2 | `10` | `0x4` | 6.9 dB |
| 3 | `11` | `0x6` | 9.3 dB |

若這個欄位確實屬於目標 SoC，可用
`new_value = (old_value & ~0x6U) | ((level & 3U) << 1)` 保留其他 bit。
該檔案來自 Realtek **AmebaSmart** 的 `sysreg_sec.h`。PRO1 自己的
[`rtl8195bhp_syson.h`](component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/lib/include/rtl8195bhp_syson.h)
也定義了 `SYSON_BASE=0x40000000`、offset `0x108` 的
`hs_efuse_syscfg2`，因此 PRO1 實際有 `0x40000108` 這個 auto-loaded
eFuse 系統設定 register。然而 PRO1 header 將全部 32 bit 只標成
`efuse_syscfg2`，**沒有**將 bit `[2:1]` 定義成 RX boost。相同 register
名稱和 offset 尚不足以證明兩顆 SoC 的 bit 配置相同；目前 archive 也沒有
PRO1 boost 的 PHY page／register／mask。因此不能只憑 Smart 的欄位定義
製作四個聲稱具備不同 PRO1 增益的 firmware。

## 目前的 wrapper

[`usb_phy_driver_wrap.c`](project/realtek_amebapro_v0_example/src/carbox/usb_phy_driver_wrap.c)
配合 GNU ld `--wrap=usb_hal_driver`，讓 `usb_hal.o`、`usbh_hcd.o`、
`usbd_pcd.o` 對原廠函式表的引用改走本地函式表。原廠 table 透過
`__real_usb_hal_driver` 保留。本版 wrapper 轉呼叫原本的初始化、反初始化、
校準表函式，建立可核對的替換點。一般建置時
`CARBOX_USB_RX_BOOST_LEVEL=-1`，wrapper 不改動寄存器。

使用外部 Realtek GCC 6.4.1 toolchain 執行 incremental `ram_is` link 已成功。
連結後 `__wrap_usb_hal_driver` 在 `0x2010fff8`，原廠 `usb_hal_driver` 在
`0x201100c0`；`usb_hal_calibrate()` 內對函式表的引用已解析為
`0x2010fff8`。這確認 wrapper 確實進入映像，而非只有編譯出物件。

## 四份測試映像

四份映像採用**待板上驗證的對應假設**：PRO1 的 `0x40000108[2:1]` 與
Smart map 裡 `RX_BOOST[2:1]` 用相同編碼。wrapper 在原廠 `chip_init` 前
read-modify-write `0x40000108`，只改 mask `0x6`，隨即讀回，再執行原廠
電源／clock 與 PHY baseline 初始化，最後再讀回一次。讀回 bit 不符合
目標值時回傳錯誤，不會把失敗當成功。它**不會寫入永久 eFuse**。

| 測試值 | 標稱增益 | flash image | SHA-256 |
| ---: | ---: | --- | --- |
| `00` | 0 dB | [flash_is_rxboost_0dB.bin](USB_PHY/boost_bins/flash_is_rxboost_0dB.bin) | `70367c0a16068449d6debfc7890bc8e8e8a9a86ba1b8f98be59ee146a3052238` |
| `01` | 3.3 dB | [flash_is_rxboost_3p3dB.bin](USB_PHY/boost_bins/flash_is_rxboost_3p3dB.bin) | `e8a3bd85ea03041e53f9028f7231dfb7674dea95c2bfea870ec05c0b012ad336` |
| `10` | 6.9 dB | [flash_is_rxboost_6p9dB.bin](USB_PHY/boost_bins/flash_is_rxboost_6p9dB.bin) | `489e42da7a4e272b9a3b57165b6f0bac624001b6238b4d9d5da0f4fc1e8d56eb` |
| `11` | 9.3 dB | [flash_is_rxboost_9p3dB.bin](USB_PHY/boost_bins/flash_is_rxboost_9p3dB.bin) | `e8d15d46a51c1ca5b7a2b3304eb8dce361b0910388dbb53356ee9adb42c03cbc` |

四份映像均由 [`build_boost_variants.sh`](USB_PHY/build_boost_variants.sh)
按序使用外部 Realtek toolchain 編譯、封裝；每份 8,298,496 bytes，且映像內各自
包含對應的 `[USB BOOST] candidate level=N` 字串。完整 hashes 另存於
[`SHA256SUMS`](USB_PHY/boost_bins/SHA256SUMS)。沒有板上讀回或電氣量測，
所以檔名中的 dB 是待驗證的 **Smart 欄位標稱值**，不能當成已量到的
PRO1 類比增益。

測試時請保留以下兩行啟動 log：

```text
[USB BOOST] candidate level=N syscfg2 before=... target=... read=...
[USB BOOST] after PHY init=...
```

第一行可判斷寄存器寫入是否成功；第二行可判斷原廠初始化有沒有覆蓋。
兩行都符合仍只能證實數位寄存器值，無法單獨證明增益的 dB 數字。
