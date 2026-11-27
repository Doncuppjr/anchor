# Hardware notes

Reference target: Pro Micro nRF52840 using Zephyr board target
`promicro_nrf52840/nrf52840/uf2`.

| Interface | Signal | MCU pin | Notes |
| --- | --- | --- | --- |
| Application UART | TX | P0.09 | UARTE0, 115200 8N1 |
| Application UART | RX | P0.10 | UARTE0, 115200 8N1 |
| USB diagnostics | CDC ACM | USB | Separate from UARTE0 |
| Bootloader UART DFU | TX | P0.09 | Custom Adafruit bootloader config |
| Bootloader UART DFU | RX | P0.10 | Custom Adafruit bootloader config |

The generated devicetree maps UARTE0 TX to P0.09 and RX to P0.10. Use 3.3 V
TTL, common ground, crossed TX/RX, and no CTS/RTS for the current configuration.
Do not connect 5 V UART logic. Verify the actual board revision and pin labels
before attaching the TX15 harness.

The USB CDC console is selected as Zephyr's `zephyr,console`; it is not the
application UART. Use it for diagnostics and the current `UART_START` / `BT_START`
commands. The application data and AT channel is UARTE0.
