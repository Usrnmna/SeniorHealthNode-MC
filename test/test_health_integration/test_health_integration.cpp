#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#define HEALTH_MONITOR_HOST_HEADER "../../test/health_integration_support/HostMesh.h"
#include "../../examples/companion_radio/HealthMonitor.cpp"
#include "../../examples/companion_radio/HealthRecording.cpp"
#include "../../examples/companion_radio/HealthDiagnostics.cpp"
#include "../../examples/companion_radio/CompanionCommandService.cpp"
#include "../../examples/companion_radio/CompanionServices.h"
#include "../../examples/companion_radio/CompanionStorageStartup.h"

namespace {
class CompanionHealth : public testing::Test {
protected:
  MpuFifoFake hardware;
  MyMesh mesh;
  virtual uint32_t initialMillis() const { return 0; }

  void SetUp() override {
    g_mock_millis = initialMillis(); g_mock_button_level = HIGH;
    sensors = HostSensors{}; Serial = HostSerial{};
    resetHostUsb();
    host_pwm_attached = false; host_pwm_starts = 0;
    ASSERT_TRUE(alarm_buzzer.begin());
    mesh.pollMotion(); g_mock_millis += 100; mesh.pollMotion();
  }
  void sample(float acceleration = 1, float rotation = 0) {
    g_mock_millis += 20; hardware.push(acceleration, rotation); mesh.pollMotion();
  }
  void arm(unsigned count = 25) {
    for (unsigned i = 0; i < count; ++i) sample();
    ASSERT_TRUE(mesh.motion_rule.available());
  }
  uint32_t fall() {
    sample(.2f, 280); sample(3, 280);
    EXPECT_TRUE(mesh.fallAlarmActive()); return millis();
  }
  void service(uint32_t at, bool pressed = false) {
    g_mock_millis = at; g_mock_button_level = pressed ? LOW : HIGH; mesh.pollFallResponse();
  }
  void click(uint32_t start) {
    service(start, true); service(start + 25, true);
    service(start + 100); service(start + 125);
  }
  void falseAlarm(uint32_t start) {
    click(start); click(start + 200); service(start + HealthNodeConfig::click_window_ms + 1);
    EXPECT_FALSE(mesh.fallAlarmActive()); EXPECT_TRUE(mesh.fall_response.falseAlarmConfirmed());
  }
  bool queuedText(const char* text) const {
    for (const auto& attempt : mesh.attempts)
      if (attempt.text().find(text) != std::string::npos) return true;
    return false;
  }
  std::vector<uint8_t> checkpoint() const {
    uint8_t bytes[MotionCapture::checkpoint_capacity];
    MotionCapture::Writer writer(bytes, sizeof(bytes));
    MotionCapture::writeCheckpoint(writer, mesh.motion_rule, mesh.fall_response);
    EXPECT_TRUE(writer.ok()); return {bytes, bytes + writer.size()};
  }
  void drainRecording() {
    Serial.capacity = 64;
    for (unsigned i = 0; (mesh.motion_recorder.pending() || mesh.usb_reply.pending() ||
         host_usb_queue.count()) && i < 10000; ++i) {
      mesh.usb_reply.drain(bounded_usb_output);
      mesh.serviceMotionRecording();
      runHostUsbWorkerOnce();
    }
    EXPECT_FALSE(mesh.motion_recorder.pending());
    EXPECT_FALSE(mesh.usb_reply.pending()); EXPECT_EQ(0u, host_usb_queue.count());
    EXPECT_EQ(0u, bounded_usb_output.pendingPackets());
  }
};

TEST_F(CompanionHealth, ActualPollMethodsProcessBothCooperativePositions) {
  arm(); sample(.2f, 280); // First main-loop health position.
  mesh.pollFallResponse();
  g_mock_millis += 20; hardware.push(3, 280); // Simulated radio/interface interval.
  mesh.pollMotion(); // Second position uses the same production implementation.
  ASSERT_TRUE(mesh.fallAlarmActive());
  EXPECT_TRUE(host_pwm_attached);
  mesh.pollFallResponse(); mesh.pollFallResponse();
  ASSERT_EQ(1u, mesh.attempts.size());
  EXPECT_TRUE(queuedText("Possible fall (strong-sequence)"));
  EXPECT_EQ(1u, mesh.fall_response.incidentId());
  EXPECT_FALSE(mesh.persistence_dirty);
}

TEST_F(CompanionHealth, BusTimeAdvancementSetsDeclarationAndDeadlineAfterRead) {
  arm(); sample(.2f, 280);
  auto response = Wire1.response;
  Wire1.response = [response](uint8_t address, uint8_t count) {
    auto bytes = response(address, count);
    if (address == 0x74) g_mock_millis += 17;
    return bytes;
  };
  const auto entered = millis() + 20;
  hardware.push(3, 280); g_mock_millis = entered; mesh.pollMotion();
  const auto declared = millis();
  ASSERT_EQ(entered + 17, declared); ASSERT_TRUE(mesh.fallAlarmActive());
  EXPECT_GE(mesh.motion_timing.max_poll_ms, 17u);
  EXPECT_GE(mesh.motion_timing.max_sample_delay_ms, 37u);
  service(entered + HealthNodeConfig::response_timeout_ms);
  EXPECT_FALSE(queuedText(HealthNodeConfig::unconfirmed_message));
  service(declared + HealthNodeConfig::response_timeout_ms);
  EXPECT_TRUE(queuedText(HealthNodeConfig::unconfirmed_message));
  EXPECT_TRUE(mesh.fallAlarmActive());
}

TEST_F(CompanionHealth, MotionPollProcessingIsBoundedAndRemainingFifoSurvives) {
  arm(); const auto before = sensors.motion.diagnostics().samples;
  for (unsigned i = 0; i < 8; ++i) hardware.push();
  g_mock_millis += 160; mesh.pollMotion();
  EXPECT_EQ(before + 4, sensors.motion.diagnostics().samples);
  EXPECT_EQ(4u, sensors.motion.queuedSamples());
  mesh.pollFallResponse(); mesh.pollMotion();
  EXPECT_EQ(before + 8, sensors.motion.diagnostics().samples);
  EXPECT_EQ(0u, sensors.motion.queuedSamples());
  EXPECT_FALSE(mesh.fallAlarmActive());
}

TEST_F(CompanionHealth, CalibrationInterruptionClearsCandidateAndRequiresFreshRecovery) {
  arm(); sample(.2f, 280);
  ASSERT_EQ(MotionRule::Tracking, mesh.motion_rule.currentState());
  char reply[160] = {};
  ASSERT_EQ(0, mesh.controlCalibration("start", reply));
  EXPECT_TRUE(mesh.gyro_calibration.active());
  sample(3, 280); EXPECT_FALSE(mesh.fallAlarmActive());
  ASSERT_EQ(0, mesh.controlCalibration("cancel", reply));
  EXPECT_FALSE(mesh.gyro_calibration.active());
  sample(3, 280); EXPECT_FALSE(mesh.fallAlarmActive());
  arm(); fall();
}

TEST_F(CompanionHealth, CalibrationChangeIsRejectedDuringActiveIncident) {
  arm(); fall(); char reply[160] = {};
  EXPECT_EQ(1, mesh.controlCalibration("start", reply));
  EXPECT_EQ(1, mesh.controlCalibration("reset", reply));
  EXPECT_EQ(0, mesh.controlCalibration("status", reply));
  EXPECT_TRUE(mesh.fallAlarmActive());
}

TEST_F(CompanionHealth, CalibrationResetPersistsAndInvalidatesFreshEvidence) {
  arm(); auto correction = GyroCalibration::Result{};
  correction.bias[0] = 3; correction.samples = HealthNodeConfig::gyro_calibration_min_samples;
  ASSERT_TRUE(sensors.motion.setCalibration(correction));
  char reply[160] = {};
  ASSERT_EQ(0, mesh.controlCalibration("reset", reply));
  EXPECT_FLOAT_EQ(0, sensors.motion.calibration().bias[0]);
  EXPECT_EQ(MotionRule::Recovering, mesh.motion_rule.currentState());
  EXPECT_EQ(1u, mesh.store.fs.writes);
  arm(); fall();
}

TEST_F(CompanionHealth, PersistenceStallIsCountedAndCannotBecomeQuiet) {
  arm(); const uint32_t declared = fall();
  const auto unavailable_before = mesh.detector_availability.unavailableMs(millis());
  mesh.store.fs.write_delay_ms = 40000;
  mesh.pollFallResponse();
  EXPECT_EQ(declared + 40000, millis());
  EXPECT_EQ(1u, mesh.motion_timing.persistence_calls);
  EXPECT_EQ(40000u, mesh.motion_timing.max_persistence_ms);
  EXPECT_GE(mesh.detector_availability.unavailableMs(millis()) - unavailable_before, 39900u);
  mesh.pollMotion(); mesh.pollFallResponse();
  EXPECT_TRUE(mesh.fallAlarmActive());
  EXPECT_TRUE(queuedText(HealthNodeConfig::unconfirmed_message));
  EXPECT_FALSE(queuedText(HealthNodeConfig::no_movement_message));
}

TEST_F(CompanionHealth, PersistenceFailureRetainsDirtyStateAndRateLimitsRetry) {
  arm(); const uint32_t declared = fall(); mesh.store.fs.fail_open = true;
  mesh.pollFallResponse();
  EXPECT_TRUE(mesh.persistence_fault); EXPECT_TRUE(mesh.persistence_dirty);
  mesh.store.fs.fail_open = false;
  service(declared + 4999); EXPECT_EQ(0u, mesh.store.fs.writes);
  service(declared + 5000); EXPECT_EQ(1u, mesh.store.fs.writes);
  EXPECT_FALSE(mesh.persistence_fault); EXPECT_FALSE(mesh.persistence_dirty);
}

TEST_F(CompanionHealth, ThirdClickAtDeadlineHasPriorityOverAutomaticWarning) {
  arm(); const uint32_t declared = fall();
  const auto deadline = declared + HealthNodeConfig::response_timeout_ms;
  click(deadline - 525); click(deadline - 325); click(deadline - 125);
  EXPECT_FALSE(mesh.fallAlarmActive()); EXPECT_FALSE(host_pwm_attached);
  EXPECT_TRUE(queuedText(HealthNodeConfig::ack_message));
  EXPECT_FALSE(queuedText(HealthNodeConfig::unconfirmed_message));
  EXPECT_EQ(FallIncidentStore::Resolved,
      FallIncidentStore::load(mesh.store.fs, mesh.clock.timestamp).outcome);
}

TEST_F(CompanionHealth, HeldButtonAtDeclarationDoesNotResolveTheNewAlarm) {
  arm(); g_mock_button_level = LOW; const uint32_t declared = fall();
  service(declared + 2000, true); service(declared + 2050); service(declared + 2075);
  EXPECT_TRUE(mesh.fallAlarmActive());
  click(declared + 2200); click(declared + 2400); click(declared + 2600);
  EXPECT_FALSE(mesh.fallAlarmActive());
}

TEST_F(CompanionHealth, RepeatedFallKeepsOriginalDeadlineChannelAndRetryOwner) {
  arm(); ASSERT_TRUE(FallChannel::parse("public #first", mesh.fall_channel));
  const uint32_t declared = fall(); mesh.pollFallResponse(); mesh.pollFallResponse();
  ASSERT_EQ(1u, mesh.attempts.size()); const auto first_tag = mesh.fall_delivery.queuedTag();
  ASSERT_TRUE(FallChannel::parse("public #second", mesh.fall_channel));
  arm(70); fall();
  EXPECT_EQ(1u, mesh.fall_response.incidentId());
  EXPECT_STREQ("#first", mesh.fall_response.incidentChannel().name);
  EXPECT_EQ(first_tag, mesh.fall_delivery.queuedTag());
  EXPECT_EQ(1u, mesh.fall_delivery.attemptCount());
  service(declared + HealthNodeConfig::response_timeout_ms);
  EXPECT_TRUE(queuedText(HealthNodeConfig::unconfirmed_message));
  for (const auto& attempt : mesh.attempts) EXPECT_STREQ("#first", attempt.channel.name);
}

TEST_F(CompanionHealth, FalseAlarmCancelsItsDeliveryAndNewEventStartsAnotherAlarm) {
  arm(); const uint32_t declared = fall(); mesh.pollFallResponse(); mesh.pollFallResponse();
  const auto old_tag = mesh.fall_delivery.queuedTag();
  ASSERT_NE(0u, old_tag); falseAlarm(declared + 200);
  EXPECT_EQ(AssistanceDelivery::Idle, mesh.fall_delivery.state());
  EXPECT_NE(mesh.cancellations.end(), std::find(mesh.cancellations.begin(), mesh.cancellations.end(), old_tag));
  EXPECT_TRUE(queuedText(HealthNodeConfig::false_alarm_message));
  mesh.onPacketTxComplete(old_tag, true);
  EXPECT_EQ(AssistanceDelivery::Idle, mesh.fall_delivery.state());
  arm(25); fall(); EXPECT_EQ(2u, mesh.fall_response.incidentId());
  EXPECT_TRUE(host_pwm_attached);
}

TEST_F(CompanionHealth, ActualCallbackRoutesTagsWithoutChangingAlarmOrOtherOwner) {
  arm(); const auto declared = fall(); mesh.pollFallResponse(); mesh.pollFallResponse();
  const auto fall_tag = mesh.fall_delivery.queuedTag(); ASSERT_NE(0u, fall_tag);
  service(declared + HealthNodeConfig::response_timeout_ms);
  const auto help_tag = mesh.assistance.queuedTag(); ASSERT_NE(0u, help_tag);
  mesh.onPacketTxComplete(fall_tag, true);
  EXPECT_EQ(AssistanceDelivery::Transmitted, mesh.fall_delivery.state());
  EXPECT_EQ(AssistanceDelivery::Queued, mesh.assistance.state());
  mesh.onPacketTxComplete(help_tag, false);
  EXPECT_EQ(AssistanceDelivery::Pending, mesh.assistance.state());
  EXPECT_TRUE(mesh.fallAlarmActive());
  const auto attempts = mesh.assistance.attemptCount();
  service(millis() + HealthNodeConfig::ack_retry_ms - 1);
  EXPECT_EQ(attempts, mesh.assistance.attemptCount());
  service(millis() + 1); EXPECT_EQ(attempts + 1, mesh.assistance.attemptCount());
}

TEST_F(CompanionHealth, UnavailableSensorCannotCancelAlarmOrBlockAcknowledgement) {
  arm(); const auto declared = fall(); hardware.short_bytes = 1;
  sample(1); EXPECT_FALSE(sensors.motion.valid);
  EXPECT_TRUE(mesh.fallAlarmActive());
  click(declared + 200); click(declared + 400); click(declared + 600);
  EXPECT_FALSE(mesh.fallAlarmActive()); EXPECT_FALSE(host_pwm_attached);
  EXPECT_TRUE(queuedText(HealthNodeConfig::ack_message));
}

TEST_F(CompanionHealth, ActualResponseAssessesLaterStillnessWithoutRestartingDeadline) {
  arm(); const auto declared = fall();
  service(declared + HealthNodeConfig::response_timeout_ms);
  ASSERT_TRUE(queuedText(HealthNodeConfig::unconfirmed_message));
  EXPECT_FALSE(queuedText(HealthNodeConfig::no_movement_message));
  // Transport completion releases immutable notice parts as in production. The
  // hash match is injected at the receive boundary; RF parsing is covered elsewhere.
  for (unsigned i = 0; i < 560; ++i) {
    sample(); mesh.pollFallResponse();
    const auto tag = mesh.assistance.queuedTag();
    if (tag) {
      const auto attempt = std::find_if(mesh.attempts.rbegin(), mesh.attempts.rend(),
          [tag](const MyMesh::Attempt& value) { return value.tag == tag; });
      ASSERT_NE(mesh.attempts.rend(), attempt);
      mesh.onPacketTxComplete(tag, true);
      ASSERT_TRUE(mesh.assistance.confirmRepeat(attempt->fingerprint.data()));
    }
  }
  EXPECT_TRUE(queuedText(HealthNodeConfig::no_movement_message));
  EXPECT_TRUE(mesh.fallAlarmActive()); EXPECT_EQ(1u, mesh.fall_response.incidentId());
}

TEST_F(CompanionHealth, NewAcknowledgedIncidentDoesNotCancelAnOlderDeliveryOwner) {
  arm(); ASSERT_TRUE(FallChannel::parse("public #old", mesh.fall_channel));
  const auto first = fall(); mesh.pollFallResponse(); mesh.pollFallResponse();
  const auto old_tag = mesh.fall_delivery.queuedTag();
  click(first + 200); click(first + 400); click(first + 600);
  ASSERT_FALSE(mesh.fallAlarmActive());
  ASSERT_TRUE(FallChannel::parse("public #new", mesh.fall_channel));
  arm(70); const auto second = fall();
  ASSERT_EQ(2u, mesh.fall_response.incidentId());
  EXPECT_STREQ("#new", mesh.fall_response.incidentChannel().name);
  falseAlarm(second + 200);
  EXPECT_EQ(old_tag, mesh.fall_delivery.queuedTag());
  EXPECT_EQ(mesh.cancellations.end(), std::find(mesh.cancellations.begin(), mesh.cancellations.end(), old_tag));
  EXPECT_EQ(1u, mesh.fall_delivery.attemptCount());
}

TEST_F(CompanionHealth, FifoOverflowCannotJoinOldLowEvidenceToLaterImpact) {
  arm(); sample(.2f, 280);
  hardware.interrupt |= 0x10;
  sample(3, 280);
  EXPECT_FALSE(mesh.fallAlarmActive());
  EXPECT_EQ(1u, sensors.motion.diagnostics().fifo_overflows);
  arm(); sample(3, 280); EXPECT_FALSE(mesh.fallAlarmActive());
  sample(.2f, 280); sample(3, 280); EXPECT_TRUE(mesh.fallAlarmActive());
}

TEST_F(CompanionHealth, DeliveryExhaustionDoesNotResolveIncidentOrRestartItsBudget) {
  arm(); fall(); mesh.queue_result = AssistanceTransport::NoPacket;
  mesh.pollFallResponse(); mesh.pollFallResponse();
  for (unsigned i = 1; i < HealthNodeConfig::ack_max_attempts; ++i)
    service(millis() + HealthNodeConfig::ack_retry_ms);
  EXPECT_EQ(HealthNodeConfig::ack_max_attempts, mesh.fall_delivery.attemptCount());
  EXPECT_EQ(AssistanceDelivery::Failed, mesh.fall_delivery.state());
  EXPECT_TRUE(mesh.fallAlarmActive());
  arm(70); fall(); mesh.pollFallResponse(); mesh.pollFallResponse();
  EXPECT_EQ(1u, mesh.fall_response.incidentId());
  EXPECT_EQ(HealthNodeConfig::ack_max_attempts, mesh.fall_delivery.attemptCount());
  EXPECT_EQ(AssistanceDelivery::Failed, mesh.fall_delivery.state());
}

TEST_F(CompanionHealth, SaturatedOutboxRetainsOldExplicitResponseAndCountsOmission) {
  for (unsigned incident = 1; incident <= 4; ++incident) {
    arm(70);
    char channel[32]; snprintf(channel, sizeof(channel), "public #incident%u", incident);
    ASSERT_TRUE(FallChannel::parse(channel, mesh.fall_channel));
    const auto declared = fall();
    mesh.pollFallResponse(); mesh.pollFallResponse();
    click(declared + 200); click(declared + 400); click(declared + 600);
    ASSERT_FALSE(mesh.fallAlarmActive());
    EXPECT_EQ(incident, mesh.fall_response.incidentId());
  }
  ASSERT_TRUE(mesh.fall_response.messagePending());
  EXPECT_EQ(FallNotice::AssistanceRequested, mesh.fall_response.pendingNotice());
  EXPECT_EQ(3u, mesh.fall_response.pendingIncident());
  EXPECT_STREQ("#incident3", mesh.fall_response.pendingChannel().name);
  EXPECT_EQ(1u, mesh.fall_response.droppedConfirmations());
  EXPECT_GE(mesh.initial_alert.coalescedCount(), 2u);
  EXPECT_EQ(1u, mesh.fall_delivery.attemptCount());
  EXPECT_FALSE(host_pwm_attached);
}

TEST_F(CompanionHealth, RestoreUsesSavedChannelAndUnknownClockDeadlineWithoutQuiet) {
  FallIncidentStore::Incident saved;
  saved.active = true; saved.incident = 42; saved.origin_epoch = 1800000000;
  ASSERT_TRUE(FallChannel::parse("public #saved", saved.channel));
  ASSERT_TRUE(FallIncidentStore::save(mesh.store.fs, saved));
  g_mock_millis = 1000; mesh.restoreFallIncident();
  EXPECT_TRUE(mesh.fallAlarmActive()); EXPECT_EQ(42u, mesh.fall_response.incidentId());
  EXPECT_EQ(FallIncidentStore::UncertainClock, mesh.recovery_outcome);
  EXPECT_EQ(1u, mesh.durable_incident.recovery_count);
  mesh.pollFallResponse();
  EXPECT_TRUE(queuedText(HealthNodeConfig::unconfirmed_message));
  EXPECT_FALSE(queuedText(HealthNodeConfig::no_movement_message));
  ASSERT_FALSE(mesh.attempts.empty()); EXPECT_STREQ("#saved", mesh.attempts.front().channel.name);
  EXPECT_FALSE(mesh.persistence_dirty);
}

TEST_F(CompanionHealth, TimerRolloverPreservesOriginalResponseDeadline) {
  g_mock_millis = UINT32_MAX - 1500;
  mesh.pollMotion(); // Expire the prior sensor timeline before a new continuous run.
  arm(50); const auto declared = fall();
  service(declared + HealthNodeConfig::response_timeout_ms - 1);
  EXPECT_FALSE(queuedText(HealthNodeConfig::unconfirmed_message));
  service(declared + HealthNodeConfig::response_timeout_ms);
  EXPECT_TRUE(queuedText(HealthNodeConfig::unconfirmed_message));
}

TEST_F(CompanionHealth, RecordingIsOffByDefaultAndStartDoesNotResetLiveCandidate) {
  arm(); sample(.2f, 280);
  ASSERT_EQ(MotionRule::Tracking, mesh.motion_rule.currentState());
  EXPECT_FALSE(mesh.motion_recorder.active()); EXPECT_EQ(0u, mesh.motion_recorder.bufferedBytes());
  const auto before = checkpoint(); const auto writes_before = mesh.store.fs.writes;
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record start"));
  ASSERT_TRUE(mesh.motion_recorder.active());
  EXPECT_EQ(before, checkpoint()); EXPECT_EQ(writes_before, mesh.store.fs.writes);
  sample(3, 280); EXPECT_TRUE(mesh.fallAlarmActive());
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record stop"));
  EXPECT_FALSE(mesh.motion_recorder.active()); EXPECT_TRUE(mesh.motion_recorder.pending());
  drainRecording(); EXPECT_EQ(0u, mesh.motion_recorder.droppedRecords());
  EXPECT_NE(std::string::npos, Serial.bytes.find('\x1e'));
}

TEST_F(CompanionHealth, RecordingAnActiveIncidentPreservesButtonProgressAndDeadline) {
  arm(); const auto declared = fall(); click(declared + 200);
  const auto before = checkpoint();
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record start"));
  ASSERT_TRUE(mesh.motion_recorder.active()); EXPECT_EQ(before, checkpoint());
  click(declared + 400); click(declared + 600);
  EXPECT_FALSE(mesh.fallAlarmActive()); EXPECT_TRUE(queuedText(HealthNodeConfig::ack_message));
  EXPECT_FALSE(queuedText(HealthNodeConfig::unconfirmed_message));
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record stop")); drainRecording();
  EXPECT_EQ(0u, mesh.motion_recorder.droppedRecords());
}

TEST_F(CompanionHealth, BlockedUsbWorkerCannotBlockDetectionOrAcknowledgement) {
  arm(); ASSERT_TRUE(mesh.handleRecordingCommand("motion record start"));
  ASSERT_TRUE(mesh.motion_recorder.active()); Serial.capacity = 0;
  for (unsigned i = 0; i < 120; ++i) {
    sample(); mesh.pollFallResponse(); mesh.serviceMotionRecording();
  }
  EXPECT_GT(mesh.motion_recorder.droppedRecords(), 0u);
  EXPECT_EQ(0u, mesh.store.fs.writes); EXPECT_EQ(0u, Serial.writes);
  EXPECT_EQ(8u, host_usb_queue.count()); EXPECT_EQ(0u, Serial.write_calls);
  EXPECT_EQ(0u, Serial.readiness_queries);
  EXPECT_LE(mesh.motion_recorder.highWaterBytes(), MotionRecorder::capacity);
  const auto declared = fall(); mesh.pollFallResponse();
  EXPECT_TRUE(mesh.fallAlarmActive()); EXPECT_EQ(1u, mesh.store.fs.writes);
  click(declared + 200); click(declared + 400); click(declared + 600);
  EXPECT_FALSE(mesh.fallAlarmActive()); EXPECT_FALSE(host_pwm_attached);
  EXPECT_EQ(2u, mesh.store.fs.writes); EXPECT_TRUE(queuedText(HealthNodeConfig::ack_message));
  EXPECT_EQ(0u, Serial.write_calls); EXPECT_EQ(0u, Serial.readiness_queries);
  const auto stopped_at = millis();
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record stop"));
  mesh.serviceMotionRecording(); EXPECT_EQ(stopped_at, millis());
  EXPECT_FALSE(mesh.motion_recorder.active()); EXPECT_TRUE(mesh.motion_recorder.pending());
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record start"));
  EXPECT_FALSE(mesh.motion_recorder.active()); // Pending session cannot be overwritten.
  drainRecording();
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record start"));
  EXPECT_TRUE(mesh.motion_recorder.active()); EXPECT_EQ(2u, mesh.capture_session);
}

TEST_F(CompanionHealth, FailedWorkerWritesAreVisibleAndCannotAlterIncidentState) {
  arm(); ASSERT_TRUE(mesh.handleRecordingCommand("motion record start"));
  const auto declared = fall(); mesh.pollFallResponse();
  while (host_usb_queue.spaces()) mesh.serviceMotionRecording();
  ASSERT_EQ(8u, host_usb_queue.count()); EXPECT_EQ(0u, Serial.write_calls);
  const auto before = checkpoint(); Serial.capacity = 0;
  while (runHostUsbWorkerOnce()) {}
  EXPECT_EQ(8u, Serial.write_calls); EXPECT_EQ(8u, bounded_usb_output.failedWrites());
  EXPECT_EQ(8u, bounded_usb_output.completedPackets());
  EXPECT_EQ(before, checkpoint()); EXPECT_EQ(declared, millis());
  click(declared + 200); click(declared + 400); click(declared + 600);
  EXPECT_FALSE(mesh.fallAlarmActive()); EXPECT_TRUE(queuedText(HealthNodeConfig::ack_message));
  EXPECT_EQ(8u, Serial.write_calls); // Main-loop operations did not retry USB writes.
}

TEST_F(CompanionHealth, RecordingStartRejectsUnavailableWorkerWithoutChangingEvidence) {
  arm(); sample(.2f, 280); const auto before = checkpoint();
  host_usb_queue.initialized = false;
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record start"));
  EXPECT_FALSE(mesh.motion_recorder.active()); EXPECT_EQ(0u, mesh.capture_session);
  EXPECT_EQ(before, checkpoint()); EXPECT_EQ(0u, mesh.motion_recorder.bufferedBytes());
  sample(3, 280); EXPECT_TRUE(mesh.fallAlarmActive());
  host_usb_queue.initialized = true; drainRecording();
  EXPECT_NE(std::string::npos, Serial.bytes.find("USB output worker unavailable"));
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record start"));
  EXPECT_TRUE(mesh.motion_recorder.active()); EXPECT_EQ(1u, mesh.capture_session);
}

TEST_F(CompanionHealth, RecordingStartAcceptsPendingDiagnosticTrafficWithoutChangingEvidence) {
  arm(); sample(.2f, 280); const auto before = checkpoint();
  while (host_usb_queue.spaces()) bounded_usb_output.printf("BLE diagnostic\n");
  ASSERT_EQ(8u, bounded_usb_output.pendingPackets());
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record start"));
  ASSERT_TRUE(mesh.motion_recorder.active()); EXPECT_EQ(1u, mesh.capture_session);
  EXPECT_EQ(before, checkpoint()); EXPECT_EQ(0u, Serial.write_calls);
  drainRecording();
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record stop")); drainRecording();
  EXPECT_EQ(0u, mesh.motion_recorder.droppedRecords());
}

TEST_F(CompanionHealth, WorkerInflightEndFramePrecedesNextSessionDespiteDiagnosticTraffic) {
  arm(); ASSERT_TRUE(mesh.handleRecordingCommand("motion record start"));
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record stop"));
  // Drain the RAM recorder but retain the final queued frame. A worker can
  // dequeue that frame and remain inside a USB write after the queue is empty.
  for (unsigned i = 0; (mesh.motion_recorder.pending() || mesh.usb_reply.pending()) && i < 10000; ++i) {
    mesh.usb_reply.drain(bounded_usb_output); mesh.serviceMotionRecording();
    while (host_usb_queue.count() > 1) runHostUsbWorkerOnce();
  }
  ASSERT_FALSE(mesh.motion_recorder.pending()); ASSERT_FALSE(mesh.usb_reply.pending());
  ASSERT_EQ(1u, host_usb_queue.count());
  BoundedUsbPacket inflight; ASSERT_TRUE(host_usb_queue.receive(inflight));
  EXPECT_EQ(0u, bounded_usb_output.queuedPackets());
  EXPECT_EQ(1u, bounded_usb_output.pendingPackets());
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record status"));
  bounded_usb_output.printf("BLE diagnostic\n");
  const auto before = checkpoint();
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record start"));
  ASSERT_TRUE(mesh.motion_recorder.active()); EXPECT_EQ(2u, mesh.capture_session);
  EXPECT_EQ(before, checkpoint());
  mesh.serviceMotionRecording(); // Begin can queue while the previous End is in flight.
  const size_t end_at = Serial.bytes.size();
  bounded_usb_output.deliver(inflight, Serial); drainRecording();
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(inflight.bytes), inflight.length),
            Serial.bytes.substr(end_at, inflight.length));
  const auto next_begin = Serial.bytes.find('\x1e', end_at + inflight.length);
  ASSERT_NE(std::string::npos, next_begin);
  EXPECT_EQ('S', Serial.bytes[next_begin + 1]);
  EXPECT_EQ("01", Serial.bytes.substr(next_begin + 6, 2));
  const auto end_sequence = std::stoul(Serial.bytes.substr(end_at + 2, 4), nullptr, 16);
  EXPECT_EQ(uint16_t(end_sequence + 1),
            std::stoul(Serial.bytes.substr(next_begin + 2, 4), nullptr, 16));
  EXPECT_NE(std::string::npos, Serial.bytes.find("recording=0 draining=0"));
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record stop")); drainRecording();
  EXPECT_EQ(0u, mesh.motion_recorder.droppedRecords());
}

TEST_F(CompanionHealth, ExhaustedSessionCounterCannotReuseSessionIdentity) {
  mesh.capture_session = UINT32_MAX;
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record start"));
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record start"));
  EXPECT_FALSE(mesh.motion_recorder.active()); EXPECT_EQ(UINT32_MAX, mesh.capture_session);
  EXPECT_EQ(0u, mesh.motion_recorder.bufferedBytes());
}

TEST_F(CompanionHealth, RecordingControlsExposeBoundedBufferAndRejectUnrelatedCommand) {
  ASSERT_FALSE(mesh.handleRecordingCommand("unrelated command"));
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record status"));
  drainRecording();
  EXPECT_NE(std::string::npos, Serial.bytes.find("recording=0"));
  EXPECT_NE(std::string::npos, Serial.bytes.find("capacity_bytes=8192"));
  arm(); ASSERT_TRUE(mesh.handleRecordingCommand("motion record start"));
  // Enqueue can fill while the worker is blocked, but only the worker touches
  // the underlying USB write operation. Once the fixed queue fills, drain stops.
  const auto writes = Serial.writes;
  while (host_usb_queue.spaces()) mesh.serviceMotionRecording();
  const auto buffered = mesh.motion_recorder.bufferedBytes();
  mesh.serviceMotionRecording();
  EXPECT_EQ(buffered, mesh.motion_recorder.bufferedBytes()); EXPECT_EQ(writes, Serial.writes);
}

TEST_F(CompanionHealth, RecordingCalibrationInterruptionDoesNotBridgeTheCandidate) {
  arm(); sample(.2f, 280); ASSERT_TRUE(mesh.handleRecordingCommand("motion record start"));
  ASSERT_TRUE(mesh.motion_recorder.active()); drainRecording();
  char reply[160] = {};
  ASSERT_EQ(0, mesh.controlCalibration("start", reply));
  sample(3, 280); drainRecording(); EXPECT_FALSE(mesh.fallAlarmActive());
  ASSERT_EQ(0, mesh.controlCalibration("cancel", reply));
  arm(); sample(3, 280); EXPECT_FALSE(mesh.fallAlarmActive());
  drainRecording(); fall(); drainRecording();
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record stop")); drainRecording();
  EXPECT_EQ(0u, mesh.motion_recorder.droppedRecords());
  EXPECT_FALSE(mesh.gyro_calibration.active());
}

TEST_F(CompanionHealth, AvailabilityCausesAreExclusiveAndConfigurationHasPrecedence) {
  EXPECT_EQ(DetectorAvailability::Startup, mesh.availabilityCause(millis()));
  sample(); EXPECT_EQ(DetectorAvailability::Recovery, mesh.availabilityCause(millis()));
  arm(); EXPECT_EQ(DetectorAvailability::Available, mesh.availabilityCause(millis()));
  char reply[160] = {}; ASSERT_EQ(0, mesh.controlCalibration("start", reply));
  EXPECT_EQ(DetectorAvailability::Calibration, mesh.availabilityCause(millis()));
  g_mock_millis += 200; mesh.observeDetectorAvailability(millis());
  auto invalid = FallDetectionConfig{}; invalid.low_g = -1;
  mesh.motion_rule = MotionRule(invalid);
  EXPECT_EQ(DetectorAvailability::ConfigurationFault, mesh.availabilityCause(millis()));
  mesh.observeDetectorAvailability(millis()); g_mock_millis += 100;
  const auto totals = mesh.detector_availability.totals(millis());
  EXPECT_EQ(millis(), totals.monitoredMs());
  EXPECT_GE(totals.ms[DetectorAvailability::Startup], 100u);
  EXPECT_GE(totals.ms[DetectorAvailability::Recovery], 200u);
  EXPECT_EQ(200u, totals.ms[DetectorAvailability::Calibration]);
  EXPECT_EQ(100u, totals.ms[DetectorAvailability::ConfigurationFault]);
}

TEST_F(CompanionHealth, MissingBootSensorBecomesUnavailableUntilFreshRecovery) {
  Wire1.read_status = ESP_FAIL;
  g_mock_millis = HealthNodeConfig::motion_fault_timeout_ms - 1;
  mesh.pollMotion();
  EXPECT_FALSE(mesh.motion_seen);
  EXPECT_FALSE(mesh.motion_health.fault());
  EXPECT_EQ(DetectorAvailability::Startup, mesh.availabilityCause(millis()));

  g_mock_millis = HealthNodeConfig::motion_fault_timeout_ms;
  mesh.pollMotion();
  ASSERT_TRUE(mesh.motion_health.fault());
  EXPECT_EQ(DetectorAvailability::SensorUnavailable, mesh.availabilityCause(millis()));
  g_mock_millis += 200;
  mesh.pollMotion();
  auto totals = mesh.detector_availability.totals(millis());
  EXPECT_EQ(HealthNodeConfig::motion_fault_timeout_ms, totals.ms[DetectorAvailability::Startup]);
  EXPECT_EQ(200u, totals.ms[DetectorAvailability::SensorUnavailable]);

  // Successful bus recovery and FIFO reset alone do not establish fresh data.
  Wire1.read_status = ESP_OK;
  g_mock_millis += HealthNodeConfig::motion_retry_ms;
  mesh.pollMotion();
  EXPECT_FALSE(mesh.motion_seen);
  EXPECT_TRUE(mesh.motion_health.fault());
  EXPECT_EQ(DetectorAvailability::SensorUnavailable, mesh.availabilityCause(millis()));
  sample();
  ASSERT_TRUE(mesh.motion_seen);
  EXPECT_FALSE(mesh.motion_health.fault());
  EXPECT_EQ(DetectorAvailability::Recovery, mesh.availabilityCause(millis()));
  const auto unavailable = mesh.detector_availability.totals(millis()).ms[DetectorAvailability::SensorUnavailable];
  arm();
  EXPECT_EQ(DetectorAvailability::Available, mesh.availabilityCause(millis()));
  totals = mesh.detector_availability.totals(millis());
  EXPECT_EQ(HealthNodeConfig::motion_fault_timeout_ms, totals.ms[DetectorAvailability::Startup]);
  EXPECT_EQ(unavailable, totals.ms[DetectorAvailability::SensorUnavailable]);
  EXPECT_EQ(millis(), totals.monitoredMs());
  EXPECT_FALSE(mesh.fallAlarmActive());
}

TEST_F(CompanionHealth, BoundedReplayOfOldFifoSamplesCannotEraseObservedStaleTime) {
  arm(); const auto before = mesh.detector_availability.totals(millis());
  for (unsigned i = 0; i < 25; ++i) hardware.push();
  g_mock_millis += 500;
  const auto stalled = mesh.detector_availability.totals(millis());
  EXPECT_GE(stalled.ms[DetectorAvailability::Stale] - before.ms[DetectorAvailability::Stale], 420u);
  while (!hardware.fifo.empty() || sensors.motion.queuedSamples()) mesh.pollMotion();
  const auto drained = mesh.detector_availability.totals(millis());
  EXPECT_GE(drained.ms[DetectorAvailability::Stale], stalled.ms[DetectorAvailability::Stale]);
  EXPECT_EQ(millis(), drained.monitoredMs());
  EXPECT_GE(mesh.motion_timing.max_service_gap_ms, 500u);
  EXPECT_GE(mesh.motion_timing.max_sample_delay_ms, 500u);
}

TEST_F(CompanionHealth, CompleteProductionCaptureCanBeExportedForHostReplay) {
  arm(); Serial.bytes.clear();
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record start"));
  ASSERT_TRUE(mesh.motion_recorder.active()); drainRecording();
  const auto recordedSample = [this](float a = 1, float r = 0) {
    sample(a, r); mesh.pollFallResponse(); drainRecording();
  };
  recordedSample(.2f, 280); // Rejected context candidate, with full expiry context.
  for (unsigned i = 0; i < 330; ++i) recordedSample();
  EXPECT_FALSE(mesh.fallAlarmActive());
  recordedSample(.2f, 280);
  char reply[160] = {}; ASSERT_EQ(0, mesh.controlCalibration("start", reply));
  recordedSample(3, 280); EXPECT_FALSE(mesh.fallAlarmActive());
  ASSERT_EQ(0, mesh.controlCalibration("cancel", reply)); drainRecording();
  for (unsigned i = 0; i < 25; ++i) recordedSample();
  hardware.interrupt |= 0x10; recordedSample(); // Explicit FIFO-loss generation.
  for (unsigned i = 0; i < 25; ++i) recordedSample();
  recordedSample(.2f, 280); recordedSample(3, 280);
  ASSERT_TRUE(mesh.fallAlarmActive()); const auto declared = millis();
  service(declared + HealthNodeConfig::response_timeout_ms); drainRecording();
  ASSERT_TRUE(queuedText(HealthNodeConfig::unconfirmed_message));
  // Continue the unresolved incident through fresh late stillness. Transport
  // completion releases owned notice parts without fabricating wearer evidence.
  for (unsigned i = 0; i < 560; ++i) {
    recordedSample();
    const auto tag = mesh.assistance.queuedTag();
    if (tag) {
      const auto attempt = std::find_if(mesh.attempts.rbegin(), mesh.attempts.rend(),
          [tag](const MyMesh::Attempt& value) { return value.tag == tag; });
      ASSERT_NE(mesh.attempts.rend(), attempt);
      mesh.onPacketTxComplete(tag, true);
      ASSERT_TRUE(mesh.assistance.confirmRepeat(attempt->fingerprint.data()));
    }
  }
  ASSERT_TRUE(queuedText(HealthNodeConfig::no_movement_message));
  const auto acknowledged = millis() + 200;
  click(acknowledged); click(acknowledged + 200); click(acknowledged + 400);
  ASSERT_FALSE(mesh.fallAlarmActive()); drainRecording();
  ASSERT_TRUE(mesh.handleRecordingCommand("motion record stop")); drainRecording();
  ASSERT_EQ(0u, mesh.motion_recorder.droppedRecords());
  ASSERT_EQ(0u, mesh.motion_recorder.shortWrites());
  if (const char* path = std::getenv("HEALTH_CAPTURE_FIXTURE")) {
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output.good()); output.write(Serial.bytes.data(), Serial.bytes.size());
    ASSERT_TRUE(output.good());
  }
}

#include "service_cases.h"
#include "storage_read_cases.h"
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS();
}
