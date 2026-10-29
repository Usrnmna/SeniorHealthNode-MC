#pragma once

// Test-only service boundaries for the production HealthMonitor.cpp translation
// unit. No detector, alarm-response, acquisition or delivery loop is copied here.
#include <Arduino.h>
#include <array>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <new>
#define ESP32
#define ENV_INCLUDE_MPU6050 1
#define ENV_PIN_SDA 4
#define ENV_PIN_SCL 6
#define PIN_USER_BTN 0
#define OUTPUT 1

// The generic Wire mock has a minimal diagnostic sink. Keep it for the driver;
// the host companion's USB boundary below also models a stalled consumer.
#define Serial SensorDriverSerial
#include <helpers/sensors/MPU6050.h>
#include <MpuFifoFake.h>
#undef Serial
#include <helpers/BatteryCalibration.h>
#include "../../examples/companion_radio/UsbReply.h"
#include "../../examples/companion_radio/health/MotionRule.h"
#include "../../examples/companion_radio/health/FallResponse.h"
#include "../../examples/companion_radio/health/LowBatteryAlert.h"
#include "../../examples/companion_radio/health/MotionHealth.h"
#include "../../examples/companion_radio/health/DetectorAvailability.h"
#include "../../examples/companion_radio/health/MotionStatistics.h"
#include "../../examples/companion_radio/health/MotionTrace.h"
#include "../../examples/companion_radio/health/MotionRecorder.h"
#include "../../examples/companion_radio/health/MotionCaptureFormat.h"
#include "../../examples/companion_radio/health/MotionServiceTiming.h"
#include "../../examples/companion_radio/health/BoundedUsbOutput.h"
#include "../../examples/companion_radio/health/FallInitialAlert.h"
#include "../../examples/companion_radio/health/FallIncidentStore.h"
#include "../../examples/companion_radio/health/FallNoticeOutbox.h"

inline bool host_pwm_attached = false;
inline unsigned host_pwm_starts = 0;
inline void digitalWrite(int, int) {}
inline uint32_t ledcSetup(uint8_t, uint32_t frequency, uint8_t) { return frequency; }
inline void ledcWrite(uint8_t, uint32_t) {}
inline void ledcAttachPin(uint8_t, uint8_t) { host_pwm_attached = true; ++host_pwm_starts; }
inline void ledcDetachPin(uint8_t) { host_pwm_attached = false; }

struct HostSerial {
  int capacity = 64;
  unsigned writes = 0, write_calls = 0;
  mutable unsigned readiness_queries = 0;
  std::string bytes;
  int availableForWrite() const { ++readiness_queries; return capacity; }
  size_t write(const uint8_t* data, size_t size) {
    ++write_calls;
    if (capacity <= 0) return 0;
    size = std::min(size, size_t(capacity));
    bytes.append(reinterpret_cast<const char*>(data), size); ++writes; return size;
  }
  void println(const char* text) { bytes += text; bytes += '\n'; }
  void printf(const char* format, ...) {
    char line[512]; va_list args; va_start(args, format);
    vsnprintf(line, sizeof(line), format, args); va_end(args); bytes += line;
  }
};
inline HostSerial Serial;

// The actual bounded USB adapter runs against a fixed host queue. Tests advance
// its worker explicitly; no host function simulates FreeRTOS scheduling or locks.
struct HostUsbQueue {
  std::array<BoundedUsbPacket, 8> packets{};
  unsigned head = 0, tail = 0, used = 0;
  bool initialized = true;
  bool ready() const { return initialized; }
  unsigned spaces() const { return initialized ? unsigned(packets.size()) - used : 0; }
  unsigned count() const { return used; }
  bool trySend(const BoundedUsbPacket& packet) {
    if (!spaces()) return false;
    packets[head] = packet; head = (head + 1) % packets.size(); ++used; return true;
  }
  bool receive(BoundedUsbPacket& packet) {
    if (!used) return false;
    packet = packets[tail]; tail = (tail + 1) % packets.size(); --used; return true;
  }
  uint32_t stackHighWaterBytes() const { return 0; } // No RTOS task exists on host.
};
inline HostUsbQueue host_usb_queue;
using HostBoundedUsbOutput = BoundedUsbOutput<HostUsbQueue>;
inline HostBoundedUsbOutput bounded_usb_output(host_usb_queue);
inline bool host_usb_initializes = true;
inline bool beginBoundedUsbOutput() {
  host_usb_queue.initialized = host_usb_initializes; return host_usb_initializes;
}
inline void resetHostUsb() {
  bounded_usb_output.~HostBoundedUsbOutput();
  host_usb_queue = HostUsbQueue{};
  new (&bounded_usb_output) HostBoundedUsbOutput(host_usb_queue);
  host_usb_initializes = true;
}
inline bool runHostUsbWorkerOnce() {
  BoundedUsbPacket packet;
  if (!host_usb_queue.receive(packet)) return false;
  bounded_usb_output.deliver(packet, Serial); return true;
}

// Immediate filesystem model for integration latency/failure checks. Physical
// durability and crash cuts are tested separately by the durability fault model.
struct HostFS {
  std::map<std::string, std::vector<uint8_t>> files;
  bool fail_open = false;
  uint32_t write_delay_ms = 0;
  unsigned writes = 0;
  struct File {
    HostFS* fs;
    std::vector<uint8_t>* bytes;
    explicit operator bool() const { return bytes != nullptr; }
    size_t size() const { return bytes->size(); }
    size_t read(uint8_t* destination, size_t count) {
      count = std::min(count, bytes->size()); memcpy(destination, bytes->data(), count); return count;
    }
    size_t write(const uint8_t* source, size_t count) {
      ++fs->writes; g_mock_millis += fs->write_delay_ms;
      bytes->assign(source, source + count); return count;
    }
    void flush() {}
    void close() {}
  };
  bool exists(const char* path) { return files.count(path) != 0; }
  File open(const char* path, const char* mode) {
    if (fail_open) return {this, nullptr};
    if (*mode == 'w') { files[path].clear(); return {this, &files[path]}; }
    const auto found = files.find(path);
    return {this, found == files.end() ? nullptr : &found->second};
  }
};
struct HostStore { HostFS fs; HostFS* getPrimaryFS() { return &fs; } };
struct HostClock {
  uint32_t timestamp = 1800000000;
  uint32_t getCurrentTime() const { return timestamp; }
  uint32_t getCurrentTimeUnique() { return ++timestamp; }
};
struct HostRng {
  void random(uint8_t* bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) bytes[i] = uint8_t(i + 1);
  }
};
struct HostLocation {
  bool isEnabled() const { return false; }
  void loop() {}
  bool isValid() const { return false; }
  int getLatitude() const { return 0; }
  int getLongitude() const { return 0; }
};
struct HostSensors {
  MPU6050Sensor motion;
  HostLocation* getLocationProvider() { return nullptr; }
};
inline HostSensors sensors;
struct HostBoard {
  void pollBattery() {}
  bool batteryReadingReady() const { return false; }
  uint16_t getBattMilliVolts() const { return 4000; }
  uint16_t batteryRawAverage() const { return 2000; }
  uint32_t batteryReadingAgeMs(uint32_t) const { return 0; }
  float getAdcMultiplier() const { return 4.9f; }
};
inline HostBoard board;
struct NodePrefs { float adc_multiplier = 0; };

class MyMesh : public AssistanceTransport {
public:
  // Production health state types, deliberately accessible for assertions.
  MotionRule motion_rule;
  FallInitialAlert initial_alert;
  FallIncidentStore::Incident durable_incident;
  FallIncidentStore::Outcome recovery_outcome = FallIncidentStore::Missing;
  uint32_t persistence_retry_at = 0;
  bool persistence_dirty = false, persistence_fault = false, persistence_attempted = false;
  FallResponse fall_response;
  FallChannel fall_channel;
  LowBatteryAlert battery_alert;
  AssistanceDelivery assistance{0}, fall_delivery{1};
  FallNoticeOutbox fall_notices;
  MotionHealth motion_health{FallDetectionConfig{}.max_sample_gap_ms, HealthNodeConfig::motion_fault_timeout_ms};
  MotionStatistics motion_statistics;
  MotionTrace motion_trace;
  uint32_t motion_generation = 0;
  DetectorAvailability detector_availability;
  MotionServiceTiming motion_timing;
  MotionRecorder motion_recorder;
  uint8_t capture_buffer[MotionRecorder::max_payload] = {};
  uint64_t capture_boot = 0;
  uint32_t capture_session = 0, capture_sample_sequence = 0, capture_calibration_id = 0;
  uint32_t capture_generation = 0, capture_discarded = 0, capture_overflows = 0, capture_discontinuities = 0;
  uint8_t capture_calibration_state = 255, capture_invalid_reason = 255;
  bool motion_seen = false, capture_buzzer = false, capture_invalid = false;
  bool last_motion_fault = false;
  GyroCalibration gyro_calibration;
  uint32_t calibration_reported_at = 0;
  GyroCalibration::State calibration_reported_state = GyroCalibration::Idle;
  AssistanceDelivery::State reported_help = AssistanceDelivery::Idle, reported_fall = AssistanceDelivery::Idle;
  UsbReply usb_reply;
  HostStore store;
  HostStore* _store = &store;
  HostClock clock;
  HostRng rng;
  NodePrefs _prefs;
  struct Attempt {
    uint32_t tag;
    std::vector<uint8_t> payload;
    std::array<uint8_t, 8> fingerprint;
    FallChannel channel;
    std::string text() const { return std::string(payload.begin() + 5, payload.end()); }
  };
  std::vector<Attempt> attempts;
  std::vector<uint32_t> cancellations;
  AssistanceTransport::Result queue_result = AssistanceTransport::Queued;
  HostClock* getRTCClock() { return &clock; }
  HostRng* getRNG() { return &rng; }
  uint8_t commitSettings(NodePrefs& next, char*) { _prefs = next; return 0; }
  void cancelAssistance(uint32_t tag) override {
    cancellations.push_back(tag); onPacketTxComplete(tag, false);
  }
  uint32_t uniqueTimestamp() override;
  Result queueAssistance(const uint8_t*, unsigned, uint32_t, uint8_t[8], const FallChannel&) override;
  void pollMotion();
  void pollFallResponse();
  void pollLowBattery();
  void restoreFallIncident();
  void pollIncidentPersistence(uint32_t);
  void recordIncident(bool);
  void onPacketTxComplete(uint32_t, bool);
  void reportAssistance();
  DetectorAvailability::Cause availabilityCause(uint32_t) const;
  bool handleRecordingCommand(const char*);
  void serviceMotionRecording();
  void captureInvalidation(uint32_t, uint32_t, uint8_t mask = 3);
  void captureSample(uint32_t, uint32_t, bool, bool, uint16_t);
  void captureResponse(uint8_t, uint32_t, bool pressed = false, bool result = false);
  void captureCalibration(bool force = false);
  void captureBuzzer(bool force = false);
  void captureAcquisition(bool force = false);
  const char* motionAvailability(uint32_t) const;
  void observeDetectorAvailability(uint32_t);
  bool handleHealthCommand(const char*, char*);
  bool calibrationIncidentBusy() const;
  uint8_t controlCalibration(const char*, char*);
  uint8_t setFallChannel(const char*);
  bool fallAlarmActive() const { return fall_response.alarmActive(); }
};

namespace FallAckSender {
// Exact-type overload replaces packet allocation/dispatch only. The production
// MyMesh::queueAssistance method and both delivery owners still run unchanged.
inline AssistanceTransport::Result queueGroup(MyMesh& network, const uint8_t* data,
    unsigned length, uint32_t tag, uint8_t fingerprint[8], const FallChannel& channel) {
  std::array<uint8_t, 8> hash{};
  memcpy(hash.data(), &tag, 4); memcpy(hash.data() + 4, &network.clock.timestamp, 4);
  memcpy(fingerprint, hash.data(), hash.size());
  network.attempts.push_back({tag, {data, data + length}, hash, channel});
  return network.queue_result;
}
}
