#pragma once
// FrSky Bluetooth trainer frame envelope used by EdgeTX sendTrainer():
// 0x7e, escaped type(0x80)+12 packed channel bytes+XOR, 0x7e.
// A complete 8-channel frame has 14 decoded bytes and up to 30 wire bytes.
// No Arduino dependencies: test the exact parser on a host compiler.
#include <stddef.h>
#include <stdint.h>

class TrainerFrameParser {
 public:
  enum Result : uint8_t { Pending, Valid, Invalid };
  static constexpr size_t kDecodedSize = 14;
  static constexpr size_t kWireCapacity = 34;
  uint8_t wire[kWireCapacity] = {};
  size_t wireLen = 0;
  void reset() { wireLen = 0; decodedLen_ = 0; xor_ = 0; escape_ = false; active_ = false; }
  Result feed(uint8_t b) {
    if (b == 0x7e) {
      if (!active_) { start(); return Pending; }
      // A delimiter terminates a frame. An empty frame only restarts it.
      if (decodedLen_ == 0) { start(); return Pending; }
      bool ok = !escape_ && decodedLen_ == kDecodedSize && xor_ == 0 && decodedType_ == 0x80;
      if (ok && wireLen < kWireCapacity) {
        wire[wireLen++] = 0x7e;
        active_ = false;
        return Valid;
      }
      start();
      return Invalid;
    }
    if (!active_) return Pending;
    if (wireLen >= kWireCapacity - 1) { reset(); return Invalid; }
    wire[wireLen++] = b;
    if (escape_) { b ^= 0x20; escape_ = false; }
    else if (b == 0x7d) { escape_ = true; return Pending; }
    if (decodedLen_ == 0) decodedType_ = b;
    xor_ ^= b;
    if (++decodedLen_ > kDecodedSize) { reset(); return Invalid; }
    return Pending;
  }
 private:
  bool active_ = false, escape_ = false;
  uint8_t decodedType_ = 0, xor_ = 0;
  size_t decodedLen_ = 0;
  void start() {
    active_ = true; escape_ = false; decodedLen_ = 0; decodedType_ = 0; xor_ = 0;
    wireLen = 1; wire[0] = 0x7e;
  }
};
