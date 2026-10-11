#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

// Calibration for this board's existing raw 10-bit ADC conversion. It does not
// assume that the nominal ADC voltage is a calibrated physical voltage.
namespace BatteryCalibration {
constexpr float max_multiplier = 10.0f;
inline bool validMultiplier(float value) {
  return isfinite(value) && value >= 0.0f && value <= max_multiplier;
}

// Parse a complete decimal setting. Zero means the board default; reject NaN,
// infinity, negative numbers, overflow and trailing command text.
inline bool parseMultiplier(const char* text, float& value) {
  if (!text || !((*text >= '0' && *text <= '9') || *text == '.')) return false;
  for (const char* p = text; *p; ++p)
    if (!((*p >= '0' && *p <= '9') || *p == '.')) return false;
  char* end;
  const float candidate = strtof(text, &end);
  if (end == text || *end || !validMultiplier(candidate)) return false;
  value = candidate;
  return true;
}

// Use integer millivolts to avoid confusing volts with mV in a meter entry.
inline bool parseMillivolts(const char* text, uint16_t& value) {
  if (!text || !*text) return false;
  uint32_t candidate = 0;
  for (const char* p = text; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    candidate = candidate * 10 + unsigned(*p - '0');
    if (candidate > 65535) return false;
  }
  value = uint16_t(candidate);
  return true;
}

// Same arithmetic as the existing board driver, preserving saved multipliers.
// Invalid input reports unavailable (0), rather than overflowing the mV value.
inline uint16_t milliVolts(uint16_t raw, float multiplier) {
  if (raw > 1023 || !validMultiplier(multiplier)) return 0;
  return uint16_t((multiplier * (3.3 / 1024.0) * raw) * 1000);
}

// Suggest only: a human meter reading is mandatory. Reject old or saturated
// samples and implausible inputs; do not change settings or save anything here.
inline bool suggest(uint16_t measured_mv, uint16_t raw, uint32_t age_ms,
                    uint32_t max_age_ms, uint16_t min_mv, uint16_t max_mv,
                    float& multiplier) {
  if (raw == 0 || raw >= 1023 || age_ms > max_age_ms ||
      measured_mv < min_mv || measured_mv > max_mv) return false;
  const float candidate = measured_mv / ((3.3 / 1024.0) * raw * 1000);
  if (!validMultiplier(candidate) || candidate == 0) return false;
  multiplier = candidate;
  return true;
}
}
