// Host verification substitutes hardware and service boundaries, while compiling
// these same orchestration methods. Firmware always uses the real companion.
#ifdef HEALTH_MONITOR_HOST_HEADER
#include HEALTH_MONITOR_HOST_HEADER
#else
#include "MyMesh.h"
#endif
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
#include "health/IncidentWriteIO.h"
#include "health/IncidentReadIO.h"
#include <helpers/ui/AlarmBuzzer.h>

AlarmBuzzer alarm_buzzer; // Include AlarmBuzzer.h elsewhere to call alarm_buzzer.beep().

// Restore only a checked unresolved incident. No samples or quiet evidence survive
// reboot. Clock plausibility and recovery limits are reported independently.
void MyMesh::restoreFallIncident() {
  // The ESP32 fallback epoch and saved contact timestamps do not prove elapsed
  // time across power loss. Use bounded unknown-clock recovery on this target.
#ifdef HEALTH_MONITOR_HOST_HEADER
  HostIncidentReadIO io{*_store->getPrimaryFS()};
  FallIncidentStore::CheckedIncidentReader<HostIncidentReadIO> reader(io);
#elif defined(ESP32)
  FallIncidentStore::IncidentReadPosix io;
  FallIncidentStore::CheckedIncidentReader<FallIncidentStore::IncidentReadPosix> reader(io);
#else
  FallIncidentStore::ArduinoReader<FILESYSTEM> reader{*_store->getPrimaryFS()};
#endif
  const auto recovery = FallIncidentStore::loadWithReader(reader, getRTCClock()->getCurrentTime(), false);
  recovery_reads[0] = recovery.reads[0]; recovery_reads[1] = recovery.reads[1];
  recovery_outcome = recovery.outcome; durable_incident = recovery.value;
  fall_response.seedIncident(durable_incident.incident);
  if (recovery.shouldResume()) {
    fall_response.restore(millis(), recovery.elapsed_ms, durable_incident.incident,
        durable_incident.channel, digitalRead(PIN_USER_BTN)==LOW);
    alarm_buzzer.startSOS();
    FallIncidentStore::countRecovery(durable_incident);
    persistence_dirty = true;
  } else if (recovery.outcome == FallIncidentStore::Expired || recovery.outcome == FallIncidentStore::RecoveryLimit) {
    durable_incident.active=false; persistence_dirty=true;
  } else if (recovery.outcome == FallIncidentStore::Corrupt) persistence_fault=true;
}

// Live observation, independent of historical begin/recovery results. It neither
// mounts storage nor retries boot recovery after the device is already running.
int MyMesh::incidentStorageMounted() const {
#ifdef HEALTH_MONITOR_HOST_HEADER
  return _store->getPrimaryFS()->mounted;
#elif defined(ESP32)
  return FallIncidentStore::IncidentReadPosix{}.mounted();
#else
  return -1;
#endif
}

// Queue one small durable state transition. A newer resolution/open supersedes
// an unwritten transition; no sample-rate flash writes are performed.
void MyMesh::recordIncident(bool active) {
  if (active) {
    durable_incident = FallIncidentStore::Incident{};
    durable_incident.incident=fall_response.incidentId();
    durable_incident.origin_epoch=0; // No qualified continuous wall clock on this target.
    durable_incident.channel=fall_response.incidentChannel();
  }
  durable_incident.active=active; persistence_dirty=true; persistence_attempted=false;
}

void MyMesh::pollIncidentPersistence(uint32_t now) {
  if (!persistence_dirty || (persistence_attempted && uint32_t(now-persistence_retry_at)<5000)) return;
  persistence_attempted=true; persistence_retry_at=now;
  const uint32_t write_started=millis();
#ifdef HEALTH_MONITOR_HOST_HEADER
  persistence_result=saveHealthIncident(*_store->getPrimaryFS(),durable_incident);
#elif defined(ESP32)
  FallIncidentStore::IncidentStdio io;
  FallIncidentStore::CheckedIncidentWriter<FallIncidentStore::IncidentStdio> writer(io);
  FallIncidentStore::IncidentReadPosix read_io;
  FallIncidentStore::CheckedIncidentReader<FallIncidentStore::IncidentReadPosix> reader(read_io);
  persistence_result=FallIncidentStore::saveWithReader(reader,durable_incident,writer);
#else
  FallIncidentStore::ArduinoWriter<FILESYSTEM> writer{*_store->getPrimaryFS()};
  persistence_result=FallIncidentStore::saveChecked(*_store->getPrimaryFS(),durable_incident,writer);
#endif
  persistence_fault=!persistence_result.success();
  motion_timing.persistence(uint32_t(millis()-write_started));
  if (!persistence_fault) persistence_dirty=false;
  // Filesystem operations may stall; FIFO captures short stalls, and longer
  // interruptions are explicitly invalidated by the acquisition layer.
}
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
    usb_reply.append("Fall channel: %s; attempts=%u\n", fall_delivery.status(), fall_delivery.attemptCount());
  } else if (reported_help != assistance.state()) {
    reported_help = assistance.state();
    usb_reply.append("Assistance channel: %s; attempts=%u\n", assistance.status(), assistance.attemptCount());
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
  if (gyro_calibration.active()) { alarm_buzzer.stopLowBattery(); captureBuzzer(); return; }
  if (!battery_alert.isActive()) alarm_buzzer.stopLowBattery(); // Never stop SOS.
  else if (due && alarm_buzzer.startLowBattery()) battery_alert.markPlayed(now);
  captureBuzzer();
}

void MyMesh::pollFallResponse() {
  const uint32_t now = millis();
  const bool pressed=digitalRead(PIN_USER_BTN)==LOW;
  uint8_t response_before[160];
  MotionCapture::Writer before(response_before,sizeof(response_before));
  if (motion_recorder.active()) MotionCaptureCheckpoint::response(before,fall_response);
  const bool acknowledged=fall_response.updateButton(now,pressed);
  if (acknowledged) {
    alarm_buzzer.stop();
    captureBuzzer();
    recordIncident(false);
    if (fall_response.falseAlarmConfirmed()) {
      const uint32_t incident = fall_response.incidentId();
      fall_delivery.supersedeIncident(*this, incident);
      initial_alert.cancel(incident);
      fall_notices.cancelWarnings(incident, *this, assistance);
    }
    usb_reply.append("Fall acknowledged: SOS stopped; user response pending\n");
  }
  fall_response.updateDeadline(now); // Never stop SOS on a timeout or delivery outcome.
  if (motion_recorder.active()) {
    MotionCapture::Writer after(capture_buffer,sizeof(capture_buffer));
    MotionCaptureCheckpoint::response(after,fall_response);
    if (!before.ok() || before.size()!=after.size() || memcmp(response_before,capture_buffer,before.size()))
      captureResponse(MotionCapture::Service,now,pressed,acknowledged);
  }
  if (fall_response.messagePending()) {
    double latitude, longitude;
    const bool fix = FallAckLocation::read(sensors, latitude, longitude);
    const auto notice = fall_response.pendingNotice();
    const auto message = FallNoticeMessage::build(notice, fix, latitude, longitude);
    if (fall_notices.enqueue(notice, fall_response.pendingIncident(), message, fall_response.pendingChannel())) {
      usb_reply.append("Fall response: %s; parts=%u\n", fallNoticeText(notice), message.count);
      fall_response.messageAccepted(); // Outbox now owns the immutable event.
      captureResponse(MotionCapture::Accept,now);
      reported_help = AssistanceDelivery::Idle;
    }
  }
  fall_notices.poll(now, *this, assistance);
  fall_delivery.poll(now, *this);
  initial_alert.poll(now, *this, fall_delivery);
  pollIncidentPersistence(now);
  reportAssistance();
  alarm_buzzer.loop();
  captureBuzzer();
}

DetectorAvailability::Cause MyMesh::availabilityCause(uint32_t now) const {
  using A=DetectorAvailability;
  if (!motion_rule.configured()) return A::ConfigurationFault;
  if (gyro_calibration.active()) return A::Calibration;
  // Startup ends when fresh data arrives or the existing outage fault latches.
  if (!motion_seen) return motion_health.fault() ? A::SensorUnavailable : A::Startup;
  if (!sensors.motion.valid) return A::SensorUnavailable;
  if (sensors.motion.sampleAgeMs(now)>motion_rule.settings().max_sample_gap_ms) return A::Stale;
  if (motion_rule.currentState()==MotionRule::Recovering) return A::Recovery;
  if (motion_rule.currentState()==MotionRule::Refractory) return A::Refractory;
  return A::Available;
}

const char* MyMesh::motionAvailability(uint32_t now) const {
  const auto cause=availabilityCause(now);
  return cause==DetectorAvailability::Available ? motion_rule.stateName() : DetectorAvailability::name(cause);
}

void MyMesh::observeDetectorAvailability(uint32_t now) {
  detector_availability.observe(now, availabilityCause(now),
      sensors.motion.sampleAgeMs(now), motion_rule.settings().max_sample_gap_ms);
}

void MyMesh::pollMotion() {
  static_assert(HealthNodeConfig::motion_sample_ms == FallDetectionConfig::sample_period_ms,
      "Detector timing validation must match acquisition cadence");
  uint32_t now = millis();
  motion_timing.begin(now);
  observeDetectorAvailability(now);
  const bool was_calibrating = gyro_calibration.active();
  // At most two small FIFO transactions per normal batch; never drain without a bound.
  for (unsigned budget = 0; budget < 4; ++budget) {
    const bool fresh = sensors.motion.poll(millis());
    // I2C and its shared-bus mutex take wall time. Freshness, declaration and
    // calibration deadlines must use the time after the acquisition completes.
    now = millis();
    captureAcquisition();
    const auto generation = sensors.motion.discontinuityGeneration();
    if (generation != motion_generation) {
      motion_generation = generation;
      fall_response.motionUnavailable();
      motion_rule.update(now, 0, 0, false);
      captureInvalidation(now,now);
      gyro_calibration.tick(now, false);
      motion_trace.record(now, 0, 0, motion_rule.currentState(), MotionRule::SampleGap, 0x100, true);
    }
    motion_health.observe(now, sensors.motion.valid, fresh &&
        sensors.motion.sampleAgeMs(now) <= motion_rule.settings().max_sample_gap_ms);
    if (!sensors.motion.valid) {
      fall_response.motionUnavailable();
      motion_rule.update(now, 0, 0, false);
      // Keep the first invalid transition and reason clearing, then omit exact
      // no-ops while the stream remains invalid instead of flooding USB.
      if (!capture_invalid || capture_invalid_reason!=uint8_t(motion_rule.lastReason()))
        captureInvalidation(now,now);
      if (motion_rule.lastReason() != MotionRule::None)
        motion_trace.record(now, 0, 0, motion_rule.currentState(), motion_rule.lastReason(), 0x100, true);
      break;
    }
    if (!fresh) break;
    motion_seen=true;
    const uint32_t sample_at = sensors.motion.sampleTimeMs();
    motion_timing.sample(sample_at,now);
    const auto& v = sensors.motion.values;
    const auto& raw = sensors.motion.raw_values;
    gyro_calibration.observe(sample_at, raw);
    const float acceleration = v.acceleration(), rotation = v.rotation();
    const bool maintenance = was_calibrating || gyro_calibration.active();
    bool fired = false;
    if (maintenance) {
      fall_response.motionUnavailable(); motion_rule.update(sample_at, 0, 0, false);
    } else {
      fall_response.observeMotion(sample_at, v.ax, v.ay, v.az, rotation);
      fired = motion_rule.update(sample_at, v.ax, v.ay, v.az, rotation, true);
    }
    // Buffered samples carry current commanded buzzer state, with temporal uncertainty
    // flagged; no sample is discarded or classified as still because sound is active.
    const uint16_t quality = sensors.motion.sampleQuality() | (maintenance ? 0x200 : 0) |
        (sensors.motion.sampleAgeMs(now) > HealthNodeConfig::motion_sample_ms ? 0x400 : 0);
    motion_trace.observe(sample_at, raw, v, motion_rule.currentState(), motion_rule.lastReason(),
        quality, alarm_buzzer.sounding(), motion_rule.decision(),
        motion_rule.referenceValid(), motion_rule.referenceTimeMs(), motion_rule.settings().pretrigger_ms);
    captureSample(sample_at,now,maintenance,fired,quality);
    if (fired) {
      const bool pressed=digitalRead(PIN_USER_BTN)==LOW;
      const bool new_alarm=fall_response.onFall(now,pressed,fall_channel);
      captureResponse(MotionCapture::Fall,now,pressed,new_alarm);
      if (new_alarm) {
        alarm_buzzer.startSOS(); recordIncident(true);
        captureBuzzer();
      }
      char text[140];
      snprintf(text, sizeof(text), "Possible fall (%s): peak accel=%.2fg rotation=%.1fdeg/s",
          MotionRule::reasonName(motion_rule.eventKind()), motion_rule.peakAcceleration(), motion_rule.peakRotation());
      // An unresolved incident keeps its original deadline and delivery retry budget.
      // A completed initial delivery is not restarted by another event in that incident.
      initial_alert.enqueue(fall_response.incidentId(), fall_response.incidentChannel(), text);
    }
    motion_statistics.observeWithMagnitudes(sample_at, v, raw, acceleration, rotation,
        motion_rule.settings().max_sample_gap_ms);
    if (!sensors.motion.queuedSamples()) break;
  }
  // Expire only after processing available chronological coverage. Any remaining
  // backlog is bounded and invalidated on overflow/age by the acquisition layer.
  now = millis();
  gyro_calibration.tick(now, sensors.motion.valid, sensors.motion.queuedSamples()!=0);
  observeDetectorAvailability(now);
  if (gyro_calibration.state() != calibration_reported_state) {
    calibration_reported_state = gyro_calibration.state(); calibration_reported_at = now;
    motion_trace.record(now, 0, 0, motion_rule.currentState(), MotionRule::InvalidSample,
        gyro_calibration.active() ? 0x200 : 0);
  }
  last_motion_fault = motion_health.fault();
  captureCalibration();
  motion_timing.finish(millis());
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
    NodePrefs candidate(_prefs); candidate.adc_multiplier = value;
    if (!commitSettings(candidate, reply))
      snprintf(reply, 160, "OK: saved multiplier %.4f; verify battery voltage with meter", board.getAdcMultiplier());
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
    snprintf(reply, 160, "fall=%s; help=%s; sensor=%s; detector=%s; age_ms=%lu; gaps=%lu",
        fall_delivery.status(), assistance.status(), motion_health.fault() ? "FAULT" :
        (sensors.motion.valid ? "valid" : "unavailable"), motionAvailability(millis()),
        (unsigned long)sensors.motion.sampleAgeMs(millis()), (unsigned long)motion_health.gapCount());
    return true;
  }
  if (strcmp(command, "beep") == 0) {
    if (gyro_calibration.active()) strcpy(reply, "Beep unavailable during calibration");
    else if (!alarm_buzzer.isEnabled()) strcpy(reply, "Buzzer disabled by buzzer_enabled=false");
    else strcpy(reply, alarm_buzzer.beep() ? "Beep started" : "Beep unavailable or alarm active");
    captureBuzzer();
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
      fall_delivery.busy() || assistance.busy() || fall_notices.busy() || initial_alert.pending();
}

// Shared USB/BLE transaction: 0 success, 1 rejected action, 2 persistence failure.
// Read-only inspection is available during collection and active fall incidents.
uint8_t MyMesh::controlCalibration(const char* action, char* reply) {
  const uint32_t now = millis();
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
    captureInvalidation(now,millis(),1);captureCalibration();
    motion_trace.record(now,0,0,motion_rule.currentState(),MotionRule::InvalidSample,0x200,true);
    observeDetectorAvailability(now);
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
    captureBuzzer();
    motion_rule.update(now, 0, 0, false);
    captureInvalidation(now,millis(),1);captureCalibration();
    motion_trace.record(now,0,0,motion_rule.currentState(),MotionRule::InvalidSample,0x200,true);
    observeDetectorAvailability(now);
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
  motion_statistics.reset(now, "calibration-changed");
  if (reset) gyro_calibration.reset(); else gyro_calibration.saved();
  motion_rule.update(now, 0, 0, false);
  captureInvalidation(now,millis(),1);captureCalibration();
  motion_trace.record(now,0,0,motion_rule.currentState(),MotionRule::InvalidSample,0x200,true);
  observeDetectorAvailability(millis());
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
