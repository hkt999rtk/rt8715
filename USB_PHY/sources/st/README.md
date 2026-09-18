# ST 文件狀態

原文章：[How to adjust USBPHYC settings using USB Eye Diagram](https://wiki.st.com/stm32mpu/wiki/How_to_adjust_USBPHYC_settings_using_USB_Eye_Diagram)。

2026-09-18 搜尋索引可讀到 STM32MP2 的 disconnect／squelch tuning 對照，但直接下載、printable／render／raw／API 及 ST 中國站入口遇到 timeout 或 HTTP/2 error。因此本目錄沒有宣稱保存了該文章完整原頁。

另已下載 [ST 官方 Linux USB2 PHY driver](phy-stm32-usb2phy_v6.6-stm32mp.c)，原始來源是 [STMicroelectronics/linux](https://github.com/STMicroelectronics/linux/blob/v6.6-stm32mp/drivers/phy/st/phy-stm32-usb2phy.c)。其 `COMPDISTUNE`、`SQRXTUNE` mask 及 `st,comp-dis-tune`、`st,sqrx-tune` 解析可直接查閱。

注意：網頁與 driver 版本可能不同，本次沒有將兩者的 SYSCFG 地址／TRIM register 編號視為逐位元相同。它們只用來佐證調整參數的種類，並非 Realtek register map。
