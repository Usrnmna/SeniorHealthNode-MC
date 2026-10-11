#pragma once
#include <stdint.h>
#include <math.h>

// Constant-memory online moments. Square roots are deferred until inspection.
// Float arithmetic avoids software double arithmetic in the ESP32 sample path.
struct SampleMoments {
  float minimum = 0, maximum = 0, mean = 0, m2 = 0;
  void add(float value, uint32_t count, float inverse_count) {
    if (count == 1) { minimum = maximum = mean = value; m2 = 0; return; }
    if (value < minimum) minimum = value;
    if (value > maximum) maximum = value;
    const float delta = value - mean;
    mean += delta * inverse_count;
    m2 += delta * (value - mean);
  }
  float standardDeviation(uint32_t count) const {
    return count > 1 ? sqrtf(fmaxf(0, m2 / (count - 1))) : 0;
  }
};

class MotionStatistics {
public:
  // Acceleration, corrected gyro, magnitudes, temperature, then uncorrected gyro.
  enum Field { Ax, Ay, Az, Gx, Gy, Gz, Acceleration, Rotation, Temperature,
               RawGx, RawGy, RawGz, RawRotation, FieldCount };
  SampleMoments fields[FieldCount];
  SampleMoments intervals;
  uint32_t samples = 0, interval_count = 0, gaps = 0, rejected = 0;
  uint32_t first_at = 0, last_at = 0, latest_interval = 0, reset_at = 0;
  uint64_t coverage_ms = 0;
  const char* reset_reason = "startup";

  // Reset diagnostic values only. Detector, sensor validity and alarms are external.
  void reset(uint32_t now, const char* reason) {
    *this = MotionStatistics{}; reset_at = now; reset_reason = reason;
  }

  // Call after fall processing, once per fresh reading. Cached/invalid samples
  // never enter the denominator. Recovery gaps remain visible between samples.
  template<class Reading> void observe(uint32_t now, const Reading& v, const Reading& raw,
                                       bool fresh, bool valid, uint32_t gap_limit) {
    if (!fresh || !valid || (samples && now == last_at)) return;
    observeWithMagnitudes(now, v, raw, v.acceleration(), v.rotation(), gap_limit);
  }

  // Fresh valid sample only. The normal detector path supplies its already
  // calculated magnitudes to avoid repeating those square roots for diagnostics.
  template<class Reading> void observeWithMagnitudes(uint32_t now, const Reading& v, const Reading& raw,
      float acceleration, float rotation, uint32_t gap_limit) {
    const float raw_rotation = v.gx == raw.gx && v.gy == raw.gy && v.gz == raw.gz ? rotation : raw.rotation();
    const float data[] = {v.ax, v.ay, v.az, v.gx, v.gy, v.gz, acceleration,
        rotation, v.temperature, raw.gx, raw.gy, raw.gz, raw_rotation};
    for (float value : data) if (!isfinite(value)) { ++rejected; return; }
    // Freeze a saturated accumulator rather than wrap a denominator to zero.
    if (samples == UINT32_MAX) return;
    if (samples) {
      latest_interval = uint32_t(now - last_at);
      coverage_ms += latest_interval;
      ++interval_count;
      intervals.add(float(latest_interval), interval_count, 1.0f / interval_count);
      if (latest_interval > gap_limit) ++gaps;
    } else first_at = now;
    last_at = now; ++samples;
    const float inverse_count = 1.0f / samples;
    for (unsigned i = 0; i < FieldCount; ++i) fields[i].add(data[i], samples, inverse_count);
  }
};
