#pragma once
#include <stdint.h>
#include <HealthNodeConfig.h>

// Pure timing logic shared by the GPIO driver and host tests. Call update often.
class BuzzerPattern {
public:
  BuzzerPattern(uint32_t dot_ms, uint32_t silence_ms) : dot(dot_ms), silence(silence_ms) {}
  bool beep(uint32_t now, uint32_t duration) {
    if (sos || low_battery) return false; // Preserve alarm sequences.
    on = duration != 0; running = on; started = now; interval = duration;
    return true;
  }
  void startSOS(uint32_t now) {
    if (sos) return; // Repeated events must not restart the current sequence.
    low_battery = false; // Fall alarms always preempt battery warnings.
    sos = running = on = true; element = 0; started = now; interval = dot;
  }
  bool startLowBattery(uint32_t now) {
    if (running) return false;
    low_battery = running = on = true; element = 0; started = now;
    interval = HealthNodeConfig::long_beep_ms;
    return true;
  }
  void stopLowBattery() { if (low_battery) stop(); }
  void stop() { sos = low_battery = running = on = false; }
  void update(uint32_t now) {
    if (!running || uint32_t(now - started) < interval) return;
    // Advance one phase per poll. A delayed loop lengthens a phase rather than
    // skipping audible marks or shortening the required five-second silence.
    started = now;
    if (low_battery) {
      if (on) {
        if (element == 2) { stop(); return; }
        on = false; interval = HealthNodeConfig::low_battery_gap_ms;
      } else {
        ++element; on = true; interval = HealthNodeConfig::long_beep_ms;
      }
      return;
    }
    if (!sos) { stop(); return; }
    if (on) {
      on = false;
      interval = element == 8 ? silence : ((element == 2 || element == 5) ? 3 * dot : dot);
    } else {
      element = (element + 1) % 9;
      on = true;
      interval = (element >= 3 && element <= 5) ? 3 * dot : dot;
    }
  }
  bool sounding() const { return on; }
  bool alarmActive() const { return sos; }
  uint32_t frequencyHz() const {
    return low_battery ? HealthNodeConfig::low_battery_tones_hz[element] : HealthNodeConfig::buzzer_frequency_hz;
  }
private:
  const uint32_t dot, silence;
  uint32_t started = 0, interval = 0;
  uint8_t element = 0;
  bool on = false, running = false, sos = false, low_battery = false;
};
