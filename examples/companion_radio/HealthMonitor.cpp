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
                                             uint8_t fingerprint[8]) {
  return FallAckSender::queueGroup(*this, data, len, tag, fingerprint);
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
  if (!battery_alert.isActive()) alarm_buzzer.stopLowBattery(); // Never stop SOS.
  else if (due && alarm_buzzer.startLowBattery()) battery_alert.markPlayed(now);
}

void MyMesh::pollFallResponse() {
  const uint32_t now = millis();
  if (fall_response.updateButton(now, digitalRead(PIN_USER_BTN) == LOW)) {
    alarm_buzzer.stop();
    Serial.println("Fall acknowledged: SOS stopped; assistance delivery pending");
  }
  if (fall_response.messagePending() && !assistance_started) {
    double latitude, longitude;
    const bool fix = FallAckLocation::read(sensors, latitude, longitude);
    uint8_t payload[FallAckMessage::max_payload_bytes];
    const unsigned len = FallAckMessage::encodeWithLocation(payload, 0, getNodeName(),
        HealthNodeConfig::ack_message, fix, latitude, longitude, HealthNodeConfig::DefaultLocation);
    assistance.begin(now, *this, payload, len); // Freeze event text/location for consistent retries.
    assistance_started = true;
    reported_help = AssistanceDelivery::Idle;
  }
  assistance.poll(now, *this);
  fall_delivery.poll(now, *this);
  if (assistance_started && !assistance.busy()) {
    fall_response.messageCompleted(); // Terminal outcomes stay visible in assistance status.
    assistance_started = false;
  }
  reportAssistance();
  alarm_buzzer.loop();
}

void MyMesh::pollMotion() {
  const uint32_t now = millis();
  const bool fresh = sensors.motion.poll(now);
  motion_health.observe(now, sensors.motion.valid, fresh);
  if (motion_health.fault() != last_motion_fault) {
    last_motion_fault = motion_health.fault();
    Serial.println(last_motion_fault ? "Motion FAULT: no fresh valid samples; detection unavailable"
                                     : "Motion recovered: fresh samples; detector must rearm");
  }
  // Sensor validity alone gates analysis; notification state cannot blind it.
  if (!sensors.motion.valid) {
    motion_rule.update(now, 0, 0, false);
    return;
  }
  if (!fresh) return;
  const auto& v = sensors.motion.values;
  if (motion_rule.update(now, v.acceleration(), v.rotation(), true)) {
    // Coalesce notifications without resetting SOS/button progress or cancelling
    // an earlier public alert. MotionRule still enforces its normal cooldown.
    if (fall_response.onFall(now, digitalRead(PIN_USER_BTN) == LOW))
      alarm_buzzer.startSOS();
    char text[120];
    // Report the detected sequence's peaks, not the later quiet sample.
    snprintf(text, sizeof(text), "Possible fall: peak accel=%.2fg rotation=%.1fdeg/s",
             motion_rule.peakAcceleration(), motion_rule.peakRotation());
    uint8_t payload[FallAckMessage::max_payload_bytes];
    const unsigned len = FallAckMessage::encode(payload, 0, getNodeName(), text);
    const bool accepted = fall_delivery.begin(now, *this, payload, len);
    Serial.printf("%s (%s)\n", text, accepted ? "public channel pending" : "coalesced with pending public alert");
  }
}
bool MyMesh::handleHealthCommand(const char* command, char* reply) {
  if (strcmp(command, "health") == 0) {
    snprintf(reply, 160, "fall=%s; help=%s; motion=%s; max_gap_ms=%lu; gaps=%lu",
        fall_delivery.status(), assistance.status(), motion_health.fault() ? "FAULT" : "monitoring",
        (unsigned long)motion_health.maxGapMs(), (unsigned long)motion_health.gapCount());
    return true;
  }
  if (strcmp(command, "beep") == 0) {
    strcpy(reply, alarm_buzzer.beep() ? "Beep started" : "Beep unavailable or alarm active");
    return true;
  }
  if (strcmp(command, "motion") == 0) {
    const auto& v = sensors.motion.values;
    if (!sensors.motion.valid) strcpy(reply, "MPU6050 unavailable (SDA=4 SCL=6 addr=0x68)");
    else snprintf(reply, 160, "a[g]=%.3f,%.3f,%.3f gyro[dps]=%.1f,%.1f,%.1f temp[C]=%.1f",
                  v.ax, v.ay, v.az, v.gx, v.gy, v.gz, v.temperature);
    return true;
  }
  return false;
}

#endif
