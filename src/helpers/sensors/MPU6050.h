#pragma once

#include <Wire.h>
#include <math.h>
#include <HealthNodeConfig.h>
#include <helpers/GyroCalibration.h>
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

  Reading raw_values = {}; // Scaled readings before bias correction; calibration input.
  const GyroCalibration::Result& calibration() const { return correction; }
  uint32_t sampleAgeMs(uint32_t now) const { return valid ? uint32_t(now - last_data_at) : UINT32_MAX; }
  // Recompute the diagnostic cache immediately; no synthetic fresh sample is emitted.
  bool setCalibration(const GyroCalibration::Result& value) {
    if (!GyroCalibration::validResult(value)) return false;
    correction = value; applyCalibration(); return true;
  }

  bool valid = false;
  struct Diagnostics {
    uint32_t bus_errors = 0, short_reads = 0, not_ready = 0, data_timeouts = 0;
    uint32_t init_attempts = 0, init_failures = 0, reinitializations = 0, recoveries = 0;
    uint32_t config_mismatches = 0, identity_mismatches = 0;
  };
  const Diagnostics& diagnostics() const { return counters; }
  void resetDiagnostics() { counters = Diagnostics{}; }
  bool initialized() const { return ready; }

  // Accept only a complete burst with DATA_RDY_INT set. The status byte and
  // measurement block share one I2C transaction; no separate INT wire is needed.
  bool poll(uint32_t now) {
    using namespace HealthNodeConfig;
    if (uint32_t(now - last_poll) < wait_ms && started) return false;
    started = true;
    last_poll = now;
    if (!ready) {
      ++counters.init_attempts;
      uint8_t id = 0;
      valid = false;
      const bool identified = read(0x75, &id, 1);
      if (identified && id != 0x68) ++counters.identity_mismatches;
      ready = identified && id == 0x68
        && configure(0x6B, 0x01, 0xEF) // Awake, temperature on, X gyro PLL.
        && configure(0x6C, 0x00, 0xFF) // All axes enabled.
        && configure(0x1A, 0x03, 0x3F) // DLPF=3, no external sync.
        && configure(0x19, 19,   0xFF) // 1 kHz / (1+19) = 50 Hz.
        && configure(0x1B, 0x08, 0xF8) // +/-500 dps; self-test off.
        && configure(0x1C, 0x10, 0xF8) // +/-8 g; self-test off.
        && configure(0x37, 0x00, 0xFE) // Clear status only when INT_STATUS is read.
        && configure(0x38, 0x01, 0x19); // Enable data-ready status; INT left unconnected.
      Serial.println(ready ? "MPU6050 ready at 0x68" : "MPU6050 unavailable; retry in 5s");
      if (!ready) { ++counters.init_failures; recovering = true; }
      wait_ms = ready ? motion_sample_ms : motion_missing_retry_ms;
      failed_reads = 0;
      ready_at = now;
      last_data_at = now;
      return false; // allow sensor to settle before first read
    }
    if (uint32_t(now - ready_at) < 100) return false;
    uint8_t burst[15]; // INT_STATUS followed by the coherent 14-byte sensor block.
    if (!read(0x3A, burst, sizeof(burst))) {
      sampleFailed();
      return false;
    }
    if (!(burst[0] & 0x01)) {
      ++counters.not_ready;
      // A healthy bus can still return an unchanged sensor sample. Do not feed
      // it to the detector or clear its fresh-data outage. Recover a stuck sensor.
      if (uint32_t(now - last_data_at) >= motion_data_timeout_ms) {
        ++counters.data_timeouts; sampleFailed();
      }
      return false;
    }
    const uint8_t* data = burst + 1;
    raw_values = {word(data)/4096.0f, word(data+2)/4096.0f, word(data+4)/4096.0f,
              word(data+8)/65.5f, word(data+10)/65.5f, word(data+12)/65.5f,
              word(data+6)/340.0f + 36.53f};
    applyCalibration();
    if (recovering) { ++counters.recoveries; recovering = false; }
    valid = true;
    last_data_at = now;
    failed_reads = 0;
    wait_ms = motion_sample_ms;
    return true;
  }

private:
  Diagnostics counters;
  bool recovering = false;
  GyroCalibration::Result correction;
  void applyCalibration() {
    values = raw_values;
    values.gx -= correction.bias[0]; values.gy -= correction.bias[1]; values.gz -= correction.bias[2];
  }
  bool ready = false, started = false;
  uint32_t last_poll = 0;
  uint32_t ready_at = 0;
  uint32_t last_data_at = 0;
  uint32_t wait_ms = HealthNodeConfig::motion_sample_ms;
  uint8_t failed_reads = 0;
  static int16_t word(const uint8_t* p) { return int16_t((uint16_t(p[0]) << 8) | p[1]); }
  // Verify relevant writable bits, ignoring reserved bits in register readback.
  // A bus failure or mismatch prevents readiness and uses the normal slow retry.
  bool configure(uint8_t reg, uint8_t value, uint8_t mask) {
    uint8_t actual;
    if (!write(reg, value) || !read(reg, &actual, 1)) return false;
    if ((actual & mask) == (value & mask)) return true;
    ++counters.config_mismatches; return false;
  }
  // Invalidate immediately, then attempt reinitialization after repeated faults.
  void sampleFailed() {
    valid = false;
    recovering = true;
    wait_ms = HealthNodeConfig::motion_retry_ms;
    if (++failed_reads >= HealthNodeConfig::motion_fast_failures) {
      ready = false;
      ++counters.reinitializations;
      Serial.println("MPU6050 read/data-ready failures; reinitializing");
    }
  }
  bool write(uint8_t reg, uint8_t value) {
#ifdef ESP32
    // Use task-local buffers and the ESP32 HAL bus mutex. Wire1's receive
    // buffer is otherwise shared with the environmental collector after
    // requestFrom() releases its object lock but before read() consumes it.
    const uint8_t data[] = {reg, value};
    const bool success = i2cWrite(1, 0x68, data, sizeof(data), HealthNodeConfig::sensor_i2c_timeout_ms) == ESP_OK;
#else
    Wire1.beginTransmission(0x68);
    Wire1.write(reg); Wire1.write(value);
    const bool success = Wire1.endTransmission() == 0;
#endif
    if (!success) ++counters.bus_errors;
    return success;
  }
  bool read(uint8_t reg, uint8_t* dest, uint8_t count) {
#ifdef ESP32
    size_t received = 0;
    const bool success = i2cWriteReadNonStop(1, 0x68, &reg, 1, dest, count,
        HealthNodeConfig::sensor_i2c_timeout_ms, &received) == ESP_OK;
    if (received != count) ++counters.short_reads;
    if (!success) { ++counters.bus_errors; return false; }
    if (received != count) return false;
    return true;
#else
    Wire1.beginTransmission(0x68);
    Wire1.write(reg);
    if (Wire1.endTransmission(false) != 0) { ++counters.bus_errors; return false; }
    if (Wire1.requestFrom(uint8_t(0x68), count) != count) {
      while (Wire1.available()) Wire1.read();
      ++counters.short_reads;
      return false;
    }
    for (uint8_t i = 0; i < count; ++i) dest[i] = Wire1.read();
    return true;
#endif
  }
};
