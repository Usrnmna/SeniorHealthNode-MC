#pragma once
#include <HealthNodeConfig.h>
#include <helpers/ui/FallButtonGesture.h>
#include "FallNotice.h"
#include "ResidualMovement.h"
#include "FallChannel.h"

// Volatile event/acknowledgement state. Construction on every reboot clears it.
class FallResponse {
public:
  // Returns true only when a new local alarm needs starting. An existing alarm
  // keeps its click progress; an older pending message keeps its retry budget.
  bool onFall(uint32_t now, bool pressed, const FallChannel& channel = FallChannel{}) {
    if (active) return false;
    active = true;
    last_response = FallNotice::None;
    incident_channel = channel;
    warned = false; started_at = now; ++incident;
    movement.reset();
    clicks.reset(now, pressed); // Never count clicks or a held press from before the fall.
    return true;
  }
  bool updateButton(uint32_t now, bool pressed) {
    if (!active) return false;
    const auto gesture = clicks.update(now, pressed);
    if (gesture == FallButtonGesture::None) return false;
    active = false;
    last_response = gesture == FallButtonGesture::FalseAlarm ? FallNotice::UserConfirmedFalseAlarm
        : (warned ? FallNotice::UserConfirmedFall : FallNotice::AssistanceRequested);
    request(last_response);
    return true; // Caller silences SOS immediately, irrespective of radio availability.
  }
  bool alarmActive() const { return active; }
  // The resolved incident is independent of an older response awaiting outbox space.
  bool falseAlarmConfirmed() const { return last_response == FallNotice::UserConfirmedFalseAlarm; }
  uint32_t incidentId() const { return incident; }
  // Call after updateButton(): a completed gesture wins a same-loop deadline.
  // A timeout requests a message but never acknowledges the user or silences SOS.
  void updateDeadline(uint32_t now) {
    if (!active || warned || uint32_t(now - started_at) < HealthNodeConfig::response_timeout_ms) return;
    warned = true;
    request(movement.quietAt(started_at + HealthNodeConfig::response_timeout_ms, now)
        ? FallNotice::NoResidualMovement : FallNotice::UserCouldNotConfirm);
  }
  // Evidence after the fixed deadline cannot retroactively strengthen a warning.
  void observeMotion(uint32_t now, float ax, float ay, float az, float rotation_dps) {
    if (active && !warned && uint32_t(now - started_at) <= HealthNodeConfig::response_timeout_ms)
      movement.observe(now, ax, ay, az, rotation_dps);
  }
  void motionUnavailable() { movement.invalidate(); }
  bool messagePending() const { return pending != FallNotice::None; }
  FallNotice pendingNotice() const { return pending; }
  uint32_t pendingIncident() const { return pending_incident; }
  const FallChannel& incidentChannel() const { return incident_channel; }
  const FallChannel& pendingChannel() const { return pending_channel; }
  // Clear only after the outbox has accepted ownership, not on an older TX callback.
  void messageAccepted() { pending = FallNotice::None; }
private:
  FallButtonGesture clicks{HealthNodeConfig::click_window_ms, HealthNodeConfig::false_alarm_click_window_ms,
                           HealthNodeConfig::long_press_ms, HealthNodeConfig::debounce_ms};
  // One unscheduled event; retain a confirmation if delivery backpressure lasts
  // across another incident. Ordinary repeated requests may coalesce here.
  void request(FallNotice notice) {
    if (isUserConfirmation(pending)) return;
    pending = notice; pending_incident = incident;
    pending_channel = incident_channel;
  }
  ResidualMovement movement;
  FallChannel incident_channel, pending_channel;
  FallNotice pending = FallNotice::None;
  FallNotice last_response = FallNotice::None;
  uint32_t started_at = 0, incident = 0, pending_incident = 0;
  bool active = false, warned = false;
};
