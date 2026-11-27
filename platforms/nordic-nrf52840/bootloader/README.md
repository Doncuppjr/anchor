# ANCHOR UART DFU bootloader patch

This directory preserves the ANCHOR-specific patch against the Adafruit nRF52
bootloader. It is a small patch on top of upstream, not a vendored copy of the
Adafruit repository.

## Provenance

- Upstream: https://github.com/adafruit/Adafruit_nRF52_Bootloader
- Base commit recorded by the working checkout:
  `0ee83433a6b8ec23341629d2407cbf0fc62335f3`
- ANCHOR patch commit:
  `e38dfcf943d29bd1ceb5659866d92ff5b8ed300e`
- Patch: `patches/0001-anchor-uart-dfu.patch`

The patch adds an `anchor` board target, selects the Nordic hardware UART for
HCI SLIP when `GPREGRET == 0x59`, and leaves the ordinary USB/UF2 and BLE DFU
paths available for their normal selectors.

## Apply and build

```sh
git clone --recurse-submodules https://github.com/adafruit/Adafruit_nRF52_Bootloader.git
cd Adafruit_nRF52_Bootloader
git checkout 0ee83433a6b8ec23341629d2407cbf0fc62335f3
git apply --check /path/to/0001-anchor-uart-dfu.patch
git apply /path/to/0001-anchor-uart-dfu.patch
make BOARD=anchor all
```

The patch was carried in the local `anchor-bootloader` checkout on branch
`anchor-uart-dfu`. Its board target enables `ENABLE_UART_DFU` and configures:

| Setting | Value |
| --- | --- |
| UART TX | GPIO 9 / P0.09 |
| UART RX | GPIO 10 / P0.10 |
| Baud | 115200 |
| Data format | 8N1 |
| Flow control | None |
| CTS / RTS | Not connected |
| UART DFU selector | `GPREGRET = 0x59` |
| UF2 family | `0xADA52840` |
| Flash / RAM | 1 MiB / 256 KiB |

These UART pins match the current Pro Micro nRF52840 Zephyr application
UARTE0 mapping. USB CDC and the hardware UART remain separate interfaces.

## Important limitations

- The board header currently contains `nice!nano` USB product and UF2 board
  strings even though the actual Zephyr reference target is Pro Micro
  nRF52840. This is inherited/misleading branding and must be corrected before
  release; see the parent bootloader investigation document.
- The UART DFU selector and transport are implemented in source, but a full
  hardware UART DFU transfer/activation/recovery cycle is not yet qualified.
- No TX15-style active-low enable pin is defined by this bootloader target.
  The running application must authorize the update, set `GPREGRET`, and
  reset into the bootloader.
- Keep the independent physical USB/UF2 recovery path. Do not claim automatic
  rollback after arbitrary power loss during an incomplete update.

The exact patch is preserved so the custom bootloader can be recreated without
copying the full upstream source into this ANCHOR repository.
