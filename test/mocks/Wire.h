#pragma once
#include <Arduino.h>
#include <functional>
#include <vector>

// Transaction-level fake for MPU recovery and AHT conversion failure tests.
class TwoWire {
public:
  std::function<std::vector<uint8_t>(uint8_t, uint8_t)> response;
  std::vector<std::vector<uint8_t>> writes;
  std::vector<uint8_t> tx, rx;
  size_t cursor = 0;
  unsigned requests = 0;
  uint8_t reg = 0, status = 0;
  void beginTransmission(uint8_t) { tx.clear(); }
  size_t write(uint8_t value) { tx.push_back(value); return 1; }
  size_t write(const uint8_t* data, size_t size) {
    tx.insert(tx.end(), data, data + size); return size;
  }
  uint8_t endTransmission(bool = true) {
    writes.push_back(tx); if (!tx.empty()) reg = tx[0]; return status;
  }
  uint8_t requestFrom(uint8_t, uint8_t count) {
    ++requests; cursor = 0; rx = response ? response(reg, count) : std::vector<uint8_t>{};
    return rx.size();
  }
  int available() { return rx.size() - cursor; }
  int read() { return cursor < rx.size() ? rx[cursor++] : -1; }
};
inline TwoWire Wire1;
struct SensorTestSerial { void println(const char*) { } };
inline SensorTestSerial Serial;
