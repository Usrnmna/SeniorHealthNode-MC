#pragma once
#include <HealthNodeConfig.h>
#include <math.h>
#include <stdio.h>

// Nonblocking stationary bias estimation. Inputs must be fresh, uncorrected MPU6050
// readings in g, degrees/s and Celsius. Accelerometer correction is not performed.
class GyroCalibration {
public:
  enum State { Idle, Preparing, Measuring, Validating, Ready, Saved, Cancelled, Failed };
  struct Result {
    float bias[3] = {};
    float noise[3] = {};
    float temperature = 0;
    float temperature_span = 0;
    uint32_t samples = 0; // Zero denotes the default, uncalibrated correction.
  };

  State state() const { return phase; }
  bool active() const { return phase == Preparing || phase == Measuring || phase == Validating; }
  bool pending() const { return phase == Ready; }
  const Result& result() const { return candidate; }
  const char* reason() const { return detail; }
  uint32_t restarts() const { return restarted; }

  // Start only after the caller checks sensor freshness and incident inactivity.
  // Repeated starts cannot replace an active or unsaved attempt.
  bool start(uint32_t now) {
    if (active() || pending()) return false;
    phase = Preparing; started_at = phase_at = last_fresh = now;
    restarted = 0; stats = Statistics{}; candidate = Result{}; detail = "keep-still";
    return true;
  }
  void cancel() { phase = Cancelled; candidate = Result{}; stats = Statistics{}; detail = "cancelled"; }
  void saved() { phase = Saved; detail = "saved"; }
  void reset() { phase = Idle; candidate = Result{}; stats = Statistics{}; detail = "default"; }

  // Check timeout and freshness on every loop, not only when a sample arrives.
  void tick(uint32_t now, bool sensor_valid, bool buffered_samples = false) {
    using namespace HealthNodeConfig;
    if (pending() && uint32_t(now - phase_at) >= gyro_calibration_candidate_ms) fail("candidate-expired");
    if (!active()) return;
    if (!sensor_valid) fail("sensor-error");
    else if (uint32_t(now - started_at) >= gyro_calibration_timeout_ms) fail("timeout");
    // A bounded FIFO batch supplies chronological coverage through observe().
    // Its backlog may defer freshness expiry, but never the overall timeout.
    else if (!buffered_samples && uint32_t(now - last_fresh) > gyro_calibration_max_gap_ms) fail("sample-gap");
  }

  // Call once per accepted DATA_RDY sample. Variability restarts collection;
  // invalid data aborts instead of turning a sensor fault into an offset.
  template<class Reading> void observe(uint32_t now, const Reading& v) {
    using namespace HealthNodeConfig;
    // FIFO samples captured before a start/restart cannot enter the new window.
    if (active() && uint32_t(now - last_fresh) >= 0x80000000u) return;
    tick(now, true);
    if (!active()) return;
    last_fresh = now;
    const float x[] = {v.ax, v.ay, v.az, v.gx, v.gy, v.gz, v.temperature};
    for (float value : x) if (!isfinite(value)) { fail("invalid-sample"); return; }
    for (unsigned i = 3; i < 6; ++i)
      if (fabsf(x[i]) > gyro_calibration_max_bias_dps) { restart(now, "rotation-too-large"); return; }
    if (v.temperature < -40 || v.temperature > 85) { fail("temperature-range"); return; }
    const float gravity = sqrtf(v.ax*v.ax + v.ay*v.ay + v.az*v.az);
    if (fabsf(gravity - 1) > gyro_calibration_gravity_tolerance_g) {
      restart(now, "acceleration-not-stationary"); return;
    }
    if (phase == Preparing) {
      if (uint32_t(now - phase_at) < gyro_calibration_prepare_ms) return;
      phase = Measuring; stats = Statistics{};
    }
    if (!stats.count) phase_at = now;
    stats.add(x);
    for (unsigned i = 0; i < 6; ++i) {
      const float limit = i < 3 ? gyro_calibration_accel_span_g : gyro_calibration_gyro_span_dps;
      if (stats.span(i) > limit) { restart(now, "movement"); return; }
    }
    if (stats.span(6) > gyro_calibration_temperature_span_c) { restart(now, "temperature-drift"); return; }
    const bool validating = phase == Validating;
    const uint32_t duration = validating ? gyro_calibration_validate_ms : gyro_calibration_measure_ms;
    if (uint32_t(now - phase_at) < duration) return;
    const uint32_t minimum = validating ? gyro_calibration_validation_samples : gyro_calibration_min_samples;
    if (stats.count < minimum) { fail("too-few-samples"); return; }
    for (unsigned i = 0; i < 3; ++i) {
      if (stats.noise(i + 3) > gyro_calibration_noise_dps) { restart(now, "gyro-noise"); return; }
      if (validating && fabsf(stats.mean[i + 3] - candidate.bias[i]) > gyro_calibration_residual_dps) {
        restart(now, "validation-residual"); return;
      }
    }
    if (validating) {
      if (fabsf(stats.mean[6] - candidate.temperature) > gyro_calibration_temperature_span_c) {
        restart(now, "temperature-drift"); return;
      }
      phase = Ready; phase_at = now; detail = "validated-unsaved";
    } else {
      for (unsigned i = 0; i < 3; ++i) {
        candidate.bias[i] = stats.mean[i + 3]; candidate.noise[i] = stats.noise(i + 3);
      }
      candidate.temperature = stats.mean[6]; candidate.temperature_span = stats.span(6);
      candidate.samples = stats.count;
      phase = Validating; phase_at = now; stats = Statistics{}; detail = "checking-residual";
    }
  }

  const char* name() const {
    switch (phase) {
      case Preparing: return "preparing"; case Measuring: return "measuring";
      case Validating: return "validating"; case Ready: return "ready-unsaved";
      case Saved: return "saved"; case Cancelled: return "cancelled";
      case Failed: return "failed"; default: return "idle";
    }
  }
  // No commas or colons: compatible with companion custom-variable framing.
  void describe(uint32_t now, char* out, size_t size) const {
    snprintf(out, size, "%s %lus n=%lu retry=%lu %s", name(),
        (unsigned long)(active() ? uint32_t(now - phase_at)/1000 : 0),
        (unsigned long)stats.count, (unsigned long)restarted, detail);
  }
  static bool validResult(const Result& r) {
    using namespace HealthNodeConfig;
    for (unsigned i = 0; i < 3; ++i) {
      if (!isfinite(r.bias[i]) || fabsf(r.bias[i]) > gyro_calibration_max_bias_dps ||
          !isfinite(r.noise[i]) || r.noise[i] < 0 || r.noise[i] > gyro_calibration_noise_dps) return false;
      if (!r.samples && (r.bias[i] != 0 || r.noise[i] != 0)) return false;
    }
    return isfinite(r.temperature) && r.temperature >= -40 && r.temperature <= 85 &&
        isfinite(r.temperature_span) && r.temperature_span >= 0 &&
        r.temperature_span <= gyro_calibration_temperature_span_c &&
        (!r.samples || (r.samples >= gyro_calibration_min_samples &&
                       r.samples <= gyro_calibration_timeout_ms / HealthNodeConfig::motion_sample_ms + 1));
  }

private:
  // Welford accumulation bounds memory use and avoids cancellation in variance.
  struct Statistics {
    uint32_t count = 0;
    float mean[7] = {}, m2[7] = {}, minimum[7] = {}, maximum[7] = {};
    void add(const float* x) {
      ++count;
      for (unsigned i = 0; i < 7; ++i) {
        const float delta = x[i] - mean[i]; mean[i] += delta / count;
        m2[i] += delta * (x[i] - mean[i]);
        if (count == 1 || x[i] < minimum[i]) minimum[i] = x[i];
        if (count == 1 || x[i] > maximum[i]) maximum[i] = x[i];
      }
    }
    float span(unsigned i) const { return maximum[i] - minimum[i]; }
    float noise(unsigned i) const { return count > 1 ? sqrtf(fmaxf(0, m2[i] / (count - 1))) : 0; }
  } stats;
  State phase = Idle;
  Result candidate;
  uint32_t started_at = 0, phase_at = 0, last_fresh = 0, restarted = 0;
  const char* detail = "default";
  void fail(const char* why) { phase = Failed; candidate = Result{}; detail = why; }
  void restart(uint32_t now, const char* why) {
    phase = Preparing; phase_at = now; stats = Statistics{}; candidate = Result{};
    ++restarted; detail = why;
  }
};
