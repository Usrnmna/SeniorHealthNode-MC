// Production recovery/save/diagnostic paths with native I/O results injected.
// The startup helper is real; SDK mount internals and BLE initialization are not.
TEST_F(CompanionServices, FailedMountIsUncertainAndNeverWritesOrRestoresAnAlarm) {
  mesh.setStorageStartupResult(false); mesh.store.fs.mounted = false;
  mesh.restoreFallIncident();
  EXPECT_EQ(FallIncidentStore::Corrupt, mesh.recovery_outcome);
  EXPECT_TRUE(mesh.persistence_fault); EXPECT_FALSE(mesh.fallAlarmActive());
  EXPECT_EQ(0u, mesh.store.fs.read_opens); EXPECT_EQ(0u, mesh.store.fs.writes);
  command("health detail"); drainCommands();
  EXPECT_NE(std::string::npos, Serial.bytes.find("storage_boot=failed mounted=no"));
  EXPECT_NE(std::string::npos, Serial.bytes.find("boot_read0=mount"));
  EXPECT_EQ(std::string::npos, Serial.bytes.find("reply capacity exceeded"));
}

TEST_F(CompanionServices, QualifiedMissingAndUnknownStartupRemainDistinct) {
  mesh.restoreFallIncident();
  EXPECT_EQ(FallIncidentStore::Missing, mesh.recovery_outcome);
  EXPECT_FALSE(mesh.persistence_fault);
  for (const auto& read : mesh.recovery_reads) {
    EXPECT_EQ(FallIncidentStore::ReadStage::Missing, read.stage); EXPECT_TRUE(read.api_checked);
  }
  command("health detail"); drainCommands();
  EXPECT_NE(std::string::npos, Serial.bytes.find("storage_boot=not-attempted mounted=yes"));
  EXPECT_EQ(std::string::npos, Serial.bytes.find("reply capacity exceeded"));
}

TEST_F(CompanionServices, UnknownPeerCannotRestoreOlderIncidentOrBeOverwritten) {
  FallIncidentStore::Incident original; original.incident=41; original.active=true;
  ASSERT_TRUE(saveHealthIncident(mesh.store.fs,original).success());
  mesh.store.fs.fail_read_slot=1; const unsigned writes=mesh.store.fs.writes;
  mesh.restoreFallIncident();
  EXPECT_EQ(FallIncidentStore::Corrupt,mesh.recovery_outcome);
  EXPECT_FALSE(mesh.fallAlarmActive()); EXPECT_TRUE(mesh.persistence_fault);
  arm(); loopFall();
  EXPECT_TRUE(mesh.fallAlarmActive()); EXPECT_TRUE(mesh.persistence_dirty);
  EXPECT_EQ(FallIncidentStore::SaveStage::ReadExisting,mesh.persistence_result.stage);
  EXPECT_EQ(1u,mesh.persistence_result.read_result.slot);
  EXPECT_EQ(writes,mesh.store.fs.writes);
}

TEST_F(CompanionServices, LaterSuccessfulSaveDoesNotRewriteFailedBootObservation) {
  mesh.setStorageStartupResult(false); mesh.store.fs.mounted=false;
  mesh.restoreFallIncident();
  mesh.store.fs.mounted=true;
  arm(); loopFall();
  EXPECT_FALSE(mesh.persistence_fault); EXPECT_FALSE(mesh.persistence_dirty);
  EXPECT_EQ(FallIncidentStore::Corrupt,mesh.recovery_outcome);
  EXPECT_EQ(FallIncidentStore::ReadStage::Mount,mesh.recovery_reads[0].stage);
  EXPECT_EQ(FallIncidentStore::ReadStage::Success,mesh.persistence_result.read_result.stage);
  EXPECT_TRUE(mesh.persistence_result.read_result.api_checked);
  command("health detail"); drainCommands();
  EXPECT_NE(std::string::npos,Serial.bytes.find("storage_boot=failed mounted=yes"));
  EXPECT_NE(std::string::npos,Serial.bytes.find("boot_read0=mount"));
  EXPECT_NE(std::string::npos,Serial.bytes.find("save_read=valid-record"));
  EXPECT_EQ(std::string::npos,Serial.bytes.find("reply capacity exceeded"));
}

// All error classes cross both user resolutions and unsigned timer rollover.
using ReadFailureCase = std::tuple<int, bool, bool>;
class CompanionReadFailure : public CompanionServices, public testing::WithParamInterface<ReadFailureCase> {
protected:
  uint32_t initialMillis() const override { return std::get<2>(GetParam()) ? UINT32_MAX-1000 : 0; }
  void fail(bool enabled) {
    auto& fs=mesh.store.fs;
    switch (std::get<0>(GetParam())) {
      case 0:fs.mounted=!enabled;break;
      case 1:fs.fail_read_slot=enabled?0:-1;break;
      case 2:fs.fail_read_stat=enabled;break;
      case 3:fs.fail_read_data=enabled;break;
      case 4:fs.fail_read_close=enabled;break;
    }
  }
};
TEST_P(CompanionReadFailure, ResolutionSurvivesReadFailureAndNewIntentSupersedesBeforeRetry) {
  arm(); const uint32_t first=loopFall(); const unsigned writes=mesh.store.fs.writes;
  fail(true);
  loopClick(first+200); loopClick(first+400);
  if (std::get<1>(GetParam())) loopAt(first+200+HealthNodeConfig::click_window_ms+1);
  else loopClick(first+600);
  ASSERT_FALSE(mesh.fallAlarmActive()); EXPECT_FALSE(host_pwm_attached);
  EXPECT_TRUE(mesh.persistence_fault); EXPECT_TRUE(mesh.persistence_dirty);
  EXPECT_EQ(FallIncidentStore::SaveStage::ReadExisting,mesh.persistence_result.stage);
  EXPECT_TRUE(mesh.persistence_result.read_result.uncertain());
  EXPECT_EQ(writes,mesh.store.fs.writes);
  fail(false);
  for (unsigned i=0;i<25;++i) loopSample();
  const uint32_t second=loopFall(); ASSERT_LT(uint32_t(second-first),5000u);
  EXPECT_FALSE(mesh.persistence_dirty); EXPECT_FALSE(mesh.persistence_fault);
  EXPECT_EQ(2u,mesh.fall_response.incidentId()); EXPECT_TRUE(mesh.fallAlarmActive());
  HostIncidentReadIO io{mesh.store.fs}; FallIncidentStore::CheckedIncidentReader<HostIncidentReadIO> reader(io);
  const auto saved=FallIncidentStore::loadWithReader(reader,0);
  ASSERT_TRUE(saved.shouldResume()); EXPECT_EQ(2u,saved.value.incident);
  EXPECT_EQ(0u,saved.value.recovery_count);
}
INSTANTIATE_TEST_SUITE_P(AllReadFailures,CompanionReadFailure,
    testing::Combine(testing::Range(0,5),testing::Bool(),testing::Bool()));

TEST(CompanionStorageStartup, ActualBoundaryPreservesFormatArgumentAndStoreMeshOrderForBothResults) {
  for (bool mounted : {false,true}) {
    std::vector<std::string> calls;
    struct FS {
      std::vector<std::string>& calls; bool result;
      bool begin(bool format) { EXPECT_TRUE(format); calls.push_back("mount"); return result; }
    } fs{calls,mounted};
    struct Store { std::vector<std::string>& calls; void begin(){calls.push_back("store");} } store{calls};
    struct Network {
      std::vector<std::string>& calls; MyMesh& mesh;
      void setStorageStartupResult(bool ok) { calls.push_back("observe"); mesh.setStorageStartupResult(ok); }
      void begin(bool display) { EXPECT_TRUE(display); calls.push_back("mesh"); }
    };
    MyMesh mesh; Network network{calls,mesh};
    startCompanionStorage(fs,store,network,true);
    EXPECT_EQ((std::vector<std::string>{"mount","observe","store","mesh"}),calls);
    EXPECT_TRUE(mesh.storage_startup_attempted); EXPECT_EQ(mounted,mesh.storage_startup_ok);
  }
}

// Recovery must not retry old boot intent after an I/O boundary becomes healthy.
// A valid older active record is paired with a newer resolved record so a failed
// inspection cannot be mistaken for permission to restore the older alarm.
class CompanionBootReadFailure : public CompanionServices, public testing::WithParamInterface<int> {};
TEST_P(CompanionBootReadFailure, FailedBootInspectionStaysHistoricalAfterStorageRecovers) {
  FallIncidentStore::Incident incident; incident.incident = 41; incident.active = true;
  ASSERT_TRUE(saveHealthIncident(mesh.store.fs, incident).success());
  incident.active = false;
  ASSERT_TRUE(saveHealthIncident(mesh.store.fs, incident).success());
  auto& fs = mesh.store.fs;
  switch (GetParam()) {
    case 0: fs.mounted = false; break;
    case 1: fs.fail_read_slot = 1; break;
    case 2: fs.fail_read_stat = true; break;
    case 3: fs.fail_read_data = true; break;
    case 4: fs.fail_read_close = true; break;
  }
  const auto files = fs.files;
  const auto writes = fs.writes;
  mesh.restoreFallIncident();
  ASSERT_EQ(FallIncidentStore::Corrupt, mesh.recovery_outcome);
  EXPECT_TRUE(mesh.persistence_fault); EXPECT_FALSE(mesh.persistence_dirty);
  EXPECT_FALSE(mesh.fallAlarmActive()); EXPECT_FALSE(host_pwm_attached);
  const auto boot_stage = mesh.recovery_reads[1].stage;
  const auto reads = fs.read_opens;
  fs.mounted = true; fs.fail_read_slot = -1;
  fs.fail_read_stat = fs.fail_read_data = fs.fail_read_close = false;
  for (unsigned i = 0; i < 300; ++i) loopSample();
  EXPECT_EQ(reads, fs.read_opens); EXPECT_EQ(writes, fs.writes); EXPECT_EQ(files, fs.files);
  EXPECT_FALSE(mesh.fallAlarmActive()); EXPECT_FALSE(host_pwm_attached);
  EXPECT_EQ(FallIncidentStore::Corrupt, mesh.recovery_outcome);
  EXPECT_EQ(boot_stage, mesh.recovery_reads[1].stage);
  EXPECT_TRUE(mesh.persistence_fault);
  loopFall(); // Fresh evidence can still open and persist a new incident.
  EXPECT_FALSE(mesh.persistence_fault); EXPECT_FALSE(mesh.persistence_dirty);
  EXPECT_EQ(FallIncidentStore::Corrupt, mesh.recovery_outcome);
  EXPECT_EQ(boot_stage, mesh.recovery_reads[1].stage);
}
INSTANTIATE_TEST_SUITE_P(AllBootReadFailures, CompanionBootReadFailure, testing::Range(0, 5));

TEST_F(CompanionServices, StorageDetailSnapshotSurvivesBackpressureWithoutReadingRecordsOrLeakingKeys) {
  arm(); mesh.setStorageStartupResult(true);
  const char* secret = "123456789abcdef00123456789abcdef";
  ASSERT_TRUE(FallChannel::parse("private sensitive 123456789abcdef00123456789abcdef", mesh.durable_incident.channel));
  mesh.recovery_outcome = FallIncidentStore::Corrupt;
  for (auto& read : mesh.recovery_reads) {
    read.stage = FallIncidentStore::ReadStage::Read;
    read.error = INT32_MAX; read.close_error = INT32_MIN; read.api_checked = true;
  }
  mesh.persistence_result.stage = FallIncidentStore::SaveStage::Verify;
  mesh.persistence_result.read_result = mesh.recovery_reads[1];
  mesh.persistence_result.read_result.slot = 1;
  const auto reads = mesh.store.fs.read_opens;
  const auto writes = mesh.store.fs.writes;
  command("health detail");
  const auto samples = sensors.motion.diagnostics().samples;
  for (unsigned i = 0; i < 50; ++i) loopSample(); // USB worker deliberately not advanced.
  EXPECT_TRUE(mesh.usb_reply.pending()); EXPECT_EQ(8u, host_usb_queue.count());
  EXPECT_GT(sensors.motion.diagnostics().samples, samples);
  EXPECT_EQ(0u, Serial.write_calls); EXPECT_EQ(0u, Serial.readiness_queries);
  mesh.store.fs.mounted = false;
  drainCommands();
  EXPECT_NE(std::string::npos, Serial.bytes.find("storage_boot=ok mounted=yes format_history=unknown"));
  EXPECT_NE(std::string::npos, Serial.bytes.find("save_read=read slot=1 errno=2147483647 close=-2147483648 checked=1"));
  EXPECT_NE(std::string::npos, Serial.bytes.find("startup_gaps=")); // Final reply field survives.
  EXPECT_EQ(std::string::npos, Serial.bytes.find("reply capacity exceeded"));
  EXPECT_EQ(std::string::npos, Serial.bytes.find(secret));
  EXPECT_EQ(std::string::npos, Serial.bytes.find("sensitive"));
  EXPECT_EQ(reads, mesh.store.fs.read_opens); EXPECT_EQ(writes, mesh.store.fs.writes);
  Serial.bytes.clear(); command("health detail"); drainCommands();
  EXPECT_NE(std::string::npos, Serial.bytes.find("storage_boot=ok mounted=no"));
  EXPECT_EQ(reads, mesh.store.fs.read_opens); EXPECT_EQ(writes, mesh.store.fs.writes);
}
