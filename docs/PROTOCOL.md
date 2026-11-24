# Generic optional Bluetooth module OTA handoff

This is **not** an ANCHOR-specific EdgeTX command. `AT+OTA` is an optional module extension.

## Transport

EdgeTX UART control channel uses CRLF-terminated ASCII AT commands and existing factory/operational baud handshake. When an operator confirms Update Bluetooth Module, EdgeTX resets and reconfigures the module at its normal baud/name/power stages, **then sends** `AT+OTA\r\n` instead of `AT+ROLE0`/`AT+ROLE1`. This ordering avoids intermixing the command with binary Telemetry or Trainer streams.

- Request: `AT+OTA\r\n`
- Success response: `OK+OTA\r\n`
- Unsupported response: `ERROR\r\n`, or no response for 3 seconds
- On success EdgeTX holds the module enabled without forwarding Telemetry or Trainer data and relies on the module for the update UI and transport.
- Unsupported/timeout: EdgeTX restarts normal Bluetooth initialization.
- Switching Bluetooth Off cancels or exits the EdgeTX maintenance state; it is **not** a substitute for preserving power while flashing.
- After OTA completion, use Bluetooth Off -> On to resume the usual handshake.

## ANCHOR implementation (non-normative)

- AP SSID `ANCHOR Chain` with user-agreed fixed WPA2 passphrase.
- Address `http://192.168.4.1/`, basic-auth-protected upload page.
- Accept app `.bin` only and reject invalid application header / merged flash images before writing.
- Uses the existing inactive ESP32-C5 app partition; reboots after successful Update completion.
- Idle timeouts; C5 must remain powered during OTA.

## Limitations before proposing an upstream API

- Initial TX15 prototype compiled but still needs hardware regression.
- UI currently reports only "request sent", not a confirmed OTA-ready state.
- No formal module capability-discovery query defined yet; unsupported modules are handled by the timeout/error fallback.
- The default fixed password is not robust against malicious nearby clients; OTA firmware signatures/secure boot and guaranteed boot rollback remain future hardening.
