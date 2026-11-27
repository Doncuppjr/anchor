# Nordic nRF52840 and Adafruit bootloader investigation

This note records the November 2026 bench work evaluating an nRF52840-based
ANCHOR implementation and the Adafruit nRF52 bootloader. It separates observed
hardware behavior from the current custom bootloader configuration. The
Nordic target is an experimental development path, not a supported ANCHOR
release target.

## Executive summary

- The application build target and reference hardware are the Pro Micro
  nRF52840 (`promicro_nrf52840/nrf52840/uf2`) running Adafruit nRF52
  bootloader v0.11.0.
- Board 1 enumerated in USB bootloader mode as VID:PID `239A:00B3`, with USB
  serial number `CABC4B7C1496E927`.
- The custom bootloader board definition uses the `nice!nano` product/UF2
  strings even though the Zephyr application target and reference hardware
  are Pro Micro nRF52840. This is inherited/misleading bootloader branding,
  not the physical board identity; correct the descriptor before release.
- UF2 mass-storage recovery was successfully demonstrated on Board 1.
  Automatic recovery after loss of power during an incomplete update was not
  demonstrated and must not be promised.
- The ANCHOR-specific Adafruit bootloader configuration currently lives in the
  separate `anchor-bootloader` source tree. Its tested baseline is commit
  `e38dfcf` (`Add opt-in hardware UART DFU transport for ANCHOR`).
- The experimental hardware-UART DFU configuration is 115200 baud, 3.3 V
  logic, TX GPIO 9 / RX GPIO 10, with hardware flow control disabled. These
  are the configured board pins; verify them against the physical board
  revision before wiring.

## Hardware identity and USB interfaces

### Board 1: Pro Micro nRF52840

| Property | Observation |
| --- | --- |
| MCU family | Nordic nRF52840 |
| Bootloader | Adafruit nRF52 bootloader v0.11.0 |
| UF2/CDC USB VID:PID | `239A:00B3` |
| USB serial number | `CABC4B7C1496E927` |
| UF2 product identity in this build | `nice!nano` |
| UF2 board ID in this build | `nRF52840-nicenano` |
| UF2 family | `0xADA52840` |

The Zephyr build reports model `Pro Micro nRF52840` and uses board target
`promicro_nrf52840/nrf52840/uf2`. The USB product and board strings come from
the custom bootloader board configuration and currently say `nice!nano`; that
configuration is not an accurate hardware label. USB VID:PID is useful for
recognizing this bootloader instance, but should not be used alone to infer
the physical PCB.

### Other nRF52840 board notes

The Yacht project records an important bootloader distinction:

- The Seeed XIAO nRF52840 BLE board ships with a UF2 bootloader pre-flashed.
- The Arduino Nano 33 BLE does not necessarily ship with the Adafruit UF2
  bootloader. The existing Yacht instructions use a J-Link or DAP-compatible
  debugger to install the Adafruit bootloader and SoftDevice.

Do not transfer the tested nice!nano-style USB identity or GPIO assignments
to either board. Board 2 was not part of the completed bootloader validation
recorded here. Record its bootloader version, USB identity, pinout, serial
number and recovery behavior independently when it is brought into the test
matrix.

### Distinguish the interfaces

The bootloader exposes distinct update/communication paths:

1. **UF2 mass storage:** the bootloader appears as a USB drive and accepts a
   UF2 application image by file copy. This is the demonstrated physical USB
   recovery path.
2. **USB CDC serial:** a USB serial interface exposed by the bootloader. It is
   distinct from the UF2 drive and from a hardware UART on the board headers.
3. **BLE DFU:** the bootloader's Bluetooth update service. This is not the same
   transport as the application-level ANCHOR BLE trainer/AT protocol.
4. **Hardware UART DFU:** an ANCHOR-specific opt-in path added to the custom
   bootloader configuration. It uses the pins and trigger described below.

A host may show the USB CDC interface as a COM port on Windows or a ttyACM
serial device on Linux. The exact COM/tty number is assigned by the host and
is not a stable device identity. Use VID:PID and serial number to identify the
correct device rather than hard-coding a port number.

## Adafruit bootloader behavior and recovery

The upstream Adafruit nRF52 bootloader supports UF2 application updates,
serial/CDC DFU and BLE OTA DFU, subject to the board configuration and
bootloader build options. The nRF52840 bootloader can enter USB/UF2 mode using
the board's DFU input or a double-reset sequence; the exact physical button
and reset wiring are board-specific.

### Verified on Board 1

- The board entered the Adafruit UF2 bootloader and enumerated as
  `239A:00B3`.
- The bootloader reported version `0.11.0`.
- A known-good ANCHOR application was restored using the USB UF2 recovery
  route, and normal application operation was recovered.
- The latest recorded transport bench run completed four full 290,860-byte
  transfers; activation completed, BLE operation and 2M PHY were observed,
  and the packet accounting recorded 1,355 valid frames and zero invalid
  frames.

The packet/PHY result is a bench result for the tested setup, not a guarantee
for every nRF52840 board, radio environment, or bootloader build.

### Recovery limitation

An interrupted single-bank update was also examined. In the incomplete case,
START and INIT had been accepted and 2,048 bytes acknowledged, but STOP and
activation had not occurred; the application was no longer valid. The board
was recoverable through the independent USB UF2 path. We did **not** establish
that arbitrary power loss during an update automatically rolls back or
recovers the application.

Destructive power-loss testing was intentionally stopped. The documented
operator recovery is to use the board's physical USB/UF2 bootloader access and
restore a known-good application. Do not describe the update mechanism as
power-fail-safe or claim automatic rollback.

## ANCHOR hardware-UART DFU configuration

The custom bootloader source is maintained separately from this application
repository in the `anchor-bootloader` working tree. The known baseline commit
is `e38dfcf943d29bd1ceb5659866d92ff5b8ed300e`.

The ANCHOR board definition currently configures:

| Setting | Value |
| --- | --- |
| UART TX | GPIO 9 |
| UART RX | GPIO 10 |
| Baud | 115200 |
| Logic level | 3.3 V TTL |
| Hardware flow control | Disabled |
| CTS / RTS | Not connected (`0xffffffff`) |
| DFU selection marker | `GPREGRET = 0x59` |

The `ENABLE_UART_DFU` build option includes the Nordic UART/HCI transport
sources and selects the ANCHOR-specific bootloader path. On reset, the
bootloader checks `GPREGRET`; value `0x59` selects UART DFU. In that path it
starts the DFU transport with the UART selected and does not initialize or
tear down the USB transport as if it were the normal USB path.

The marker is a bootloader-entry mechanism, not an AT command. An application
or host-side control path must arrange for the marker to be set and the target
to reset before the bootloader can enter this UART DFU mode. The current
hardware-UART configuration does not define a separate TX15-style active-low
module-enable wire. A board-specific way to request/reset into this mode, and
recovery when the application cannot run, must be documented before treating
UART DFU as a field-service path.

### Wiring precautions

- Cross TX to RX: host TX to target GPIO 10 (target RX), and target GPIO 9
  (target TX) to host RX.
- Connect grounds.
- Use 3.3 V logic; do not apply 5 V UART signals.
- Do not connect CTS/RTS for this configuration.
- Confirm the pin numbering against the exact board pinout. GPIO numbers and
  silkscreen labels are not interchangeable on every nRF52840 carrier.
- Do not infer that the USB CDC port and the GPIO 9/10 UART are the same
  serial port. They are separate physical interfaces.

## Application serial / AT protocol

The ANCHOR application AT command channel and bootloader DFU transport are
separate layers. The GPIO 9/10 mapping above is the custom bootloader UART
configuration; it is not a confirmed application UART mapping. On Arduino-
style nRF52840 cores, the USB CDC `Serial` interface and hardware `Serial1`
UART are separate resources, but the concrete pins and peripheral instance
are core- and board-variant-specific. The current records do not yet establish
which TinyGo/Arduino serial instance and physical pins should carry ANCHOR AT
commands on each candidate board. Do not wire or document a generic `Serial1`
pinout without checking the selected board definition and testing TX/RX.

The ANCHOR application AT command channel and bootloader DFU transport are
separate layers. AT commands are handled by the running application; the
bootloader's `GPREGRET` selector is used only after the target resets into the
bootloader. A successful USB CDC enumeration or UF2 mount does not, by itself,
prove that the application UART is correctly wired or that AT commands work.

For each physical board, record the application UART's TX/RX pin mapping,
voltage, baud, framing, and any enable/reset signal separately from the
bootloader UART mapping. The GPIO 9/10, 115200, no-flow-control values above refer specifically to
the current ANCHOR bootloader configuration. For the Pro Micro nRF52840
Zephyr target, the generated devicetree independently confirms UARTE0 TX
P0.09 and RX P0.10 at 115200 baud. The USB CDC ACM device is selected as
`zephyr,console` and is a separate interface. Do not copy this mapping to
another board without checking its board definition.

## Development status and next steps

- [x] Identify the reference target as Pro Micro nRF52840 and confirm Board 1
      bootloader USB identity and version.
- [x] Demonstrate UF2 USB recovery of a known-good ANCHOR application.
- [x] Preserve the custom bootloader baseline at `e38dfcf`.
- [x] Record the opt-in UART DFU pins, baud, flow-control setting and reset
      selector from the source configuration.
- [ ] Identify Board 2 independently and add its USB identity, bootloader
      version and pinout to the hardware matrix.
- [ ] Correct the custom bootloader USB product/UF2 identity from
      `nice!nano` to an accurate Pro Micro ANCHOR identity.
- [ ] Confirm the physical GPIO 9/10 mapping against the exact Board 1
      schematic/pinout before making a harness.
- [ ] Establish the application AT UART instance and TX/RX pins separately
      for each board/core (USB CDC `Serial` is not the hardware UART).
- [ ] Validate a complete hardware-UART DFU session end-to-end, including
      entry, transfer, verification, activation and return to the application.
- [ ] Define and test a safe application-to-bootloader request/reset path.
- [ ] Keep USB UF2 recovery documented as the fallback; do not claim automatic
      recovery from arbitrary interrupted writes.

## Source references

- [Adafruit nRF52 Bootloader](https://github.com/adafruit/Adafruit_nRF52_Bootloader)
- [Adafruit nRF52 DFU utility](https://github.com/adafruit/Adafruit_nRF52_nrfutil)
- [Microsoft UF2 format and tools](https://github.com/microsoft/uf2)
- ANCHOR custom bootloader baseline: `anchor-bootloader`, commit
  `e38dfcf943d29bd1ceb5659866d92ff5b8ed300e`
