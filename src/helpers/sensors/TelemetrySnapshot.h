#pragma once
#include <stdint.h>
#include <string.h>

// A fixed-size per-channel snapshot. Caller supplies synchronization when shared.
struct TelemetrySnapshot {
  static constexpr unsigned capacity = 64;
  uint8_t bytes[capacity] = {};
  uint8_t size = 0;
  uint32_t sampled_at = 0;
  void publish(const uint8_t* data, unsigned length, uint32_t now) {
    size = length <= capacity ? length : 0;
    if (size) memcpy(bytes, data, size);
    sampled_at = now; // Empty publication invalidates the preceding sample.
  }
  bool fresh(uint32_t now, uint32_t max_age) const {
    return size != 0 && uint32_t(now - sampled_at) < max_age;
  }
};
