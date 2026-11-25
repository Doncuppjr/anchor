# Update an installed Anchor module through the TX15 USB port

This is the bench workflow for the USB bridge development branches. It uses
the radio's USB port while the module remains installed. The illustrated
[direct-module flashing guide](USB-FLASHING.md) describes a different connection;
its screenshots do not show this bridge workflow.

Use a matching hosted installer from the development build, plus EdgeTX with
Anchor USB bridge protocol 2. The public installer may lag these branches.
The tested module is a Seeed XIAO ESP32-C5. C3 builds pass CI but still need
hardware validation.

## Flashing

1. Keep the radio powered. In Bluetooth settings choose **Update module > USB**
   and confirm **Stop Bluetooth and update the module?**
2. Wait for **Connect radio USB to the browser installer.**
3. Connect the radio to desktop Chrome or Edge. If the radio asks for a USB
   mode, choose **Serial**.
4. In the installer choose the exact installed board, then **Production** or
   **Diagnostic** under **Installed module: USB through the TX15**.
5. Click **Connect radio and flash module**. Choose the radio's USB serial
   port. On the Windows test bench this appeared as **USB Serial Device
   (COM25)**; another computer may assign a different number.
6. Keep USB connected and the radio powered through writing and verification.
7. Wait for the radio to say **Module restarted. Bluetooth restored.**
   The browser's flash verification or restart-request message alone does
   not establish that Bluetooth has returned.

The installer checks the downloaded full image's SHA-256 and target chip,
then checks flash readback before requesting watchdog restart. It uses the
same authorized serial port again for a separate acknowledged completion
request. No CLI commands or ESP USB auto-reset wiring are needed.

A full image may overwrite pairing/settings. Check Trainer channel mapping
and direction afterward, then test automatic reconnection with the tracker
powered first and with the radio powered first.

## Cancellation and recovery

Before the module enters its ROM loader, closing the host connection after
the bridge handshake or pressing Back on the armed radio cancels maintenance.

Once the module is in ROM, Back closes the status dialog while maintenance
ownership remains active. **Update module > USB update status** reopens it.
Reconnect the installer to retry; it recognizes a retained recovery session.

After an interrupted write, keep the radio powered and retry a complete
verified flash. The application may be incomplete, so power-cycling is not
a guaranteed recovery path. If power is lost with an incomplete boot image,
physical module download-mode access may be needed. The TX15's PH11 enable
signal does not reset a module executing ROM.

Use application-only images for Wi-Fi or Bluetooth OTA. Do not upload a
full merged USB image through an OTA transport.

## Bench evidence and remaining checks

On 25 November 2026, the TX15/C5 bench passed:

- Windows serial enumeration and pre-ROM host-close cancellation.
- C5 identification and a verified 8 MiB full-image ROM flash.
- Interrupted writing halfway through a ROM frame, recovery reconnect and
  a complete verified reflash.
- Physical USB removal/reconnect with the radio still powered.
- Watchdog restart, acknowledged completion and radio-confirmed Bluetooth
  restoration over repeated sessions.
- Trainer channel mapping/direction and automatic reconnection after power
  cycling either device in both startup orders.
- A fresh Wi-Fi OTA test: invalid-image rejection, verified application upload
  in 11.9 seconds and operator-confirmed Trainer operation after restart.

The interrupted-write recovery recorded zero UART errors and RX overruns,
with a maximum USB service gap of 1.028 ms. Normal Bluetooth FIFO sizes
remained unchanged.

BLE OTA protocol testing with a Linux client also passed fresh session-PIN
authentication, partial-upload abort and full application validation/boot
selection. The operator also confirmed Trainer channel operation returned
after the BLE update restart. Browser end-to-end validation, C3/CC2540 hardware and Telemetry
regression checks remain pending. Physical Back dismissal was
intermittent. A separate popup-focus fix passed its host regression test and
CI, then closed promptly on the first RTN tap in a repeated no-write cable-
reconnect session. This single repeat does not prove all intermittent cases
are eliminated. These results do not establish a production release.
