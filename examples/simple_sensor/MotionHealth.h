#pragma once
#include <stdint.h>

// Diagnostic only: never alters MotionRule evidence, thresholds, or sample timing.
// Caller supplies the detector's gap threshold and the prolonged outage timeout (ms).
class MotionHealth {
public:
  MotionHealth(uint32_t max_sample_gap_ms, uint32_t fault_timeout_ms)
      : _gap_threshold(max_sample_gap_ms), _fault_timeout(fault_timeout_ms) {}

  // Call every sensor poll, including failed reads and unchanged cached readings.
  // A valid cached reading is not fresh evidence and cannot clear an outage.
  void observe(uint32_t now, bool valid, bool fresh) {
    if (!_started) {
      _started = true;
      _last_fresh = now;  // Explicit flag makes millis()==0 a valid start time.
    }
    if (valid && fresh) {
      if (_have_fresh) {
        const uint32_t gap = uint32_t(now - _last_fresh);
        if (gap > _max_gap) _max_gap = gap;
        // Saturate the lifetime counter instead of wrapping to zero.
        if (gap > _gap_threshold && _gap_count != UINT32_MAX) ++_gap_count;
      }
      _last_fresh = now;
      _have_fresh = true;
      _fault = false;
    } else if (uint32_t(now - _last_fresh) >= _fault_timeout) {
      _fault = true;  // Latch until a fresh valid reading, even across clock wrap.
    }
  }

  bool fault() const { return _fault; }
  uint32_t maxGapMs() const { return _max_gap; }
  uint32_t gapCount() const { return _gap_count; }

private:
  const uint32_t _gap_threshold, _fault_timeout;
  uint32_t _last_fresh = 0, _max_gap = 0, _gap_count = 0;
  bool _started = false, _have_fresh = false, _fault = false;
};
