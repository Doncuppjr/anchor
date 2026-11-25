#pragma once
#include <esp_sleep.h>
// Waveshare ESP32-C3-Zero; Waveshare RX/TX markings are ROM UART0; confirm GPIO3 pad.
// TX15 UART TX -> RX GPIO20; TX15 UART RX <- TX GPIO21.
// TX15 PH11 active-LOW module enable -> GPIO3 (input).
// GPIOs 20/21 are the chip ROM UART0 pads, for future passthrough.
inline constexpr int RX_PIN = 20;
inline constexpr int TX_PIN = 21;
inline constexpr int EN_MONITOR_PIN = 3;
inline esp_err_t anchorConfigureWakeOnEnableLow() {
  return esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << EN_MONITOR_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
}
inline bool anchorWokeFromEnable() {
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO;
}
