#pragma once
#include <stdint.h>

// Authorization is volatile and is never restored from NVS or across PH11 reset.
class UartMaintenanceGate {
 public:
  static constexpr uint32_t timeoutMs = 120000;
  bool arm(uint32_t now, bool awaitingRole) {
    if (!awaitingRole) return false;
    armed_ = true; since_ = now; return true;
  }
  bool armed() const { return armed_; }
  bool expired(uint32_t now) const {
    return armed_ && uint32_t(now - since_) >= timeoutMs;
  }
  bool take(uint32_t now, bool awaitingRole) {
    bool allowed = armed_ && !expired(now) && awaitingRole;
    armed_ = false;
    return allowed;
  }
  void reset() { armed_ = false; }
 private:
  bool armed_ = false;
  uint32_t since_ = 0;
};
