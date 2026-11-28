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

## TX15 trainer and System OFF validation (2026-11-27)

- Native ANCHOR on XIAO nRF52840 Sense connected to TX15 UART4: D6 TX, D7 RX, D0 active-low enable. YACHT uses XIAO Sense.
- Canonical Connected/DisConnected notifications and gated trainer forwarding restored working EdgeTX trainer channels.
- Blue heartbeat; green searching/connecting breathing or solid connected; red sleep preparation/update indication. All controlled LEDs off during System OFF.
- Sustained EN HIGH for 2500 ms initiates shutdown; EN LOW cancels pending sleep. Active USB CDC DTR inhibits sleep, VBUS alone does not.
- USB SLEEP? reports GPIO, USB, countdown, shutdown and reset diagnostics. GPREGRET2=0xA6 marks the poweroff path; RESETREAS.OFF confirms wake.
- Hardware test with USB attached: LEDs extinguished and USB disconnected on disable; Trainer re-enable restored USB and automatically connected YACHT. RESETREAS=00010000, OFF_WAKE=1, RETAINED=a6, PEER=1, ROLE=1, STARTED=1.
- Post-wake snapshot: 9494 valid frames, 8620 UART completions, zero invalid frames, UART errors or aborts. User confirmed short-disable cancellation and another successful sleep/reconnect cycle; subsequent diagnostic again reported OFF_WAKE=1 and RETAINED=a6.
- Host regression: 100000 vectors passed. Instrumented build: flash 300644 bytes, RAM 55912 bytes; UF2 installed on verified ANCHOR serial DEC210BA233A837A.
- Remaining: standalone operation without USB, current measurement, remote software-triggered UF2 entry (connector blocked the command), actual TX15 passthrough DFU, discovery/persistence, telemetry and SCI performance. USB diagnostics required sudo on AiPal. No production qualification claimed.

## TX15 USB UART DFU at 460800 baud

Hardware validation on 2026-11-28 used the XIAO nRF52840 Sense ANCHOR, a TX15
running EdgeTX build `8a70a1c0` (GitLab pipeline 2731, job 12089), and an
application-only Nordic package with device type `0x0052` and revision `52840`.
The TX15 installer runs on the host and communicates through the radio's
external USB CDC port. The radio negotiates the module UART rate separately.

Select **SYS → Hardware → Bluetooth → Update module → USB**. From this platform
directory, run the host wrapper with a Python environment containing
`adafruit-nrfutil`. Use the verified TX15 CDC device path.

```sh
python tools/usb_dfu.py --port "$TX15_CDC" --baud 460800 --package anchor.zip
```

The wrapper requires an exact protocol-2 baud banner before issuing DOWNLOAD.
It keeps the CDC connection open from READY through the stock HCI SLIP
transfer and sends FINISH on a fresh connection. To resume a retained
bootloader session, run the same command with `--recover`.

| Bench run | START through STOP | HCI sends | UART errors | RX overruns |
| --- | ---: | ---: | ---: | ---: |
| First 460800-baud transfer | 23.571 s | 591 of 591 expected | 0 | 0 |
| Reconnect after DOWNLOAD, before START | 23.572 s | 591 of 591 expected | 0 | 0 |

Both runs sent the 300,788-byte application, received FINISH, and showed
ANCHOR re-enumerating in application mode. After the first run, the TX15
displayed `Module restarted. Bluetooth restored`, and ANCHOR again received
valid BLE trainer frames and forwarded them over UART. Each run forwarded
309,229 host bytes and 3,559 response bytes. The elapsed time includes the
stock tool's flash erase wait.

The recovery run verified host reconnection after the radio had switched baud
and before the tool sent START. Recovery during erase or DATA and the 115200
fallback through this bridge remain to be checked.
