# Build and validation record

## Reproducible build environment

The latest recorded build used:

| Component | Version / value |
| --- | --- |
| nRF Connect SDK | 3.3.0 |
| Zephyr | 4.3.99 |
| West | 1.5.0 |
| Latest AiPal toolchain variant | GNU Arm Embedded (gnuarmemb) |
| Latest AiPal GNU Arm compiler | 14.2.1 |
| Latest AiPal CMake | 4.3.0 |
| Earlier CI reference toolchain | Zephyr SDK 0.17.0 / GCC 12.2.0 |
| Board | `promicro_nrf52840/nrf52840/uf2` |
| Build mode | Sysbuild |
| Additional config | `conf/control.conf` |

The original CI/bench container had the NCS workspace at `/workspace/ncs` and
the project at `/workspace/adapt/anchor-native`. Its successful build command
was equivalent to:

```sh
west build -b promicro_nrf52840/nrf52840/uf2 \
  /workspace/adapt/anchor-native \
  -d /workspace/build-anchor-native-dt \
  -- -DEXTRA_CONF_FILE=/workspace/adapt/control.conf
```

The latest AiPal build used Arm GNU Embedded 14.2.1 rather than a bundled
Zephyr SDK. From an NCS 3.3.0 workspace with that toolchain installed, run:

    export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
    export GNUARMEMB_TOOLCHAIN_PATH=/path/to/arm-gnu-toolchain-14.2.rel1-aarch64-arm-none-eabi
    west build -p always -b promicro_nrf52840/nrf52840/uf2 "$PWD/platforms/nordic-nrf52840" -d "$PWD/build/nordic-nrf52840" -- -DEXTRA_CONF_FILE="$PWD/platforms/nordic-nrf52840/conf/control.conf"

A normal NCS installation with its supported Zephyr SDK can instead use its
configured toolchain variant; do not set the gnuarmemb variables in that case.

The workspace must be initialized from the nRF Connect SDK 3.3.0 manifest and
updated with `west update`; the Zephyr board definition and toolchain must be
available before building. The exact toolchain bundle used in the recorded
container was `911f4c5c26`.

## Board UART mapping

The generated devicetree for `promicro_nrf52840/nrf52840/uf2` selects USB CDC
ACM as `zephyr,console`. It separately enables `uart0` as `nordic,nrf-uarte`
with the board's `uart0_default` pinctrl group:

- UART TX: P0.09
- UART RX: P0.10
- Startup speed: 57600 baud (EdgeTX factory baud); AT+BAUD4 switches to
  115200 baud

This is the application hardware UART. It is not the USB CDC console. The
current ANCHOR bootloader board definition uses the same GPIO 9/10 assignment
for its optional UART DFU transport. Check the schematic/pin labels on the
physical board revision before wiring; do not assume other nRF52840 carriers
use this mapping.

## Build configuration

The project `prj.conf` enables the BLE central/client path, scanning, GATT
client, asynchronous UART API and Nordic enhanced UARTE RX. The additional
`conf/control.conf` enables USB CDC ACM, line control, UF2 output and reboot.
The application uses the USB CDC device for diagnostics while UARTE0 carries
the radio/AT data path. UARTE0 is initialized automatically at 57600 baud
at application startup; AT+BAUD4 acknowledges at that speed and switches to
115200 baud after a 50 ms drain/settle interval. A bounded retry loop attempts
startup five additional times at 500 ms intervals if initial configuration
fails. The USB UART_START command remains available as an explicit retry.

The generated configuration for the successful build included:

- `CONFIG_BT_CENTRAL=y`
- `CONFIG_BT_PERIPHERAL=y` (enabled in Kconfig, but a complete peripheral
  advertising role is not yet implemented by the application source)
- `CONFIG_BT_SCAN=y`
- `CONFIG_BT_GATT_CLIENT=y`
- `CONFIG_UART_ASYNC_API=y`
- `CONFIG_UART_0_ASYNC=y`
- `CONFIG_UART_NRFX_UARTE_ENHANCED_RX=y`
- `CONFIG_USB_DEVICE_STACK_NEXT=y`
- `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=y`
- `CONFIG_BUILD_OUTPUT_UF2=y`

## Host-side trainer decoder test

The test is independent of Zephyr and the target hardware:

```sh
make test
```

The recorded run passed 100,000 generated vectors, including legacy and
escaped checksum handling, reserved checksum values, corruption rejection
and truncated-frame rejection. It exercised 486 reserved checksum cases.

## Latest recorded target build

The 57600-baud factory-startup build was generated on 27 November 2026 at
17:11:28 local time. Its compiled src/main.c SHA-256 is
ba4b081c057932e13868f3b1a8ff6eb6acfe20e791cb00e2f7752a3503fe0b60.
The resulting build report was:

| Metric | Result |
| --- | ---: |
| Application flash use | 290,612 bytes / 792 KiB (35.83%) |
| RAM use | 55,656 bytes / 256 KiB (21.23%) |
| UF2 file size | 581,632 bytes |
| UF2 start address | 0x26000 |
| UF2 SHA-256 | 7ebfe954439e21586b0786b3097e54f8631dc0be2d68afc31564f9f74dfe94bb |

The latest build has no application C compiler warnings. Kconfig still
reports UART_NRFX_UARTE_ENHANCED_RX as deprecated (it remains enabled by
default unless the TIMER peripheral is used) and BT_HCI_ERR_TO_STR as
experimental. The generated UF2/ELF/HEX files are not committed; rebuild
from source using the command above to produce fresh artifacts.

## Bench validation status

Validated in the current native application work:

- BLE central scan for the FrSky-compatible `0xFFF0` service.
- GATT service/characteristic discovery and subscription to `0xFFF6`.
- Trainer notification validation and channel decode.
- Asynchronous UARTE0 transmit using EasyDMA.
- UART receive callback, RX buffer rotation and bounded AT message queue.
- Host-side frame parser regression test (100,000 vectors).

Still requiring a deliberate regression pass on the exact target hardware:

- TX15-originated AT command reception and ROLE negotiation over the hardware
  UART; no AT RX bytes were observed in the latest bench capture.
- Full AT compatibility and role-state behavior against the TX15.
- BLE reconnect and startup-order regression after repeated resets.
- Complete UART DFU transfer, verification, activation and return to the
  application using the custom bootloader.
- Recovery from interrupted UART DFU using the independent physical USB/UF2
  path.


## Latest hardware bench result (27 November 2026)

The latest UF2 was copied through the custom Adafruit bootloader to the
connected ANCHOR device (USB serial CABC4B7C1496E927). The bootloader reports
version 0.11.0-7-ge38dfcf-dirty and USB model nice!nano; that model string
is inherited from the custom bootloader board header and is not a reliable
physical PCB identification.

After the application rebooted, USB CDC returned. No UART_START command was
sent. The USB diagnostic BT_START command was used to exercise the central
path independently of the TX15 AT command path:

- BLE central initialized and scan started successfully.
- Connected to the advertising Yacht peer at RSSI -55 dBm.
- Discovered service 0xFFF0, characteristic 0xFFF6, and subscribed.
- Cumulative trainer notifications: 2,038; valid: 2,038; invalid: 0.
- UARTE TX completions: 1,923; mailbox-superseded frames: 115.
- UART TX aborts: 0; UART TX errors: 0.

TX completions plus superseded frames account for all 2,038 notifications.
This verifies automatic UARTE startup, BLE central operation, trainer decode,
and the asynchronous UARTE TX path on the flashed device. It does not prove
that an external receiver consumed every transmitted byte, and it does not
yet validate the TX15-originated AT RX/ROLE handshake. No AT RX events were
seen in the capture.

## Bootloader handoff contract

The application AT parser accepts `AT+UPDATE=UART` only before a BLE role is
active. After authorization, `AT+DOWNLOAD` replies, waits briefly, writes
`0x59` to `NRF_POWER->GPREGRET`, and cold-reboots. The custom Adafruit
bootloader's ANCHOR board configuration recognizes `GPREGRET == 0x59` as the
UART DFU selection marker. This marker is not itself an AT command and does
not prove that the subsequent DFU transfer has been validated end-to-end.

The USB diagnostic command `UF2` currently writes `0x57` and cold-reboots;
that is a separate debug/recovery path. Preserve physical USB/UF2 recovery
independently of the UART DFU transport.


## Factory-baud correction (27 November 2026)

The TX15 EdgeTX source for this target defines BLUETOOTH_FACTORY_BAUDRATE
as 57600 and BLUETOOTH_DEFAULT_BAUDRATE as 115200. Its initialization
sequence opens at 57600, transmits AT+BAUD4, waits 100 ms, then reopens at
115200 before sending the name and power commands. The Nordic app acknowledges
at 57600 and switches after 50 ms, leaving margin before EdgeTX changes baud.

The Nordic application now mirrors that contract:

- UARTE0 starts at 57600, including its RX and TX paths.
- AT+BAUD4 replies OK while still at 57600.
- After a 50 ms drain/settle interval, the UART is configured to 115200.
- Subsequent AT+NAME, AT+TXPW2, and AT+ROLE1 traffic is expected at 115200.
- TESTBAUD57600 remains a USB diagnostic command.

The corrected 57600-startup UF2 was rebuilt and flashed through the physical
Adafruit UF2 volume on the ANCHOR board. The bootloader UART DFU itself uses
115200 baud (HCI UART configuration); that is independent of the application
factory baud. Build and host parser tests passed.
The exact TX15 hardware handshake has not yet been exercised after this
correction; connect the TX15 UART before claiming the complete baud transition
and AT sequence validated.


## Teyleten Robot Pro Micro switched-VCC polarity

The exact purchased board listing is Teyleten Robot Pro Micro nRF52840,
ASIN B0CYLNZ6V4. The listing specifies a 3.7 V lithium battery interface and
a software switch for LED power. The corresponding Pro Micro pinout documents
P0.13 LOW as VCC off and P0.13 HIGH as VCC = 3.3 V. The application now
defaults P0.13 HIGH, and P0.15 is toggled every 500 ms as a heartbeat.

The earlier P0.13-LOW default was based on a different nice!nano revision
and was incorrect for this Teyleten Robot board. USB flashing of this
corrected build remains pending until the board re-enumerates; do not claim
the change is installed until its new build marker is observed.

## Teyleten Robot switched-VCC / heartbeat build (28 November 2026)

The corrected application build configures P0.13 HIGH at startup (switched
VCC enabled) and P0.15 as a 1 Hz blue LED heartbeat. USB diagnostics can
manually set the rail with VCC_LOW (off) and VCC_HIGH (on). The host trainer
parser regression passed 100,000 vectors.

- UF2 SHA-256: 9013851bc5c3e4fcee3e93388d5c3d0f6ce21849ec19cc849f98506df32ebb78
- Application flash: 291,648 bytes / 792 KiB (35.96%)
- RAM: 55,656 bytes / 256 KiB (21.23%)
- UF2 size: 583,680 bytes

This image has been built but has **not yet been flashed**. The board was not
enumerating as USB mass storage during this build session. Verify the startup
marker and LED heartbeat after the next successful UF2 flash.
