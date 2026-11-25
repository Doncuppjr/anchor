# Board profiles

Anchor core logic is shared in `firmware/anchor/src/anchor.cpp`. Add a new board
by providing `boards/<name>.mk` with its Arduino FQBN, output prefix and flash
constraints, plus `boards/<name>/anchor_board.h` with its pin and wake mapping.
The build routes both compiler configuration and generated image names through
those files. The host trainer protocol test is board-independent.

| Board profile | FQBN | Flash | OTA slots | Status |
| --- | --- | --- | --- | --- |
| `c5` | `esp32:esp32:XIAO_ESP32C5` | 8 MB | 3 MB each | Reference hardware, previously tested |
| `c3-supermini` | `esp32:esp32:esp32c3:PartitionScheme=no_fs,CDCOnBoot=cdc` | 4 MB | 0x1F0000 each | Compiled, hardware testing required |

## Additional compact ESP32 board targets

These have board profiles using their chip's default ROM UART0 pins. Firmware
compilation is tracked separately from **physical hardware validation**.

| Profile | MCU | TX15 -> board RX | Board TX -> TX15 | Enable input | Flash / OTA layout |
| --- | --- | --- | --- | --- | --- |
| `xiao-c3` | ESP32-C3 | GPIO20 | GPIO21 | GPIO3 | 4 MB, dual 0x1E0000 |
| `c3-zero` | ESP32-C3 | GPIO20 | GPIO21 | GPIO3 | 4 MB, dual 0x1F0000 |
| `xiao-c6` | ESP32-C6 | GPIO17 | GPIO16 | GPIO1 | 4 MB, dual 0x1F0000 (build override) |
| `c6-supermini` | ESP32-C6 | GPIO17 | GPIO16 | GPIO1 | 4 MB, dual 0x1E0000 |

Board variants differ: confirm ROM UART pads, GPIO accessibility, antenna,
USB interface and wake-pin availability against your actual schematic. New
profiles are not yet listed in the live installer; verified compilation is
not equivalent to hardware testing.

See [nRF52840 candidates](candidates/nrf52840.md) and [Arduino Nano/Uno
candidates](candidates/arduino.md) for boards requiring a different backend
or additional hardware design.

## C3 Super Mini proposed wiring

* Anchor **RX GPIO20** ← TX15 transmit, 3.3 V UART
* Anchor **TX GPIO21** → TX15 receive, 3.3 V UART
* Anchor **enable GPIO3** ← TX15 PH11 signal, active-low
* Common GND, power from a regulated 3.3 V rail with sufficient peak current

GPIO20 (UART0 RX) and GPIO21 (UART0 TX) are the ESP32-C3 ROM bootloader
defaults. This makes a future transparent TX15 USB-to-UART flashing bridge
possible without moving the UART wires; see [radio USB update](../docs/RADIO-USB-UPDATE.md) for implementation and reference-hardware validation. C3 hardware remains unverified.

These are **different** connections from C5 wiring! Avoid GPIO2/8/9 boot-strap pins and
GPIO18/19 native USB pins. Check the actual pin mapping on your Super Mini.
Native USB CDC is enabled for debug/maintenance, since the generic C3 board disables it by default.
A 4 MB `no_fs` partition scheme creates two 0x1F0000 application slots for OTA.
Do not use the default ~1.2 MB app partitions: Anchor does not fit.

C3 uses GPIO deep-sleep wake because the C5's EXT1 ANY_LOW wake API is unavailable.
The C3 pin selection and wake behavior need hardware confirmation before
connecting to a transmitter or flying. BLE throughput, reconnection and OTA
are unvalidated on the C3. Select the exact board model in the installer; C5 images are **not** suitable for C3.
