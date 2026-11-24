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

## C3 Super Mini proposed wiring

* Anchor **RX GPIO4** ← TX15 transmit, 3.3 V UART
* Anchor **TX GPIO5** → TX15 receive, 3.3 V UART
* Anchor **enable GPIO3** ← TX15 PH11 signal, active-low
* Common GND, power from a regulated 3.3 V rail with sufficient peak current

These are **new** connections, not C5 wiring! Avoid GPIO2/8/9 boot-strap pins and
GPIO18/19 native USB pins. Check the actual pin mapping on your Super Mini.
Native USB CDC is enabled for debug/maintenance, since the generic C3 board disables it by default.
A 4 MB `no_fs` partition scheme creates two 0x1F0000 application slots for OTA.
Do not use the default ~1.2 MB app partitions: Anchor does not fit.

C3 uses GPIO deep-sleep wake because the C5's EXT1 ANY_LOW wake API is unavailable.
The C3 pin selection and wake behavior need hardware confirmation before
connecting to a transmitter or flying. BLE throughput, reconnection and OTA
are unvalidated on the C3. The C5 installer is **not** suitable for the C3.
