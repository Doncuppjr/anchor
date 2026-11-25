#pragma once
#include <esp_sleep.h>
// XIAO ESP32-C6; Seeed D7 RX / D6 TX / D1 enable; GPIO3 reserved RF switch.
// TX15 UART TX -> RX GPIO17; TX15 UART RX <- TX GPIO16.
// TX15 PH11 active-LOW module enable -> GPIO1 (input).
// GPIOs 17/16 are the chip ROM UART0 pads, for future passthrough.
inline constexpr int RX_PIN = 17;
inline constexpr int TX_PIN = 16;
inline constexpr int EN_MONITOR_PIN = 1;
inline esp_err_t anchorConfigureWakeOnEnableLow() {
  return esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << EN_MONITOR_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
}
inline bool anchorWokeFromEnable() {
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO;
}
