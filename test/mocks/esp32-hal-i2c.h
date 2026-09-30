#pragma once
#include <Wire.h>
constexpr int ESP_OK = 0, ESP_FAIL = 1;
inline int i2cWrite(uint8_t, uint16_t, const uint8_t* data, size_t count, uint32_t) {
  Wire1.writes.emplace_back(data, data + count);
  return Wire1.status == 0 ? ESP_OK : ESP_FAIL;
}
inline int i2cWriteReadNonStop(uint8_t, uint16_t, const uint8_t* data, size_t count,
    uint8_t* dest, size_t size, uint32_t, size_t* received) {
  Wire1.writes.emplace_back(data, data + count); ++Wire1.requests;
  auto bytes = Wire1.response ? Wire1.response(data[0], size) : std::vector<uint8_t>{};
  *received = bytes.size();
  if (bytes.size() != size) return ESP_FAIL;
  std::copy(bytes.begin(), bytes.end(), dest); return ESP_OK;
}
