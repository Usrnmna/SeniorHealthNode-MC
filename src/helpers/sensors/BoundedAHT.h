#pragma once
#include <Wire.h>

// AHT10/AHT20 command sequence from the installed Adafruit driver. No busy loops:
// the background collector calls poll() between task yields. Failed reads and a
// stuck BUSY bit terminate within timeout_ms, including during initialization.
class BoundedAHT {
public:
  enum Result { Pending, Complete, Failed };
  void begin(TwoWire& bus, uint8_t address, uint32_t now) {
    wire = &bus; addr = address; started_at = changed_at = now;
    const uint8_t reset[] = {0xBA};
    state = send(reset, 1) ? ResetWait : Error;
  }
  bool start(uint32_t now) {
    if (state != Ready) return false;
    const uint8_t trigger[] = {0xAC, 0x33, 0x00};
    started_at = changed_at = now;
    state = send(trigger, 3) ? Measuring : Error;
    return state == Measuring;
  }
  Result poll(uint32_t now, uint32_t timeout_ms) {
    if (state == Ready) return Complete;
    if (state == Error) return Failed;
    if (uint32_t(now - started_at) >= timeout_ms) { state = Error; return Failed; }
    if (uint32_t(now - changed_at) < (state == Measuring ? 80U : 20U)) return Pending;
    uint8_t status;
    if (!read(&status, 1)) { state = Error; return Failed; }
    if (status & 0x80) return Pending;
    if (state == ResetWait) {
      const uint8_t calibrate[] = {0xE1, 0x08, 0x00};
      // Some AHT20 versions NACK calibration; the subsequent status is authoritative.
      send(calibrate, 3); changed_at = now; state = CalibrateWait;
      return Pending;
    }
    if (!(status & 0x08)) { state = Error; return Failed; }
    if (state == Measuring) {
      uint8_t data[6];
      if (!read(data, sizeof(data)) || (data[0] & 0x80)) { state = Error; return Failed; }
      const uint32_t h = (uint32_t(data[1]) << 12) | (uint32_t(data[2]) << 4) | (data[3] >> 4);
      const uint32_t t = (uint32_t(data[3] & 15) << 16) | (uint32_t(data[4]) << 8) | data[5];
      humidity = h * 100.0f / 1048576.0f;
      temperature = t * 200.0f / 1048576.0f - 50.0f;
    }
    state = Ready;
    return Complete;
  }
  float temperature = 0, humidity = 0;
private:
  enum State { Error, ResetWait, CalibrateWait, Ready, Measuring } state = Error;
  TwoWire* wire = nullptr;
  uint8_t addr = 0x38;
  uint32_t started_at = 0, changed_at = 0;
  bool send(const uint8_t* data, size_t count) {
    wire->beginTransmission(addr); wire->write(data, count);
    return wire->endTransmission() == 0;
  }
  bool read(uint8_t* data, uint8_t count) {
    if (wire->requestFrom(addr, count) != count) {
      while (wire->available()) wire->read();
      return false;
    }
    for (uint8_t i = 0; i < count; ++i) data[i] = wire->read();
    return true;
  }
};
