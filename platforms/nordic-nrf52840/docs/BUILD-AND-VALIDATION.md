# Build and validation record

## Reproducible build environment

The latest recorded build used:

| Component | Version / value |
| --- | --- |
| nRF Connect SDK | 3.3.0 |
| Zephyr | 4.3.99 |
| West | 1.5.0 |
| Zephyr SDK | 0.17.0 |
| GNU Arm compiler | GCC 12.2.0 |
| CMake | 4.2.1 |
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

From a normal NCS 3.3.0 workspace, run:

```sh
west build -p always \
  -b promicro_nrf52840/nrf52840/uf2 \
  "$PWD/platforms/nordic-nrf52840" \
  -d "$PWD/build/nordic-nrf52840" \
  -- -DEXTRA_CONF_FILE="$PWD/platforms/nordic-nrf52840/conf/control.conf"
```

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
- Speed: 115200 baud

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
the radio/AT data path.

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

The latest source modification was made on 27 November 2026 at 12:20:54 local
time, and the target UF2 was generated at 12:21:12. The compiled `src/main.c`
SHA-256 was `f0be08dbe26ea24d9963735518756ccc1cc79125ea71e6b7952d4dfb70c96e54`.
The resulting build report was:

| Metric | Result |
| --- | ---: |
| Application flash use | 290,860 bytes / 792 KiB (35.86%) |
| RAM use | 55,656 bytes / 256 KiB (21.23%) |
| UF2 file size | 582,144 bytes |
| UF2 start address | `0x26000` |
| UF2 SHA-256 | `4331829f71e9e7c625c6ea5c4cf7ee586357dabe04a7eedc5b485755c7b201bf` |

The successful build emitted compiler warnings for misleading indentation
and possible `snprintf` truncation in the AT response formatting. These should
be cleaned up before treating the build as release quality. The generated
UF2/ELF/HEX files are not committed; rebuild from source using the command
above to produce fresh artifacts.

## Bench validation status

Validated in the current native application work:

- BLE central scan for the FrSky-compatible `0xFFF0` service.
- GATT service/characteristic discovery and subscription to `0xFFF6`.
- Trainer notification validation and channel decode.
- Asynchronous UARTE0 transmit using EasyDMA.
- UART receive callback, RX buffer rotation and bounded AT message queue.
- Host-side frame parser regression test (100,000 vectors).

Still requiring a deliberate regression pass on the exact target hardware:

- Cold-start UART availability without first issuing the USB diagnostic
  `UART_START` command.
- Full AT compatibility and role-state behavior against the TX15.
- BLE reconnect and startup-order regression after repeated resets.
- Complete UART DFU transfer, verification, activation and return to the
  application using the custom bootloader.
- Recovery from interrupted UART DFU using the independent physical USB/UF2
  path.

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
