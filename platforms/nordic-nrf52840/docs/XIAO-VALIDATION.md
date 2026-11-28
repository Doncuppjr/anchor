# XIAO nRF52840 Sense baseline - 2026-11-27 (local)

ANCHOR and YACHT will use Seeed XIAO nRF52840 Sense as the current reference hardware. The Teyleten Pro Micro showed intermittent heartbeat/USB behavior, even with USB alone; touching ground changed the heartbeat. Root cause was not established. Preserve the old work, but do not spend further development time on that board.

## Software and flash layout

- NCS 3.3.0 / Zephyr 4.3.99, target xiao_ble/nrf52840/sense.
- Factory bootloader 0.6.1 upgraded to 0.11.0-8-g2aea5a8 (verified via INFO_UF2.TXT).
- S140 7.3.0 retained; legacy S140 serves bootloader BLE OTA, not the application's linked modern SDC/MPSL. SCI negotiation on the XIAO pair remains to be tested.
- Application starts at 0x27000; bootloader at 0xF4000, MBR params at 0xFE000.
- Bootloader upstream base 0ee83433a6b8ec23341629d2407cbf0fc62335f3, UART transport patch e38dfcf943d29bd1ceb5659866d92ff5b8ed300e, XIAO board patch 2aea5a8. Original Nordic UART driver retained. No custom UART transmitter or protocol changes required.
- Factory UF2 supports bootloader self-update with the generated nosd UF2 (family 0xd663823c); S140 unchanged. Verify target serial and INFO_UF2.TXT before copying; verify version after reboot. A sync I/O error on automatic USB disconnect is not itself proof of failure or success.

## Hardware

| Signal | XIAO | AiPal Pi 5 physical pin | TX15 UART4 |
| --- | --- | --- | --- |
| TX | D6/P1.11 | 10 / GPIO15 RX | RX PB8 |
| RX | D7/P1.12 | 8 / GPIO14 TX | TX PB9 |
| EN sense (active low) | D0/P0.02 | 11 / GPIO17 | PH11 |
| Ground | GND | 6 | GND |

Use 3.3 V logic, crossed TX/RX, common ground. Use only one intended power source; do not parallel Pi 3.3 V and USB supply. Standalone power qualification still pending. The bench transfer had USB diagnostics available. AiPal UART is /dev/ttyAMA0, separate from debug console ttyAMA10. ANCHOR bootloader/application serial DEC210BA233A837A; verify device identity rather than relying on ttyACM numbering.

Green P0.30 heartbeat toggles every 500 ms after main starts, active low. Bootloader red P0.26 and blue P0.06 active low. Do not drive the Pro Micro switched VCC pin P0.13 on XIAO. USB diagnostic PING LED=0 means LED initialization succeeded, not LED state. VCC control returns -ENOTSUP on XIAO.

## Build / update

Build the application with make build in platforms/nordic-nrf52840 inside an initialized NCS workspace. Build the bootloader from the two preserved patches:

    cmake -S . -B build -DBOARD=xiao_nrf52840_ble_sense -DSD_VERSION=7.3.0
    cmake --build build

Use Python with intelhex installed. Copy the generated update-*nosd.uf2 to the verified XIAO-SENSE drive while in factory UF2 mode; do not copy the merged SoftDevice HEX to the UF2 drive.

Generate the application DFU package with make dfu-package (NRFUTIL may point to the venv executable). Equivalent:

    adafruit-nrfutil dfu genpkg --dev-type 0x0052 --dev-revision 52840 --application zephyr.hex anchor.zip

Critical: default genpkg metadata is FFFF/FFFF, which the Adafruit bootloader rejects during INIT, after START has erased application flash. This caused failed attempts. Device type 0x0052 is mandatory; retain revision 52840. The working package has SD requirement 0xFFFE. Do not diagnose this as a UART driver defect. The package checker verifies metadata.

On a clean application start, using 8N1, no flow control:

1. At 57600 send AT, expect OK.
2. Send AT+BAUD4, expect OK at 57600; application switches after 50 ms; host waited 150 ms and selected 115200.
3. At 115200 send AT, expect OK.
4. Send AT+UPDATE=UART, expect OK+UPDATE:UART; role must not be active.
5. Send AT+DOWNLOAD, expect OK+DOWNLOAD; application waits 100 ms then reboots with GPREGRET=0x59.
6. Host waited 250 ms after the reply, then ran:

    adafruit-nrfutil --verbose dfu serial -pkg anchor.zip -p /dev/ttyAMA0 -b 115200 -sb

Use unmodified nrfutil HCI SLIP implementation, including its flash erase wait after START. No hardware flow control or touch reset. USB recovery selector is GPREGRET=0x57; physical double-reset also available. Interrupted single-bank transfers can erase the old application; USB UF2 recovery was successfully demonstrated. Do not promise automatic rollback.

## Verified bench results

- 100,000 host trainer vectors passed.
- Application flash 294596 bytes, RAM 55784 bytes.
- AT, NAME, TXPW/capabilities and both baud rates verified over Pi hardware UART.
- GPIO17 LOW/HIGH/LOW read back correctly on P0.02 over USB diagnostics; first query gave no response. GPIO17 returned to input afterward. Enable-driven role/sleep behavior is not yet qualified.
- Corrected UART DFU package: 294596 bytes, full START/INIT/DATA/activation, 42.089 seconds; nrfutil reported Device programmed.
- User confirmed green heartbeat after activation. Independent hardware UART AT at 57600 returned OK after update.
- YACHT not modified in this session.
- Next: standalone power/cold starts, real TX15 UART4 handshake and passthrough DFU, BLE role behavior and SCI negotiation.
