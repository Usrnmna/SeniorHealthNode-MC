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
    pending = true; // Coalesce requests; delivery retry state lives in AssistanceDelivery.
    return true; // Caller silences SOS immediately, irrespective of radio availability.
  }
  bool alarmActive() const { return active; }
  bool messagePending() const { return pending; }
  // Delivery controller owns end-to-end retries; clear only at a terminal outcome.
  void messageCompleted() { pending = false; }
private:
  TripleClick clicks{HealthNodeConfig::click_window_ms, HealthNodeConfig::long_press_ms,
                     HealthNodeConfig::debounce_ms};
  bool active = false, pending = false;
};
