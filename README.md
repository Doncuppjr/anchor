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
- YACHT: XIAO nRF52840 Sense, TinyGo BLE trainer notifications, notification credit window 5 (validated production setting).
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
- `firmware/YACHT-nRF52840-credit-window4-tested.uf2`: historical four-credit firmware snapshot for rollback and comparison, **not** the final five-credit production candidate. The active source default is five; rebuild and verify before deployment.
- `docs/PROTOCOL.md` and `docs/BENCH-TEST.md`: transport contract and remaining checks.
- `../patches/tinygo-bluetooth-notification-complete.patch`: required TinyGo BLE TX completion API patch.
- `../patches/yacht-latest-state-ble.patch`: source changes relative to the YACHT low-latency branch's prior commit.

See `firmware/SHA256SUMS` to verify downloaded firmware bytes.

## Provenance and attribution

ANCHOR is an independently developed ESP32-C5 implementation of the existing
FrSky-compatible UART/AT and BLE trainer protocols, **not a source fork** of
[Cliff's BTWifiModule](https://github.com/dlktdr/BTWifiModule). Cliff's project
provided a valuable reference for the protocol and its GATT identifiers
(`0xFFF0`, `0xFFF6`). We acknowledge that work without claiming its code was
incorporated. A formal code-provenance and license review should precede
wider distribution; Cliff's repository uses GPL-3.0.

## GitHub Actions build workflow

[ANCHOR C5 firmware](../.github/workflows/anchor-firmware.yml) builds two
application-only ESP32-C5 images on every relevant push to an `anchor-yacht/*`
branch, on relevant pull requests, and via manual `workflow_dispatch`.
It pins Arduino CLI **1.6.0**, Espressif Arduino core **4.0.0**, and board
`esp32:esp32:XIAO_ESP32C5` to match our tested development toolchain.
Each run produces separate `production` and `debug` artifact downloads, with a
SHA-256 file and source/toolchain metadata. The production job checks that
bench-only diagnostic strings are absent; both jobs verify that the OTA
protocol remains present. Artifacts are retained for 30 days.

**These are firmware build checks, not hardware certification.** The runner
cannot verify Trainer/Telemetry functionality, deep-sleep wake, BLE latency,
or an OTA update on real hardware. Never flash an untested CI image during a
flight; test a candidate on the bench first.

## Production and diagnostic builds

`ANCHOR_DEBUG` defaults to `0` for production. Run `./ANCHOR/build-firmware.sh production` or `./ANCHOR/build-firmware.sh debug`; see [BUILD-VARIANTS.md](BUILD-VARIANTS.md). The production image excludes USB bench controls and verbose diagnostic logging. Both profiles retain the AT protocol, BLE transport, PH11 and OTA behavior. These variants have been compiled but not yet tested on hardware.
