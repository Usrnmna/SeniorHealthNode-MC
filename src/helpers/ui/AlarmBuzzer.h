#pragma once
#include <Arduino.h>
#include <HealthNodeConfig.h>
#include "BuzzerPattern.h"

// Shared nonblocking passive-buzzer API for the heltec_v4_companion_radio_ble firmware.
// One instance is defined in HealthMonitor.cpp. Do not share its LEDC channel with tone().
class AlarmBuzzer {
public:
  explicit AlarmBuzzer(bool active_low = HealthNodeConfig::buzzer_active_low,
                       bool enabled = HealthNodeConfig::buzzer_enabled)
    : enabled(enabled), active_low(active_low) {}

  // Reports compiled output selection, not electrical detection of a connected module.
  bool isEnabled() const { return enabled; }

  // Establish the driver's inactive level before enabling GPIO output or attaching PWM.
  // Failed PWM initialization leaves the pin inactive and ordinary beeps unavailable.
  // Disabled output is a successful no-op; no GPIO or LEDC resources are configured.
  bool begin() {
    using namespace HealthNodeConfig;
    pattern.stop(); ready = false; applied = false; applied_frequency = buzzer_frequency_hz;
    if (!enabled) return true;
    digitalWrite(buzzer_pin, active_low ? HIGH : LOW);
    pinMode(buzzer_pin, OUTPUT);
    ready = ledcSetup(buzzer_pwm_channel, buzzer_frequency_hz, 8) > 0;
    if (ready) {
      ledcWrite(buzzer_pwm_channel, silentDuty());
      ledcAttachPin(buzzer_pin, buzzer_pwm_channel);
    }
    return ready;
  }
  bool beep(uint32_t duration_ms = HealthNodeConfig::beep_ms) {
    if (!ready || !pattern.beep(millis(), duration_ms)) return false;
    apply(); return true;
  }
  void startSOS() { if (ready) { pattern.startSOS(millis()); apply(); } }
  bool startLowBattery() {
    if (!ready || !pattern.startLowBattery(millis())) return false;
    apply(); return true;
  }
  void stopLowBattery() { pattern.stopLowBattery(); apply(); }
  void stop() { pattern.stop(); apply(); }
  void loop() { if (ready) { pattern.update(millis()); apply(); } }
private:
  const bool enabled;
  const bool active_low;
  BuzzerPattern pattern{HealthNodeConfig::morse_dot_ms, HealthNodeConfig::sos_silence_ms};
  bool ready = false, applied = false;
  uint32_t applied_frequency = 0;
  // Arduino ESP32 2.x maps 255 at 8-bit resolution to fully HIGH, with no LOW pulse.
  uint32_t silentDuty() const { return active_low ? 255 : 0; }

  // Both polarities use 50% PWM for sound; only the inactive output level changes.
  void apply() {
    if (!ready) return;
    const uint32_t frequency = pattern.frequencyHz();
    if (applied == pattern.sounding() && applied_frequency == frequency) return;
    if (applied_frequency != frequency) {
      ledcWrite(HealthNodeConfig::buzzer_pwm_channel, silentDuty());
      ledcSetup(HealthNodeConfig::buzzer_pwm_channel, frequency, 8);
      applied_frequency = frequency;
    }
    applied = pattern.sounding();
    ledcWrite(HealthNodeConfig::buzzer_pwm_channel, applied ? 128 : silentDuty());
  }
};

extern AlarmBuzzer alarm_buzzer;
