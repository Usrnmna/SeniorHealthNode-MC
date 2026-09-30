#pragma once

#include <Wire.h>
#include <math.h>
#include <HealthNodeConfig.h>
#ifdef ESP32
#include <esp32-hal-i2c.h>
#endif

// GY-521 with AD0 grounded. Wire1 is initialized by EnvironmentSensorManager.
class MPU6050Sensor {
public:
  struct Reading {
    float ax, ay, az;       // g, including gravity
    float gx, gy, gz;       // degrees/second
    float temperature;     // chip temperature, Celsius
    float acceleration() const { return sqrtf(ax*ax + ay*ay + az*az); }
    float rotation() const { return sqrtf(gx*gx + gy*gy + gz*gz); }
  } values = {};

  bool valid = false;

  // Returns true only for a fresh complete sample. Failed reads invalidate cache.
  bool poll(uint32_t now) {
    using namespace HealthNodeConfig;
    if (uint32_t(now - last_poll) < wait_ms && started) return false;
    started = true;
    last_poll = now;
    if (!ready) {
      uint8_t id;
      ready = read(0x75, &id, 1) && id == 0x68
        && write(0x6B, 0x01) && write(0x6C, 0x00) // wake, all axes enabled
        && write(0x1A, 0x03) && write(0x19, 19)   // DLPF, 50 Hz
        && write(0x1B, 0x08) && write(0x1C, 0x10) // +/-500 dps, +/-8 g
        && write(0x38, 0x00);                    // no interrupt pin needed
      Serial.println(ready ? "MPU6050 ready at 0x68" : "MPU6050 unavailable; retry in 5s");
      wait_ms = ready ? motion_sample_ms : motion_missing_retry_ms;
      failed_reads = 0;
      ready_at = now;
      return false; // allow sensor to settle before first read
    }
    if (uint32_t(now - ready_at) < 100) return false;
    uint8_t data[14];
    if (!read(0x3B, data, sizeof(data))) {
      valid = false; // Break analysis evidence immediately; do not reuse stale values.
      wait_ms = motion_retry_ms;
      if (++failed_reads >= motion_fast_failures) {
        ready = false; // Try reinitializing on the next short retry, before slow backoff.
        Serial.println("MPU6050 consecutive read failures; reinitializing");
      }
      return false;
    }
    values = {word(data)/4096.0f, word(data+2)/4096.0f, word(data+4)/4096.0f,
              word(data+8)/65.5f, word(data+10)/65.5f, word(data+12)/65.5f,
              word(data+6)/340.0f + 36.53f};
    valid = true;
    failed_reads = 0;
    wait_ms = motion_sample_ms;
    return true;
  }

private:
  bool ready = false, started = false;
  uint32_t last_poll = 0;
  uint32_t ready_at = 0;
  uint32_t wait_ms = HealthNodeConfig::motion_sample_ms;
  uint8_t failed_reads = 0;
  static int16_t word(const uint8_t* p) { return int16_t((uint16_t(p[0]) << 8) | p[1]); }
  bool write(uint8_t reg, uint8_t value) {
#ifdef ESP32
    // Use task-local buffers and the ESP32 HAL bus mutex. Wire1's receive
    // buffer is otherwise shared with the environmental collector after
    // requestFrom() releases its object lock but before read() consumes it.
    const uint8_t data[] = {reg, value};
    return i2cWrite(1, 0x68, data, sizeof(data), HealthNodeConfig::sensor_i2c_timeout_ms) == ESP_OK;
#else
    Wire1.beginTransmission(0x68);
    Wire1.write(reg); Wire1.write(value);
    return Wire1.endTransmission() == 0;
#endif
  }
  bool read(uint8_t reg, uint8_t* dest, uint8_t count) {
#ifdef ESP32
    size_t received = 0;
    return i2cWriteReadNonStop(1, 0x68, &reg, 1, dest, count,
        HealthNodeConfig::sensor_i2c_timeout_ms, &received) == ESP_OK && received == count;
#else
    Wire1.beginTransmission(0x68);
    Wire1.write(reg);
    if (Wire1.endTransmission(false) != 0) return false;
    if (Wire1.requestFrom(uint8_t(0x68), count) != count) {
      while (Wire1.available()) Wire1.read();
      return false;
    }
    for (uint8_t i = 0; i < count; ++i) dest[i] = Wire1.read();
    return true;
#endif
  }
};
