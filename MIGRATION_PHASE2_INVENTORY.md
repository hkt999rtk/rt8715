# carbox_pro1 Phase 2 Smart Feature Inventory

See `MIGRATION_CHANGELOG.md` for the cumulative migration change history,
including the current experimental Smart `.a` link-probe findings.

## Purpose

Phase 2 inventories the Smart product behavior that must be considered for the
Pro1 migration. This phase does not port Smart code into the Pro1 build and does
not add Smart libraries to the normal Pro1 baseline link flags.

The Pro1 acceptance baseline remains:

```bash
docker run --rm -v "$PWD":/work carbox-pro1-build
carbox_pro1/tools/carbox/check_carplay_libs.sh
```

## Source Snapshot

- Smart source tree: `carbox_smart`
- Smart project root: `carbox_smart/amebasmart_gcc_project`
- Current Smart head observed during inventory: `15682fc` (`蓝牙一键切换手机`)
- Pro1 baseline: `carbox_pro1/project/realtek_amebapro_v0_example/GCC-RELEASE`
- Smart CarPlay reference artifacts: `carbox_pro1/third_party/carbox_smart/carplay_app`

## Smart Product Entry Points

| Area | Smart source | Observed behavior | Pro1 migration status |
| --- | --- | --- | --- |
| AP main | `carbox_smart/amebasmart_gcc_project/project_ap/src/main.c` | Initializes OTP, IPC AP IRQ, shell, PMU, VFS/LittleFS/KV, FTL, Wi-Fi, USB initcalls, factory-mode gate, then `app_example()` and CarPlay app startup. | Adapter required. Do not copy over Pro1 `main.c`. Extract order and product hooks only. |
| HP main | `carbox_smart/amebasmart_gcc_project/project_hp/src/main.c` | Initializes KM4 side IPC, shell, filesystem, FTL, Wi-Fi, PMU, boot functional check, debug masks, then `app_example()`. | Mostly SDK-native concepts, but Smart HP topology differs from Pro1 IS. Re-map to Pro1 startup explicitly. |
| LP main | `carbox_smart/amebasmart_gcc_project/project_lp/src/main.c` | Initializes LP shell, CHIPEN/PMC, IPC, power management, optional `MSFT_FW_init()`, Wi-Fi firmware task, IWDG refresh timer. | Hardware-confirmation required. Pro1 LP already has its own low-power startup. |
| Pro1 IS main | `carbox_pro1/project/realtek_amebapro_v0_example/src/main.c` | Baseline Pro1 console, examples, Wi-Fi, scheduler. | Keep as baseline; introduce CarBox hooks in small slices later. |
| Pro1 LP main | `carbox_pro1/project/realtek_amebapro_v0_example/src/main_lp.c` | Baseline Pro1 LP shell loop. | Keep as baseline until a specific LP requirement is proven. |

## Current Smart Configuration Highlights

Current generated Smart config files show these product-relevant selections:

| Feature | Smart config evidence | Classification |
| --- | --- | --- |
| Smart AP + HP Wi-Fi split | AP has `CONFIG_AS_INIC_AP`; HP has `CONFIG_AS_INIC_NP`. | Adapter required. Pro1 does not have Smart CA32 AP topology. |
| AP USB product mode | AP has `CONFIG_USB_DEV`, `CONFIG_USBD_CARPLAY`, `CONFIG_USBH_CDC_NCM`; `CONFIG_USB_DEVICE_EN` is undefined. | Adapter required and hardware-confirmation required. |
| Bluetooth profile set | AP/HP/LP config enables BT dual mode, A2DP+HFP, SPP, RFC, BT audio codec SBC/CVSD. | Adapter required; depends on Pro1 BT stack availability and profile parity. |
| FTL | AP enables `CONFIG_FTL_ENABLED`. | Likely Pro1 SDK-native equivalent, but layout must be checked. |
| VFS/LittleFS/FATFS/KV | AP/HP/LP enable LittleFS and FATFS menu items; AP main mounts LittleFS/KV when AP role is active. | Adapter required. Pro1 storage layout differs. |
| MP/factory | Current generated config has `CONFIG_MP_INCLUDED` undefined, but factory flow is documented and guarded in source. | Separate factory image milestone. |
| Compressed OTA image | Current generated config has `CONFIG_COMPRESS_OTA_IMG` undefined, but OTA scripts and docs exist. | Adapter required; Pro1 image format must lead. |
| Clintwood/MSFT | `CONFIG_CLINTWOOD` currently undefined; LP/HP mains include conditional hooks. | Smart-only unless product requirement is confirmed. |

## CarPlay / Android Auto Inventory

Smart CarPlay integration is AP-side and binary-heavy:

- `project_ap/src/main.c` calls `user_use_config_ip()` and `CarApp_Start()` when `CONFIG_USB_DEV` is enabled.
- `project_ap/asdk/Makefile` links `project_ap/asdk/lib/carplay_app/*.a` when `CONFIG_USB_DEV` is enabled and MP is not enabled.
- The staged Smart CarPlay libraries are already copied as reference-only artifacts under `third_party/carbox_smart/carplay_app`.
- ABI check confirms these libraries are `armv8-a`, not usable as executable Pro1 Cortex-M IS/LP firmware.
- A guarded `CARBOX_EXPERIMENTAL_SMART_A_LINK=1` mode exists only for link dependency discovery.

Migration classification:

| Item | Classification | Next action |
| --- | --- | --- |
| `CarApp_Start()` product startup point | Adapter required | Identify public header/API owner or closed library symbol map before porting. |
| `lib_CarPlay.a`, `lib_AndroidAuto.a`, accessory/system/UI/audio/image libraries | Smart-only binary for now | Need Pro1/Cortex-M compatible libraries or source rebuild. |
| `lib_usbdev.a`, `lib_ncm.a` | Smart-only binary for now | Use Pro1 USB/NCM stack or request Pro1-compatible binaries. |
| CarPlay USB descriptors | Adapter required | Compare Smart `component/usb` CarPlay composite sources against Pro1 USB class support. |
| Rootfs CarPlay images | Smart AP/rootfs artifact | Only relevant if Pro1 product UI needs equivalent assets. |

## USB / NCM Inventory

Smart USB evidence:

- AP menu exposes `CONFIG_USB_DEV_FOR_AP`, USB device/host/DRD choices, CarPlay device option, CDC ECM, and CDC NCM.
- `project_ap/asdk/make/usb_otg/drd/class/Makefile` includes CarPlay composite sources when `CONFIG_USBD_CARPLAY=y`.
- The same Makefile includes CDC ECM/NCM host class sources when `CONFIG_USBH_CDC_ECM` or `CONFIG_USBH_CDC_NCM` is enabled.
- Smart component source contains `component/usb/host/cdc_ecm/*` and device composite/vendor classes.

Migration classification:

| Item | Classification | Notes |
| --- | --- | --- |
| USB controller enable/init order | Hardware-confirmation required | Pro1 USB hardware, pins, and role support must be verified. |
| USB device composite CarPlay descriptors | Adapter required | Port descriptors only after Pro1 USB class baseline is known. |
| CDC NCM host behavior | Adapter required | Smart enables NCM; Pro1 equivalent must be proven with a small smoke test. |
| USB MP test | Adapter required | Keep separate from normal runtime USB flow. |

## Wi-Fi Inventory

Smart Wi-Fi evidence:

- AP generated config enables `CONFIG_WLAN` and `CONFIG_AS_INIC_AP`.
- HP generated config enables `CONFIG_WLAN` and `CONFIG_AS_INIC_NP`, then later undefines NP and defines `CONFIG_SINGLE_CORE_WIFI` in the generated compatibility section.
- LP generated config enables `CONFIG_WIFI_FW_EN`; LP main creates `wififw_task_create()` unless Clintwood is enabled.
- Commit history includes product fixes around wireless restart, Wi-Fi start timing, Wi-Fi password, IP/gateway/DNS configuration, and connection type reporting.

Migration classification:

| Item | Classification | Notes |
| --- | --- | --- |
| Basic Wi-Fi bring-up | Pro1 SDK native replacement | Start from Pro1 Wi-Fi APIs and keep baseline link passing. |
| Stored SSID/password/product config | Adapter required | Inventory Smart persistence format before adding Pro1 config storage. |
| Static IP/gateway/DNS | Adapter required | Smart has `user_use_config_ip()` in `component/lwip/api/lwip_netconf.c`; port as a Pro1 network config adapter later. |
| Wireless CarPlay behavior | Adapter required | Depends on CarPlay stack and Wi-Fi control APIs. |

## Bluetooth Inventory

Smart Bluetooth evidence:

- Generated config enables dual mode, A2DP+HFP, SPP, RFC, BT app audio data, SBC, and CVSD.
- Makefiles include BT profile modules under `project_ap/asdk/make/bluetooth` and `project_hp/asdk/make/bluetooth`.
- Commit history contains multiple Bluetooth changes, including one-key phone switching.

Migration classification:

| Item | Classification | Notes |
| --- | --- | --- |
| BT power/profile init | Adapter required | Must map to Pro1 BT stack and profile availability. |
| A2DP/HFP audio routing | Adapter required | Audio path likely differs from Pro1 baseline. |
| SPP/RFC control channel | Adapter required | Need product protocol inventory. |
| One-key phone switching | Product logic, adapter required | Use Smart commit history and symbols to isolate behavior. |
| BT MP commands | Adapter required | Keep factory/MP isolated from runtime profile migration. |

## OTA / Factory / MP Inventory

Smart OTA and factory evidence:

- `carbox_smart/redeme.txt` documents `factory_all.bin`, `ota_all.bin`, `ota_app.bin`, and `ota_fatfs.bin`.
- `amebasmart_gcc_project/Makefile` builds/copies OTA artifacts and concatenates boot, app, FATFS, and MP images into `factory_all.bin`.
- Factory flow uses `AT+MP=0` to enter factory mode, `AT+MP=1/2` for USB load/unload tests, `AT+MP=3` for I2C test, and `AT+OTARECOVER` to switch to OTA1 user software.
- Smart AP main checks `BKUP_REG2 == 0x5A5A5A5A` for factory mode before running `app_example()`.

Migration classification:

| Item | Classification | Notes |
| --- | --- | --- |
| OTA app update | Adapter required | Pro1 image format and partition layout must lead. |
| OTA FATFS/UI update | Hardware/product-confirmation required | Only port if Pro1 product has equivalent UI/FATFS payload. |
| `factory_all.bin` composition | Adapter required | Rebuild using Pro1 flash layout and image tools. |
| `AT+MP` factory commands | Adapter required | Port after Pro1 AT command surface and hardware smoke tests exist. |
| Factory-mode backup register trigger | Hardware-confirmation required | Pro1 backup register semantics must be checked. |

## Initial Migration Slices

Do these in order, with `CARBOX_SKIP_IMAGE=1` link baseline after each code slice:

1. Product startup skeleton
   - Done: add CarBox-specific init hook points to Pro1 IS only.
   - No USB, BT, CarPlay, or OTA logic yet.

2. Config/log/console baseline
   - In progress: define app version reporting and persistent config abstraction.
   - Keep storage implementation minimal until flash layout is confirmed.

3. Network config adapter
   - Started: port the behavior behind Smart `user_use_config_ip()` using Pro1 network APIs.
   - Pro1 now owns a `carbox_network_config` adapter that stages AP IPv4 defaults without starting Wi-Fi or CarPlay.
   - Direct application to `ap_ip`, `ap_netmask`, and `ap_gw` is deferred because those globals only exist when Pro1 `ATCMD_VER == ATVER_2`.
   - Verify by build first, then serial log.

4. Factory command skeleton
   - Add Pro1 AT command placeholders for MP state transitions.
   - Do not enable destructive flash/image operations until Pro1 layout is signed off.

5. USB smoke test
   - Bring up Pro1 USB role/class with a minimal native Pro1 example.
   - Only after this, compare Smart CarPlay composite/NCM behavior.

6. CarPlay/Android Auto integration gate
   - Do not link Smart `armv8-a` libraries.
   - Continue only with Pro1-compatible libraries or source rebuild.

7. Bluetooth profile migration
   - Start from Pro1 BT stack support matrix.
   - Port product pairing/switching logic separately from profile/audio transport.

## Open Questions

- Are Pro1-compatible CarPlay/AndroidAuto/Accessory libraries available, or can the source be rebuilt for Cortex-M?
- Which USB role is required on Pro1 for the final product: device, host, or DRD?
- Does Pro1 hardware expose the same USB/NCM and BT audio routing needed by the Smart product?
- Which Smart factory tests are mandatory for Pro1 production: USB, I2C, Wi-Fi, BT, or all of them?
- Should Pro1 preserve Smart OTA artifact semantics (`ota_all`, `ota_app`, `ota_fatfs`) or switch to Pro1-native image names?

## Verification For This Phase

Phase 2 is accepted when:

- This inventory document exists and captures source paths, classification, and next actions.
- Smart CarPlay reference libraries remain isolated under `third_party/carbox_smart`.
- `carbox_pro1/tools/carbox/check_carplay_libs.sh` passes.
- Docker link-only Pro1 baseline still passes.
