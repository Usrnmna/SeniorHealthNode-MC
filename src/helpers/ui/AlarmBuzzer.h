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

  // Establish the driver's inactive level before enabling GPIO output. PWM is
  // connected only during a tone, matching tone/noTone operation without delays.
  // Failed PWM initialization leaves the pin inactive and ordinary beeps unavailable.
  // Disabled output is a successful no-op; no GPIO or LEDC resources are configured.
  bool begin() {
    using namespace HealthNodeConfig;
    silence();
    pattern.stop(); ready = false; applied_frequency = buzzer_frequency_hz;
    if (!enabled) return true;
    digitalWrite(buzzer_pin, active_low ? HIGH : LOW);
    pinMode(buzzer_pin, OUTPUT);
    ready = ledcSetup(buzzer_pwm_channel, buzzer_frequency_hz, 8) > 0;
    if (ready) {
      ledcWrite(buzzer_pwm_channel, silentDuty());
    }
    return ready;
  }
  bool beep(uint32_t duration_ms = HealthNodeConfig::beep_ms) {
    if (!ready || !pattern.beep(millis(), duration_ms)) return false;
    return apply();
  }
  void startSOS() { if (ready) { pattern.startSOS(millis()); apply(); } }
  bool startLowBattery() {
    if (!ready || !pattern.startLowBattery(millis())) return false;
    return apply();
  }
  void stopLowBattery() { pattern.stopLowBattery(); apply(); }
  void stop() { pattern.stop(); apply(); }
  void loop() { if (ready) { pattern.update(millis()); apply(); } }
private:
  const bool enabled;
  const bool active_low;
  BuzzerPattern pattern{HealthNodeConfig::morse_dot_ms, HealthNodeConfig::sos_silence_ms};
  bool ready = false, attached = false;
  uint32_t applied_frequency = 0;
  // Arduino ESP32 2.x maps 255 at 8-bit resolution to fully HIGH, with no LOW pulse.
  uint32_t silentDuty() const { return active_low ? 255 : 0; }

  // Stop edges, disconnect LEDC, and explicitly restore the GPIO's inactive
  // level. Compiled polarity selects this level independently of PWM pitch.
  void silence() {
    if (!attached) return;
    ledcWrite(HealthNodeConfig::buzzer_pwm_channel, silentDuty());
    ledcDetachPin(HealthNodeConfig::buzzer_pin);
    digitalWrite(HealthNodeConfig::buzzer_pin, active_low ? HIGH : LOW);
    attached = false;
  }

  // Both polarities use 50% PWM for sound. Configure only at pitch changes and
  // attach only at tone starts; idle loop calls perform no GPIO/PWM operations.
  bool apply() {
    if (!ready) return false;
    if (!pattern.sounding()) { silence(); return true; }
    const uint32_t frequency = pattern.frequencyHz();
    if (attached && applied_frequency == frequency) return true;
    silence();
    if (applied_frequency != frequency) {
      ledcWrite(HealthNodeConfig::buzzer_pwm_channel, silentDuty());
      if (ledcSetup(HealthNodeConfig::buzzer_pwm_channel, frequency, 8) == 0) {
        ready = false; pattern.stop(); return false;
      }
      applied_frequency = frequency;
    }
    ledcAttachPin(HealthNodeConfig::buzzer_pin, HealthNodeConfig::buzzer_pwm_channel);
    // Attach configures the channel from hardware duty readback, which may still
    // reflect the previous PWM cycle. Apply sounding duty after that configuration.
    ledcWrite(HealthNodeConfig::buzzer_pwm_channel, 128);
    attached = true;
    return true;
  }
};

extern AlarmBuzzer alarm_buzzer;
