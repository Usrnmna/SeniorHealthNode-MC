#pragma once
#include <stdint.h>

// Millisecond-resolution service measurements. No sample clock is inferred from
// these counters, and no timer/interrupt or sample-rate output is introduced.
struct MotionServiceTiming {
  uint64_t polls = 0, busy_ms = 0, samples = 0, sample_delay_ms = 0;
  uint64_t persistence_calls = 0, persistence_ms = 0;
  uint32_t max_service_gap_ms = 0, max_poll_ms = 0, max_sample_delay_ms = 0;
  uint32_t max_persistence_ms = 0;
  void begin(uint32_t now) {
    if (polls) maximum(max_service_gap_ms, uint32_t(now - previous_entry));
    previous_entry = entry = now; ++polls;
  }
  void finish(uint32_t now) {
    const uint32_t duration = now - entry;
    busy_ms += duration; maximum(max_poll_ms, duration);
  }
  void sample(uint32_t captured, uint32_t processed) {
    const uint32_t delay = processed - captured;
    ++samples; sample_delay_ms += delay; maximum(max_sample_delay_ms, delay);
  }
  void persistence(uint32_t duration) {
    ++persistence_calls; persistence_ms += duration; maximum(max_persistence_ms, duration);
  }
private:
  uint32_t previous_entry = 0, entry = 0;
  static void maximum(uint32_t& current, uint32_t next) { if (next > current) current = next; }
};
