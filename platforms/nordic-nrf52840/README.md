# ANCHOR native Nordic nRF52840 application

This is the native Zephyr/Nordic Connect SDK implementation of ANCHOR. It is
separate from the ESP32-C5 Arduino implementation in `firmware/anchor/` and
from the TinyGo YACHT head-tracker firmware in the `yacht` repository.

The current reference target is the Seeed XIAO nRF52840 Sense. See [XIAO validation](docs/XIAO-VALIDATION.md) for the verified baseline.

- Zephyr board: `xiao_ble/nrf52840/sense`
- MCU: Nordic nRF52840
- SDK: nRF Connect SDK 3.3.0 (Zephyr 4.3.99)
- USB console: USB CDC ACM
- Application UART: UARTE0, initially 57600 baud (EdgeTX factory baud);
  AT+BAUD4 switches it to 115200 baud. Both modes use 8N1, no flow control.
- Application UART pins on this Zephyr board definition: TX D6/P1.11, RX D7/P1.12
- BLE role: central scanner/client for the FrSky-compatible trainer service

## Current functionality

- Starts the Zephyr Bluetooth host and scans for advertising devices carrying
  16-bit service UUID `0xFFF0`.
- Connects to a matching device, discovers service `0xFFF0`, characteristic
  `0xFFF6`, discovers the CCC descriptor and subscribes to notifications.
- Validates received trainer frames with the shared `trainer_decode()` parser.
- Maintains a latest-frame mailbox and forwards the original validated frame
  bytes over UARTE0 using the asynchronous UART API / EasyDMA. Intermediate
  frames may be skipped when the UART is busy; the skipped-frame counter is
  reported over the USB console.
- Implements an asynchronous UART RX buffer pool and a bounded message queue
  for AT command input.
- Implements the currently supported AT command subset and the guarded
  `AT+UPDATE=UART` / `AT+DOWNLOAD` handoff to the custom Adafruit bootloader.
- Provides a USB CDC diagnostic console for retrying UART startup, starting
  BLE and inspecting packet, frame and UART counters.

## Important current limitations

This is the latest integrated bench application, not yet a production-ready
replacement for every ESP32 ANCHOR behavior. In the current source:

- UARTE0 is initialized automatically at 57600 baud during application startup.
  AT+BAUD4 is acknowledged at 57600, then the application switches to 115200
  after 50 ms. The USB CDC diagnostic UART_START command remains available as
  a manual retry if initial setup fails; startup retries five times at 500 ms
  intervals.
- `AT+ROLE1` enables BLE and starts central scanning.
- `AT+ROLE0` stops the trainer central and acknowledges the radio's role
  change. The native application has no BLE peripheral telemetry implementation.
- `AT+DISC?` reports matching trainer advertisements and `AT+CON...` selects
  the peer for the current session. EdgeTX keeps the saved peer on the radio.
- `AT+NAME` and `AT+TXPW` currently echo compatibility responses rather than
  configuring a full persistent radio parameter set.
- The hardware-UART DFU handoff and full transfer have been validated using
  TX15 USB passthrough at 460800 baud and with a 115200-baud fallback.

The [XIAO validation record](docs/XIAO-VALIDATION.md) covers the TX15 AT
handshake, Trainer reception and reconnect, USB and Nordic Legacy BLE DFU,
and the 875 µs SCI trainer link. Legacy BLE DFU and SCI are opt-in profiles.

## Build and test

See [BUILD-AND-VALIDATION.md](docs/BUILD-AND-VALIDATION.md). The custom Adafruit
bootloader change is preserved as a patch under [bootloader](bootloader/README.md).
Run the host-side
trainer decoder test with `make test`. Build the UF2 image with `make build`
from an initialized NCS 3.3.0 workspace and toolchain.

## Shorter Connection Intervals bench profile

Append `conf/sci-bench.conf` to the `EXTRA_CONF_FILE` list along with
`conf/control.conf` and any DFU profile in use. The build initially connects
at 7.5 ms and accepts a peripheral-initiated SCI request if both controllers
support it. Use the native NCS YACHT bench application for this test.
The USB console reports the controller minimum at startup and prints the
actual connection interval, latency and subrate when the controller reports
a connection-rate change.

## Source layout

- `src/main.c`: BLE central, GATT discovery/subscription, trainer forwarding,
  asynchronous UART TX/RX and AT command handling.
- `src/trainer.h`: portable FrSky PARA trainer frame validation/decoding.
- `prj.conf`: Bluetooth, UARTE async RX/TX and application configuration.
- `conf/control.conf`: USB CDC console, UF2 output and reboot configuration
  used by the recorded successful build.
- `Kconfig.sysbuild`: sysbuild configuration.
- `tests/trainer_test.c`: host-side frame parser regression test.

The NCS/Zephyr source tree, build directory, generated ELF/HEX/UF2 files and
host test executable are intentionally not committed. They are build inputs
or outputs, not application source.


## Historical Teyleten Robot Pro Micro power control

The retired bench reference hardware was the Teyleten Robot Pro Micro nRF52840 (Amazon
ASIN B0CYLNZ6V4). Its product listing describes a 3.7 V lithium battery
interface and a software-controlled LED/VCC power switch. The matching
Pro Micro pinout identifies P0.13 as the switched VCC control:

- P0.13 LOW: switched VCC output off (0 V).
- P0.13 HIGH: switched VCC output on (3.3 V).
- P0.15: onboard status LED.

On the Pro Micro target ANCHOR configures P0.13 HIGH at startup and blinks its board LED as an
application heartbeat. USB diagnostic commands VCC_LOW and VCC_HIGH remain
available to test the rail manually. This pin polarity is specific to the
Teyleten Robot / nRFMicro-style board documented here; do not generalize
it to every board sold as a Pro Micro nRF52840.
