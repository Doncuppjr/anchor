# Production vs diagnostic ANCHOR builds

`ANCHOR_DEBUG` is a **compile-time** flag; default is 0.

- `ANCHOR_DEBUG=0`: normal BLE roles, UART AT protocol, four-slot FIFO, PH11 sleep/recovery, OTA and cancellation remain enabled. USB bench commands are not compiled in. Verbose `Serial.printf/println` traces, including heartbeats, are compiled out. Internal counters remain available for a future diagnostic interface.
- `ANCHOR_DEBUG=1`: same operational behavior plus USB maintenance/bench commands and full debug messages (`DROP`, `RESYNC`, `SYNCROLE`, `BLEFAST`, `BLE7`, `BLE15`, `BLE2M`, `BLEINFO`, `EN`, `PROFILE`, `OTA`, `TELEMSTART`, `TELEMCOMPAT`, `TELEMRAW`, `TELEMTEST`, `TELEMSTATUS`, `STATUS`, `SCAN`, `CONNECT`, `FORGET`, `RESETUART`, `UART115200`, `REBOOT`).

Run using Arduino CLI and the installed ESP32 4.0.0 core:

```sh
ARDUINO_CLI=/home/davinci/tx15-bridge/bin/arduino-cli ./ANCHOR/build-firmware.sh production
ARDUINO_CLI=/home/davinci/tx15-bridge/bin/arduino-cli ./ANCHOR/build-firmware.sh debug
```

Both builds place an OTA-compatible **application-only** image at `ANCHOR/dist/<variant>/ANCHOR-C5-<variant>-app.bin`. Never upload a merged full-flash image through the OTA webpage. `dist/` is ignored by Git; release binaries are copied separately into `ANCHOR/firmware/` after build verification.

**Status:** This debug-flag change is compile-verified for both configurations, not yet flashed or hardware regression-tested. The current hardware-tested image remains `ANCHOR/firmware/ANCHOR-C5-OTA-cancel-app.bin`.

**Scope:** Compile-time bench/debug separation only; OTA authentication and signature/rollback hardening are separate production-release tasks.

## Continuous integration

GitHub Actions runs `.github/workflows/anchor-firmware.yml` on relevant branch
pushes, pull requests and by manual dispatch. It uses the same build script as
local development, with separately packaged production and debug application
binaries, SHA-256 checksums and build metadata. Release assets should be
promoted only after hardware regression testing; green CI means compilation and
static profile checks passed, not OTA or BLE validation.

Each build also generates `ANCHOR/dist/<variant>/ANCHOR-C5-<variant>-full.bin`,
an 8 MB merged USB-install/recovery image. Flash the full image at address
`0x0` using esptool; it may erase stored settings. The GitHub Actions artifacts
include both images, checksums, and build metadata.
