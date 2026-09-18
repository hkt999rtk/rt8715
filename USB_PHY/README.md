# USB PHY 調查資料

整理日期：2026-09-18。比較目前 `rt8715` 專案與 `../carbox_smart.orig` 的 USB PHY 初始化，並保存相關技術文件。

- [調查報告](REPORT.zh-TW.md)：設定差異、library 反組譯、可確認的影響及仍缺少的寄存器資料。
- [文件與來源索引](SOURCES.md)：每份資料的用途、原始網址、離線檔案及下載狀態。
- [Rockchip USB PHY 設計指南](sources/rockchip/Rockchip_Developer_Guide_USB_PHY_CN.pdf)：30 頁，含 Synopsys PHY 調整參數與異常斷線案例。
- [NXP PTN3222DUK datasheet](sources/nxp/PTN3222DUK.pdf)：48 頁，含可調 disconnect threshold 範例。
- [Synopsys controller/PHY 整合文件（含離線圖片）](sources/synopsys/nanophy_to_otg.offline.html)。
- [本地證據清單](evidence/local_manifest.json)：來源路徑、Git revision、archive/member SHA-256。
- [下載清單](sources_manifest.json)與[完整檔案校驗值](SHA256SUMS)。

PDF、原始碼及 `.txt` 可離線閱讀。原始 `.html` 保留下載內容，網站樣式可能仍需要網路；Synopsys 整合文章另提供包含五張原圖的 `.offline.html`。PDF 抽取文字的表格可能失去欄位對齊，數值請以原始 PDF 為準。

Intel、ST 的部分網頁及 Rockchip HTML 鏡像未成功下載，原因與替代資料已列入索引；沒有把錯誤頁當成文件。Synopsys 的文件索引頁也不等於完整 databook。

目前可確認 controller 為 **Synopsys DWC2 3.10a**；類比 PHY 的供應商／macro 版本，以及 Realtek `E6`、`F0` 的精確位元定義仍未確認。本目錄是調查資料，不含 firmware 設定修改或板上驗證結果。

校驗方式：在本目錄執行 `sha256sum -c SHA256SUMS`。來源檔案保留各自的版權／授權註記；Rockchip PDF 由公開鏡像取得，封面標示「内部资料」，來源身分詳見索引。
