#include <gtest/gtest.h>
#include "../mocks/HealthCaptureFixture.h"
#include "../../tools/health_replay_engine.h"

static bool replayBytes(HealthReplay& replay,const std::vector<uint8_t>& bytes) {
  for(size_t offset=0;offset<bytes.size();) {
    if(bytes.size()-offset<7) return false;
    size_t length=size_t(bytes[offset+1])+(size_t(bytes[offset+2])<<8)+7;
    if(length>bytes.size()-offset || !replay.feed(bytes.data()+offset,length)) return false;
    offset+=length;
  }
  return replay.finish();
}
static size_t recordAt(const std::vector<uint8_t>& bytes,MotionCapture::Kind kind,unsigned occurrence=0) {
  for(size_t offset=0;offset<bytes.size();) {
    if(bytes[offset]==kind && occurrence--==0) return offset;
    offset+=size_t(bytes[offset+1])+(size_t(bytes[offset+2])<<8)+7;
  }
  return bytes.size();
}
TEST(MotionCaptureCodec, PortableCheckpointIsBoundedAndPreservesInFlightCandidate) {
  HealthCaptureFixture f; f.hold(1200); f.sample(.3f); f.begin(); f.sample(3,0,300); f.end();
  HealthReplay replay(f.source); ASSERT_TRUE(replayBytes(replay,f.bytes))<<replay.failure();
  EXPECT_EQ(1u,replay.totals.falls); EXPECT_LT(f.bytes[1]+(size_t(f.bytes[2])<<8),2048u);
}
TEST(MotionCaptureCodec, StrongEventAndRejectedCandidateRoundTripDeterministically) {
  HealthCaptureFixture f; f.begin(); f.hold(1200); f.sample(.3f); f.hold(6200); f.sample(.3f); f.sample(3,0,300); f.hold(1400,1.4f,0,40); f.end();
  HealthReplay first(f.source),second(f.source);
  ASSERT_TRUE(replayBytes(first,f.bytes))<<first.failure(); ASSERT_TRUE(replayBytes(second,f.bytes))<<second.failure();
  EXPECT_EQ(first.summary(),second.summary()); EXPECT_EQ(1u,first.totals.falls); EXPECT_GE(first.totals.rejections,1u);
}
TEST(MotionCaptureCodec, SlowContextAndLaterStillnessUseProductionResponse) {
  HealthCaptureFixture f; f.begin(); f.hold(1200); f.sample(.8f,20,20); f.sample(1.2f,40,20); f.hold(2200,1,70);
  ASSERT_TRUE(f.response.alarmActive()); f.hold(31000,1.4f,70,40); f.hold(11000,1,70); f.end();
  HealthReplay replay(f.source); ASSERT_TRUE(replayBytes(replay,f.bytes))<<replay.failure(); EXPECT_EQ(1u,replay.totals.falls);
}
TEST(MotionCaptureCodec, RawCorrectedAndCalibrationInterruptionAreExplicit) {
  HealthCaptureFixture f; f.calibration.bias[0]=.125f; f.calibration.bias[1]=-.25f; f.calibration.samples=700;
  f.begin(); f.hold(1200); f.sample(.3f); f.calibrationRecord(true); f.invalidate(1);
  for(unsigned i=0;i<20;++i) f.sample(1,0,0,20,true);
  f.calibrationRecord(false); f.invalidate(1); f.hold(240); f.sample(3,0,300); f.end();
  HealthReplay replay(f.source); ASSERT_TRUE(replayBytes(replay,f.bytes))<<replay.failure(); EXPECT_EQ(0u,replay.totals.falls);
}
TEST(MotionCaptureCodec, RejectsCorrectionsThatTheProductionSensorCannotApply) {
  for(unsigned invalid=0;invalid<5;++invalid) {
    SCOPED_TRACE(invalid);
    HealthCaptureFixture f;
    f.calibration.samples=700;
    switch(invalid) {
      case 0: f.calibration.noise[0]=-.01f; break;
      case 1: f.calibration.bias[1]=HealthNodeConfig::gyro_calibration_max_bias_dps+1; break;
      case 2: f.calibration.samples=HealthNodeConfig::gyro_calibration_min_samples-1; break;
      case 3: f.calibration.temperature_span=-1; break;
      case 4: f.calibration.samples=0; f.calibration.bias[2]=.1f; break;
    }
    ASSERT_FALSE(GyroCalibration::validResult(f.calibration));
    // Fingerprints and corrected samples agree, so this requires validation of
    // the correction itself rather than transport corruption detection.
    f.begin(); f.sample(); f.end();
    HealthReplay replay(f.source); EXPECT_FALSE(replayBytes(replay,f.bytes));
    EXPECT_NE(std::string::npos,replay.failure().find("invalid calibration values"));
  }
}
TEST(MotionCaptureCodec, CalibrationReplacementAndResetReplayWithExactCorrections) {
  HealthCaptureFixture f; f.begin(); f.hold(1200);
  f.calibration.bias[0]=.125f; f.calibration.bias[1]=-.25f; f.calibration.samples=700;
  f.calibrationRecord(false); f.invalidate(1); f.hold(240);
  f.calibration=GyroCalibration::Result{};
  f.calibrationRecord(false); f.invalidate(1); f.hold(240); f.end();
  HealthReplay replay(f.source); ASSERT_TRUE(replayBytes(replay,f.bytes))<<replay.failure();
  EXPECT_EQ(3u,replay.totals.calibration_changes);
}
TEST(MotionCaptureCodec, AcquisitionLossBreaksEvidenceWithoutInventingQuiet) {
  HealthCaptureFixture f; f.begin(); f.hold(1200); f.sample(.3f); f.discontinuity(); f.hold(240); f.sample(3,0,300); f.end();
  HealthReplay replay(f.source); ASSERT_TRUE(replayBytes(replay,f.bytes))<<replay.failure();
  EXPECT_EQ(0u,replay.totals.falls); EXPECT_EQ(1u,replay.totals.acquisition_changes);
}
TEST(MotionCaptureCodec, MillisecondRolloverReplays) {
  HealthCaptureFixture f; f.now=UINT32_MAX-1300; f.begin(); f.hold(1200); f.sample(.3f); f.sample(3,0,300); f.hold(400); f.end();
  HealthReplay replay(f.source); ASSERT_TRUE(replayBytes(replay,f.bytes))<<replay.failure(); EXPECT_EQ(1u,replay.totals.falls);
}
TEST(MotionCaptureCodec, SampleSequenceRolloverAndDiagnosticCounterResetReplay) {
  HealthCaptureFixture f; f.sample_sequence=UINT32_MAX-1; f.discarded=10;
  f.begin(); f.sample(); f.sample();
  ASSERT_EQ(0u,f.sample_sequence);
  f.discarded=0;
  f.record(MotionCapture::Acquisition,[&](MotionCapture::Writer& w){
    w.u32(f.now); w.u32(f.generation); w.u32(0); w.u32(0); w.u32(0);
  });
  f.sample(); f.end();
  HealthReplay replay(f.source); ASSERT_TRUE(replayBytes(replay,f.bytes))<<replay.failure();
  EXPECT_EQ(3u,replay.totals.samples); EXPECT_EQ(0u,replay.totals.invalidations);
}
TEST(MotionCaptureCodec, MetadataMayPrecedeAnOperationWithEarlierLogicalTimestamp) {
  HealthCaptureFixture f; f.begin();
  f.record(MotionCapture::Buzzer,[&](MotionCapture::Writer& w){ w.u32(25); w.u8(1); });
  f.sample(1,0,0,20); f.now=30; f.end();
  HealthReplay replay(f.source); ASSERT_TRUE(replayBytes(replay,f.bytes))<<replay.failure();
}
TEST(MotionCaptureCodec, MidIncidentCheckpointRetainsDeadlineAndButtonProgress) {
  HealthCaptureFixture f; f.hold(1200); f.sample(.3f); f.sample(3,0,300);
  f.now+=100; f.service(true); f.now+=30; f.service(true); f.now+=100; f.service(false); f.now+=30; f.service(false);
  f.begin(); f.now+=100; f.service(true); f.now+=30; f.service(true); f.now+=100; f.service(false); f.now+=30; f.service(false);
  f.now+=3000; f.service(); f.end();
  HealthReplay replay(f.source); ASSERT_TRUE(replayBytes(replay,f.bytes))<<replay.failure(); EXPECT_FALSE(f.response.alarmActive());
}
TEST(MotionCaptureCodec, CheckpointNeverExportsChannelNamesOrKeys) {
  HealthCaptureFixture f; FallChannel private_channel; strcpy(private_channel.name,"PRIVATE_CHANNEL_MARKER"); memset(private_channel.key,0x9a,32);
  private_channel.key_length=32; f.response.onFall(0,false,private_channel); f.begin(); f.end();
  std::string bytes(f.bytes.begin(),f.bytes.end()); EXPECT_EQ(std::string::npos,bytes.find("PRIVATE_CHANNEL_MARKER"));
  EXPECT_EQ(std::string::npos,bytes.find(std::string(16,char(0x9a))));
}
TEST(MotionCaptureCodec, RejectsVersionSourceConfigAndMalformedCheckpoint) {
  HealthCaptureFixture f; f.begin(); f.end();
  for(size_t offset:{7u,9u,73u,105u}) {
    auto changed=f.bytes; changed[offset]^=0x40; HealthReplay replay(f.source); EXPECT_FALSE(replayBytes(replay,changed))<<offset;
  }
}
TEST(MotionCaptureCodec, RejectsUnknownRecordAndAmbiguousSession) {
  HealthCaptureFixture f; f.begin(); f.end(); auto changed=f.bytes; changed[0]=99;
  HealthReplay unknown(f.source); EXPECT_FALSE(replayBytes(unknown,changed));
  auto doubled=f.bytes; doubled.insert(doubled.end(),f.bytes.begin(),f.bytes.end());
  HealthReplay ambiguous(f.source); EXPECT_FALSE(replayBytes(ambiguous,doubled));
}
TEST(MotionCaptureCodec, RejectsRecordingLossAndMissingEnd) {
  HealthCaptureFixture f; f.begin(); f.hold(240); f.end();
  auto changed=f.bytes; size_t sample=recordAt(changed,MotionCapture::Sample); changed[sample+3]++;
  HealthReplay lost(f.source); EXPECT_FALSE(replayBytes(lost,changed));
  auto truncated=f.bytes; truncated.resize(recordAt(truncated,MotionCapture::End));
  HealthReplay incomplete(f.source); EXPECT_FALSE(replayBytes(incomplete,truncated));
}
TEST(MotionCaptureCodec, RejectsUndeclaredCalibrationAndNonfiniteSamples) {
  HealthCaptureFixture f; f.begin(); f.hold(240); f.end(); const size_t sample=recordAt(f.bytes,MotionCapture::Sample);
  auto changed=f.bytes; changed[sample+7+12+56+2]^=1;
  HealthReplay calibration(f.source); EXPECT_FALSE(replayBytes(calibration,changed));
  changed=f.bytes; const uint32_t nan=0x7fc00000; memcpy(changed.data()+sample+7+12,&nan,4);
  HealthReplay invalid(f.source); EXPECT_FALSE(replayBytes(invalid,changed));
}
TEST(MotionCaptureCodec, WriterCannotOverrunAndReaderRejectsInvalidBooleans) {
  uint8_t bytes[2]={0,0x55}; MotionCapture::Writer writer(bytes,1); writer.u32(123); EXPECT_FALSE(writer.ok()); EXPECT_EQ(0x55,bytes[1]);
  bytes[0]=2; MotionCapture::Reader reader(bytes,1); reader.boolean(); EXPECT_FALSE(reader.ok());
}
TEST(MotionCaptureCodec, ResponseFingerprintChangesOnlyForStateChanges) {
  FallResponse response; auto initial=MotionCapture::responseFingerprint(response); response.updateDeadline(100);
  EXPECT_EQ(initial,MotionCapture::responseFingerprint(response)); response.onFall(100,false);
  EXPECT_NE(initial,MotionCapture::responseFingerprint(response));
}
int main(int argc,char** argv) { ::testing::InitGoogleTest(&argc,argv); return RUN_ALL_TESTS(); }
