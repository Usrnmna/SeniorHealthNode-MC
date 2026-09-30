#pragma once
#include <stdint.h>
#include <HealthNodeConfig.h>

// Tracks one 128x64 framebuffer without allocating a second copy.
class OledFrameTransfer {
  uint16_t offset = 1024;
public:
  static constexpr uint8_t chunk_bytes = HealthNodeConfig::oled_chunk_bytes;
  static_assert(chunk_bytes > 0 && chunk_bytes <= 32 && 128 % chunk_bytes == 0,
                "OLED chunks must divide a page");
  void start() { offset = 0; }
  void cancel() { offset = 1024; }
  bool pending() const { return offset < 1024; }
  uint16_t position() const { return offset; }
  void completed(bool success) {
    if (pending()) offset = success ? offset + chunk_bytes : 1024;
  }
};
