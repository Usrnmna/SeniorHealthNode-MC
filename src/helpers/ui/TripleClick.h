#pragma once
#include <stdint.h>

// Debounced, nonblocking three-click gesture. Times are observed raw edge times,
// confirmed after debounce; held buttons at reset cannot count as a first click.
class TripleClick {
public:
  TripleClick(uint32_t window_ms, uint32_t long_ms, uint32_t debounce_ms)
    : window(window_ms), long_press(long_ms), debounce(debounce_ms) {}

  void reset(uint32_t now, bool pressed) {
    stable = candidate = pressed;
    ignore_release = pressed;
    edge_at = now;
    count = 0;
  }

  bool update(uint32_t now, bool pressed) {
    if (pressed != candidate) { candidate = pressed; edge_at = now; }
    // Expire even with no edges, so stale clicks cannot revive after millis wraps.
    // A release awaiting debounce is judged at its edge, not its confirmation.
    const uint32_t observed_at = candidate != stable ? edge_at : now;
    if (count && uint32_t(observed_at - first_down) > window) {
      count = 0; ignore_release = stable;
    }
    if (stable && candidate && uint32_t(now - down_at) >= long_press) {
      count = 0; ignore_release = true;
    }
    if (candidate == stable || uint32_t(now - edge_at) < debounce) return false;
    stable = candidate;
    if (stable) {
      if (count && uint32_t(edge_at - first_down) > window) count = 0;
      down_at = edge_at;
      if (!count) first_down = edge_at;
      return false;
    }
    if (ignore_release) { ignore_release = false; count = 0; return false; }
    if (uint32_t(edge_at - down_at) >= long_press || uint32_t(edge_at - first_down) > window) {
      count = 0;
      return false;
    }
    if (++count != 3) return false;
    count = 0;
    return true;
  }

private:
  const uint32_t window, long_press, debounce;
  uint32_t edge_at = 0, down_at = 0, first_down = 0;
  uint8_t count = 0;
  bool stable = false, candidate = false, ignore_release = false;
};
