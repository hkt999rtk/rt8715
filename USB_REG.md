# RT8715 / AmebaPro USB PHY register 與 RX boost

## 依據與範圍

主要依據是本機 [`USB20PHY_Register Table_RLE0799(AmebaPro).doc`](pro1_usb_material/USB20PHY_Register%20Table_RLE0799%28AmebaPro%29.doc) 的 **Register Table**。這份 Word 文件列的是 USB 2.0 PHY 的 8-bit、分頁寄存器；`E4` 是 PHY register 位址，不是 CPU MMIO 位址。以下程式路徑另參照本機 `lib_usbsmart.a(amebapro_usb.o)` 的[行為還原](USB_PHY/evidence/amebapro_usb.recovered.c)。還原檔不是原廠 C 原始碼；實際類比增益仍需板上量測。

## PRO1 RX boost：page 0 / E4[3:2]

Word 文件在 `(Address:0xE4)` 表格的 bit `3:2` 列出 `REG_RX_BOOST[1:0]`，R/W，預設 `00`；Description 為 `D2S boost`，只明確寫出 `00: no boost` 與 `11: max boost`。四種編碼如下：

| Level | E4[3:2] | E4 欄位值 | 文件可確認的意義 |
| ---: | :---: | ---: | --- |
| 0 | `00` | `0x00` | 無 boost，文件預設值 |
| 1 | `01` | `0x04` | 中間檔；未提供增益數值 |
| 2 | `10` | `0x08` | 中間檔；未提供增益數值 |
| 3 | `11` | `0x0C` | 最大 boost；未提供增益數值 |

遮罩為 `0x0C`，移位為 2。保留 `E4` 其他 bit 的修改式為：

```c
target = (before & ~0x0Cu) | ((level & 3u) << 2);
```

同一個 `E4` 的 `7:6` 是 `REG_RPD`、`5:4` 是 `REG_RPU`、`1:0` 是 `REG_SD`，因此不可直接覆蓋整個 byte。Word 文件對 `REG_SD` 的欄位預設值與文字敘述不一致；wrapper 讀取實值再只改 boost 位元，不依賴整個 `E4` 的文件預設 byte。

文件中的 `00` 是 **PHY 欄位預設值**。一般韌體使用 `CARBOX_USB_RX_BOOST_LEVEL=-1`，完全不改 `E4`；開機後實值應以板上讀取為準。Word 文件沒有 `01`、`10`、`11` 的 dB 對照，也沒有證明每檔補償量呈線性。

## 寄存器所在頁與相關控制

Word 文件對 page 1 的位址會加註 `/Page1`；上面的 `(Address:0xE4)` 沒有此註記，屬於 page 0。`F4[6:5]` 是 `page_sel`，`00` 選 page 0；不可將 page 1 的 `E4` 當作 RX boost。

同一份 Word 文件還有幾個容易混淆的欄位：

| PHY 位址 | 欄位 | 文件說明 |
| --- | --- | --- |
| page 0 / `E5[7:4]` | `REG_SEN_DEV` | Device sensitivity reference voltage |
| page 0 / `E5[3:0]` | `REG_SEN_HST` | Host sensitivity reference voltage；預設 `1001` 對應 562.5 mV |
| page 0 / `E7[7:5]` | `REG_SRC` | HS transmitter slew rate control |
| page 0 / `F0[6]` | `discon_enable` | 啟用 host disconnect option |
| `F4[6:5]` | `page_sel` | PHY register page selection |

`E5` 的 sensitivity、`F0` 的 disconnect 與 `E4` 的 RX boost 是不同控制。`pro1_usb_material/comments.txt` 提到長線可嘗試提高 boost，也提醒需要向 AIC 確認各檔補償量；這是工程討論紀錄，不是量測表。

## 本機 `lib_usbsmart.a` 與 wrapper

原廠 `amebapro_usb.o` 的 `chip_init` 先開 USB 電源和 clock，再透過 DWC2 controller 的 `GPVNDCTL` 寫 PHY：page 0 的 `E0=6C`、`E1=81`、`E2=62`、`E7=41`，page 1 的 `E0=91`，最後切回 page 0。此版本沒有顯式寫 `E4`，`get_cal_data()` 回傳空指標。[還原路徑](USB_PHY/evidence/amebapro_usb.recovered.c)

[`usb_phy_driver_wrap.c`](project/realtek_amebapro_v0_example/src/carbox/usb_phy_driver_wrap.c) 透過 GNU ld `--wrap=usb_hal_driver` 轉接原廠函式表。`CARBOX_USB_RX_BOOST_LEVEL=-1` 時只呼叫原廠初始化。Level `0..3` 時先呼叫原廠初始化，確認成功後在 page 0 讀 `E4`、只改 `[3:2]`、寫回並重新讀取驗證。PHY 的讀寫透過 controller `GPVNDCTL=0x400C0034`；寫入資料使用 `0x400F001C`。這不是燒寫 eFuse。讀寫逾時或欄位讀回不符時，wrapper 回傳錯誤並輸出 log：

```text
[USB BOOST] PHY p0:E4 level=N before=XX target=XX read=XX
```

讀回一致只證明寄存器操作成功；通訊相容性和雜訊表現仍需板測。

## 四份測試映像

[`USB_PHY/build_boost_variants.sh`](USB_PHY/build_boost_variants.sh) 依序以 level 0、1、2、3 建置本機映像：

```text
USB_PHY/boost_bins/flash_is_rxboost_level0.bin
USB_PHY/boost_bins/flash_is_rxboost_level1.bin
USB_PHY/boost_bins/flash_is_rxboost_level2.bin
USB_PHY/boost_bins/flash_is_rxboost_level3.bin
```

腳本產生 `SHA256SUMS` 並把一般 `flash_is.bin` 恢復為 `-1` 的原廠設定狀態。映像與 log 是本機產物，不納入 Git。先前命名為 `0dB/3p3dB/6p9dB/9p3dB` 的四份映像改的是 **AmebaSmart SYSCFG2**，不符合這份 PRO1 PHY register table；不得拿來代表 `E4` 四檔，重新建置成功後由腳本清除。

## 舊判斷為何錯誤

先前將 AmebaSmart `SEC_OTP_SYSCFG2` offset `0x108` 的 `USB_PHY_RX_BOOST[2:1]` 套到 PRO1 的 `0x40000108[2:1]`。PRO1 的 SYSON header 只確認 `0x40000108` 是 `hs_efuse_syscfg2`，沒有將其中兩 bit 命名為 USB RX boost。現在有直接標示 AmebaPro PHY `REG_RX_BOOST` 的 Word register table，應採用 **page 0 / E4[3:2]**。AmebaSmart 的 0、3.3、6.9、9.3 dB 編碼不能直接套到這個欄位。
