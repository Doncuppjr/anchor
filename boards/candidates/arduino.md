# Arduino-family board suitability (not build targets yet)

`Nano` and `Uno` identify form factors or product families, not an MCU/bootloader.
Choose an actual board and a transport/bootloader solution before adding a build.

| Board | Suitability | Why |
| --- | --- | --- |
| Arduino Nano ESP32 (ESP32-S3) | Strong ESP32 backend candidate | BLE + Wi-Fi, 16 MB flash; needs ROM-UART pad accessibility, UART pin and flash/partition verification; Arduino-specific FQBN/USB differences |
| Arduino Nano 33 BLE / Sense (nRF52840) | Nordic backend candidate | BLE but no onboard Wi-Fi; see nrf52840.md |
| Arduino Nano RP2040 Connect | Significant port needed | Separate connectivity subsystem; no direct ESP32 firmware target |
| Arduino Nano Every / classic Nano | Not standalone Anchor targets | No integrated BLE and limited memory |
| Arduino Uno R3 | Not suitable as standalone Anchor | 32 KB flash, 2 KB RAM, no integrated BLE |
| Arduino Uno R4 Minima | Not suitable standalone | No integrated BLE or Wi-Fi |
| Arduino Uno R4 WiFi | Complex dual-MCU platform | RA4M1 drives board; ESP32-S3 is a connectivity coprocessor with special update path and 5 V Uno I/O |

Do not assume the 5 V GPIO found on many Uno boards is safe for TX15 or any
3.3 V BLE module. Never list a board in the browser installer until a working
firmware image, board-specific ROM pinout, power/wake mapping, recovery method,
and hardware regression results exist.

References: Arduino Nano ESP32, Nano 33 BLE, Uno R3, and Uno R4 WiFi official documentation.
