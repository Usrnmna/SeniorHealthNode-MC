// Included by the actual-source integration suite after CompanionHealth. These
// tests call the same command owner and cooperative sequence as the firmware.
// Peripheral work, RTOS execution and rescue side effects remain host boundaries.
class CompanionServices : public CompanionHealth {
protected:
  std::vector<std::string> service_order;
  std::vector<bool> ui_button_suppressed;
  std::function<void(unsigned, const char*)> before_service;

  void boundary(const char* name) {
    const unsigned position = unsigned(service_order.size());
    service_order.emplace_back(name);
    if (before_service) before_service(position, name);
  }
  struct NetworkBoundary {
    CompanionServices& test;
    void pollMotion() { test.boundary("motion"); test.mesh.pollMotion(); }
    void pollFallResponse() { test.boundary("response"); test.mesh.pollFallResponse(); }
    void pollLowBattery() { test.boundary("battery"); test.mesh.pollLowBattery(); }
    bool fallAlarmActive() const { return test.mesh.fallAlarmActive(); }
    void loop() { test.boundary("mesh"); test.mesh.loop(); }
  };
  struct ExternalBoundary {
    CompanionServices& test;
    const char* name;
    void loop() { test.boundary(name); }
  };
  void runLoop() {
    service_order.clear();
    NetworkBoundary network{*this};
    ExternalBoundary interfaces{*this, "interface"}, environment{*this, "sensors"};
    serviceCompanionLoop(network, interfaces, environment, [this](bool suppress) {
      boundary("ui"); ui_button_suppressed.push_back(suppress);
    });
  }
  void loopSample(float acceleration = 1, float rotation = 0) {
    g_mock_millis += 20; hardware.push(acceleration, rotation); runLoop();
  }
  uint32_t loopFall() {
    loopSample(.2f, 280); loopSample(3, 280);
    EXPECT_TRUE(mesh.fallAlarmActive()); return millis();
  }
  void loopAt(uint32_t at, bool pressed = false) {
    g_mock_millis = at; g_mock_button_level = pressed ? LOW : HIGH; runLoop();
  }
  void loopClick(uint32_t start) {
    loopAt(start, true); loopAt(start + 25, true);
    loopAt(start + 100); loopAt(start + 125);
  }
  // Feed every byte through the production RX budget and dispatcher. No time or
  // USB worker progress is invented by this helper.
  void commandBytes(const std::string& bytes) {
    Serial.receive(bytes);
    unsigned calls = 0;
    while (Serial.available() && calls++ < 1000) {
      const auto before = Serial.reads;
      mesh.serviceCompanionCommands();
      EXPECT_LE(Serial.reads - before, HealthNodeConfig::serial_rx_bytes_per_loop);
    }
    ASSERT_EQ(0, Serial.available());
  }
  void command(const char* value) { commandBytes(std::string(value) + '\n'); }
  void drainCommands() {
    unsigned calls = 0;
    while ((mesh.usb_reply.pending() || mesh.motion_recorder.pending() ||
        host_usb_queue.count() || mesh.motion_stats_row >= 0 ||
        mesh.trace_reply_row < mesh.trace_reply_count) && calls++ < 12000) {
      mesh.serviceCompanionCommands(); runHostUsbWorkerOnce();
    }
    ASSERT_LT(calls, 12000u);
    ASSERT_EQ(0u, bounded_usb_output.pendingPackets());
  }
};

TEST_F(CompanionServices, PartialLinesAndAllTerminatorsRespectRxAndCommandBudgets) {
  arm();
  commandBytes("motion record ");
  EXPECT_FALSE(mesh.motion_recorder.active()); EXPECT_TRUE(mesh.settings_commands.empty());
  commandBytes("start\r\nhealth\nmotion\rbattery\r\n");
  EXPECT_TRUE(mesh.motion_recorder.active());
  ASSERT_EQ(3u, mesh.settings_commands.size());
  EXPECT_EQ((std::vector<std::string>{"health", "motion", "battery"}), mesh.settings_commands);
  command("motion record stop"); drainCommands();
  // Recording frames may separate text chunks; the prefix fits one chunk.
  EXPECT_NE(std::string::npos, Serial.bytes.find("motion recording v1 started"));
  EXPECT_EQ(1u, mesh.capture_session);
  EXPECT_NE(std::string::npos, Serial.bytes.find("fall="));
  EXPECT_NE(std::string::npos, Serial.bytes.find("a[g]="));
  EXPECT_NE(std::string::npos, Serial.bytes.find("Battery reading unavailable"));
  EXPECT_TRUE(mesh.rescue_commands.empty());
  Serial.receive("health\nhealth\n");
  const auto count = mesh.settings_commands.size();
  mesh.serviceCompanionCommands(); EXPECT_EQ(count + 1, mesh.settings_commands.size());
  EXPECT_GT(Serial.available(), 0);
  mesh.serviceCompanionCommands(); EXPECT_EQ(count + 2, mesh.settings_commands.size());
}

TEST_F(CompanionServices, InvalidAndOversizedLinesDiscardTheirEntireContentsAndRecover) {
  const std::string secret = "private-key-must-not-be-echoed";
  commandBytes(std::string(180, 'x') + secret + "\n");
  commandBytes(std::string("be\0ep\n", 6));
  EXPECT_TRUE(mesh.settings_commands.empty()); EXPECT_TRUE(mesh.rescue_commands.empty());
  EXPECT_FALSE(mesh.cli_overflow); EXPECT_STREQ("", mesh.cli_command);
  command("health"); drainCommands();
  ASSERT_EQ(1u, mesh.settings_commands.size()); EXPECT_EQ("health", mesh.settings_commands[0]);
  EXPECT_NE(std::string::npos, Serial.bytes.find("command too long or contains invalid bytes"));
  EXPECT_NE(std::string::npos, Serial.bytes.find("fall="));
  EXPECT_EQ(std::string::npos, Serial.bytes.find(secret));
  EXPECT_EQ(0u, host_pwm_starts);
}

TEST_F(CompanionServices, UnavailableRxPreservesPartialCommandUntilRealBytesResume) {
  arm(); commandBytes("motion record ");
  const auto reads = Serial.reads;
  Serial.rx_ready = false;
  for (unsigned i = 0; i < 16; ++i) mesh.serviceCompanionCommands();
  EXPECT_EQ(reads, Serial.reads);
  EXPECT_STREQ("motion record ", mesh.cli_command); EXPECT_FALSE(mesh.cli_overflow);
  EXPECT_TRUE(mesh.settings_commands.empty()); EXPECT_FALSE(mesh.motion_recorder.active());
  Serial.rx_ready = true;
  commandBytes("start\n"); EXPECT_TRUE(mesh.motion_recorder.active());
  command("motion record stop"); drainCommands();
  EXPECT_EQ(0u, mesh.motion_recorder.droppedRecords());
}

TEST_F(CompanionServices, FailedReadDoesNotBecomeCommandDataOrConsumeTheNextByte) {
  Serial.receive("health\n"); Serial.fail_next_read = true;
  mesh.serviceCompanionCommands();
  EXPECT_EQ(1u, Serial.reads); EXPECT_EQ(0u, Serial.read_at);
  EXPECT_STREQ("", mesh.cli_command); EXPECT_TRUE(mesh.settings_commands.empty());
  mesh.serviceCompanionCommands(); drainCommands();
  ASSERT_EQ(1u, mesh.settings_commands.size()); EXPECT_EQ("health", mesh.settings_commands[0]);
  EXPECT_NE(std::string::npos, Serial.bytes.find("fall="));
  EXPECT_EQ(std::string::npos, Serial.bytes.find("unknown command"));
}

TEST_F(CompanionServices, ProductionModeSelectorPreservesHandlerPriorityAndRescueIsolation) {
  arm(); command("motion record status"); command("health timing");
  EXPECT_TRUE(mesh.settings_commands.empty());
  command("host settings"); command("health"); command("erase");
  EXPECT_EQ((std::vector<std::string>{"host settings", "health", "erase"}), mesh.settings_commands);
  EXPECT_TRUE(mesh.rescue_commands.empty()); EXPECT_GT(mesh.protocol_services, 0u);
  drainCommands();
  EXPECT_NE(std::string::npos, Serial.bytes.find("host settings boundary"));
  EXPECT_NE(std::string::npos, Serial.bytes.find("Error: unknown command; use help"));
  const auto protocol_calls = mesh.protocol_services;
  mesh._cli_rescue = true;
  commandBytes("erase\rhealth\rhost settings\r"); drainCommands();
  EXPECT_EQ(protocol_calls, mesh.protocol_services);
  EXPECT_EQ((std::vector<std::string>{"erase", "host settings"}), mesh.rescue_commands);
  EXPECT_EQ(3u, mesh.settings_commands.size());
}

TEST_F(CompanionServices, PendingRowsAndFullWorkerDoNotTrapRecordingControls) {
  arm(); sample(.2f, 280); const auto before = checkpoint();
  while (host_usb_queue.spaces()) bounded_usb_output.printf("BLE diagnostic\n");
  command("motion stats"); ASSERT_EQ(0, mesh.motion_stats_row);
  command("motion record start"); ASSERT_TRUE(mesh.motion_recorder.active());
  command("motion record status"); command("motion record stop");
  EXPECT_FALSE(mesh.motion_recorder.active()); EXPECT_TRUE(mesh.motion_recorder.pending());
  EXPECT_EQ(before, checkpoint()); EXPECT_EQ(8u, host_usb_queue.count());
  EXPECT_EQ(0u, Serial.write_calls); EXPECT_EQ(0u, Serial.readiness_queries);
  drainCommands();
  EXPECT_EQ(-1, mesh.motion_stats_row);
  EXPECT_NE(std::string::npos, Serial.bytes.find("recording=1"));
  EXPECT_NE(std::string::npos, Serial.bytes.find("recording stopped"));
  EXPECT_NE(std::string::npos, Serial.bytes.find("chip_temp[C]"));
  EXPECT_EQ(0u, mesh.motion_recorder.droppedRecords());
}

TEST_F(CompanionServices, UnavailableWorkerRejectsRecordingThroughActualDispatch) {
  arm(); sample(.2f, 280); const auto before = checkpoint();
  host_usb_queue.initialized = false; command("motion record start");
  EXPECT_FALSE(mesh.motion_recorder.active()); EXPECT_EQ(before, checkpoint());
  host_usb_queue.initialized = true; drainCommands();
  EXPECT_NE(std::string::npos, Serial.bytes.find("USB output worker unavailable"));
  command("motion record start"); EXPECT_TRUE(mesh.motion_recorder.active());
  command("motion record stop"); drainCommands();
}

TEST_F(CompanionServices, OneCommandServiceOffersOneReplyChunkBeforeOneRecordingFrame) {
  arm(); command("motion record start"); ASSERT_EQ(0u, host_usb_queue.count());
  const auto accepted = bounded_usb_output.acceptedPackets();
  const auto next = host_usb_queue.head;
  mesh.serviceCompanionCommands();
  ASSERT_EQ(accepted + 2, bounded_usb_output.acceptedPackets());
  const auto& reply = host_usb_queue.packets[next];
  const auto& frame = host_usb_queue.packets[(next + 1) % host_usb_queue.packets.size()];
  EXPECT_EQ(32u, reply.length); EXPECT_EQ('O', reply.bytes[0]);
  EXPECT_EQ(MotionRecorder::frame_size, frame.length); EXPECT_EQ(0x1e, frame.bytes[0]);
  EXPECT_EQ(0u, Serial.write_calls); EXPECT_EQ(0u, Serial.readiness_queries);
  command("motion record stop"); drainCommands();
}

TEST_F(CompanionServices, RejectedQueueAdmissionsRetainEvidenceAndDoNotStopRx) {
  arm(); const auto before = checkpoint(); command("motion record start");
  host_usb_queue.reject_sends = 1000;
  command("motion record status"); command("motion record stop");
  EXPECT_GT(bounded_usb_output.rejectedWrites(), 0u);
  EXPECT_EQ(0u, mesh.motion_recorder.exportedPackets());
  EXPECT_EQ(0u, bounded_usb_output.pendingPackets()); EXPECT_EQ(before, checkpoint());
  EXPECT_FALSE(mesh.motion_recorder.active()); EXPECT_TRUE(mesh.motion_recorder.pending());
  host_usb_queue.reject_sends = 0; drainCommands();
  EXPECT_EQ(0u, mesh.motion_recorder.shortWrites()); EXPECT_EQ(0u, mesh.motion_recorder.droppedRecords());
  EXPECT_NE(std::string::npos, Serial.bytes.find("recording stopped"));
}

TEST_F(CompanionServices, PartialWorkerWritesRemainObservableWhileCommandsProgress) {
  arm(); const auto before = checkpoint(); Serial.capacity = 7;
  command("motion record status"); drainCommands();
  EXPECT_GT(bounded_usb_output.failedWrites(), 0u);
  const auto count = mesh.settings_commands.size(); command("health"); drainCommands();
  EXPECT_EQ(count + 1, mesh.settings_commands.size()); EXPECT_EQ(before, checkpoint());
  EXPECT_EQ(0u, bounded_usb_output.pendingPackets());
}

TEST_F(CompanionServices, InflightEndPrecedesNextBeginWithProductionRecordingDispatch) {
  arm(); command("motion record start"); command("motion record stop");
  for (unsigned i = 0; (mesh.motion_recorder.pending() || mesh.usb_reply.pending()) && i < 12000; ++i) {
    mesh.serviceCompanionCommands();
    while (host_usb_queue.count() > 1) runHostUsbWorkerOnce();
  }
  ASSERT_FALSE(mesh.motion_recorder.pending()); ASSERT_FALSE(mesh.usb_reply.pending());
  ASSERT_EQ(1u, host_usb_queue.count());
  BoundedUsbPacket inflight; ASSERT_TRUE(host_usb_queue.receive(inflight));
  ASSERT_EQ(1u, bounded_usb_output.pendingPackets());
  command("motion record status"); bounded_usb_output.printf("BLE diagnostic\n");
  command("motion record start"); ASSERT_TRUE(mesh.motion_recorder.active());
  EXPECT_EQ(2u, mesh.capture_session);
  const auto end_at = Serial.bytes.size();
  bounded_usb_output.deliver(inflight, Serial); drainCommands();
  const auto begin_at = Serial.bytes.find('\x1e', end_at + inflight.length);
  ASSERT_NE(std::string::npos, begin_at);
  EXPECT_EQ('S', Serial.bytes[begin_at + 1]); EXPECT_EQ("01", Serial.bytes.substr(begin_at + 6, 2));
  const auto end_sequence = std::stoul(Serial.bytes.substr(end_at + 2, 4), nullptr, 16);
  EXPECT_EQ(uint16_t(end_sequence + 1), std::stoul(Serial.bytes.substr(begin_at + 2, 4), nullptr, 16));
  command("motion record stop"); drainCommands();
}

TEST_F(CompanionServices, ProductionSequenceDetectsAcrossRadioAndSuppressesAlarmOwnedButton) {
  arm(); g_mock_millis += 20; hardware.push(.2f, 280);
  before_service = [this](unsigned position, const char*) {
    if (position == 4) { g_mock_millis += 20; hardware.push(3, 280); }
  };
  runLoop(); before_service = {};
  EXPECT_EQ((std::vector<std::string>{"motion", "response", "battery", "mesh", "interface",
      "motion", "response", "sensors", "ui"}), service_order);
  EXPECT_TRUE(mesh.fallAlarmActive()); EXPECT_TRUE(host_pwm_attached);
  EXPECT_TRUE(ui_button_suppressed.back());
  // The second response position transfers ownership; its existing delivery
  // poll order sends the initial notice in the next response service.
  runLoop(); ASSERT_EQ(1u, mesh.attempts.size());
  EXPECT_TRUE(queuedText("Possible fall (strong-sequence)"));
}

TEST_F(CompanionServices, DeadlineCollisionKeepsGesturePriorityAndUiSuppressionAfterResolution) {
  arm(); const auto declared = loopFall();
  const auto deadline = declared + HealthNodeConfig::response_timeout_ms;
  loopClick(deadline - 525); loopClick(deadline - 325); loopClick(deadline - 125);
  EXPECT_FALSE(mesh.fallAlarmActive()); EXPECT_FALSE(host_pwm_attached);
  EXPECT_TRUE(ui_button_suppressed.back());
  EXPECT_TRUE(queuedText(HealthNodeConfig::ack_message));
  EXPECT_FALSE(queuedText(HealthNodeConfig::unconfirmed_message));
  runLoop(); EXPECT_FALSE(ui_button_suppressed.back());
}

TEST_F(CompanionServices, FalseDismissalAndNewFallRetainOwnershipAgainstStaleCallback) {
  arm(); const auto first = loopFall(); const auto old_tag = mesh.fall_delivery.queuedTag();
  ASSERT_NE(0u, old_tag);
  loopClick(first + 200); loopClick(first + 400);
  loopAt(first + 200 + HealthNodeConfig::click_window_ms + 1);
  ASSERT_FALSE(mesh.fallAlarmActive()); EXPECT_TRUE(mesh.fall_response.falseAlarmConfirmed());
  EXPECT_TRUE(queuedText(HealthNodeConfig::false_alarm_message));
  for (unsigned i = 0; i < 25; ++i) loopSample();
  const auto second = loopFall(); ASSERT_LT(uint32_t(second - first), 60000u);
  ASSERT_EQ(2u, mesh.fall_response.incidentId());
  const auto current_tag = mesh.fall_delivery.queuedTag(); ASSERT_NE(old_tag, current_tag);
  before_service = [this, old_tag](unsigned position, const char*) {
    if (position == 4) mesh.onPacketTxComplete(old_tag, true);
  };
  runLoop(); before_service = {};
  EXPECT_TRUE(mesh.fallAlarmActive()); EXPECT_EQ(current_tag, mesh.fall_delivery.queuedTag());
  EXPECT_EQ(AssistanceDelivery::Queued, mesh.fall_delivery.state());
}

TEST_F(CompanionServices, RepeatedEventKeepsOriginalDeadlineChannelAndDeliveryOwner) {
  arm(); ASSERT_TRUE(FallChannel::parse("public #first", mesh.fall_channel));
  const auto declared = loopFall(); const auto tag = mesh.fall_delivery.queuedTag();
  ASSERT_NE(0u, tag); ASSERT_TRUE(FallChannel::parse("public #second", mesh.fall_channel));
  for (unsigned i = 0; i < 70; ++i) loopSample();
  loopFall(); EXPECT_EQ(1u, mesh.fall_response.incidentId());
  EXPECT_STREQ("#first", mesh.fall_response.incidentChannel().name);
  EXPECT_EQ(tag, mesh.fall_delivery.queuedTag()); EXPECT_EQ(1u, mesh.fall_delivery.attemptCount());
  loopAt(declared + HealthNodeConfig::response_timeout_ms);
  EXPECT_TRUE(queuedText(HealthNodeConfig::unconfirmed_message));
  for (const auto& attempt : mesh.attempts) EXPECT_STREQ("#first", attempt.channel.name);
}

TEST_F(CompanionServices, StrongEventSurvivesMovementAndLaterStillnessUpdatesOriginalIncident) {
  arm(); const auto declared = loopFall();
  for (unsigned i = 0; i < 75; ++i) loopSample(1.4f, 80);
  EXPECT_TRUE(mesh.fallAlarmActive());
  loopAt(declared + HealthNodeConfig::response_timeout_ms);
  EXPECT_TRUE(queuedText(HealthNodeConfig::unconfirmed_message));
  EXPECT_FALSE(queuedText(HealthNodeConfig::no_movement_message));
  for (unsigned i = 0; i < 560; ++i) {
    loopSample();
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

class CompanionStorageFailure : public CompanionServices, public testing::WithParamInterface<bool> {};
TEST_P(CompanionStorageFailure, CheckedStorageFailureCannotBlockAcknowledgementOrOverwriteNextIncident) {
  arm();
  mesh.store.fs.fail_flush = GetParam(); mesh.store.fs.fail_close = !GetParam();
  const auto first = loopFall();
  ASSERT_TRUE(mesh.persistence_fault); ASSERT_TRUE(mesh.persistence_dirty);
  EXPECT_EQ(GetParam() ? FallIncidentStore::SaveStage::Flush : FallIncidentStore::SaveStage::Close,
      mesh.persistence_result.stage);
  EXPECT_TRUE(mesh.persistence_result.api_checked);
  loopClick(first + 200); loopClick(first + 400); loopClick(first + 600);
  EXPECT_FALSE(mesh.fallAlarmActive()); EXPECT_FALSE(host_pwm_attached);
  EXPECT_TRUE(queuedText(HealthNodeConfig::ack_message)); EXPECT_TRUE(mesh.persistence_dirty);
  mesh.store.fs.fail_flush = mesh.store.fs.fail_close = false;
  for (unsigned i = 0; i < 25; ++i) loopSample();
  const auto second = loopFall(); ASSERT_LT(uint32_t(second - first), 5000u);
  EXPECT_FALSE(mesh.persistence_fault); EXPECT_FALSE(mesh.persistence_dirty);
  const auto recovered = FallIncidentStore::load(mesh.store.fs, mesh.clock.timestamp, false);
  EXPECT_TRUE(recovered.shouldResume()); EXPECT_EQ(2u, recovered.value.incident);
  EXPECT_TRUE(recovered.value.active); EXPECT_TRUE(mesh.fallAlarmActive());
}
INSTANTIATE_TEST_SUITE_P(FlushAndClose, CompanionStorageFailure, testing::Bool());

TEST_F(CompanionServices, StorageDiagnosticsExposeFailureAndRetryWithoutClaimingDurability) {
  arm(); command("health detail"); drainCommands();
  EXPECT_NE(std::string::npos, Serial.bytes.find("persistence_result=not-attempted"));
  Serial.bytes.clear(); mesh.store.fs.fail_close = true;
  loopFall();
  ASSERT_TRUE(mesh.persistence_fault); ASSERT_TRUE(mesh.persistence_dirty);
  command("health detail"); drainCommands();
  EXPECT_NE(std::string::npos, Serial.bytes.find("persistence_result=close"));
  EXPECT_NE(std::string::npos, Serial.bytes.find("checked_api=1"));
  EXPECT_NE(std::string::npos, Serial.bytes.find("physical_durability=unverified"));
  EXPECT_EQ(std::string::npos, Serial.bytes.find("reply capacity exceeded"));
  Serial.bytes.clear(); mesh.store.fs.fail_close = false;
  loopAt(mesh.persistence_retry_at + 5000u);
  ASSERT_FALSE(mesh.persistence_fault); ASSERT_FALSE(mesh.persistence_dirty);
  command("health detail"); drainCommands();
  EXPECT_NE(std::string::npos, Serial.bytes.find("persistence_result=verified-readback"));
  EXPECT_NE(std::string::npos, Serial.bytes.find("errno=0 close_errno=0 checked_api=1"));
  EXPECT_NE(std::string::npos, Serial.bytes.find("physical_durability=unverified"));
  EXPECT_EQ(std::string::npos, Serial.bytes.find("reply capacity exceeded"));
}

using ServiceDelay = std::tuple<uint32_t, unsigned, bool>;
class CompanionDelayedService : public CompanionServices, public testing::WithParamInterface<ServiceDelay> {
protected:
  uint32_t initialMillis() const override {
    // Setup and arming consume 600 ms, leaving the delayed loop five ms before
    // rollover. The other half of the matrix starts at ordinary uptime.
    return std::get<2>(GetParam()) ? UINT32_MAX - 605u : 0;
  }
};
TEST_P(CompanionDelayedService, EveryServiceDelayIsAccountedWithoutInventingStillness) {
  arm(); const auto started = millis();
  const auto delay = std::get<0>(GetParam()); const auto stage = std::get<1>(GetParam());
  const auto unavailable = mesh.detector_availability.unavailableMs(started);
  before_service = [delay, stage](unsigned position, const char*) {
    if (position == stage) g_mock_millis += delay;
  };
  runLoop(); before_service = {}; runLoop();
  EXPECT_EQ(delay, uint32_t(millis() - started));
  EXPECT_GE(mesh.motion_timing.max_service_gap_ms, delay);
  EXPECT_FALSE(mesh.fallAlarmActive()); EXPECT_FALSE(host_pwm_attached);
  EXPECT_TRUE(mesh.attempts.empty());
  if (delay > FallDetectionConfig{}.max_sample_gap_ms) {
    EXPECT_NE(DetectorAvailability::Available, mesh.availabilityCause(millis()));
    EXPECT_GE(mesh.detector_availability.unavailableMs(millis()) - unavailable,
        delay - FallDetectionConfig{}.max_sample_gap_ms);
  }
}
INSTANTIATE_TEST_SUITE_P(AllBoundariesAndRollover, CompanionDelayedService,
    testing::Combine(testing::Values(20u, 99u, 100u, 101u, 250u, 1000u, 5000u),
                     testing::Range(0u, 9u), testing::Bool()));
