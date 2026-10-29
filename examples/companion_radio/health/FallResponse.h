#pragma once
#include <HealthNodeConfig.h>
#include <helpers/ui/FallButtonGesture.h>
#include "FallNotice.h"
#include "ResidualMovement.h"
#include "FallChannel.h"

// Live response state. An explicitly restored incident starts with empty motion
// evidence; storage never restores a quiet window or a partially entered gesture.
class FallResponse {
  // The capture codec excludes channel names and keys; live ownership is unchanged.
  friend struct MotionCaptureCheckpoint;
public:
  // Returns true only when a new local alarm needs starting. An existing alarm
  // keeps its click progress; an older pending message keeps its retry budget.
  bool onFall(uint32_t now, bool pressed, const FallChannel& channel = FallChannel{}) {
    if (active) return false;
    active = true;
    last_response = FallNotice::None;
    incident_channel = channel;
    warned = quiet_reported = deadline_snapshot = false; started_at = now;
    deferred_initial = FallNotice::None;
    nextIncident();
    movement.reset();
    clicks.reset(now, pressed); // Never count clicks or a held press from before the fall.
    return true;
  }
  // Seed the identifier after loading a resolved/expired record. Pending notices
  // remain owned by their original incident; normal new events skip their ID.
  void seedIncident(uint32_t previous) {
    if (!active && previous && (!incident || newer(previous, incident))) incident = previous;
  }
  // Resume only the explicit unresolved record selected by storage policy. Age
  // is capped at the first warning deadline; fresh samples establish later quiet.
  bool restore(uint32_t now, uint32_t elapsed_ms, uint32_t saved_incident,
               const FallChannel& channel, bool pressed = false) {
    if (active || !saved_incident || !channel.valid() ||
        (pending != FallNotice::None && pending_incident == saved_incident) ||
        (incident && saved_incident != incident && !newer(saved_incident, incident))) return false;
    incident = saved_incident; incident_channel = channel;
    active = true; warned = quiet_reported = deadline_snapshot = false;
    deferred_initial = FallNotice::None;
    last_response = FallNotice::None;
    const uint32_t age = elapsed_ms < HealthNodeConfig::response_timeout_ms ? elapsed_ms :
        HealthNodeConfig::response_timeout_ms;
    started_at = now - age;
    movement.reset(); deadline_movement.reset(); clicks.reset(now, pressed);
    return true;
  }
  bool updateButton(uint32_t now, bool pressed) {
    if (!active) return false;
    const auto gesture = clicks.update(now, pressed);
    if (gesture == FallButtonGesture::None) return false;
    active = false;
    deferred_initial = FallNotice::None;
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
    if (!active) return;
    if (!warned) {
      if (uint32_t(now - started_at) < HealthNodeConfig::response_timeout_ms) return;
      warned = true;
      const auto& initial = deadline_snapshot ? deadline_movement : movement;
      quiet_reported = initial.quietAt(started_at + HealthNodeConfig::response_timeout_ms, now);
      deferred_initial = quiet_reported ? FallNotice::NoResidualMovement : FallNotice::UserCouldNotConfirm;
    }
    // An earlier incident may still own the pending slot. Freeze this incident's
    // first assessment at its deadline and retain it until that owner is accepted.
    if (deferred_initial != FallNotice::None) {
      if (pending == FallNotice::None) {
        request(deferred_initial); deferred_initial = FallNotice::None;
      }
      return;
    }
    // One subsequent upgrade is sufficient. Never replace an unscheduled warning
    // or user confirmation: outbox acceptance precedes a changed assessment.
    if (!quiet_reported && pending == FallNotice::None && movement.quietAt(now, now)) {
      quiet_reported = true; request(FallNotice::NoResidualMovement);
    }
  }
  // Preserve the evidence at the original deadline before accepting later data.
  // Late quiet may justify a separate update, never a retroactive initial claim.
  void observeMotion(uint32_t now, float ax, float ay, float az, float rotation_dps) {
    if (!active) return;
    // FIFO samples captured before declaration must not enter this incident.
    if (uint32_t(now - started_at) >= 0x80000000u) return;
    if (!warned && !deadline_snapshot && uint32_t(now - started_at) > HealthNodeConfig::response_timeout_ms) {
      deadline_movement = movement; deadline_snapshot = true;
    }
    movement.observe(now, ax, ay, az, rotation_dps);
  }
  void motionUnavailable() { movement.invalidate(); deadline_movement.invalidate(); }
  bool messagePending() const { return pending != FallNotice::None; }
  FallNotice pendingNotice() const { return pending; }
  uint32_t pendingIncident() const { return pending_incident; }
  const FallChannel& incidentChannel() const { return incident_channel; }
  const FallChannel& pendingChannel() const { return pending_channel; }
  uint32_t droppedConfirmations() const { return dropped_confirmations; }
  // Clear only after the outbox has accepted ownership, not on an older TX callback.
  void messageAccepted() { pending = FallNotice::None; }
private:
  static bool newer(uint32_t a, uint32_t b) { return a != b && uint32_t(a - b) < 0x80000000u; }
  void nextIncident() {
    do { ++incident; } while (!incident || (pending != FallNotice::None && incident == pending_incident));
  }
  FallButtonGesture clicks{HealthNodeConfig::click_window_ms, HealthNodeConfig::false_alarm_click_window_ms,
                           HealthNodeConfig::long_press_ms, HealthNodeConfig::debounce_ms};
  // One unscheduled event; retain any explicit PRG response if delivery
  // backpressure lasts across another incident. Automatic warnings may coalesce.
  void request(FallNotice notice) {
    if (isExplicitResponse(pending)) {
      // Local resolution still takes effect when bounded delivery storage fills.
      // Preserve the older owner and count every omitted newer PRG response.
      if (isExplicitResponse(notice) && pending_incident != incident) ++dropped_confirmations;
      return;
    }
    pending = notice; pending_incident = incident;
    pending_channel = incident_channel;
  }
  ResidualMovement movement, deadline_movement;
  FallChannel incident_channel, pending_channel;
  FallNotice pending = FallNotice::None;
  FallNotice deferred_initial = FallNotice::None;
  FallNotice last_response = FallNotice::None;
  uint32_t started_at = 0, incident = 0, pending_incident = 0;
  uint32_t dropped_confirmations = 0;
  bool active = false, warned = false, quiet_reported = false, deadline_snapshot = false;
};
