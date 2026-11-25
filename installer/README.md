# Anchor multi-board browser installer

`generate-site.py` builds board-specific ESP Web Tools manifests from validated
production and debug CI images. The board selector is populated from the
supported board profiles in `boards/<board>.mk` and `site/boards.json`.

Supported selections are currently XIAO ESP32-C5 (`c5`, 8 MB) and ESP32-C3
Super Mini (`c3-supermini`, 4 MB). The C5 is the reference board; the C3 is
compile-tested only and the installer shows a warning before flashing.

Each board/profile has its own full-image manifest at
`<board>/<profile>/manifest.json`, with the correct ESP chip family and
flash length. The installer uses USB ROM flashing, not UF2. Wi-Fi OTA on
an already installed device accepts application-only firmware, never the
merged/full image.

The GitHub Actions matrix builds both boards and both profiles, assembles the
four verified images, and publishes one Pages site. Refactor branches build
and validate the site artifact without altering live GitHub Pages. Main-branch
runs deploy the installer automatically when Pages is enabled.

Local packaging example after all four builds:

```sh
python3 installer/generate-site.py --dist dist --output site --version local-test --boards c5 c3-supermini
```

Use a Chromium browser with Web Serial. Choose the **exact hardware board**
before clicking Install. Do not flash a C5 full image onto a C3 or vice versa.

## Installed-module USB bridge

The "Installed module: USB through the TX15" section uses the radio's CDC
serial interface, rather than connecting directly to the ESP USB interface.
It requires the capability-aware EdgeTX branch and Anchor firmware announcing
UPDATE_UART. The older direct-module screenshots in docs/USB-FLASHING.md do
not show this bridge flow.

1. On the radio choose Update module > USB and confirm stopping Bluetooth.
2. Connect the radio USB port to the computer.
3. Select the exact installed board and Production or Diagnostic profile.
4. Click "Connect radio and flash module" and choose the radio serial port.
5. Keep the radio powered through verification and module restart.
6. Confirm the radio says "Module restarted. Bluetooth restored."

The installer checks image SHA-256/structure and detected chip family, flashes
the full merged image at 115200 without a stub, and confirms whole-image MD5.
Full flashing may overwrite pairing/settings. The completion signal to the
radio requests restart verification; it is not itself proof Bluetooth returned.

Cancellation before ROM entry restores normal Bluetooth. After entering ROM,
closing the page/port or Back on the radio leaves a recoverable maintenance
session. Reconnect the installer to retry; do not rely on Back to reset ROM.
After an interrupted write, complete a verified flash before expecting the
application to boot. The C3/C5 restart sequence uses the RTC watchdog recipe
from Espressif esptool 5.3.1 because esptool-js 0.7.1 has no C5 watchdog reset.

Bench status on 25 November 2026: C5 identification, full-image flashing and
readback passed through a TX15 using Python esptool. A separate watchdog reset
restored Bluetooth. Browser end-to-end and C3 hardware validation are pending.


The installed-module installer requires bridge protocol 2. Older bridge
versions are rejected before ROM entry. After watchdog restart it closes
the ROM connection, reopens the same authorized port and sends
AT+BRIDGE=FINISH before any probe or binary traffic. EdgeTX acknowledges
OK+BRIDGE:FINISH before beginning normal module initialization. This avoids
depending on RTS-only control changes, which failed on the Windows bench.
The acknowledgment confirms receipt of the request; Bluetooth restoration
still requires the radio's fresh role/identity handshake.
