#pragma once
#include <HealthNodeConfig.h>
#include <helpers/ui/TripleClick.h>

// Volatile event/acknowledgement state. Construction on every reboot clears it.
class FallResponse {
public:
  // Returns true only when a new local alarm needs starting. An existing alarm
  // keeps its click progress; an older pending message keeps its retry budget.
  bool onFall(uint32_t now, bool pressed) {
    if (active) return false;
    active = true;
    clicks.reset(now, pressed); // Never count clicks or a held press from before the fall.
    return true;
  }
  bool updateButton(uint32_t now, bool pressed) {
    if (!active || !clicks.update(now, pressed)) return false;
    active = false;
    if (!pending) { pending = true; attempts = 0; } // Coalesce outstanding assistance requests.
    return true; // Caller silences SOS immediately, irrespective of radio availability.
  }
  bool alarmActive() const { return active; }
  bool messagePending() const { return pending; }
  bool messageDue(uint32_t now) const {
    return pending && (attempts == 0 || uint32_t(now - attempted_at) >= HealthNodeConfig::ack_retry_ms);
  }
  void messageAttempted(uint32_t now, bool queued) {
    attempted_at = now;
    if (++attempts >= HealthNodeConfig::ack_max_attempts || queued) pending = false;
  }
private:
  TripleClick clicks{HealthNodeConfig::click_window_ms, HealthNodeConfig::long_press_ms,
                     HealthNodeConfig::debounce_ms};
  bool active = false, pending = false;
  uint8_t attempts = 0;
  uint32_t attempted_at = 0;
};
