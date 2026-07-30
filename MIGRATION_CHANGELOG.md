# carbox_pro1 Migration Change Log

本文記錄 `carbox_smart` 移植到 Pro1 SDK (`carbox_pro1`) 的目前做法、已修改位置、驗證結果與下一步風險。此文件可作為後續提供客戶的移植過程說明基礎。

## 目前結論

- `carbox_pro1` 以 Pro1 SDK `sdk-ameba-v5.2j` 為 baseline，不直接混入 Smart SDK component。
- 正常 Pro1 baseline build 仍維持乾淨，目標是可產出 LP/IS `.axf`。
- macOS host native compiler 已測試但不列入正式支援；目前 Mac 使用 Docker 並預設跳過 image manipulation。客戶 native Linux build 透過 `tools/carbox/build_baseline_host_linux.sh` 明確切回 SDK 內建 Linux cross-compiler，且預設執行完整 SDK image manipulation。
- Smart CarPlay/AndroidAuto `.a` 已搬到 `third_party` 作為 reference artifacts。
- Smart CarPlay `.a` 經確認是 `armv8-a`；Pro1 IS/LP 是 `armv8-m`。目前所有 Smart `.a` integration 都只做 experimental link probe，不代表可執行 firmware。
- 早期 `CARBOX_EXPERIMENTAL_SMART_A_LINK=1 make all` 曾可完成完整 SDK link probe，但該路徑會拉入大量 Pro1 examples，不適合作為 CarPlay 專用判斷。
- 目前已改用 standalone CarPlay link probe；結果顯示 Smart archives 可被 linker 讀取。probe 只使用 Pro1 native source/object/archive、`smart_wrapper`、已核准的 `experimental_stubs`，目前 link probe 可通過。
- 目前仍留在空殼目錄的 symbols 是明確的 runtime integration gap；已找到 Pro1 對應或可控 wrapper 的項目會移出空殼目錄。

## Phase 1: Pro1 SDK Baseline

### 做法

建立 `carbox_pro1/` 為完整 Pro1 SDK copy，保留原始 SDK 結構：

- `component/common`
- `component/soc/realtek/8195b`
- `project/realtek_amebapro_v0_example/GCC-RELEASE`
- `tools/arm-none-eabi-gcc`

Pro1 SDK source revision 記錄為：

```text
a3491e39395ec8ccc96d21a2353e40cb753e275f
```

### Build wrapper

新增 Docker build wrapper：

- `carbox_pro1/docker/Dockerfile`
- `carbox_pro1/docker/build-baseline.sh`

正式 baseline command：

```bash
docker build -t carbox-pro1-build carbox_pro1/docker
docker run --rm -v "$PWD":/work carbox-pro1-build
```

Docker wrapper 預設使用：

```bash
CARBOX_SKIP_IMAGE=1 make all
```

原因是第一階段先確認 LP/IS compile + link 成功，不把 vendor image manipulation tool 當作 blocking item。

### Image manipulation bypass

修改 `application.is.mk`，加入 `CARBOX_SKIP_IMAGE` guard：

```make
ifeq ($(CARBOX_SKIP_IMAGE),1)
all: prebuild build_info application sensor
else
all: prebuild build_info application sensor manipulate_images
endif
```

目的：

- 正常 SDK full image build 流程仍保留。
- `CARBOX_SKIP_IMAGE=1` 時只驗證 link output。
- 避免目前 host/Docker 下 `elf2bin.linux` runtime 問題阻塞 baseline。

已觀察到的 `elf2bin.linux` 問題：

- Pro1 vendor `elf2bin.linux` 在 Docker 內執行 `convert amebapro_bootloader.json PARTITIONTABLE secure_bit=0` 時曾出現 segmentation fault。
- Smart tree 內找到的 cross-platform `elf2bin` 可執行該 command，但沒有產生 Pro1 所需 `partition.bin`，所以不視為可替代 Pro1 image tool。
- 目前第一里程碑以 `.axf` link 成功為驗收重點。

### Baseline 驗證

已驗證：

```bash
docker run --rm -v "$PWD":/work carbox-pro1-build
```

結果：

- `application_lp/Debug/bin/application_lp.axf` 存在。
- `application_is/Debug/bin/application_is.axf` 存在。
- `carbox_pro1 baseline build passed`。

### Host toolchain policy

Mac host 曾測試 `/Applications/ARM/bin/arm-none-eabi-gcc`：

```text
arm-none-eabi-gcc (GNU Arm Embedded Toolchain 9-2020-q2-update) 9.3.1
```

結果 LP link 失敗於 host newlib/toolchain 相容性問題：

```text
undefined reference to `_fini'
```

因此目前不為了 Mac host compiler 修改 SDK/runtime。Mac build 維持 Docker。

新增 customer Linux host wrapper：

- `tools/carbox/build_baseline_host_linux.sh`

該 wrapper 只允許在 native Linux 執行，並明確指定 SDK 內建 compiler：

```text
tools/arm-none-eabi-gcc/asdk/linux/newlib/bin/arm-none-eabi-
```

此 wrapper 預設：

```text
CARBOX_SKIP_IMAGE=0
```

也就是客戶 native Linux 機器會跑完整 SDK build，包括 vendor
`elf2bin.linux` image manipulation。若工程診斷只需 link-only，可手動覆蓋：

```bash
CARBOX_SKIP_IMAGE=1 carbox_pro1/tools/carbox/build_baseline_host_linux.sh
```

目的：

- 客戶 Linux 機器不依賴 host `PATH` 裡的 cross-compiler。
- 避免不同 `arm-none-eabi-gcc` / newlib 版本造成 link 差異。
- 保持 Docker 與 native Linux 都使用同一套 Pro1 SDK compiler。
- 將 Docker/macOS 的 link-only baseline 與客戶 native Linux full image
  acceptance 明確分開。

## Phase 2: Smart 功能盤點與 Pro1 可控移植

### Inventory 文件

新增：

- `carbox_pro1/MIGRATION_PHASE2_INVENTORY.md`

內容涵蓋：

- Smart AP/HP/LP `main.c` startup flow。
- Pro1 IS/LP startup 對照。
- Smart Wi-Fi、USB/NCM、Bluetooth、OTA、Factory/MP、CarPlay/AndroidAuto 功能分類。
- 每個功能標記為 Pro1 native replacement、adapter required、Smart-only、hardware-confirmation required。

### Pro1 product hook skeleton

新增 Pro1 可控 product entry，不直接覆蓋 Pro1 main：

- `project/realtek_amebapro_v0_example/src/carbox/carbox_app.c`
- `project/realtek_amebapro_v0_example/src/carbox/carbox_app.h`
- `project/realtek_amebapro_v0_example/src/carbox/carbox_network_config.c`
- `project/realtek_amebapro_v0_example/src/carbox/carbox_network_config.h`
- `project/realtek_amebapro_v0_example/src/carbox/carbox_usb_smoke.c`
- `project/realtek_amebapro_v0_example/src/carbox/carbox_usb_smoke.h`

修改：

- `project/realtek_amebapro_v0_example/src/main.c`
- `project/realtek_amebapro_v0_example/GCC-RELEASE/application.is.mk`

目前 product hook 只做：

- 印出 CarBox Pro1 product entry log。
- 建立 Smart `user_use_config_ip()` 對應的 network config staging adapter。
- 建立 USB/NCM smoke adapter placeholder，只記錄狀態，不初始化 USB hardware。
- 建立 factory mode check placeholder。

尚未做：

- 不啟動 CarPlay。
- 不啟動 AndroidAuto。
- 不改 USB runtime behavior。
- 不改 Wi-Fi bring-up。
- 不改 OTA/factory image flow。

原因：

- 先建立 Pro1-controlled extension point，避免直接移植 Smart AP-side main 導致 Pro1 startup、FreeRTOS、IPC、USB/Wi-Fi 初始化順序失控。

## Smart CarPlay `.a` Reference Staging

### 搬移內容

Smart CarPlay libraries 來源：

```text
carbox_smart/amebasmart_gcc_project/project_ap/asdk/lib/carplay_app
```

搬移到：

```text
carbox_pro1/third_party/carbox_smart/carplay_app
```

已 staging 的 libraries：

```text
lib_link.a
lib_x264.a
lib_png.a
lib_SystemLib.a
lib_SystemLibEx.a
lib_UiLib.a
lib_accessory.a
lib_fdkaac.a
lib_init.a
lib_zlib.a
lib_AndroidAuto.a
lib_CarPlay.a
lib_Accessory2.a
lib_jpeg.a
lib_usbdev.a
lib_ncm.a
```

### ABI manifest 與檢查工具

新增：

- `carbox_pro1/third_party/carbox_smart/README.md`
- `carbox_pro1/third_party/carbox_smart/carplay_app/ABI_MANIFEST.tsv`
- `carbox_pro1/tools/carbox/check_carplay_libs.sh`

檢查結果：

- Smart CarPlay `.a` 為 `armv8-a`。
- Pro1 SDK native libraries 為 `armv8-m.*`。
- 因此這些 Smart `.a` 目前預設狀態是 `reference-only`。

重要限制：

```text
armv8-a Smart AP library cannot be linked into Pro1 armv8-m IS image as executable firmware.
```

## CarPlay Symbol Gap Analysis

新增分析 script：

- `carbox_pro1/tools/carbox/analyze_carplay_symbol_gaps.sh`

輸出：

- `carbox_pro1/third_party/carbox_smart/reports/carplay_symbol_gap.md`
- `carbox_pro1/third_party/carbox_smart/reports/carplay_missing_symbols.tsv`
- `carbox_pro1/third_party/carbox_smart/reports/usb_ncm_migration_note.md`

目的：

- 在不修改 Pro1 linker 的情況下，先盤點 Smart CarPlay `.a` 需要哪些外部 symbols。
- 判斷哪些可由 Pro1 baseline 提供，哪些需要 adapter 或 Pro1-compatible binary/source。

早期 gap analysis 報告曾顯示 Smart AP-side / POSIX-like / BT / OTA / lwIP / initcall 相關缺口，例如：

- `pthread_*`
- `lwip_recvmsg`
- `lwip_if_nametoindex`
- `bt_*`
- `ota_update_*`
- `__initcall*_start`
- `ap_ip`, `ap_netmask`, `ap_gw`

後續 standalone probe 已逐批收斂：POSIX、LwIP、RTOS、FLASH、Wi-Fi/network、
HAL/system、USB OS、TomMath alias、部分 OTA chunk-writer symbols 已改由
Pro1 SDK object/source 或 `smart_wrapper` 解決；仍留在
`experimental_stubs` 的項目才是目前明確 runtime integration gap。

這些是後續 adapter 設計的依據，不代表可以直接補空函式後執行。

## Experimental Smart `.a` Link Probe

### 目的

使用者接受「只 link，不執行」的探索方式後，新增 `CARBOX_EXPERIMENTAL_SMART_A_LINK=1` 模式。

此模式目的只有一個：

```text
探索 Smart CarPlay `.a` 放進 Pro1 IS link 後，實際會遇到哪些 duplicated/missing symbols。
```

此模式不是可執行 firmware integration。

### Link guard

修改：

- `project/realtek_amebapro_v0_example/GCC-RELEASE/application.is.mk`

新增 experimental link block：

```make
ifeq ($(CARBOX_EXPERIMENTAL_SMART_A_LINK),1)
CARBOX_SMART_CARPLAY_LIB_DIR := ../../../third_party/carbox_smart/carplay_app
LFLAGS += -Wl,--no-warn-mismatch
LIBFLAGS += -Wl,--whole-archive
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_link.a
...
LIBFLAGS += $(CARBOX_SMART_CARPLAY_LIB_DIR)/lib_ncm.a
LIBFLAGS += -Wl,--no-whole-archive
endif
```

說明：

- `--no-warn-mismatch` 只用於 experimental link probe，避免 linker 因 `armv8-a` / `armv8-m` mismatch 直接停止。
- `--whole-archive` 用來強迫暴露 Smart `.a` 內部依賴，方便找完整 link blocker。
- 正常 baseline 不啟用此 block。

### JPEG collision 處理

問題：

- Smart `lib_jpeg.a` 和 Pro1 `module_jpeg.o` / Pro1 JPEG example flow 有 symbol collision。

處理：

在 `CARBOX_EXPERIMENTAL_SMART_A_LINK=1` 時排除：

- `component/common/media/mmfv2/module_jpeg.c`
- `component/common/example/media_framework/mmf2_example_v3_init.c`
- `component/common/example/media_uvcd/example_media_uvcd.c`

原因：

- Experimental probe 讓 Smart `lib_jpeg.a` 提供 libjpeg-style symbols。
- `mmf2_example_v3_init.c` / `example_media_uvcd.c` 會引用 Pro1 JPEG/UVCD example path，對 CarPlay `.a` link 探索不是必要項目。
- 正常 Pro1 baseline 仍保留這些 sources。

### USB/dev example collision 處理

問題：

- Smart `lib_usbdev.a` / `lib_ncm.a` 進入 experimental link 後，Pro1 範例程式中的 USB/HTTP/cJSON 相關 symbols 會先造成 collision 或牽出非必要依賴。

處理：

在 `CARBOX_EXPERIMENTAL_SMART_A_LINK=1` 時排除 Pro1 example sources：

- `component/common/example/cJSON/example_cJSON.c`
- `component/common/example/httpd/example_httpd.c`
- `component/common/example/media_uvcd/example_media_uvcd.c`

說明：

- 這不是正式 USB migration。
- 只是避免 Pro1 SDK example code 先搶 symbols 或拉進不相關 dependency，讓 Smart `.a` 的下一個真實 blocker 浮出來。
- 正常 baseline 不受影響。

### FDK-AAC / FAAC collision 處理

問題：

- Smart `lib_fdkaac.a` 和 Pro1 `lib_faac.a` 都帶有 FFT/AAC 相關 symbols。
- Experimental link 曾卡在 `lib_faac.a(fft.o)` duplicated symbols。

處理：

把 `-l_faac` 從 `all` / `mp` common lib list 中移出，改成只在非 experimental mode 加回：

```make
ifneq ($(CARBOX_EXPERIMENTAL_SMART_A_LINK),1)
all mp: LIBFLAGS += -l_faac
endif
```

結果：

- Normal baseline link line 仍包含 `-l_faac`。
- Experimental mode 不 link Pro1 `lib_faac.a`。
- `fft.o` duplicated symbol blocker 消失。

### mDNS / DNS-SD collision 處理

問題：

- Pro1 `lib_mdns.a` 和 Smart `lib_CarPlay.a` 都帶有 Apple mDNS/DNS-SD implementation symbols。
- Experimental link 曾卡在 duplicated symbols。

代表性 duplicated symbols：

```text
mDNSPlatformSendUDP
mDNSPlatformInit
mDNSPlatformMemAllocate
mDNSPlatformMemFree
mDNSPlatformRawTime
mDNSPosixGetFDSet
mDNSPosixProcessFDSet
GetRRDisplayString_rdb
DNSServiceRefDeallocate
DNSServiceRegister
mDNSPlatformClose
mDNSPlatformMemCopy
mDNSPlatformMemSame
mDNSPlatformMemCmp
mDNSPlatformMemZero
mDNSPlatformOneSecond
gMDNSPlatformPosixVerboseLevel
```

處理：

把 `-l_mdns` 從 `all` / `mp` common lib list 中移出，改成只在非 experimental mode 加回：

```make
ifneq ($(CARBOX_EXPERIMENTAL_SMART_A_LINK),1)
all mp: LIBFLAGS += -l_mdns
endif
```

目的：

- Normal baseline 仍使用 Pro1 native `lib_mdns.a`。
- Experimental mode 讓 Smart `lib_CarPlay.a` 內建 mDNS/DNS-SD 先成為 owner。
- 只用來探索 link dependency，不代表選定正式 runtime mDNS owner。

### 目前 experimental link 結果

早期 full-SDK probe command：

```bash
docker run --rm -v "$PWD":/work carbox-pro1-build \
  bash -lc "cd /work/carbox_pro1/project/realtek_amebapro_v0_example/GCC-RELEASE && CARBOX_EXPERIMENTAL_SMART_A_LINK=1 CARBOX_SKIP_IMAGE=1 make all"
```

結果：

- Exit code: 0。
- `lib_faac.a(fft.o)` blocker 已消失。
- Pro1 `lib_mdns.a` vs Smart `lib_CarPlay.a` duplicated symbol blocker 已消失。
- Linker 仍出現 warning:

```text
cannot find entry symbol Reset_Handler; not setting start address
```

判斷：

- Experimental Smart `.a` dependency closure 目前可 link 到 `.axf`。
- 此 `.axf` 不是可執行 firmware，因為 Smart `.a` 仍是 `armv8-a`，且 link probe 使用 `--no-warn-mismatch`。
- 真正可交付版本仍需要 Pro1-compatible CarPlay/AndroidAuto libraries，或取得 source 以 Pro1/Cortex-M target 重新 build。
- 下一步應轉回 Pro1 可控移植：用 Pro1 native stack 建 USB/NCM/Wi-Fi/BT/OTA/factory smoke tests，再決定哪些 Smart product logic 透過 adapter 搬入。

### Standalone CarPlay link probe

使用者釐清目標是 rebuild/link CarPlay libraries，不需要 Pro1 examples 參與。因此新增 standalone probe：

- `tools/carbox/probe_carplay_link.sh`
- `tools/carbox/carplay_link_probe_entry.c`

Command：

```bash
docker run --rm -v "$PWD":/work carbox-pro1-build \
  bash -lc "cd /work && carbox_pro1/tools/carbox/probe_carplay_link.sh"
```

此 probe 只連結：

- minimal `main()`，用來引用 `CarApp_Start()`。
- historical note: this probe previously used minimal link-only
  `pthread_self()` / `usleep()` placeholders, but those have been removed.
  Current probe status is real unresolved-symbol reporting only.
- Smart `carplay_app/*.a` archive group；預設使用正常 archive resolution
  (`--start-group` / `--end-group`)，不使用 `--whole-archive`。
- ARM newlib standard libraries。

初始結果：

- Exit code: 1。
- Smart archives 可被 linker 讀取。
- 此 probe 未觀察到 duplicated symbol failure。
- 此 probe 未觀察到 archive format / architecture diagnostic。
- Initial link blocker 是 156 個 unique undefined symbols；加入 Pro1 SDK
  native `component/common/utilities/cJSON.c` 後，cJSON symbols 已解掉，
  目前剩 150 個 unique undefined symbols。
- 剩餘分類為 POSIX/threading、LwIP/socket、USB OS、Wi-Fi、BT、HTTPD/OTA、GPIO/flash/filesystem、crypto/utility/product-info。
- 以正常 archive resolution 重跑後，HTTPD/OTA/USB 相關 blocker 仍存在，
  表示這些不是 `--whole-archive` 強迫拉入造成的額外噪音，而是目前
  `CarApp_Start()` dependency closure 會牽出的 Smart platform/service 依賴。
- 詳細報告：`third_party/carbox_smart/reports/carplay_link_probe_report.md`。
- Raw log：`build/carbox/carplay_link_probe/carplay_link_probe.log`。
- Unique symbol list：`build/carbox/carplay_link_probe/carplay_link_probe_undefined_symbols.txt`。

### Standalone CarPlay real symbol-resolution pass

依使用者要求，對 unresolved symbols 做真實狀態收斂，只納入 Pro1 native source：

- Pro1 native source：
  - `component/common/utilities/cJSON.c`
  - `component/common/network/ssl/mbedtls-2.4.0/library/md5.c`
- Probe workflow 分成：
  - `CARBOX_PROBE_NATIVE_SRCS`
  - `CARBOX_PROBE_NATIVE_OBJS`

目前結果：

- `tools/carbox/probe_carplay_link.sh`: exit code 1。
- cJSON 與 mbedTLS MD5 由 Pro1 native source 解掉。
- 預設真實模式剩餘 147 個 unique undefined symbols。
- 沒有保留 synthetic adapter 模式；後續 adapter 必須是真實 Pro1 compatibility implementation。
- 詳細報告：`third_party/carbox_smart/reports/carplay_symbol_resolution_full.md`。

## 目前改動清單

### Build / baseline

- `carbox_pro1/docker/Dockerfile`
- `carbox_pro1/docker/build-baseline.sh`
- `carbox_pro1/MIGRATION_BASELINE.md`
- `project/realtek_amebapro_v0_example/GCC-RELEASE/application.is.mk`

### Product skeleton

- `project/realtek_amebapro_v0_example/src/main.c`
- `project/realtek_amebapro_v0_example/src/carbox/carbox_app.c`
- `project/realtek_amebapro_v0_example/src/carbox/carbox_app.h`
- `project/realtek_amebapro_v0_example/src/carbox/carbox_network_config.c`
- `project/realtek_amebapro_v0_example/src/carbox/carbox_network_config.h`

### Smart reference artifacts

- `third_party/carbox_smart/README.md`
- `third_party/carbox_smart/carplay_app/*.a`
- `third_party/carbox_smart/carplay_app/ABI_MANIFEST.tsv`
- `third_party/carbox_smart/reports/carplay_symbol_gap.md`
- `third_party/carbox_smart/reports/carplay_missing_symbols.tsv`
- `third_party/carbox_smart/reports/carplay_conflict_link_policy.md`
- `third_party/carbox_smart/reports/carplay_link_probe_report.md`
- `third_party/carbox_smart/reports/carplay_symbol_resolution_full.md`
- `third_party/carbox_smart/reports/usb_ncm_migration_note.md`

### Tools

- `tools/carbox/check_carplay_libs.sh`
- `tools/carbox/analyze_carplay_symbol_gaps.sh`
- `tools/carbox/probe_carplay_link.sh`
- `tools/carbox/carplay_link_probe_entry.c`

### Planning / customer-facing notes

- `MIGRATION_PHASE2_INVENTORY.md`
- `MIGRATION_CHANGELOG.md`

## Verification Commands

Normal baseline:

```bash
docker run --rm -v "$PWD":/work carbox-pro1-build
```

Artifact check:

```bash
test -f carbox_pro1/project/realtek_amebapro_v0_example/GCC-RELEASE/application_lp/Debug/bin/application_lp.axf
test -f carbox_pro1/project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/Debug/bin/application_is.axf
```

Smart reference ABI check:

```bash
carbox_pro1/tools/carbox/check_carplay_libs.sh
```

Smart symbol gap report:

```bash
carbox_pro1/tools/carbox/analyze_carplay_symbol_gaps.sh
```

Experimental link probe:

```bash
docker run --rm -v "$PWD":/work carbox-pro1-build \
  bash -lc "cd /work/carbox_pro1/project/realtek_amebapro_v0_example/GCC-RELEASE && CARBOX_EXPERIMENTAL_SMART_A_LINK=1 CARBOX_SKIP_IMAGE=1 make all"
```

Standalone CarPlay archive link probe:

```bash
docker run --rm -v "$PWD":/work carbox-pro1-build \
  bash -lc "cd /work && carbox_pro1/tools/carbox/probe_carplay_link.sh"
```

## Latest Verification

After standalone CarPlay archive probe files were added on 2026-05-26:

- `docker run --rm -v "$PWD":/work carbox-pro1-build`: exit code 0.
- LP/IS artifact check: pass.
- Verified artifacts:
  - `application_lp/Debug/bin/application_lp.axf`
  - `application_is/Debug/bin/application_is.axf`

After Smart product-logic and Smart SDK library reference staging:

- `docker run --rm -v "$PWD":/work carbox-pro1-build`: exit code 0.
- `tools/carbox/check_carplay_libs.sh`: pass.
- `tools/carbox/check_smart_staging.sh`: pass.
- LP/IS artifacts exist:
  - `application_lp/Debug/bin/application_lp.axf`
  - `application_is/Debug/bin/application_is.axf`
- The normal `application_is.map` does not contain `third_party/carbox_smart`,
  `product_logic`, `smart_sdk_libs`, `lib_CarPlay`, or `lib_AndroidAuto`.
- Product logic manifest row count: 673.
- Non-CarPlay Smart SDK library manifest row count: 79.

Previous experimental checks still stand:

- `CARBOX_EXPERIMENTAL_SMART_A_LINK=1 CARBOX_SKIP_IMAGE=1 make all`: exit code 0.
- `CARBOX_ENABLE_USB_SMOKE=1 CARBOX_SKIP_IMAGE=1 make all`: exit code 0.

Standalone CarPlay archive probe:

- `tools/carbox/probe_carplay_link.sh`: exit code 1.
- Unique undefined symbols in default real-status mode: 147.
- Resolved by Pro1 native source: cJSON and mbedTLS MD5.
- No synthetic adapter is linked.

## Conflict / Link Policy Note

The conflict-driven archive policy is documented in
`third_party/carbox_smart/reports/carplay_conflict_link_policy.md`.

Current production guidance:

- Do not formally link Smart `lib_jpeg.a` together with Pro1 JPEG/MMF JPEG.
- Do not formally link Smart `lib_fdkaac.a` together with Pro1 `lib_faac.a`.
- Do not formally link two mDNS/DNS-SD owners: Smart `lib_CarPlay.a` bundled mDNS and Pro1 `lib_mdns.a`.
- `lib_usbdev.a` conflicts observed so far are with Pro1 examples, not Pro1 `lib_usbd.a`; this should be handled by removing examples and designing a Pro1 USB/NCM adapter.
- `lib_zlib.a` has a `crc32` overlap with Pro1 `lib_dct.a`, but it has not been a current linker blocker.

## USB / NCM Note

The Pro1 USB/NCM finding is documented in
`third_party/carbox_smart/reports/usb_ncm_migration_note.md`.

Current status:

- Pro1 has native USB device/host infrastructure and examples for UVC, MSC, DFU, and UVC host.
- No Pro1 native CDC-NCM class source was found in the current SDK scan.
- Smart `lib_ncm.a` has no current symbol overlap with the normal Pro1 IS baseline, but it remains an `armv8-a` Smart AP-side archive.
- A guarded `carbox_usb_smoke` adapter was added. Normal build logs migration status only and does not initialize USB hardware.
- With `CARBOX_ENABLE_USB_SMOKE=1`, the adapter starts a Pro1 native DFU smoke task using `_usb_init()`, `wait_usb_ready()`, and `usbd_dfu_init()`.

## Product Logic / Library Staging

Customer goal clarified: before runtime execution is required, the Smart product
procedure logic and relevant libraries must be moved into `carbox_pro1` with
traceability. This was done as reference staging, not as Pro1 build integration.

新增 staging 位置：

- `third_party/carbox_smart/product_logic/reference`
- `third_party/carbox_smart/product_logic/PRODUCT_LOGIC_MANIFEST.tsv`
- `third_party/carbox_smart/product_logic/README.md`
- `third_party/carbox_smart/smart_sdk_libs/reference`
- `third_party/carbox_smart/smart_sdk_libs/SMART_SDK_LIBS_MANIFEST.tsv`
- `third_party/carbox_smart/smart_sdk_libs/README.md`
- `tools/carbox/check_smart_staging.sh`

Product logic staging scope:

- Smart AP/HP/LP startup entry files and `main.c` flow.
- Smart AP/HP/LP `platform_autoconf.h`, FreeRTOS config, build info, menuconfig.
- Smart AP/HP/LP make flow, generated include makefiles, OTA/factory/image utility scripts.
- Smart USB OTG make logic and USB component/example references.
- Smart Wi-Fi OTA API and lwIP/Freertos network references used by NCM-style flow.
- Smart BT API, BT audio, BT MP, HCI driver, and selected BT product examples.
- Smart audio interface/HAL references used by CarPlay/AndroidAuto dependency analysis.

Current staged file count:

- Product logic/reference files: 673.
- Non-CarPlay Smart SDK `.a` references: 79.
- CarPlay/AndroidAuto `.a` references: 16, already staged under `carplay_app`.

Non-CarPlay Smart SDK archive architecture summary:

- `armv8-a`: 46 archives, Smart AP / CA32-side binaries.
- `armv8.1-m.main`: 29 archives, Smart HP / KM4-side binaries and KM4 BT stack binaries.
- `armv8-m.base`: 4 archives, Smart LP-side binaries.

Important policy:

- These staged files are `reference-only` or `binary-reference`.
- They must not be added to normal Pro1 source paths or linker flags.
- Even Smart Cortex-M archives are still Smart SDK binaries, not Pro1-native
  libraries; they require separate ABI, linker-script, memory-model, system
  service, interrupt, and license checks before any formal integration.
- Product flow should be ported through Pro1 adapters under
  `project/realtek_amebapro_v0_example/src/carbox/`.

Smart AP flow captured:

- Initialize IPC, console, PMU, filesystem/FTL, examples, and Wi-Fi.
- If `CONFIG_USB_DEV` is enabled and MP mode is disabled, run `do_initcalls()`.
- Run `app_example()`.
- If `CONFIG_USB_DEV` is enabled and MP mode is disabled, run
  `user_use_config_ip()` and `CarApp_Start()`.
- Start scheduler.

Smart AP USB/CarPlay/NCM config captured:

- `CONFIG_USB_DEV`
- `CONFIG_USB_OTG_EN`
- `CONFIG_USB_DRD_EN`
- `CONFIG_USBD_AOA`
- `CONFIG_USBD_CARPLAY`
- `CONFIG_USBH_CDC_NCM`
- `CONFIG_ETHERNET`

Known source gap:

- Smart make logic references CarPlay USB composite and CDC-NCM class source
  paths, but those implementations were not found as ordinary source files in
  the current `carbox_smart/component` tree. The shipped implementation appears
  represented by Smart `lib_usbdev.a`, `lib_ncm.a`, and the CarPlay archive set.

CarPlay app entry symbol:

- `CarApp_Start()` is not source-level code in the staged Smart tree; it is
  provided by `third_party/carbox_smart/carplay_app/lib_SystemLib.a`.
- `arm-none-eabi-nm` locates it in `CarApp.o`:

```text
lib_SystemLib.a:CarApp.o:00000000 T CarApp_Start
```

- `CarApp.o` architecture is `armv8-a`.
- Its direct undefined dependencies are mostly satisfied by the Smart CarPlay
  archive group, including `lib_SystemLib.a`, `lib_SystemLibEx.a`, and
  `lib_init.a`; remaining OS adapter dependencies include `pthread_self` and
  `usleep`.

## Customer-facing Risk Notes

- Smart CarPlay/AndroidAuto `.a` 是 Smart AP-side `armv8-a` binary；Pro1 IS/LP 是 Cortex-M `armv8-m`。目前 link probe 即使能推進，也不能視為可執行移植完成。
- 真正可交付的 CarPlay/AndroidAuto integration 需要 Pro1-compatible libraries，或取得 source 重新以 Pro1 target build。
- 目前可控部分是 Pro1 SDK baseline、product hook skeleton、config adapter skeleton、feature inventory、reference artifact traceability。
- 後續應逐項移植：startup/config/logging、Wi-Fi、OTA/factory/MP、USB/NCM、Bluetooth，最後才處理 CarPlay/AndroidAuto stack integration。

## 2026-05-26 CarPlay Probe Archive Provider Check

- Re-scanned the current CarPlay-only unresolved list against Pro1 native SDK
  archives in Docker/Linux.
- Added the Pro1 SDK archive
  `component/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/lib/lib/hal_pmc_hs.a`
  to `tools/carbox/probe_carplay_link.sh`.
- This resolved the real Pro1 power-management symbols:
  `slp_hs_cg_cmd_handler` and `slp_hs_pd_cmd_handler`.
- Current CarPlay-only probe status: link still fails, with 90 real unresolved
  symbols.
- For the remaining unresolved symbols, no Pro1 native SDK archive provider was
  found except newlib monitor/syscall archives for `mkdir` and `usleep`; those
  are not selected because they would hide the need for real Pro1 filesystem
  and time/POSIX mappings.
- Many Pro1 providers currently used by the probe are baseline build objects
  rather than SDK archives. They should remain visible in the report until a
  real Pro1 archive or source-level build target replaces them.

## 2026-05-26 CarPlay Probe POSIX/Pthread Pass

- Removed the old `carbox_posix_link_shim.c` placeholder from the experimental
  IS build path and deleted the file.
- Added Pro1 FreeRTOS-Plus-POSIX source compilation to
  `tools/carbox/probe_carplay_link.sh`.
- Used the Pro1 Realtek POSIX portable include path and disabled duplicated
  newlib-owned typedefs only for the probe POSIX compilation.
- Resolved standard POSIX pthread/time symbols including:
  `pthread_create`, `pthread_self`, `pthread_mutex_*`, `pthread_cond_*`,
  `pthread_attr_*`, `pthread_setschedparam`, `clock_gettime`,
  `sched_get_priority_max`, and `usleep`.
- Added `tools/carbox/carplay_posix_compat.c` for `pthread_attr_setname_np`.
  This is an explicit unsupported compatibility provider returning `ENOTSUP`,
  not a weak or silent success placeholder. Runtime behavior still needs a
  product decision if CarPlay depends on thread names.
- Current CarPlay-only probe status: link still fails, with 66 real unresolved
  symbols and no remaining `pthread*` unresolved symbols.

## 2026-05-26 CarPlay Probe Approved Stub Policy

- Added `tools/carbox/experimental_stubs/` as the only approved location for
  link-only empty stubs used by the standalone CarPlay probe.
- Added `tools/carbox/experimental_stubs/README.md` with the rule that no stub
  may be added without explicit user approval.
- Added approved link-only thermal monitor stubs for:
  `TM_Display_Result`, `TM_Cmd`, `TM_INTClearPendingBits`, `TM_Init`,
  `TM_StructInit`, `TM_GetISR`, `TM_GetTempResult`, and `TM_GetCdegree`.
- Reason: these symbols are from Smart `ameba_thermal` / thermal monitor API,
  and no Pro1 native provider has been found. They remain runtime integration
  gaps and are not linked into the normal Pro1 SDK baseline build.
- 2026-05-26 clarification: current migration assumption is that CarPlay does
  not need the `TM_*` thermal monitor functions. They are treated as Smart
  archive side-effect dependencies pulled into the CarPlay link closure, not as
  required CarPlay runtime APIs. If later runtime traces show a `TM_*` call from
  the CarPlay path, this classification must be reopened before product
  validation.

## 2026-05-26 CarPlay Probe RTOS Wrapper Pass

- Investigated Smart `rtos_*` unresolved symbols:
  `rtos_mem_free`, `rtos_mem_malloc`, `rtos_mem_zmalloc`,
  `rtos_task_create`, `rtos_task_delete`, and `rtos_time_delay_ms`.
- Pro1 SDK does not export the same `rtos_*` wrapper names, but Pro1 baseline
  objects provide the equivalent FreeRTOS primitives:
  `pvPortMalloc`, `vPortFree`, `xTaskCreate`, `vTaskDelete`, and `vTaskDelay`.
- Added `tools/carbox/smart_wrapper/carplay_rtos_compat.c` as a real
  compatibility adapter for the standalone CarPlay probe. This is not an empty
  stub and is not linked into the normal Pro1 SDK baseline build.
- Verification: the standalone CarPlay probe still fails, but the unresolved
  count dropped from 58 to 52 and no `rtos_*` symbols remain unresolved.
- Moved real compatibility adapters under `tools/carbox/smart_wrapper/` to keep
  them separate from approved empty stubs under `tools/carbox/experimental_stubs/`.

## 2026-05-26 CarPlay Probe FLASH Wrapper Pass

- Investigated Smart `FLASH_*` unresolved symbols:
  `FLASH_RxData`, `FLASH_SetSpiMode`, `FLASH_Write_Lock`, and
  `FLASH_Write_Unlock`.
- These are referenced by Smart `lib_SystemLib.a` flash UUID/OTP paths
  (`nor_read_uuid`, `nor_read_otp`).
- Pro1 SDK does not export the same `FLASH_*` names, but provides flash/SPIC
  services through `flash_api`, `hal_flash`, and `hal_spic`.
- Added `tools/carbox/smart_wrapper/carplay_flash_compat.c` as a real compatibility adapter
  for the standalone CarPlay probe:
  - `FLASH_Write_Lock` -> `flash_global_lock`
  - `FLASH_Write_Unlock` -> `flash_global_unlock`
  - `FLASH_SetSpiMode` -> Pro1 `spic_init` / `spic_init_setting`
  - `FLASH_RxData(0x4B, ...)` -> `flash_read_unique_id`
  - other `FLASH_RxData` commands -> `flash_stream_read` for link-probe
    dependency closure
- Runtime OTP/security-register behavior still requires Pro1 hardware
  validation and may need a more exact command-level adapter.
- Verification: the standalone CarPlay probe still fails, but the unresolved
  count dropped from 52 to 48 and no `FLASH_*` symbols remain unresolved.

## 2026-05-26 CarPlay Probe Full Symbol Closure

- Enforced the migration rule that no additional Smart SDK `.a` may be used to
  satisfy unresolved symbols. The probe may link only:
  - the staged CarPlay reference archives under
    `third_party/carbox_smart/carplay_app/`
  - pure Pro1 SDK source/object/archive providers
  - `tools/carbox/smart_wrapper/` Pro1 compatibility wrappers
  - explicitly approved link-only stubs under
    `tools/carbox/experimental_stubs/`
- Added real Pro1 compatibility wrappers:
  - `carplay_lwip_compat.c` for Smart lwIP 2.1-style APIs mapped to Pro1
    lwIP 2.0.2.
  - `carplay_tommath_alias.c` for `mp_*` aliases to real `mp_*1`
    implementations already present inside staged `lib_CarPlay.a`.
  - `carplay_usb_os_compat.c` for Smart USB OS wrappers mapped to Pro1
    FreeRTOS/HAL primitives.
- Added Pro1 `ff_driver.o` to resolve the real Pro1 FatFS `disk` provider.
- Added approved link-only placeholders in
  `experimental_stubs/carplay_missing_pro1_stubs.c` for the remaining symbols
  not found in pure Pro1 SDK at that stage. Later passes moved Wi-Fi/network
  and HAL/system items from this file into `smart_wrapper/`.
- Added a probe-only generated linker script that expands `XIP_FLASH` length
  so the standalone dependency probe can finish symbol linking despite the
  non-production archive size. The normal Pro1 linker script is not modified.
- Verification:

```bash
docker run --rm -v "$PWD":/work carbox-pro1-build \
  bash -lc "cd /work && carbox_pro1/tools/carbox/probe_carplay_link.sh"
```

Result: `CarPlay link probe passed`; no unresolved symbols remain for the
standalone link probe. This is still not executable firmware.

## 2026-05-26 CarPlay Probe Wi-Fi/Network Wrapper Pass

- Revisited the Wi-Fi/network items that had previously been classified as
  link-only stubs: `ap_ip`, `ap_netmask`, `ap_gw`, `user_ip`, `at_apuserip`,
  `wifi_stop_ap`, and `wifi_del_station`.
- Found Pro1-native API mappings for the functional calls:
  - `wifi_stop_ap` -> Pro1 `wifi_off` from `wifi_conf.o`.
  - `wifi_del_station` -> Pro1 `wext_del_station` from `wifi_util.o`.
- Moved these symbols out of `experimental_stubs/carplay_missing_pro1_stubs.c`
  and into `smart_wrapper/carplay_wifi_network_compat.c`.
- Added Pro1 `dns.o` to the probe native object list after `wifi_off` pulled in
  `dns_setserver` through the Pro1 DHCP/Wi-Fi shutdown path.
- Added Pro1 fast-connect compatibility globals in the wrapper:
  `p_write_reconnect_ptr`, `offer_ip`, and `server_ip`. Pro1 normally gets
  these from `example_wlan_fast_connect.o`, but the CarPlay-only probe does not
  link Pro1 example applications.
- Verification:

```bash
docker run --rm -v "$PWD":/work carbox-pro1-build \
  bash -lc "cd /work && carbox_pro1/tools/carbox/probe_carplay_link.sh"
```

Result: `CarPlay link probe passed`; the Wi-Fi/network symbols above are no
longer counted as approved empty stubs. Runtime behavior still needs product
validation, especially AP interface naming and IP defaults.

## 2026-05-26 CarPlay Probe HAL/System Wrapper Pass

- Revisited the HAL/system items that had previously been grouped under
  approved link-only stubs: `flash_init_para`, `total_heap_size`,
  `vPortGetHeapStats`, `RCC_PeriphClockCmd`, `irq_register`, `irq_enable`, and
  `io_assert_failed`.
- No Pro1 object/archive exports these exact Smart symbol names directly.
- Added `tools/carbox/smart_wrapper/carplay_hal_system_compat.c`:
  - `vPortGetHeapStats` now reports through Pro1
    `xPortGetFreeHeapSize` / `xPortGetMinimumEverFreeHeapSize`.
  - `total_heap_size` is updated from Pro1 `configTOTAL_HEAP_SIZE` when heap
    stats are requested.
  - `irq_register` / `irq_enable` route through Pro1 `hal_int_vector_stubs`.
  - `io_assert_failed` is a hard-stop compatibility handler.
  - `flash_init_para` is retained as aligned compatibility storage because
    Pro1 does not expose the same Smart flash descriptor structure.
  - `RCC_PeriphClockCmd` is retained as an explicit compatibility surface; a
    real runtime mapping still needs Smart APB peripheral/clock IDs translated
    to Pro1 syson/clock controls.
- Removed these seven symbols from
  `tools/carbox/experimental_stubs/carplay_missing_pro1_stubs.c`.
- Verification:

```bash
docker run --rm -v "$PWD":/work carbox-pro1-build \
  bash -lc "cd /work && carbox_pro1/tools/carbox/probe_carplay_link.sh"
```

Result: `CarPlay link probe passed`; no unresolved-symbol regression.

## 2026-05-26 CarPlay Probe OTA Wrapper Pass

- Revisited the OTA/factory-update symbols previously grouped as approved
  link-only stubs: `download_fw_program`, `get_flash_percentage`, `ota_log`,
  `ota_update_init`, and `ota_update_deinit`.
- Confirmed these symbols are pulled by Smart staged `lib_usbdev.a`
  (`ota_httpd.o` / `sta_ota.o`) and match the Smart HTTPD OTA chunk-writing
  flow.
- Found Pro1 SDK OTA/flash equivalents in:
  - `component/soc/realtek/8195b/misc/platform/ota_8195b.c`
  - `component/soc/realtek/8195b/cmsis/rtl8195b-hp/lib/include/fw_img.h`
  - Pro1 baseline objects `flash_api.o`, `device_lock.o`, `sys_api.o`, and
    flash/HAL support objects already linked by the probe.
- Added `tools/carbox/smart_wrapper/carplay_ota_compat.c`:
  - `ota_update_init` allocates Pro1 heap state and selects the inactive Pro1
    OTA slot through `hal_flash_boot_stubs.fw_img_info_tbl_query`.
  - `download_fw_program` writes incoming OTA chunks through Pro1 flash
    erase/write APIs and preserves the delayed image-signature write pattern.
  - `ota_update_deinit` releases Pro1 heap state.
  - `get_flash_percentage` and `ota_log` provide probe-visible progress/log
    surfaces backed by the wrapper.
- Removed these five symbols from
  `tools/carbox/experimental_stubs/carplay_missing_pro1_stubs.c`.
- Important runtime caveat: this is a Pro1-SDK-backed wrapper for link and
  dependency discovery. Smart OTA package format, final signature/boot-select
  behavior, progress semantics, and reset flow still need real hardware/runtime
  validation.
- Verification:

```bash
docker run --rm -v "$PWD":/work carbox-pro1-build \
  bash -lc "cd /work && carbox_pro1/tools/carbox/probe_carplay_link.sh"
```

Result: `CarPlay link probe passed`; no unresolved-symbol regression. Symbol
inspection confirms the OTA names are now defined by
`build/carbox/carplay_link_probe/carplay_ota_compat.o`, not by
`carplay_missing_pro1_stubs.o`.

## 2026-05-26 CarPlay Probe Product Info Wrapper Pass

- Revisited the product info/log symbols previously grouped as approved
  link-only stubs: `get_build_version`, `get_log_len`, and `get_system_log`.
- Found Pro1 build metadata generated by `application.is.mk` in
  `project/realtek_amebapro_v0_example/inc/build_info.h`.
- Added `tools/carbox/smart_wrapper/carplay_product_info_compat.c`:
  - `get_build_version` returns a Pro1-style version string derived from
    `RTL_FW_COMPILE_DATE`, matching the version pattern used by Pro1 AT
    `ATS?`.
  - `get_log_len` and `get_system_log` expose a probe-local product log string
    seeded from Pro1 compile time/compiler metadata.
- Removed these three symbols from
  `tools/carbox/experimental_stubs/carplay_missing_pro1_stubs.c`.
- Runtime caveat: this is no longer an empty stub, but it is not a full runtime
  system log ring export. The final product log service still needs to decide
  where real persistent/runtime logs are collected.
- Verification:

```bash
docker run --rm -v "$PWD":/work carbox-pro1-build \
  bash -lc "cd /work && carbox_pro1/tools/carbox/probe_carplay_link.sh"
```

Result: `CarPlay link probe passed`; no unresolved-symbol regression. Symbol
inspection confirms the product info/log names are now defined by
`build/carbox/carplay_link_probe/carplay_product_info_compat.o`, not by
`carplay_missing_pro1_stubs.o`.
