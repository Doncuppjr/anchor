#pragma once
#include <esp_sleep.h>
// Generic ESP32-C3 Super Mini; direct 3.3V UART and separate enable signal.
// Proposed wiring, NOT the XIAO C5 pinout. Check your actual board silkscreen.
// TX15 TX -> GPIO4 RX; TX15 RX <- GPIO5 TX; TX15 PH11 -> GPIO3 enable.
// GPIO18/19 are reserved for native USB; GPIO2/8/9 boot strapping avoided.
inline constexpr int RX_PIN = 4;
inline constexpr int TX_PIN = 5;
inline constexpr int EN_MONITOR_PIN = 3; // RTC capable on ESP32-C3
inline esp_err_t anchorConfigureWakeOnEnableLow() {
  return esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << EN_MONITOR_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
}
inline bool anchorWokeFromEnable() {
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO;
}
