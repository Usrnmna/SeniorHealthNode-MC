#pragma once
#include <stdint.h>

// Exclusive wall-time causes, observed at service boundaries. Freshness expiry
// splits an interval; a subsequently drained FIFO cannot rewrite past wall time.
class DetectorAvailability {
public:
  enum Cause : uint8_t { Available, Startup, ConfigurationFault, Calibration,
    SensorUnavailable, Stale, Recovery, Refractory, CauseCount };
  struct Totals {
    uint64_t ms[CauseCount] = {};
    uint64_t monitoredMs() const {
      uint64_t result = 0; for (auto value : ms) result += value; return result;
    }
    uint64_t unavailableMs() const { return monitoredMs() - ms[Available]; }
  };
  // Monitoring starts at the first call. Causes are selected by the companion:
  // configuration > calibration > startup before a latched sensor fault >
  // sensor unavailable > stale > state.
  void observe(uint32_t now, Cause cause, uint32_t sample_age, uint32_t max_age) {
    accumulated = totals(now);
    observed_at = now; started = true; current = cause;
    freshness_limited = cause == Available || cause == Recovery || cause == Refractory;
    fresh_for_ms = sample_age <= max_age ? max_age - sample_age : 0;
  }
  // Retain the small component-test interface; false denotes observed recovery.
  void observe(uint32_t now, bool eligible, uint32_t sample_age, uint32_t max_age) {
    observe(now, eligible ? Available : Recovery, sample_age, max_age);
    if (!eligible) freshness_limited = false;
  }
  Totals totals(uint32_t now) const {
    Totals result = accumulated;
    if (!started) return result;
    const uint32_t elapsed = uint32_t(now - observed_at);
    const uint32_t first = freshness_limited && elapsed > fresh_for_ms ? fresh_for_ms : elapsed;
    result.ms[current] += first;
    result.ms[Stale] += elapsed - first;
    return result;
  }
  uint64_t unavailableMs(uint32_t now) const { return totals(now).unavailableMs(); }
  static const char* name(Cause cause) {
    static const char* const names[] = {"available", "startup", "config-fault", "calibrating",
      "sensor-unavailable", "stale", "recovering", "refractory"};
    return cause < CauseCount ? names[cause] : "unknown";
  }
private:
  Totals accumulated;
  uint32_t observed_at = 0, fresh_for_ms = 0;
  Cause current = Startup;
  bool started = false, freshness_limited = false;
};
