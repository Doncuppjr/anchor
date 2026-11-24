# ROLE0 Trainer Slave relay (experimental)

EdgeTX uses the **same** `AT+ROLE0` for Bluetooth Telemetry and for
Bluetooth Trainer **Slave**. The module cannot distinguish them from the
initial AT handshake. It must inspect the subsequent UART data:

- EdgeTX Trainer Slave emits a FrSky framed 8-channel message with decoded
  type `0x80`, 12 packed channel bytes, XOR checksum and `0x7e` delimiters.
  Reserved bytes are escaped using `0x7d` and XOR `0x20`.
- EdgeTX Bluetooth Telemetry emits a telemetry stream instead.
- Both use BLE GATT service `0xFFF0` and notification characteristic `0xFFF6`.

ANCHOR now starts ROLE0 in **undecided** state. If the first traffic is a
fully validated Trainer Slave frame, it selects **Trainer**. Any other start,
invalid frame, or incomplete probe lasting over 50 ms selects **Telemetry**.
The selected application protocol remains locked for the rest of the ROLE0
session; it resets on the next AT role handshake or PH11 radio reset.

In Trainer mode, ANCHOR forwards **only validated complete UART trainer
frames** to the BLE central via FFF6 notifications. It retains the newest
pending frame, rather than allowing old control samples to queue. A frame
already being transmitted in 20-byte chunks is completed before switching to
the latest frame. No synthetic channel values are generated. The original
Trainer RX path (`AT+ROLE1`) is unchanged.

Telemetry remains a raw UART-to-notification stream, including its optional
CRSF address translation for TelemetryViewer. The 50 ms initial probe is
buffered so a prospective trainer packet is not leaked as telemetry before
classification. Debug USB `TELEMSTATUS` reports protocol state and trainer
frame counters.

## Bench acceptance (not yet completed)

1. Verify two ANCHOR boards: TX15 A set to Bluetooth Trainer **Slave**,
   TX15 B set to Bluetooth Trainer **Master**. Pair B with A; check all eight
   channels, polarity, response and link timeout/failsafe handling.
2. Move sticks rapidly. Verify that stale frames are superseded and that
   frame-to-frame UART/BLE latency remains acceptable under sustained load.
3. Disconnect and reconnect repeatedly; test PH11 OFF/ON and deep sleep.
4. Switch each mode through EdgeTX, including Telemetry. Confirm Telemetry
   still reaches a BLE subscriber with no trainer-frame contamination.
5. Check frames containing escaped bytes, damaged checksums, and interrupted
   frames. Confirm that no invalid trainer frames are forwarded to the receiver.

**Status:** Host parser tests and both Arduino firmware variants compiled.
No two-radio or BLE hardware regression has been performed yet. Do not use
this feature to control an airborne model before full bench validation.
