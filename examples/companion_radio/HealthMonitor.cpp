#include "MyMesh.h"
#if ENV_INCLUDE_MPU6050
#include <HealthNodeConfig.h>
#include "health/MotionRule.h"
#include "health/FallResponse.h"
#include "health/FallAckSender.h"
#include "health/FallAckLocation.h"
#include "health/MotionHealth.h"
#include <initializer_list>
#include "health/LowBatteryAlert.h"
#include "health/FallChannelStore.h"
#include "health/GyroCalibrationStore.h"
#include <helpers/ui/AlarmBuzzer.h>

AlarmBuzzer alarm_buzzer; // Include AlarmBuzzer.h elsewhere to call alarm_buzzer.beep().
static_assert(HealthNodeConfig::buzzer_pin != ENV_PIN_SDA &&
              HealthNodeConfig::buzzer_pin != ENV_PIN_SCL &&
              HealthNodeConfig::buzzer_pin != PIN_USER_BTN, "Buzzer pin conflicts with sensor/PRG");
static_assert(HealthNodeConfig::charging_status_pin < 0 ||
              (HealthNodeConfig::charging_status_pin != ENV_PIN_SDA &&
               HealthNodeConfig::charging_status_pin != ENV_PIN_SCL &&
               HealthNodeConfig::charging_status_pin != PIN_USER_BTN), "Charging pin conflicts with sensor/PRG");


void MyMesh::onPacketTxComplete(uint32_t tag, bool sent) {
  assistance.txComplete(millis(), tag, sent);
  fall_delivery.txComplete(millis(), tag, sent);
}
void MyMesh::reportAssistance() {
  // Serial only: no OLED/UI modifications or alterations to public text.
  if (reported_fall != fall_delivery.state()) {
    reported_fall = fall_delivery.state();
    Serial.printf("Fall channel: %s; attempts=%u\n", fall_delivery.status(), fall_delivery.attemptCount());
  } else if (reported_help != assistance.state()) {
    reported_help = assistance.state();
    Serial.printf("Assistance channel: %s; attempts=%u\n", assistance.status(), assistance.attemptCount());
  }
}
uint32_t MyMesh::uniqueTimestamp() { return getRTCClock()->getCurrentTimeUnique(); }
AssistanceTransport::Result MyMesh::queueAssistance(const uint8_t* data, unsigned len, uint32_t tag,
                                             uint8_t fingerprint[8], const FallChannel& channel) {
  return FallAckSender::queueGroup(*this, data, len, tag, fingerprint, channel);
}
void MyMesh::pollLowBattery() {
  using namespace HealthNodeConfig;
  board.pollBattery();
  if (!board.batteryReadingReady()) return; // Startup has no voltage yet, not 0 V.
  const uint32_t now = millis();
  const uint16_t battery_mv = board.getBattMilliVolts();
  const bool charging = charging_status_pin >= 0 &&
      digitalRead(charging_status_pin) == (charging_status_active_low ? LOW : HIGH);
  const bool due = battery_alert.update(now, battery_mv, charging);
  if (gyro_calibration.active()) { alarm_buzzer.stopLowBattery(); return; }
  if (!battery_alert.isActive()) alarm_buzzer.stopLowBattery(); // Never stop SOS.
  else if (due && alarm_buzzer.startLowBattery()) battery_alert.markPlayed(now);
}

void MyMesh::pollFallResponse() {
  const uint32_t now = millis();
  if (fall_response.updateButton(now, digitalRead(PIN_USER_BTN) == LOW)) {
    alarm_buzzer.stop();
    if (fall_response.falseAlarmConfirmed()) {
      const uint32_t incident = fall_response.incidentId();
      fall_delivery.supersedeIncident(*this, incident);
      fall_notices.cancelWarnings(incident, *this, assistance);
    }
    Serial.println("Fall acknowledged: SOS stopped; user response pending");
  }
  fall_response.updateDeadline(now); // Never stop SOS on a timeout or delivery outcome.
  if (fall_response.messagePending()) {
    double latitude, longitude;
    const bool fix = FallAckLocation::read(sensors, latitude, longitude);
    const auto notice = fall_response.pendingNotice();
    const auto message = FallNoticeMessage::build(notice, fix, latitude, longitude);
    if (fall_notices.enqueue(notice, fall_response.pendingIncident(), message, fall_response.pendingChannel())) {
      Serial.printf("Fall response: %s; parts=%u\n", fallNoticeText(notice), message.count);
      fall_response.messageAccepted(); // Outbox now owns the immutable event.
      reported_help = AssistanceDelivery::Idle;
    }
  }
  fall_notices.poll(now, *this, assistance);
  fall_delivery.poll(now, *this);
  reportAssistance();
  alarm_buzzer.loop();
}

void MyMesh::pollMotion() {
  const uint32_t now = millis();
  const bool fresh = sensors.motion.poll(now);
  motion_health.observe(now, sensors.motion.valid, fresh);
  const bool was_calibrating = gyro_calibration.active();
  gyro_calibration.tick(now, sensors.motion.valid);
  if (fresh) gyro_calibration.observe(now, sensors.motion.raw_values);
  if (gyro_calibration.state() != calibration_reported_state ||
      (gyro_calibration.active() && uint32_t(now - calibration_reported_at) >= 1000)) {
    char status[120]; gyro_calibration.describe(now, status, sizeof(status));
    Serial.printf("Calibration: %s; detection=%s\n", status,
        gyro_calibration.active() ? "paused" : "resuming-with-normal-rearm");
    calibration_reported_state = gyro_calibration.state(); calibration_reported_at = now;
  }
  if (motion_health.fault() != last_motion_fault) {
    last_motion_fault = motion_health.fault();
    Serial.println(last_motion_fault ? "Motion FAULT: no fresh valid samples; detection unavailable"
                                     : "Motion recovered: fresh samples; detector must rearm");
  }
  // Calibration is an explicit maintenance pause. Invalidate partial evidence on
  // entry and exit so handling or a coefficient change cannot complete a fall.
  if (was_calibrating || gyro_calibration.active()) {
    fall_response.motionUnavailable();
    motion_rule.update(now, 0, 0, false);
    return;
  }
  // Outside maintenance, sensor validity gates analysis independently of delivery.
  if (!sensors.motion.valid) {
    fall_response.motionUnavailable();
    motion_rule.update(now, 0, 0, false);
    return;
  }
  if (!fresh) return;
  const auto& v = sensors.motion.values;
  fall_response.observeMotion(now, v.ax, v.ay, v.az, v.rotation());
  if (motion_rule.update(now, v.acceleration(), v.rotation(), true)) {
    // Coalesce notifications without resetting SOS/button progress or cancelling
    // an earlier channel alert. MotionRule still enforces its normal cooldown.
    if (fall_response.onFall(now, digitalRead(PIN_USER_BTN) == LOW, fall_channel)) {
      alarm_buzzer.startSOS();
      fall_response.observeMotion(now, v.ax, v.ay, v.az, v.rotation());
    }
    char text[120];
    // Report the detected sequence's peaks, not the later quiet sample.
    snprintf(text, sizeof(text), "Possible fall: peak accel=%.2fg rotation=%.1fdeg/s",
             motion_rule.peakAcceleration(), motion_rule.peakRotation());
    uint8_t payload[FallAckMessage::max_payload_bytes];
    const unsigned len = FallAckMessage::encode(payload, 0, text);
    const bool accepted = fall_delivery.begin(now, *this, payload, len,
        fall_response.incidentChannel(), fall_response.incidentId());
    Serial.printf("%s (%s)\n", text, accepted ? "channel event accepted" : "coalesced with pending alert");
  }
}
bool MyMesh::handleHealthCommand(const char* command, char* reply) {
  if (strcmp(command, "calibrate") == 0 || strncmp(command, "calibrate ", 10) == 0) {
    controlCalibration(command[9] ? command + 10 : "start", reply);
    return true;
  }
  // USB aliases match the companion custom-variable interface.
  if (strcmp(command, "get calibrate") == 0) {
    controlCalibration("status", reply); return true;
  }
  if (strncmp(command, "set calibrate ", 14) == 0) {
    controlCalibration(command + 14, reply); return true;
  }
  if (strcmp(command, "get adc.multiplier") == 0) {
    snprintf(reply, 160, "%.4f", board.getAdcMultiplier());
    return true;
  }
  if (strncmp(command, "set adc.multiplier ", 19) == 0) {
    float value;
    if (!BatteryCalibration::parseMultiplier(command + 19, value)) {
      strcpy(reply, "Error: multiplier must be 0..10; 0 restores board default");
      return true;
    }
    const float previous = _prefs.adc_multiplier;
    _prefs.adc_multiplier = value;
    if (!_store->savePrefs(_prefs)) {
      _prefs.adc_multiplier = previous;
      strcpy(reply, "Error: calibration save failed; live multiplier unchanged");
      return true;
    }
    board.setAdcMultiplier(value);
    snprintf(reply, 160, "OK: saved multiplier %.4f; verify battery voltage with meter",
             board.getAdcMultiplier());
    return true;
  }
  if (strcmp(command, "battery") == 0) {
    if (!board.batteryReadingReady()) strcpy(reply, "Battery reading unavailable; wait for ADC sampling");
    else snprintf(reply, 160, "raw=%u; mV=%u; age_ms=%lu; multiplier=%.4f",
        unsigned(board.batteryRawAverage()), unsigned(board.getBattMilliVolts()),
        (unsigned long)board.batteryReadingAgeMs(millis()), board.getAdcMultiplier());
    return true;
  }
  if (strncmp(command, "battery calibrate ", 18) == 0) {
    uint16_t measured_mv;
    float suggested;
    using namespace HealthNodeConfig;
    if (!BatteryCalibration::parseMillivolts(command + 18, measured_mv) ||
        !board.batteryReadingReady() ||
        !BatteryCalibration::suggest(measured_mv, board.batteryRawAverage(),
            board.batteryReadingAgeMs(millis()), battery_calibration_max_age_ms,
            battery_calibration_min_mv, battery_calibration_max_mv, suggested)) {
      snprintf(reply, 160, "Error: use meter mV (%u..%u) with a fresh, nonsaturated battery reading",
               unsigned(battery_calibration_min_mv), unsigned(battery_calibration_max_mv));
    } else {
      snprintf(reply, 160, "Suggested: set adc.multiplier %.4f; not applied or saved", suggested);
    }
    return true;
  }
  if (strcmp(command, "get fall.channel") == 0) {
    fall_channel.describe(reply, 160);
    return true;
  }
  if (strncmp(command, "set fall.channel ", 17) == 0) {
    const auto result = setFallChannel(command + 17);
    if (result == 0) strcpy(reply, "OK: saved; applies to new fall incidents");
    else if (result == 1) strcpy(reply, "Error: use public #name or private Name <16/32-byte hex or Base64 key>");
    else strcpy(reply, "Error: could not save channel; previous selection retained");
    return true;
  }
  if (strcmp(command, "health") == 0) {
    snprintf(reply, 160, "fall=%s; help=%s; motion=%s; max_gap_ms=%lu; gaps=%lu; part_failed=%u",
        fall_delivery.status(), assistance.status(), gyro_calibration.active() ? "CALIBRATING" :
        (motion_health.fault() ? "FAULT" : "monitoring"),
        (unsigned long)motion_health.maxGapMs(), (unsigned long)motion_health.gapCount(),
        unsigned(fall_notices.partFailed()));
    return true;
  }
  if (strcmp(command, "beep") == 0) {
    if (gyro_calibration.active()) strcpy(reply, "Beep unavailable during calibration");
    else if (!alarm_buzzer.isEnabled()) strcpy(reply, "Buzzer disabled by buzzer_enabled=false");
    else strcpy(reply, alarm_buzzer.beep() ? "Beep started" : "Beep unavailable or alarm active");
    return true;
  }
  if (strcmp(command, "motion") == 0 || strcmp(command, "motion raw") == 0) {
    const auto& v = strcmp(command, "motion raw") == 0 ? sensors.motion.raw_values : sensors.motion.values;
    if (!sensors.motion.valid) strcpy(reply, "MPU6050 unavailable (SDA=4 SCL=6 addr=0x68)");
    else snprintf(reply, 160, "a[g]=%.3f,%.3f,%.3f gyro[dps]=%.1f,%.1f,%.1f temp[C]=%.1f",
                  v.ax, v.ay, v.az, v.gx, v.gy, v.gz, v.temperature);
    return true;
  }
  return false;
}

// Active alarms and queued incident traffic prevent maintenance from interrupting
// an unresolved response. Ordinary BLE and mesh traffic remain available.
bool MyMesh::calibrationIncidentBusy() const {
  return fall_response.alarmActive() || fall_response.messagePending() ||
      fall_delivery.busy() || assistance.busy() || fall_notices.busy();
}

// Shared USB/BLE transaction: 0 success, 1 rejected action, 2 persistence failure.
// Read-only inspection is available during collection and active fall incidents.
uint8_t MyMesh::controlCalibration(const char* action, char* reply) {
  const uint32_t now = millis();
  gyro_calibration.tick(now, sensors.motion.valid);
  if (strcmp(action, "status") == 0) {
    gyro_calibration.describe(now, reply, 160); return 0;
  }
  if (strcmp(action, "show") == 0 || strcmp(action, "show saved") == 0) {
    const bool pending = strcmp(action, "show") == 0 && gyro_calibration.pending();
    const auto& r = pending ? gyro_calibration.result() : sensors.motion.calibration();
    snprintf(reply, 160, "%s bias[dps]=%.3f/%.3f/%.3f noise=%.3f/%.3f/%.3f temp=%.2f span=%.2f n=%lu",
        pending ? "UNSAVED" : (r.samples ? "SAVED" : "DEFAULT"),
        r.bias[0], r.bias[1], r.bias[2], r.noise[0], r.noise[1], r.noise[2],
        r.temperature, r.temperature_span, (unsigned long)r.samples);
    return 0;
  }
  if (strcmp(action, "cancel") == 0) {
    gyro_calibration.cancel();
    motion_rule.update(now, 0, 0, false);
    strcpy(reply, "Cancelled; saved correction retained; detection resumes with normal rearm"); return 0;
  }
  if (strcmp(action, "start") && strcmp(action, "save") && strcmp(action, "reset")) {
    strcpy(reply, "Error: calibrate [start|status|show|show saved|save|cancel|reset]"); return 1;
  }
  if (calibrationIncidentBusy()) {
    strcpy(reply, "Error: fall/help incident active; calibration changes unavailable"); return 1;
  }
  if (strcmp(action, "start") == 0) {
    if (!sensors.motion.valid || motion_health.fault() ||
        sensors.motion.sampleAgeMs(now) > HealthNodeConfig::gyro_calibration_max_gap_ms) {
      strcpy(reply, "Error: fresh healthy MPU6050 data required"); return 1;
    }
    if (!gyro_calibration.start(now)) {
      strcpy(reply, "Error: calibration active or unsaved; use status/save/cancel"); return 1;
    }
    alarm_buzzer.stop();
    motion_rule.update(now, 0, 0, false);
    strcpy(reply, "Started; detection paused; keep still for countdown, 15s measurement and validation"); return 0;
  }
  if (gyro_calibration.active()) {
    strcpy(reply, "Error: calibration active; wait or cancel first"); return 1;
  }
  const bool reset = strcmp(action, "reset") == 0;
  if (!reset && !gyro_calibration.pending()) {
    strcpy(reply, "Error: no validated pending calibration"); return 1;
  }
  const auto candidate = reset ? GyroCalibration::Result{} : gyro_calibration.result();
  if (!GyroCalibrationStore::save(*_store->getPrimaryFS(), candidate)) {
    strcpy(reply, "Error: calibration save failed; live correction unchanged"); return 2;
  }
  sensors.motion.setCalibration(candidate);
  if (reset) gyro_calibration.reset(); else gyro_calibration.saved();
  motion_rule.update(now, 0, 0, false);
  strcpy(reply, reset ? "Default zero correction saved; detector must rearm"
                      : "Validated correction saved and applied; detector must rearm");
  return 0;
}

// Both USB text commands and BLE custom-variable writes use this transaction.
// Return 0 on success, 1 for invalid input, 2 for a filesystem failure.
uint8_t MyMesh::setFallChannel(const char* value) {
  FallChannel candidate;
  if (!FallChannel::parse(value, candidate)) return 1;
  if (!FallChannelStore::save(*_store->getPrimaryFS(), candidate)) return 2;
  fall_channel = candidate;
  return 0;
}

#endif
