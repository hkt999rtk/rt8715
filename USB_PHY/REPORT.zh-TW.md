# RTL8715／AmebaSmart USB PHY 設定差異調查

整理日期：2026-09-18。範圍：比較本地原始碼、實際 USB archive 與 link map，再以晶片廠文件解釋可能的電氣影響。來源總表見 [SOURCES.md](SOURCES.md)。

**1. 目前能下的結論**

`carbox_smart.orig` 比 Realtek AmebaSmart 官方基準表多了 **page 0 / E6 = 0x90** 與 **page 0 / F0 = 0xAC**。Git 歷史分別把用途描述為 RX 閾值調整，以及處理地線干擾相關的 disconnect 問題。這能確認修改者的意圖，還不能證明確切改了哪些類比參數、調整方向或多少 mV。

目前 `rt8715` USB library 使用另一套初始化值，`usb_chip_get_cal_data()` 回傳 `NULL`；在已核對的 chip init/calibration 路徑中，沒有上述兩筆額外寫入，也沒有 `p1:E5=0x0A`。未顯式寫入的寄存器，其實際值可能來自硬體預設、boot 或其他設定，不能當作零。

使用者先前提供的 `GSNPSID=0x4f54310a` 對應 **Synopsys DWC2 controller 3.10a**。它不能辨識類比 PHY 供應商或 macro 版本。因此先前「PHY 很可能也是 Synopsys picoPHY／femtoPHY」的說法應修正為：**目前證據不足以判定**。[Linux 定義原檔](sources/linux/dwc2_core_bce1305c0ece3.h)、[原始來源](https://android.googlesource.com/kernel/common/+/bce1305c0ece3/drivers/usb/dwc2/core.h)。

**2. 比較對象與可重現證據**

| 對象 | 本次核對的資料 |
| --- | --- |
| 目前專案 | `/home/kevin/work/rt8715`，HEAD `16b42a59d3816dd3cc2306bc2dc2fa726937bb5e`；工作目錄已有其他未提交修改 |
| Smart 參考專案 | `/home/kevin/work/carbox_smart.orig`，HEAD `15682fc26fc7cf2f38f4072a8b7a844e5322a063`；被比較的 `ameba_usb.c` 無未提交修改 |
| Smart 初始化檔 | `component/soc/amebasmart/fwlib/ram_common/ameba_usb.c`，已保存[可讀副本](evidence/smart_ameba_usb.utf8.c)及原始 bytes |
| 本專案 vendor archive | `project/realtek_amebapro_v0_example/GCC-RELEASE/usb_lib/build/lib_usbsmart.a` |
| 本專案 link archive | 同目錄的 `lib_usbsmart_link.a`；[build 規則](evidence/build_archive_selection.txt)、[link map 摘錄](evidence/firmware_link_map_excerpt.txt) |
| 反組譯工具 | 外部 Realtek GCC 6.4.1 toolchain 的 `arm-none-eabi-objdump -dr` |

兩份 archive 裡的 `amebapro_usb.o` SHA-256 相同：

```text
9f929c44bf77543ffc521b3cf0c14cd0aa7ba3cbf031230a78c2bc941e86e182
```

完整來源／archive／member 指紋見 [local_manifest.json](evidence/local_manifest.json)。結論針對本次磁碟快照，不把前次裝置 log 當成本次 image 的執行證明。

**3. 校準表逐項比較**

下表中「未寫」表示本次檢查的初始化路徑沒有顯式寫該值，不表示寄存器不存在或值為零。`page` 是 PHY 寄存器分頁，不能省略。

| Page / register | 目前 rt8715 chip init | Realtek Smart v1.0.1 基準 | carbox_smart.orig |
| --- | --- | --- | --- |
| 0 / E0 | `6C` | `9D` | `9D` |
| 0 / E1 | `81` | `19` | `19` |
| 0 / E2 | `62` | `DB` | `DB` |
| 0 / E4 | 未寫 | `68` | `68` |
| 0 / E6 | 未寫 | 未寫 | **`90`，後加** |
| 0 / E7 | `41` | 未寫 | 未寫 |
| 0 / F0 | 未寫 | 未寫 | **`AC`，後加** |
| 1 / E5 | 未寫 | `0A` | `0A` |
| 1 / E6 | 未寫 | `D8` | `D8` |
| 2 / E7 | 未寫 | `52` | `52` |
| 1 / E0 | `91` | 依序 `04 → 00 → 04` | 依序 `04 → 00 → 04` |

Smart 表內的 `p1:E0` 是三次有順序的寫入，不能去重成最後一筆；目前沒有找到足以解釋該序列內部功能的位元文件。`{FF,00,00}` 是表結束標記。結構是三個 byte：`page/addr/val`；Smart getter 忽略 `mode`，回傳同一張表。

官方比對依據：[R01 原始碼](sources/realtek/ameba_usb_v1.0.1.c)、[R02 結構定義](sources/realtek/ameba_usb_v1.0.1.h)，以及 [Realtek 官方 Linux USB 文件](https://ameba-aiot.github.io/ameba-iot-docs/linux/en/latest/rst_linux/7_usb/1_usb_otg_toprst.html)的 [DTS 離線副本](sources/realtek/ameba_linux_usb_otg.html)。

兩筆後加設定的歷史：

| Commit / 日期 | 實際變更 | 能支持的用途判斷 |
| --- | --- | --- |
| `46b4f9a9` / 2025-10-02 | 加 `p0:F0=AC` | commit 與註解明示 disconnect 閾值／地線干擾 |
| `b02f2423` / 2026-03-21 | 加 `p0:E6=90` | commit 明示 RX 閾值；同一 commit 還提到 library 更新 |

原始差異：[F0 patch](evidence/smart_46b4f9a.patch)、[E6 patch](evidence/smart_b02f242.patch)。使用者貼文雖在 `p1:E5=0A` 後加了 `new config` 註解，但官方基準原本就有這個值，不能將它視為這兩次調整新增的項目。

**4. 目前 library 實際執行什麼**

`amebapro_usb.o` 的 `usb_chip_get_cal_data()` 只有 `movs r0,#0; bx lr`。`usb_hal_calibrate()` 遇到空指標就返回，所以目前 getter 不會提供 Smart 那張表。[chip 反組譯](evidence/lib_usbsmart.a.amebapro_usb.o.disassembly.txt)、[calibrate 反組譯](evidence/lib_usbsmart.a.usb_hal.o.disassembly.txt)。

`usb_chip_init()` 本身仍有設定 PHY：完成電源／clock ready 流程後，透過讀 `D4`、寫 `F4` 控制 page bits `[6:5]`，在 page 0 寫 `E0/E1/E2/E7`，切 page 1 寫 `E0=91`，最後回 page 0。**getter 回傳 NULL 不代表 PHY 完全沒初始化。**

目前 archive 內的 `DWCWritePhyReg()` 可還原為以下存取順序：

```text
等待 0x400C0034 的 bit 26 清除
將 data 寫入 0x400F001C
寫 0x0A300000 | ((addr << 8) & 0xF00) 至 0x400C0034
等待 bit 27 置位
寫 0x0A300000 | ((addr << 4) & 0xF00) 至 0x400C0034
等待 bit 27 置位
```

`0x400C0034 = USB_BASE + 0x34`，對應 controller 的 **GPVNDCTL**；PHY 地址拆成低／高 nibble 送出。本地 [DWC 寄存器 header](evidence/rtl8195b_dwc_otg_regs.h) 定義 `NewRegReq=bit25`、`VStsBsy=bit26`、`VStsDone=bit27`。[USB base 定義](evidence/rtl8195b_usb_base.txt)。

Intel 同類 controller 的 [GPVNDCTL 說明](https://www.intel.com/content/www/us/en/programmable/hps/agilex/gwy1561686904303.html)也描述 vendor control；該網頁未成功完整下載，存取狀態及摘要見 [D02 說明](sources/intel/README.md)。所以表中的 `E6/F0` 不能當成 DWC MMIO offset，也不能直接套用其他 SoC 的 PHY 位元配置。`0x400F001C` 在反組譯中可確認被用來寫入 data，但本次未取得其正式寄存器名稱。

另外，該 write routine 的等待迴圈沒有看見軟體 timeout；read routine 有有限次 polling，失敗回 `FF`。這是 driver 診斷上的限制，不能單憑 `FF` 就認定讀值有效，也不能由此認定現有 disconnect 是 polling 造成。

**5. 文件能解釋的電氣機制**

Synopsys 自己把 controller 與 PHY 分成兩個模組，以 UTMI／UTMI+ 連接，且有各自的 reset／clock 條件。[整合文章原始網址](https://www.synopsys.com/dw/dwtb.php?a=usb2_nanophy_to_otg)、[含五張原圖的離線版](sources/synopsys/nanophy_to_otg.offline.html)。官方也提供多種 PHY 系列；同一 controller 品牌不足以推定相同 PHY。[PHY 產品資料](https://www.synopsys.com/designware-ip/interface-ip/usb/usb-2-0-phy.html)、[離線副本](sources/synopsys/usb2_phy_overview.html)。

Rockchip 文件第 2.2 節將 RK3066／RK3188／RK3288 的 USB2 PHY 列為 Synopsys picoPHY；表 2-7（PDF 第 15–17 頁）提供下列調整例子：

| 類別 | 文件中的參數 | 文件中的範例／作用 |
| --- | --- | --- |
| Host disconnect | `COMPDISTUNE[2:0]` | `100` 為基準，`111` 為 +4.5%，`000` 為 −6% |
| HS 接收 squelch | `SQRXTUNE[2:0]` | `011` 為基準，`111` 為 −20%，`001` 為 +10% |
| TX 波形 | `TXVREFTUNE`、`TXRISETUNE`、`TXPREEMPAMPTUNE` | 分別涉及振幅、上升／下降時間、預加重 |
| 阻抗 | `TXRESTUNE` | 調整 source impedance |

同文件第 4.2／4.3 節（PDF 第 27–28 頁）說明弱訊號可能低於 squelch 閾值，以及過大振幅可能觸發誤斷線。這些是其他晶片的機制例證，數值不能換算成本案 `E6/F0`。[Rockchip PDF](sources/rockchip/Rockchip_Developer_Guide_USB_PHY_CN.pdf)、[公開鏡像來源](https://usermanual.wiki/Document/RockchipDeveloperGuideUSBPHYCN.1009664934.pdf)。

ST 官方 driver 同時定義 `COMPDISTUNE` 與 `SQRXTUNE`，並分別讀取 `st,comp-dis-tune`、`st,sqrx-tune`，進一步佐證這是兩種不同控制。其 SYSCFG 位元配置只適用該 ST 實作。[T02 driver 副本](sources/st/phy-stm32-usb2phy_v6.6-stm32mp.c)、[ST 官方原始碼](https://github.com/STMicroelectronics/linux/blob/v6.6-stm32mp/drivers/phy/st/phy-stm32-usb2phy.c)。相關 [ST 眼圖調整文章](https://wiki.st.com/stm32mpu/wiki/How_to_adjust_USBPHYC_settings_using_USB_Eye_Diagram)本次未能下載，詳見 [T01 狀態](sources/st/README.md)。

NXP PTN3222DUK datasheet 的表 20（PDF 第 21 頁）以 register `0x0A[1:0]` 提供 575／675／775／875 mV 四檔及 30 mV hysteresis，顯示實作可有自己的編碼與電壓範圍。這顆是 eUSB2 redriver，不能用來推定 Realtek 或 Synopsys PHY 型號。[datasheet](sources/nxp/PTN3222DUK.pdf)、[NXP 原始來源](https://www.nxp.com/docs/en/data-sheet/PTN3222DUK.pdf)。另有 NXP 作者的 [Cadence Salvo patch 郵件](sources/nxp/cadence_salvo_disconnect_threshold_patch.html)以調整 disconnect 閾值處理誤斷線；這是設計案例，非本案可直接套用的 patch。[原始郵件鏡像](https://lkml.indiana.edu/hypermail/linux/kernel/2305.2/00920.html)。

Realtek 自己的 [Smart OTP 定義](sources/realtek/sysreg_sec_v1.0.1.h)也列出 TX swing、slew rate、host/device squelch、阻抗及 RX boost。它支持「確實存在多種校準維度」，但未建立這些欄位與 `p0:E6`／`p0:F0` 的對應。[官方原始碼](https://github.com/Ameba-AIoT/ameba-rtos/blob/v1.0.1/component/soc/amebasmart/fwlib/include/sysreg_sec.h)。

**6. 對本案兩筆修改的判斷**

| 修改 | 已確認 | 合理但待驗證的解釋 | 目前無法宣稱 |
| --- | --- | --- | --- |
| `p0:E6=90` | Smart 的 RX 閾值相關 commit 新增此值 | 可能改變接收偵測能力／雜訊容忍度 | 不能確認就是 squelch、不能確認升降方向或 mV |
| `p0:F0=AC` | 原 commit 明示 disconnect／地線干擾用途 | 可能改變 host disconnect 判斷餘量 | 不能確認對應 `COMPDISTUNE`，也不能確認降低了本案斷線率 |
| `p1:E5=0A` | Smart 官方基準既有 | 屬該 SoC 的基準校準 | 不能僅憑 `new config` 註解判定功能或移植必要性 |

若 E6 確實控制 squelch，降低閾值可能接收到衰減較大的訊號，也可能更容易把雜訊當作有效訊號；若 F0 確實提高 disconnect 閾值，可能減少某些誤觸發，但仍要驗證真正拔除的偵測行為。這兩段是依一般比較機制推導的條件性解釋，並非已解碼 `90/AC`。

「RX 閾值」與「host disconnect 閾值」不等同於應用層 TX／RX 分流。PHY 調整作用於該埠及當下角色；若 driver 在 host/device 初始化都套用相同表，不能假設只影響 NCM RX。也不能用調整閾值取代對地電位差、供電、佈線與連接器的量測。

先前使用者 log 出現過 `CAL p0 A0xE6 W0x90 R0x90 OK`、`CAL p0 A0xF0 W0xAC R0xAC OK`、`CAL p1 A0xE5 W0x0A R0x0A OK`，代表那次映像有額外寫入與讀回的紀錄。本次 library 的上述初始化路徑沒有這些寫入，因此兩者應按版本分開；不能假設新版 library 已保留先前 patch。[使用者 log 摘錄](evidence/user_log_excerpt.txt)。讀回一致僅證明設定存取，不能證明電氣效果。

**7. 後續如何驗證**

1. 先取得 RTL8195B／RTL8715 對應 silicon revision 的 PHY register map，確認 `p0:E6`、`p0:F0` 的 bit mask、編碼、reserved bits，以及適用 host/device 角色。若只有 AmebaSmart 資料，仍不足以解碼本平台。
2. 在裝置目前基準版本記錄角色、初始化次數、page、修改前值、目標值及讀回結果；確認讀／寫地址別名和 page 還原。只在位元定義已知時做欄位 read-modify-write，未知 mask 不能自行臆測。
3. 確認平台適用性後，分別測試基準、僅 F0、僅 E6、兩者一起；保留本平台其他基準設定，以區分效果。此表是實驗規劃，尚未實施。
4. 固定板子、車機、線材與供電，對照 attach／enumeration／port disconnect／reset、CRC／transaction error 及吞吐紀錄；搭配 DP/DM 波形與真正拔除測試。單看大量 NAK 不能建立 PHY 閾值是原因的證據。

Realtek 官方文件將 PHY calibration 定義為 SoC 相關設定，並建議必要調整時聯繫 FAE；可用來索取上面缺少的位元資料。[官方說明](https://ameba-aiot.github.io/ameba-iot-docs/freertos/cn/latest/rst_rtos/7_usb/1_usb_otg_toprst_cn.html)、[離線文字](sources/realtek/ameba_freertos_usb_otg_zh.txt)。

本次完成的是資料比對、下載保存與證據整理。未做板上量測，也未更動 USB PHY 初始化或產生新的 firmware。
