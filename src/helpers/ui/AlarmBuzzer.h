#pragma once
#include <Arduino.h>
#include <HealthNodeConfig.h>
#include "BuzzerPattern.h"

// Shared nonblocking passive-buzzer API for the heltec_v4_sensor firmware.
// One instance is defined in main.cpp. Do not share its LEDC channel with tone().
class AlarmBuzzer {
public:
  bool begin() {
    using namespace HealthNodeConfig;
    digitalWrite(buzzer_pin, LOW);
    pinMode(buzzer_pin, OUTPUT);
    ready = ledcSetup(buzzer_pwm_channel, buzzer_frequency_hz, 8) > 0;
    if (ready) {
      ledcWrite(buzzer_pwm_channel, 0);
      ledcAttachPin(buzzer_pin, buzzer_pwm_channel);
    }
    pattern.stop(); applied = false; applied_frequency = buzzer_frequency_hz;
    return ready;
  }
  bool beep(uint32_t duration_ms = HealthNodeConfig::beep_ms) {
    if (!ready || !pattern.beep(millis(), duration_ms)) return false;
    apply(); return true;
  }
  void startSOS() { pattern.startSOS(millis()); apply(); }
  bool startLowBattery() {
    if (!ready || !pattern.startLowBattery(millis())) return false;
    apply(); return true;
  }
  void stopLowBattery() { pattern.stopLowBattery(); apply(); }
  void stop() { pattern.stop(); apply(); }
  void loop() { pattern.update(millis()); apply(); }
private:
  BuzzerPattern pattern{HealthNodeConfig::morse_dot_ms, HealthNodeConfig::sos_silence_ms};
  bool ready = false, applied = false;
  uint32_t applied_frequency = 0;
  void apply() {
    if (!ready) return;
    const uint32_t frequency = pattern.frequencyHz();
    if (applied == pattern.sounding() && applied_frequency == frequency) return;
    if (applied_frequency != frequency) {
      ledcWrite(HealthNodeConfig::buzzer_pwm_channel, 0);
      ledcSetup(HealthNodeConfig::buzzer_pwm_channel, frequency, 8);
      applied_frequency = frequency;
    }
    applied = pattern.sounding();
    ledcWrite(HealthNodeConfig::buzzer_pwm_channel, applied ? 128 : 0); // 50% duty or LOW.
  }
};

extern AlarmBuzzer alarm_buzzer;
