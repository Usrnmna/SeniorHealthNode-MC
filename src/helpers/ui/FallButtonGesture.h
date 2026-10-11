#pragma once
#include <stdint.h>

// Nonblocking post-fall gestures, measured at observed edges after debounce.
// A fast double click waits for the full window; a third click takes priority.
class FallButtonGesture {
public:
  enum Result { None, ConfirmFall, FalseAlarm };
  FallButtonGesture(uint32_t window_ms, uint32_t double_ms, uint32_t long_ms, uint32_t debounce_ms)
    : window(window_ms), double_window(double_ms), long_press(long_ms), debounce(debounce_ms) {}

  void reset(uint32_t now, bool pressed) {
    stable = candidate = pressed;
    ignore_release = pressed;
    edge_at = now;
    count = 0;
    double_pending = false;
  }

  // A qualifying double is returned only after the inclusive third-release
  // deadline. A release observed on that deadline still gets its debounce time.
  // Any debounced third down cancels the double candidate, even if that press
  // subsequently proves too long/late: attempted confirmation must not cancel SOS.
  Result update(uint32_t now, bool pressed) {
    if (pressed != candidate) { candidate = pressed; edge_at = now; }
    // Expire even with no edges, so stale clicks cannot revive after millis wraps.
    // A release awaiting debounce is judged at its edge, not its confirmation.
    const uint32_t observed_at = candidate != stable ? edge_at : now;
    if (count && uint32_t(observed_at - first_down) > window) {
      const bool accept_double = double_pending && !stable && !candidate;
      count = 0; double_pending = false; ignore_release = stable;
      if (accept_double) return FalseAlarm;
    }
    if (stable && candidate && uint32_t(now - down_at) >= long_press) {
      count = 0; double_pending = false; ignore_release = true;
    }
    if (candidate == stable || uint32_t(now - edge_at) < debounce) return None;
    stable = candidate;
    if (stable) {
      double_pending = false;
      down_at = edge_at;
      if (!count) first_down = edge_at;
      return None;
    }
    if (ignore_release) { ignore_release = false; count = 0; return None; }
    if (uint32_t(edge_at - down_at) >= long_press || uint32_t(edge_at - first_down) > window) {
      count = 0; double_pending = false;
      return None;
    }
    ++count;
    if (count == 2) double_pending = uint32_t(edge_at - first_down) <= double_window;
    if (count != 3) return None;
    count = 0;
    return ConfirmFall;
  }

private:
  const uint32_t window, double_window, long_press, debounce;
  uint32_t edge_at = 0, down_at = 0, first_down = 0;
  uint8_t count = 0;
  bool stable = false, candidate = false, ignore_release = false;
  bool double_pending = false;
};
