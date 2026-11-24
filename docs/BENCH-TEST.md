# ANCHOR Chain controlled bench regression

Do not test while operating/flying. Current C5 on AiPal is `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_10:BD:A3:CF:B9:B0-if00`, TX15 is the OpenTX RadioMaster serial device, and the nRF52840 is separate. Check port identities again before wired flashing.

1. Back up the currently flashed ANCHOR image/NVS if needed and retain the two existing known-good merged images under `c5-unified/c5-dual-role-telemetry/recovery`. Confirm USB/BOOT fallback is physically available.
2. Flash **only** the C5 candidate to the identified ESP32-C5 over USB. Verify normal Trainer stream before attempting OTA; test Telemetry mode and dynamic BLE advertising name.
3. Flash a TX15 firmware that includes the four Bluetooth reliability fixes, fast Trainer receive, **and** optional `AT+OTA` UI. Use the `bluetooth/tx15-ota-integration` build, not the standalone clean-2.12 OTA branch.
4. From Bluetooth Settings, select Trainer or Telemetry, press `Update module`, confirm, and watch USB diagnostics for `ANCHOR_OTA_REQUEST` and `ANCHOR_OTA_READY`.
5. On a phone/PC connect Wi-Fi to `ANCHOR Chain` using `anchor-update`; browse `http://192.168.4.1`; authenticate `admin` / `anchor-update`.
6. Attempt an unauthorized HTTP request, an invalid `.bin`, and an 8 MiB merged-image upload; all must be rejected without changing the boot partition.
7. Upload the *application-only* ANCHOR candidate `.bin`; verify success, reboot, and intact NVS name/role/peer. The full merged recovery image is for wired flashing only and may overwrite NVS.
8. Toggle Bluetooth Off->On on the TX15 and check Trainer stream, CRC errors, connect/disconnect, Telemetry and dynamic name. Repeat OTA entry from Telemetry.
9. Verify portal inactivity shutdown and interrupted upload handling. Do not claim automatic boot rollback; recovery still depends on valid hardware access.
