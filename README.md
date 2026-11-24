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
Each run produces separate `production` and `debug` artifact downloads,
each with a full USB-flash and an application-only OTA image, with a SHA-256 file and source/toolchain metadata. The production job checks that
bench-only diagnostic strings are absent; both jobs verify that the OTA
protocol remains present. Artifacts are retained for 30 days.

**These are firmware build checks, not hardware certification.** The runner
cannot verify Trainer/Telemetry functionality, deep-sleep wake, BLE latency,
or an OTA update on real hardware. Never flash an untested CI image during a
flight; test a candidate on the bench first.

## Initial USB installation (XIAO ESP32-C5)

The ESP32-C5 uses the Espressif ROM serial-download interface. The XIAO
ESP32-C5 does not normally present a UF2 mass-storage drive; generating a
UF2 file alone will not enable drag-and-drop installation.

1. Download and extract the ANCHOR C5 production artifact from the GitHub
   Actions workflow. Use the **full** image for first installation.
2. Connect a data-capable USB-C cable to the C5. If necessary, hold BOOT,
   tap RESET, then release BOOT to enter ROM download mode.
3. Install esptool (`python -m pip install esptool`), identify the USB serial
   port, and flash the full merged image at address 0x0:

   ```sh
   esptool --chip esp32c5 --port /dev/ttyACM0 write-flash 0x0 ANCHOR-C5-production-full.bin
   ```

   Replace `/dev/ttyACM0` with the correct port, for example `COM4` on Windows.
4. Reset the board and verify the UART/PH11 interface with the TX15.

**Never upload a `*-full.bin` to the Wi-Fi OTA page.** The 8 MB merged image
contains bootloader (at 0x2000), partition table (at 0x8000) and application (at 0x10000) for factory USB flashing
and may erase previously stored NVS pairing/settings. Upload only the
`ANCHOR-C5-production-app.bin` application image for subsequent OTA updates.

Firmware is build-verified, but USB recovery, the complete OTA upload path,
and secure update authenticity still require production regression testing.
See the [Espressif ESP32-C5 esptool guide](https://docs.espressif.com/projects/esptool/en/latest/esp32c5/esptool/basic-commands.html).

## Production and diagnostic builds

`ANCHOR_DEBUG` defaults to `0` for production. Run `./ANCHOR/build-firmware.sh production` or `./ANCHOR/build-firmware.sh debug`; see [BUILD-VARIANTS.md](BUILD-VARIANTS.md). The production image excludes USB bench controls and verbose diagnostic logging. Both profiles retain the AT protocol, BLE transport, PH11 and OTA behavior. These variants have been compiled but not yet tested on hardware.

## Browser installer (first-time USB setup)

A static, no-command-line installer is provided in [`installer/`](installer/).
It uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/) and Chrome or
Edge desktop Web Serial to program a connected **XIAO ESP32-C5**. Users select
**Production** or **Debug**, choose their USB serial port, and confirm flashing.
The installer uses the same pinned-source **full** image as our CI firmware
artifacts; there is no separate, untracked firmware image or UF2 bootloader.

The [`ANCHOR C5 firmware` GitHub Actions workflow](../.github/workflows/anchor-firmware.yml)
builds both variants, checks the images, generates ESP Web Tools manifests,
provides a downloadable `ANCHOR-browser-installer-*` site artifact, and attempts
to publish the site to GitHub Pages on integration-branch pushes or manual runs.
The proposed site URL is:

**https://doncuppjr.github.io/HeadTracker/**

**Site availability requires the repository's GitHub Pages source to be set to
GitHub Actions** in Settings > Pages. If that has not been configured, the
website may be unavailable even while firmware and installer artifacts build
successfully. Pull requests build but do not deploy the site.

**Caution:** The full image can overwrite pairing settings and NVS. The current
firmware uses a development OTA password and has not undergone firmware signing
or complete OTA regression testing. A successful CI build does not authorize
flight use; verify on hardware first. The OTA application-only `.bin` remains
available for subsequent updates.

## Experimental ANCHOR-to-ANCHOR Trainer Slave relay

In `AT+ROLE0`, ANCHOR now distinguishes EdgeTX Bluetooth Trainer **Slave**
from Bluetooth Telemetry by validating the first full FrSky trainer frame.
Trainer packets are forwarded on BLE GATT `FFF6` through a latest-frame
mailbox; telemetry retains the previous raw stream behavior. This does not
change the validated `AT+ROLE1` trainer receiver. See
[TRAINER-RELAY.md](docs/TRAINER-RELAY.md) for protocol details and the
**required hardware regression checklist**. The feature is compile-verified,
not yet tested between two transmitters.
