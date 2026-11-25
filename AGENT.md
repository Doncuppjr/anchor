# Anchor repository agent guide

## Mission and architecture
Anchor is an independently developed Bluetooth trainer/telemetry module for
EdgeTX transmitters. Yacht is a **separate** nRF52840 head tracker; EdgeTX is a
**separate** radio firmware repository. This repo contains the Anchor bridge.
The application in `firmware/anchor/src/anchor.cpp` currently uses the ESP32
Arduino/ESP-IDF backend. The FrSky trainer parser `trainer_protocol.h` and its
host test are reusable. Board-specific pin assignments, wake source, Arduino
FQBN, flash size and installer chip family live in `boards/<board>.mk` and
`boards/<board>/anchor_board.h`. Avoid putting board pins or chip-specific
assumptions into shared protocol code.

## Build and test
- Canonical: `make BOARD=c5 PROFILE=production`; `PROFILE=debug` enables USB
  bench commands; `make c5 PROD=TRUE` is only a shortcut.
- Use Arduino CLI 1.6.0 and Espressif Arduino core 4.0.0 (as pinned in CI).
- `make test` checks FrSky parser framing; `python3 -m unittest discover -s
  tests -p test_installer.py` checks the installer using local build products.
- `dist/<board>/<profile>/*-app.bin` is **application-only OTA**.
  `*-full.bin` is **USB full-flash**. Never send a merged image to Wi-Fi OTA.
- Production must omit diagnostic injection/TELEMTEST strings; both builds
  must retain AT+OTA, and firmware images must be validated against partitions.
- Firmware binaries belong in CI artifacts, **not Git history**.

## Physical interface and bootloader constraints
- All module UART connections are 3.3 V and TX/RX cross at the radio.
- TX15: PB9 UART TX -> Anchor RX; PB8 UART RX <- Anchor TX; PH11 is
  active-low module enable. Check actual hardware revisions before wiring.
- Prefer the chip's ROM UART0 pins for normal Anchor UART, so a future
  EdgeTX USB serial passthrough can communicate with the bootloader unchanged.
  C3 ROM UART0 RX=GPIO20 TX=GPIO21; C5 RX=GPIO12 TX=GPIO11;
  C6 RX=GPIO17 TX=GPIO16. On other chips, check Espressif headers/datasheets.
- Assign module enable to a separately accessible low-wake-capable input;
  avoid USB pins, bootstraps, connected onboard peripherals and flash pins.
  GPIO availability is board-dependent; verify schematics before soldering.
- 4 MB ESP32 images need a deliberate dual-app partition scheme; default
  ~1.2 MB OTA partitions will not fit Anchor. Confirm exact slot sizes and
  bootloader/partition offsets against **built** full images.
- C5 has distinct boot offset 0x2000; C3/C6 boot offset 0. C3/C6 need a
  board-specific GPIO sleep-wake mechanism; C5 uses EXT1 ANY_LOW.
- Update transports and installed-module USB maintenance are documented in
  `docs/RADIO-USB-UPDATE.md`; distinguish reference-hardware evidence from
  unverified board variants. Preserve recovery paths and protocol compatibility.
- Hardware test: factory AT setup, role0 telemetry/trainer behavior, role1
  RX, FrSky framing, reconnection, deep sleep/wake, UART baud rates, OTA
  upload/cancel, image recovery, and both power-on orders. No flight tests
  until bench checks and failsafe verification are complete.

## Board support and installer policy
- Existing C5 is the reference. C3 Super Mini is compile-verified, **not**
  physical-hardware verified. New C3/C6 profiles should be called
  experimental until actual boards are measured/tested.
- Add an ESP32 target only after verifying Arduino FQBN, UART ROM pins,
  hardware pin accessibility, flash capacity, partition slots, native USB,
  BLE/Wi-Fi, and sleep/wake behavior. Build both production and debug.
- `boards/candidates/` records boards with no working backend. Nordic
  nRF52840 requires a separate BLE/storage/power/DFU implementation; do not
  fake ESP32 compatibility. Uno/Nano are families, not one MCU type.
- `installer/generate_site.py` assembles `boards.json` and ESP Web Tools
  manifests from board profiles. GitHub Pages deployment currently includes
  only C5 and C3 Super Mini deliberately; do **not** automatically expose
  all newly compiling profiles to users. Each installer entry needs valid
  chip family, full image, metadata, and an accurate experimental warning.

## Git workflow and provenance
- Work on focused feature branches; keep production `main` unchanged until
  reviews, builds, and required hardware checks. GitHub SSH push access on
  AiPal may work even if GitHub integration PR creation returns 403.
- Use exact relevant component paths and preserve existing docs/photos.
- Avoid copying from BTWifiModule; it is protocol reference, not a source
  dependency. Attribute protocol discoveries, respect licenses and verify
  provenance before upstreaming/releasing.
- Report precise build/bench/test status, not just "supported".
