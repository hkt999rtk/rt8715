# Intel GPVNDCTL 資料狀態

來源：[Intel Agilex HPS GPVNDCTL](https://www.intel.com/content/www/us/en/programmable/hps/agilex/gwy1561686904303.html)。

2026-09-18 的搜尋索引可讀到該頁欄位說明，但完整頁面下載未成功。HTTPS requests 遇到 certificate chain 錯誤；curl 及 Altera 對應網址回 HTTP 403。本檔是調查摘要，**不是原始網頁的完整副本**。

索引說明可確認：GPVNDCTL 位於 controller offset `0x34`，`NewRegReq`、`VStsBsy`、`VStsDone` 分別為 bit 25／26／27；UTMI vendor control 使用 `VCtrl[11:8]`。該欄位同時有 ULPI 模式用途，需依整合方式判讀。

本地 [Realtek DWC header](../../evidence/rtl8195b_dwc_otg_regs.h)也保存相同完成／忙碌欄位定義，可離線交叉核對。兩者都沒有提供 Realtek PHY `E6/F0` 的類比位元定義。Intel 頁面上的 SoC base address 不能套到 Realtek。
