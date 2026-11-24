# ANCHOR + YACHT Bluetooth Trainer bridge

This directory contains the current ESP32-C5 ANCHOR bridge firmware, the matching EdgeTX test firmware, the last tested YACHT nRF52840 firmware, and the generic optional AT OTA contract. The YACHT source changes for the project are in this branch's `src/` tree. Upstream TinyGo Bluetooth API changes are preserved as a patch in `../patches/`.

## Repositories and branches

- EdgeTX general-purpose `AT+OTA` extension: [Doncuppjr/edgetx, bluetooth/generic-module-ota](https://github.com/Doncuppjr/edgetx/tree/bluetooth/generic-module-ota)
- Full TX15 BLE reliability / low-latency / OTA / Back-to-cancel integration: [Doncuppjr/edgetx, bluetooth/tx15-ota-integration](https://github.com/Doncuppjr/edgetx/tree/bluetooth/tx15-ota-integration)
- This branch: `Doncuppjr/HeadTracker:anchor-yacht/integration-20261123`.

## Hardware / wire contract

- RadioMaster TX15 PB9 -> C5 GPIO12 UART RX.
- RadioMaster TX15 PB8 <- C5 GPIO11 UART TX.
- RadioMaster TX15 PH11 -> C5 GPIO1 active-low module enable; HIGH cancels a waiting OTA maintenance session.
- ANCHOR: XIAO ESP32-C5, Arduino ESP32 core 4.0.0; build FQBN `esp32:esp32:XIAO_ESP32C5`.
- YACHT: XIAO nRF52840 Sense, TinyGo BLE trainer notifications, notification credit window 4.
- Serial AT handshake: `AT+BAUD4`, `AT+NAME...`, `AT+TXPW...`, then `AT+OTA` for maintenance or `AT+ROLE0` / `AT+ROLE1` for normal roles.

## OTA behavior tested so far

The TX15 hardware displayed the optional Update module button, and the command successfully enabled the `ANCHOR Chain` Wi-Fi AP. Back from that specific dialog was then tested with matching firmware and successfully closed the AP and resumed Bluetooth after several seconds. This does not constitute a complete OTA-upload regression.

- SSID `ANCHOR Chain`, WPA2 password `anchor-update` (fixed development password).
- Web interface `http://192.168.4.1/`, HTTP Basic Auth `admin` / `anchor-update`.
- Upload **application-only** `ANCHOR-C5-OTA-cancel-app.bin`, **not** the merged image. The merged image is for wired full recovery and may overwrite NVS settings if written to flash in full.
- No image signing/secure boot and no proven automatic rollback on failed boot. The default password should be changed before production use.
- Firmware transfer through the webpage, malformed upload recovery, active-upload cancellation, Telemetry return, and inactivity shutdown require more testing.
- On Back from the update dialog, EdgeTX drives PH11 HIGH for 1.8 seconds. The C5 recognizes HIGH for at least 100 ms; it reboots to return to normal Bluetooth unless a write is active. Generic EdgeTX UI remains hardware-neutral.

## Files

- `firmware/anchor_c5.ino`: latest ANCHOR source.
- `firmware/ANCHOR-C5-OTA-cancel-app.bin`: current compiled application, used in the last USB flash.
- `firmware/ANCHOR-C5-USB-recovery-merged.bin`: full recovery flash image. Never upload it via Wi-Fi OTA.
- `firmware/EdgeTX-TX15-OTA-cancel.uf2`: matching EdgeTX test build for TX15 UF2 mode.
- `firmware/YACHT-nRF52840-credit-window4-tested.uf2`: tested YACHT firmware snapshot, which can be older than subsequent uncommitted source edits now saved on this branch.
- `docs/PROTOCOL.md` and `docs/BENCH-TEST.md`: transport contract and remaining checks.
- `../patches/tinygo-bluetooth-notification-complete.patch`: required TinyGo BLE TX completion API patch.
- `../patches/yacht-latest-state-ble.patch`: source changes relative to the YACHT low-latency branch's prior commit.

See `firmware/SHA256SUMS` to verify downloaded firmware bytes.
