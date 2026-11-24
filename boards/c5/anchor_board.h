#pragma once
#include <esp_sleep.h>
// Seeed XIAO ESP32-C5 mapping; TX15 PB9 TX -> RX, TX15 PB8 RX <- TX.
inline constexpr int RX_PIN = 12;
inline constexpr int TX_PIN = 11;
inline constexpr int EN_MONITOR_PIN = 1; // XIAO D0; TX15 PH11 active LOW
inline esp_err_t anchorConfigureWakeOnEnableLow() {
  return esp_sleep_enable_ext1_wakeup_io(1ULL << EN_MONITOR_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
}
inline bool anchorWokeFromEnable() {
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1;
}
