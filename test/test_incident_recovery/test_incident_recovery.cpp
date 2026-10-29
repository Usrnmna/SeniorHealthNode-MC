#include <gtest/gtest.h>
#include "../../examples/companion_radio/health/FallIncidentStore.h"
#include "../../examples/companion_radio/health/FallResponse.h"
#include <map>
#include <string>
#include <vector>
#include <algorithm>

namespace {
struct MemoryFS {
  std::map<std::string, std::vector<uint8_t>> files;
  size_t write_limit = 10000;
  bool fail_open = false, corrupt_write = false, fail_readback = false;
  unsigned fail_write_number = 0;
  unsigned writes = 0;
  struct File {
    MemoryFS* fs; std::vector<uint8_t>* bytes;
    explicit operator bool() const { return bytes; }
    size_t size() const { return bytes->size(); }
    size_t read(uint8_t* dest, size_t len) {
      len=std::min(len,bytes->size()); memcpy(dest,bytes->data(),len); return len;
    }
    size_t write(const uint8_t* src, size_t len) {
      ++fs->writes; len=std::min(len,fs->write_limit);
      if (fs->fail_write_number == fs->writes) len=std::min(len,size_t(8));
      bytes->assign(src,src+len);
      if (fs->corrupt_write && len) (*bytes)[0]^=1;
      return len;
    }
    void flush() {}
    void close() {}
  };
  bool exists(const char* name) { return files.count(name); }
  File open(const char* name, const char* mode) {
    if (fail_open || (*mode=='r' && fail_readback && writes)) return {this,nullptr};
    if (*mode=='w') { files[name].clear(); return {this,&files[name]}; }
    auto found=files.find(name); return {this,found==files.end() ? nullptr : &found->second};
  }
};
FallIncidentStore::Incident incident() {
  FallIncidentStore::Incident result;
  result.active=true; result.incident=17; result.origin_epoch=1800000000;
  return result;
}
bool click(FallResponse& response, uint32_t now) {
  response.updateButton(now,true); response.updateButton(now+25,true);
  response.updateButton(now+100,false); return response.updateButton(now+125,false);
}
void quiet(FallResponse& response, uint32_t start, uint32_t duration) {
  for (uint32_t offset=0;offset<=duration;offset+=20) response.observeMotion(start+offset,0,0,1,0);
}

TEST(IncidentStorage, MissingRecordAndCompleteChannelRoundTrip) {
  MemoryFS fs;
  EXPECT_EQ(FallIncidentStore::Missing,FallIncidentStore::load(fs,1800000000).outcome);
  auto value=incident();
  ASSERT_TRUE(FallChannel::parse("private Care 00112233445566778899aabbccddeeff",value.channel));
  ASSERT_TRUE(FallIncidentStore::save(fs,value));
  const auto loaded=FallIncidentStore::load(fs,value.origin_epoch+12,true);
  EXPECT_EQ(FallIncidentStore::Resume,loaded.outcome); EXPECT_TRUE(loaded.shouldResume());
  EXPECT_EQ(12000u,loaded.elapsed_ms); EXPECT_EQ(17u,loaded.value.incident);
  EXPECT_STREQ("Care",loaded.value.channel.name);
  EXPECT_EQ(0,memcmp(value.channel.key,loaded.value.channel.key,32));
  EXPECT_EQ(1u,fs.writes); // load must not write.
}
TEST(IncidentStorage, ResolutionTombstonePreventsEarlierActiveRecordFromWinning) {
  MemoryFS fs; auto value=incident();
  ASSERT_TRUE(FallIncidentStore::save(fs,value));
  value.active=false; ASSERT_TRUE(FallIncidentStore::save(fs,value));
  auto result=FallIncidentStore::load(fs,value.origin_epoch+20);
  EXPECT_EQ(FallIncidentStore::Resolved,result.outcome); EXPECT_FALSE(result.shouldResume());
  value.active=true; ++value.incident; ASSERT_TRUE(FallIncidentStore::save(fs,value));
  result=FallIncidentStore::load(fs,value.origin_epoch+20);
  EXPECT_TRUE(result.shouldResume()); EXPECT_EQ(18u,result.value.incident);
}
TEST(IncidentStorage, PartialResolutionIsUncertainAndCannotResurrectOldAlarm) {
  MemoryFS fs; auto value=incident();
  ASSERT_TRUE(FallIncidentStore::save(fs,value)); value.active=false; fs.write_limit=25;
  EXPECT_FALSE(FallIncidentStore::save(fs,value));
  auto result=FallIncidentStore::load(fs,value.origin_epoch+20);
  EXPECT_EQ(FallIncidentStore::Corrupt,result.outcome); EXPECT_FALSE(result.shouldResume());
  EXPECT_EQ(17u,result.value.incident);
  fs.write_limit=10000; ASSERT_TRUE(FallIncidentStore::save(fs,value));
  EXPECT_EQ(FallIncidentStore::Resolved,FallIncidentStore::load(fs,value.origin_epoch+20).outcome);
}
TEST(IncidentStorage, ChecksumCorruptionAndReadbackFailureAreVisible) {
  MemoryFS fs; auto value=incident();
  ASSERT_TRUE(FallIncidentStore::save(fs,value));
  fs.files[FallIncidentStore::path(0)][0]^=1;
  EXPECT_EQ(FallIncidentStore::Corrupt,FallIncidentStore::load(fs,value.origin_epoch).outcome);
  MemoryFS writeback; writeback.fail_readback=true;
  EXPECT_FALSE(FallIncidentStore::save(writeback,value));
  MemoryFS corrupt; corrupt.corrupt_write=true;
  EXPECT_FALSE(FallIncidentStore::save(corrupt,value));
}
TEST(IncidentStorage, RepairOfOneOrTwoDamagedRecordsProducesLoadableIntent) {
  for (bool two : {false, true}) {
    MemoryFS fs; auto value=incident();
    fs.files[FallIncidentStore::path(0)]={1,2,3};
    if (two) fs.files[FallIncidentStore::path(1)]={4,5,6};
    ASSERT_TRUE(FallIncidentStore::save(fs,value));
    EXPECT_EQ(FallIncidentStore::Resume,FallIncidentStore::load(fs,value.origin_epoch+1,true).outcome);
    EXPECT_EQ(2u,fs.writes);
  }
}
TEST(IncidentStorage, TornSecondRepairNeverReportsCommitSuccessAndCanBeRetried) {
  MemoryFS fs; auto value=incident(); fs.files[FallIncidentStore::path(0)]={1,2,3};
  fs.fail_write_number=2;
  EXPECT_FALSE(FallIncidentStore::save(fs,value));
  EXPECT_EQ(FallIncidentStore::Corrupt,FallIncidentStore::load(fs,value.origin_epoch+1).outcome);
  fs.fail_write_number=0; ASSERT_TRUE(FallIncidentStore::save(fs,value));
  EXPECT_EQ(FallIncidentStore::Resume,FallIncidentStore::load(fs,value.origin_epoch+1,true).outcome);
}
TEST(IncidentStorage, UnreadableRecordsAreNeverOverwritten) {
  MemoryFS fs; auto value=incident(); ASSERT_TRUE(FallIncidentStore::save(fs,value));
  const auto before=fs.files; fs.fail_open=true; value.active=false;
  EXPECT_FALSE(FallIncidentStore::save(fs,value)); EXPECT_EQ(before,fs.files);
  EXPECT_EQ(FallIncidentStore::Corrupt,FallIncidentStore::load(fs,value.origin_epoch).outcome);
}
TEST(IncidentStorage, KnownAgeExpiresAtBoundaryAndCapsWarningAge) {
  MemoryFS fs; auto value=incident(); ASSERT_TRUE(FallIncidentStore::save(fs,value));
  const auto age=FallIncidentStore::maximum_age_seconds;
  auto recovery=FallIncidentStore::load(fs,value.origin_epoch+age-1,true);
  EXPECT_TRUE(recovery.shouldResume()); EXPECT_EQ(HealthNodeConfig::response_timeout_ms,recovery.elapsed_ms);
  recovery=FallIncidentStore::load(fs,value.origin_epoch+age,true);
  EXPECT_EQ(FallIncidentStore::Expired,recovery.outcome); EXPECT_FALSE(recovery.shouldResume());
}
TEST(IncidentStorage, UnknownOrRollbackClocksUseFiniteRecoveryBudget) {
  for (uint32_t now : {0u,1699999999u,1799999999u}) {
    MemoryFS fs; auto value=incident(); ASSERT_TRUE(FallIncidentStore::save(fs,value));
    for (unsigned count=0;count<FallIncidentStore::uncertain_recovery_limit;++count) {
      auto recovery=FallIncidentStore::load(fs,now,true);
      ASSERT_EQ(FallIncidentStore::UncertainClock,recovery.outcome);
      EXPECT_EQ(HealthNodeConfig::response_timeout_ms,recovery.elapsed_ms);
      FallIncidentStore::countRecovery(recovery.value);
      ASSERT_TRUE(FallIncidentStore::save(fs,recovery.value));
    }
    EXPECT_EQ(FallIncidentStore::RecoveryLimit,FallIncidentStore::load(fs,now,true).outcome);
  }
}
TEST(IncidentStorage, UnknownOriginRemainsUnknownEvenWithPlausibleCurrentClock) {
  MemoryFS fs; auto value=incident(); value.origin_epoch=0;
  ASSERT_TRUE(FallIncidentStore::save(fs,value));
  EXPECT_EQ(FallIncidentStore::UncertainClock,FallIncidentStore::load(fs,1800000000,true).outcome);
  value.recovery_count=255; FallIncidentStore::countRecovery(value); EXPECT_EQ(255u,value.recovery_count);
}
TEST(IncidentStorage, PlausibleFallbackEpochDoesNotProveClockContinuity) {
  MemoryFS fs; auto value=incident(); value.origin_epoch=1715770351;
  ASSERT_TRUE(FallIncidentStore::save(fs,value));
  const auto recovery=FallIncidentStore::load(fs,value.origin_epoch+60);
  EXPECT_EQ(FallIncidentStore::UncertainClock,recovery.outcome);
  EXPECT_EQ(HealthNodeConfig::response_timeout_ms,recovery.elapsed_ms);
  EXPECT_EQ(FallIncidentStore::UncertainClock,
      FallIncidentStore::load(fs,value.origin_epoch+FallIncidentStore::maximum_age_seconds,false).outcome);
  value.recovery_count=FallIncidentStore::uncertain_recovery_limit;
  ASSERT_TRUE(FallIncidentStore::save(fs,value));
  EXPECT_EQ(FallIncidentStore::RecoveryLimit,FallIncidentStore::load(fs,value.origin_epoch+60).outcome);
}
TEST(IncidentStorage, GenerationOrderSurvivesWrap) {
  MemoryFS fs; auto value=incident(); ASSERT_TRUE(FallIncidentStore::save(fs,value));
  auto& bytes=fs.files[FallIncidentStore::path(0)];
  FallIncidentStore::put32(bytes.data()+4,UINT32_MAX);
  mesh::Utils::sha256(bytes.data()+FallIncidentStore::checksum_offset,16,bytes.data(),FallIncidentStore::checksum_offset);
  value.active=false; ASSERT_TRUE(FallIncidentStore::save(fs,value));
  EXPECT_EQ(0u,FallIncidentStore::read(fs,1).generation);
  EXPECT_EQ(FallIncidentStore::Resolved,FallIncidentStore::load(fs,value.origin_epoch).outcome);
}
TEST(IncidentStorage, InvalidMetadataCannotReplaceGoodRecord) {
  MemoryFS fs; auto value=incident(); ASSERT_TRUE(FallIncidentStore::save(fs,value));
  const auto before=fs.files; value.incident=0; EXPECT_FALSE(FallIncidentStore::save(fs,value));
  value=incident(); memset(value.channel.name,'x',sizeof(value.channel.name));
  EXPECT_FALSE(FallIncidentStore::save(fs,value)); EXPECT_EQ(before,fs.files);
}
TEST(IncidentRecovery, RestoreUsesOriginalDeadlineAndRequiresNewMotion) {
  FallResponse response; FallChannel channel;
  ASSERT_TRUE(response.restore(100,20000,12,channel));
  response.updateDeadline(10099); EXPECT_FALSE(response.messagePending());
  response.updateDeadline(10100);
  EXPECT_EQ(FallNotice::UserCouldNotConfirm,response.pendingNotice()); EXPECT_TRUE(response.alarmActive());
  EXPECT_EQ(12u,response.incidentId());
}
TEST(IncidentRecovery, RestoredOldIncidentWarnsImmediatelyAndLaterQuietUpgradesOnce) {
  FallResponse response;
  ASSERT_TRUE(response.restore(100,60000,12,FallChannel{}));
  response.updateDeadline(100); ASSERT_EQ(FallNotice::UserCouldNotConfirm,response.pendingNotice());
  response.messageAccepted(); quiet(response,100,10000); response.updateDeadline(10100);
  EXPECT_EQ(FallNotice::NoResidualMovement,response.pendingNotice()); response.messageAccepted();
  quiet(response,10120,10000); response.updateDeadline(20120);
  EXPECT_FALSE(response.messagePending()); EXPECT_TRUE(response.alarmActive());
}
TEST(IncidentRecovery, IdentifierSeedAndRestorationSurviveWrapWithoutPendingCollision) {
  FallResponse response; response.seedIncident(UINT32_MAX);
  ASSERT_TRUE(response.onFall(UINT32_MAX-500,false)); EXPECT_EQ(1u,response.incidentId());
  click(response,UINT32_MAX-400); click(response,0); EXPECT_TRUE(click(response,400));
  const auto pending=response.pendingIncident(); ASSERT_TRUE(response.messagePending());
  ASSERT_TRUE(response.onFall(800,false)); EXPECT_NE(pending,response.incidentId());
  EXPECT_EQ(pending,response.pendingIncident());
}
TEST(IncidentRecovery, RestorePreservesOlderPendingConfirmationAndRejectsSameIncident) {
  FallResponse response; response.onFall(0,false); click(response,100); click(response,500);
  ASSERT_TRUE(response.updateButton(3101,false));
  const auto old=response.pendingIncident();
  EXPECT_FALSE(response.restore(4000,0,old,FallChannel{}));
  ASSERT_TRUE(response.restore(4000,30000,old+1,FallChannel{}));
  response.updateDeadline(4000);
  EXPECT_EQ(FallNotice::UserConfirmedFalseAlarm,response.pendingNotice());
  EXPECT_EQ(old,response.pendingIncident());
  response.messageAccepted(); response.updateDeadline(4001);
  EXPECT_EQ(FallNotice::UserCouldNotConfirm,response.pendingNotice());
  EXPECT_EQ(old+1,response.pendingIncident()); EXPECT_TRUE(response.alarmActive());
}
TEST(IncidentRecovery, ClockWrapAndHeldButtonDoNotAcknowledgeRecoveredIncident) {
  const uint32_t now=UINT32_MAX-1000; FallResponse response;
  ASSERT_TRUE(response.restore(now,29000,9,FallChannel{},true));
  response.updateDeadline(now+1000); EXPECT_EQ(FallNotice::UserCouldNotConfirm,response.pendingNotice());
  EXPECT_FALSE(response.updateButton(now+1100,true)); EXPECT_TRUE(response.alarmActive());
}
TEST(IncidentRecovery, MotionCapturedBeforeDeclarationCannotEstablishQuiet) {
  FallResponse response; ASSERT_TRUE(response.onFall(1000,false));
  quiet(response,0,980); response.updateDeadline(31000);
  EXPECT_EQ(FallNotice::UserCouldNotConfirm,response.pendingNotice());
}
}
int main(int argc,char** argv) { ::testing::InitGoogleTest(&argc,argv); return RUN_ALL_TESTS(); }
