# ANCHOR + YACHT Bluetooth Trainer bridge

This repository contains the ANCHOR receiver/bridge firmware, documented trainer protocol, browser installer, and update contracts. The ESP32-C5 implementation remains the reference firmware; an experimental native Nordic nRF52840 / Zephyr implementation is now preserved under `platforms/nordic-nrf52840/`. Yacht firmware and TinyGo BLE changes are maintained separately in [yacht](https://github.com/Doncuppjr/yacht).

## Supported boards and builds

Anchor's shared application is in `firmware/anchor/src/anchor.cpp`; GPIO, wake and
flash settings live under `boards/`. Build with:

```sh
make BOARD=c5 PROFILE=production
make BOARD=c5 PROFILE=debug
make BOARD=c3-supermini PROFILE=production
make BOARD=c3-supermini PROFILE=debug
make c5 PROD=TRUE
make c3-supermini PROD=FALSE
make test
```

The [USB installer](https://doncuppjr.github.io/anchor/) provides a board
selector for XIAO ESP32-C5 and **experimental** ESP32-C3 Super Mini firmware.
The C3 target has passed compilation and image validation, but **has not been
hardware-validated**. Check [board wiring](boards/README.md) before testing.

Additional compact ESP32 profiles: XIAO C3/C6, Waveshare C3-Zero and C6 Super Mini; see [boards](boards/README.md). Nordic nRF52840 and Uno/Nano options are classified in [board candidates](boards/candidates/). These are not yet hardware-verified.

## Repositories and branches

- EdgeTX general-purpose `AT+OTA` extension: [Doncuppjr/edgetx, bluetooth/generic-module-ota](https://github.com/Doncuppjr/edgetx/tree/bluetooth/generic-module-ota)
- Full TX15 BLE reliability / low-latency / OTA / Back-to-cancel integration: [Doncuppjr/edgetx, bluetooth/tx15-ota-integration](https://github.com/Doncuppjr/edgetx/tree/bluetooth/tx15-ota-integration)
- Repository: `Doncuppjr/anchor:main`.

## Hardware / wire contract

See the [TX15 hardware build example](docs/HARDWARE-BUILD.md) for photos of
the XIAO ESP32-C5, soldered harness, connector orientation, and placement
inside the radio.

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
- Upload the CI-generated **application-only** `ANCHOR-C5-production-app.bin` for OTA, **not** the merged image. A full image is for wired USB installation/recovery and may overwrite NVS settings.
- No image signing/secure boot and no proven automatic rollback on failed boot. The default password should be changed before production use.
- Firmware transfer through the webpage, malformed upload recovery, active-upload cancellation, Telemetry return, and inactivity shutdown require more testing.
- On Back from the update dialog, EdgeTX drives PH11 HIGH for 1.8 seconds. The C5 recognizes HIGH for at least 100 ms; it reboots to return to normal Bluetooth unless a write is active. Generic EdgeTX UI remains hardware-neutral.

## Files

- `firmware/anchor/src/anchor.cpp`: shared application code, built with `Makefile` and `boards/` profiles.
- EdgeTX radio firmware is maintained separately; it is not built or shipped by Anchor.
- Yacht/nRF52840 firmware is maintained in [yacht](https://github.com/Doncuppjr/yacht) and is not shipped by Anchor.
- `docs/PROTOCOL.md` and `docs/BENCH-TEST.md`: transport contract and remaining checks.
- `platforms/nordic-nrf52840/`: native NCS 3.3.0 / Zephyr application, hardware UART, AT command path, host parser test, build record and the custom Adafruit UART-DFU patch. This target is experimental; see its README for implemented behavior and known gaps.
- [Nordic nRF52840 / Adafruit bootloader investigation](docs/NORDIC-NRF52840-BOOTLOADER.md): observed USB identity, UF2 recovery, UART DFU configuration and known limitations.
- [USB web flashing guide](docs/USB-FLASHING.md): step-by-step browser installation with screenshots, erase choices, and recovery instructions.
- [Installed-module USB update guide](docs/RADIO-USB-UPDATE.md): TX15 bridge workflow, recovery behavior and current bench validation.

Firmware binaries and SHA-256 manifests are generated as GitHub Actions artifacts rather than stored in source control. Refer to the build run for the matching checksums.

## Provenance and attribution

ANCHOR is an independently developed ESP32-C5 implementation of the existing
FrSky-compatible UART/AT and BLE trainer protocols, **not a source fork** of
[Cliff's BTWifiModule](https://github.com/dlktdr/BTWifiModule). Cliff's project
provided a valuable reference for the protocol and its GATT identifiers
(`0xFFF0`, `0xFFF6`). We acknowledge that work without claiming its code was
incorporated. A formal code-provenance and license review should precede
wider distribution; Cliff's repository uses GPL-3.0.

## GitHub Actions build workflow

[Anchor build workflow](.github/workflows/firmware.yml) builds two
application-only ESP32-C5 images on every push to `main`
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

`ANCHOR_DEBUG` defaults to `0` for production. Run `make BOARD=c5 PROFILE=production` or `make BOARD=c5 PROFILE=debug`; see [BUILD-VARIANTS.md](BUILD-VARIANTS.md). The production image excludes USB bench controls and verbose diagnostic logging. Both profiles retain the AT protocol, BLE transport, PH11 and OTA behavior. C5 Production has been exercised on the TX15 for Trainer and USB/Wi-Fi maintenance; C3 and Diagnostic hardware validation remain pending. See the [installed-module USB guide](docs/RADIO-USB-UPDATE.md) for the current bench results and limitations.

## Browser installer (first-time USB setup)

Follow the [illustrated USB web flashing guide](docs/USB-FLASHING.md) for
each dialog and choice, including port selection, the erase option,
installation confirmation, and completion.

A static, no-command-line installer is provided in [`installer/`](installer/).
It uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/) and Chrome or
Edge desktop Web Serial to program a connected **XIAO ESP32-C5**. Users select
**Production** or **Debug**, choose their USB serial port, and confirm flashing.
The installer uses the same pinned-source **full** image as our CI firmware
artifacts; there is no separate, untracked firmware image or UF2 bootloader.

The [Anchor multi-board GitHub Actions workflow](.github/workflows/firmware.yml)
builds both variants, checks the images, generates ESP Web Tools manifests,
provides a downloadable `ANCHOR-browser-installer-*` site artifact, and attempts
to publish the site to GitHub Pages on integration-branch pushes or manual runs.
The proposed site URL is:

**https://doncuppjr.github.io/anchor/**

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
