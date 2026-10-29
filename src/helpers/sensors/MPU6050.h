#pragma once

#include <Wire.h>
#include <math.h>
#include <HealthNodeConfig.h>
#include <helpers/GyroCalibration.h>
#ifdef ESP32
#include <esp32-hal-i2c.h>
#endif

// GY-521 on the companion's Wire1 bus. FIFO order: accel, temperature, gyro.
class MPU6050Sensor {
public:
  struct Reading {
    float ax, ay, az;       // g, including gravity
    float gx, gy, gz;       // degrees/second
    float temperature;     // chip temperature, Celsius
    float acceleration() const { return sqrtf(ax*ax + ay*ay + az*az); }
    float rotation() const { return sqrtf(gx*gx + gy*gy + gz*gz); }
  } values = {};

  enum Quality : uint8_t {
    EstimatedTime = 1, AccelSaturated = 2, GyroSaturated = 4, EstimatedTimeAdjusted = 8
  };
  // Two packets fit a 28-byte transaction at the existing 100 kHz bus rate.
  static constexpr unsigned fifo_packet_bytes = 14, batch_samples = 2;
  static constexpr uint32_t max_backlog_ms = 1000;
  static_assert(HealthNodeConfig::motion_sample_ms == 20,
      "FIFO timestamp spacing must match the configured 50 Hz sensor rate");
  Reading raw_values = {};
  bool valid = false;
  const GyroCalibration::Result& calibration() const { return correction; }
  uint32_t sampleAgeMs(uint32_t now) const { return valid ? uint32_t(now - last_data_at) : UINT32_MAX; }
  uint32_t sampleTimeMs() const { return last_data_at; }
  uint32_t discontinuityGeneration() const { return discontinuity; }
  uint8_t sampleQuality() const { return quality; }
  // Software records plus the unconsumed hardware count snapshot.
  unsigned queuedSamples() const { return buffered - cursor + fifo_pending; }
  bool setCalibration(const GyroCalibration::Result& value) {
    if (!GyroCalibration::validResult(value)) return false;
    correction = value; applyCalibration(); return true;
  }
  struct Diagnostics {
    uint32_t bus_errors = 0, short_reads = 0, not_ready = 0, data_timeouts = 0;
    uint32_t init_attempts = 0, init_failures = 0, reinitializations = 0, recoveries = 0;
    uint32_t config_mismatches = 0, identity_mismatches = 0;
    uint32_t fifo_overflows = 0, fifo_resets = 0, discarded_samples = 0;
    uint32_t timing_discontinuities = 0, timestamp_adjustments = 0, samples = 0, saturation_samples = 0;
    uint32_t max_fifo_samples = 0;
  };
  const Diagnostics& diagnostics() const { return counters; }
  void resetDiagnostics() { counters = Diagnostics{}; }
  bool initialized() const { return ready; }

  // Returns one chronological sample. Callers bound calls per loop. Buffered
  // samples bypass the poll interval. Newest is dated one nominal period before
  // the completed count read; phase, count-read latency and clock drift remain
  // uncertainty rather than measured interrupt timestamps.
  bool poll(uint32_t now) {
    using namespace HealthNodeConfig;
    const uint32_t clock_at_entry = millis();
    if (queuedSamples() && uint32_t(now - next_sample_at) > max_backlog_ms) {
      ++counters.timing_discontinuities; discardFifo(now); return false;
    }
    if (cursor < buffered) return emit();
    if (!fifo_pending && started && uint32_t(now - last_poll) < wait_ms) return false;
    const uint32_t since_poll = uint32_t(now - last_poll);
    last_poll = now; started = true;
    if (!ready) {
      ++counters.init_attempts;
      uint8_t id = 0;
      breakContinuity();
      const bool identified = read(0x75, &id, 1);
      if (identified && id != 0x68) ++counters.identity_mismatches;
      ready = identified && id == 0x68
        && configure(0x6B, 0x01, 0xEF)
        && configure(0x6C, 0x00, 0xFF)
        && configure(0x1A, 0x03, 0x3F)
        && configure(0x19, 19,   0xFF)
        && configure(0x1B, 0x08, 0xF8)
        && configure(0x1C, 0x10, 0xF8)
        && configure(0x37, 0x00, 0xFE)
        && configure(0x38, 0x11, 0x19) // Data-ready and overflow status.
        && configure(0x23, 0xF8, 0xFF) // All six axes and temperature.
        && configure(0x6A, 0x00, 0xF0); // Disable FIFO during sensor settling.
      if (!ready) { ++counters.init_failures; recovering = true; }
      wait_ms = ready ? motion_sample_ms : motion_missing_retry_ms;
      failed_reads = 0; ready_at = last_arrival_at = now; fifo_started = false;
      return false;
    }
    if (uint32_t(now - ready_at) < 100) return false;
    if (!fifo_started || reset_required) {
      if (!resetFifo()) { sampleFailed(); return false; }
      if (!fifo_started) last_arrival_at = now;
      fifo_started = true; reset_required = false;
      return false; // Collect settled output starting with the next sensor sample.
    }
    uint8_t status = 0;
    if (!read(0x3A, &status, 1)) { sampleFailed(); return false; }
    if (status & 0x10) { ++counters.fifo_overflows; discardFifo(now); return false; }
    if (!fifo_pending) {
      uint8_t count_bytes[2];
      if (!read(0x72, count_bytes, 2)) { sampleFailed(); return false; }
      const unsigned bytes = (unsigned(count_bytes[0]) << 8) | count_bytes[1];
      if (bytes >= 1024) { ++counters.fifo_overflows; discardFifo(now, bytes / fifo_packet_bytes); return false; }
      const unsigned records = bytes / fifo_packet_bytes;
      if (records > counters.max_fifo_samples) counters.max_fifo_samples = records;
      // A partial trailing packet may be in progress; never consume its bytes.
      if (!records) {
        ++counters.not_ready;
        if (uint32_t(now - last_arrival_at) >= motion_data_timeout_ms) {
          ++counters.data_timeouts; sampleFailed();
        }
        return false;
      }
      const uint32_t age = records * motion_sample_ms;
      if (since_poll > max_backlog_ms || age > max_backlog_ms) {
        ++counters.timing_discontinuities; discardFifo(now, records); return false;
      }
      // I2C work can span a sample boundary. Anchor to count-read completion,
      // excluding the earlier poll-entry delay from the timestamp estimate.
      // The elapsed offset preserves the caller's clock origin for host tests.
      const uint32_t snapshot_at = now + uint32_t(millis() - clock_at_entry);
      next_sample_at = snapshot_at - age;
      time_adjusted = false;
      if (have_sample) {
        const int32_t separation = int32_t(next_sample_at - last_data_at);
        // Count-latch timing and sampling phase can overlap estimates despite
        // known FIFO order. Correct only a sub-period overlap, by at most one
        // period; the newest corrected sample remains at or before snapshot_at.
        if (separation <= 0 && separation > -int32_t(motion_sample_ms)) {
          next_sample_at = last_data_at + 1;
          time_adjusted = true; ++counters.timestamp_adjustments;
        }
      }
      // Do not invent continuous evidence when FIFO progress contradicts wall time.
      if (have_sample && (int32_t(next_sample_at - last_data_at) <= 0 ||
          uint32_t(next_sample_at - last_data_at) > 100)) {
        ++counters.timing_discontinuities; discardFifo(now, records); return false;
      }
      fifo_pending = records; last_arrival_at = snapshot_at;
    }
    buffered = fifo_pending < batch_samples ? fifo_pending : batch_samples;
    cursor = 0;
    fifo_pending -= buffered;
    if (!read(0x74, batch, uint8_t(buffered * fifo_packet_bytes))) {
      sampleFailed(); return false; // Partial FIFO consumption destroys alignment.
    }
    // Detect overflow during transfer before publishing any of this batch.
    if (!read(0x3A, &status, 1)) { sampleFailed(); return false; }
    if (status & 0x10) { ++counters.fifo_overflows; discardFifo(now); return false; }
    failed_reads = 0; wait_ms = motion_sample_ms;
    return emit();
  }

private:
  Diagnostics counters;
  GyroCalibration::Result correction;
  uint8_t batch[batch_samples * fifo_packet_bytes] = {};
  unsigned buffered = 0, cursor = 0, fifo_pending = 0;
  bool ready = false, started = false, fifo_started = false, reset_required = false;
  bool recovering = false, have_sample = false, time_adjusted = false;
  uint8_t quality = 0, failed_reads = 0;
  uint32_t discontinuity = 0, last_poll = 0, ready_at = 0, last_data_at = 0;
  uint32_t next_sample_at = 0, last_arrival_at = 0;
  uint32_t wait_ms = HealthNodeConfig::motion_sample_ms;

  static int16_t word(const uint8_t* p) { return int16_t((uint16_t(p[0]) << 8) | p[1]); }
  static bool saturated(int16_t value) { return value <= -32760 || value >= 32760; }
  void applyCalibration() {
    values = raw_values;
    values.gx -= correction.bias[0]; values.gy -= correction.bias[1]; values.gz -= correction.bias[2];
  }
  // Only complete packets reach this publisher.
  bool emit() {
    const uint8_t* data = batch + cursor++ * fifo_packet_bytes;
    raw_values = {word(data)/4096.0f, word(data+2)/4096.0f, word(data+4)/4096.0f,
                  word(data+8)/65.5f, word(data+10)/65.5f, word(data+12)/65.5f,
                  word(data+6)/340.0f + 36.53f};
    quality = EstimatedTime | (time_adjusted ? EstimatedTimeAdjusted : 0);
    for (unsigned i = 0; i < 3; ++i) {
      if (saturated(word(data + 2*i))) quality |= AccelSaturated;
      if (saturated(word(data + 8 + 2*i))) quality |= GyroSaturated;
    }
    if (quality & (AccelSaturated | GyroSaturated)) ++counters.saturation_samples;
    applyCalibration(); last_data_at = next_sample_at;
    next_sample_at += HealthNodeConfig::motion_sample_ms;
    valid = have_sample = true; ++counters.samples;
    if (recovering) { ++counters.recoveries; recovering = false; }
    return true;
  }
  // Generation survives diagnostic resets. Discard counts are lower bounds when
  // overflow or an unreadable FIFO makes the exact number of lost records unknown.
  void breakContinuity() {
    counters.discarded_samples += queuedSamples();
    buffered = cursor = fifo_pending = 0;
    valid = have_sample = time_adjusted = false; quality = 0; ++discontinuity;
  }
  bool configure(uint8_t reg, uint8_t value, uint8_t mask) {
    uint8_t actual;
    if (!write(reg, value) || !read(reg, &actual, 1)) return false;
    if ((actual & mask) == (value & mask)) return true;
    ++counters.config_mismatches; return false;
  }
  bool resetFifo() {
    ++counters.fifo_resets;
    // USER_CTRL.FIFO_RESET requires USER_CTRL.FIFO_EN=0; no I2C master is used.
    return configure(0x6A, 0x00, 0xF0) && write(0x6A, 0x04) &&
           configure(0x6A, 0x40, 0xF0);
  }
  void discardFifo(uint32_t now, unsigned known_records = 0) {
    counters.discarded_samples += known_records;
    breakContinuity(); recovering = true; reset_required = true; last_arrival_at = now;
    if (resetFifo()) reset_required = false;
    else sampleFailed();
  }
  void sampleFailed() {
    breakContinuity(); recovering = true; reset_required = true;
    wait_ms = HealthNodeConfig::motion_retry_ms;
    if (++failed_reads >= HealthNodeConfig::motion_fast_failures) {
      ready = false; ++counters.reinitializations;
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
