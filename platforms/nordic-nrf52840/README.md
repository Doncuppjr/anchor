# ANCHOR native Nordic nRF52840 application

This is the native Zephyr/Nordic Connect SDK implementation of ANCHOR. It is
separate from the ESP32-C5 Arduino implementation in `firmware/anchor/` and
from the TinyGo YACHT head-tracker firmware in the `yacht` repository.

The current reference target is the Pro Micro nRF52840:

- Zephyr board: `promicro_nrf52840/nrf52840/uf2`
- MCU: Nordic nRF52840
- SDK: nRF Connect SDK 3.3.0 (Zephyr 4.3.99)
- USB console: USB CDC ACM
- Application UART: UARTE0, 115200 baud, 8 data bits, no parity, 1 stop bit,
  no hardware flow control
- Application UART pins on this Zephyr board definition: TX P0.09, RX P0.10
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

- UARTE0 is initialized automatically during application startup. The USB CDC
  diagnostic command `UART_START` remains available as a manual retry if the
  initial driver setup fails. Startup retries up to five times at 500 ms
  intervals before leaving the diagnostic retry command available.
- `AT+ROLE1` enables BLE and starts central scanning.
- `AT+ROLE0` enables the BLE host but does not yet implement a complete BLE
  peripheral advertising/connection role.
- `AT+DISC?` and `AT+CON...` currently return compatibility placeholder
  responses; they do not implement a full remote discovery/connect workflow.
- `AT+NAME` and `AT+TXPW` currently echo compatibility responses rather than
  configuring a full persistent radio parameter set.
- The hardware-UART DFU handoff is implemented in the application, but a full
  end-to-end UART DFU transfer and recovery regression remains a separate
  validation item.

The latest bench run verified automatic UARTE startup and TX completions while
forwarding valid BLE trainer notifications. TX15-originated AT RX and role
negotiation have not yet been observed. These distinctions are intentional
documentation of the actual source, not claims that unfinished paths are complete.

## Build and test

See [BUILD-AND-VALIDATION.md](docs/BUILD-AND-VALIDATION.md). The custom Adafruit
bootloader change is preserved as a patch under [bootloader](bootloader/README.md).
Run the host-side
trainer decoder test with `make test`. Build the UF2 image with `make build`
from an initialized NCS 3.3.0 workspace and toolchain.

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
