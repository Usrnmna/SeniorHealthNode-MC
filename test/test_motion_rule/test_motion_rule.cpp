#include <gtest/gtest.h>
#include "../../examples/companion_radio/health/MotionRule.h"
#include "../../examples/companion_radio/health/MotionTrace.h"
#include "../../examples/companion_radio/health/FallResponse.h"

// Synthetic recordings establish logic and timing, not real-world accuracy.
struct Recording {
  MotionRule rule;
  uint32_t now; unsigned alerts=0;
  explicit Recording(uint32_t start=0, const FallDetectionConfig& cfg=FallDetectionConfig()) : rule(cfg),now(start) {}
  bool sample(float a=1,float r=0,bool valid=true,uint32_t step=20) {
    now+=step; const bool fired=rule.update(now,a,r,valid); if(fired) ++alerts; return fired;
  }
  bool vector(float a, float angle, float r=0, uint32_t step=20) {
    now+=step; const float rad=angle*0.0174532925f;
    const bool fired=rule.update(now,a*sinf(rad),0,a*cosf(rad),r,true);
    if(fired) ++alerts;
    return fired;
  }
  void hold(unsigned ms,float a=1,float r=0) { for(unsigned i=0;i<ms;i+=20) sample(a,r); }
  void arm() { hold(240); }
  void torso() { for(unsigned i=0;i<60;++i) vector(1,0); }
  void quietTilt(float degrees,unsigned ms) { for(unsigned i=0;i<ms;i+=20) vector(1,degrees); }
  void sequence() { sample(.3f); sample(3,300); }
};

// Replay accepted samples across a complete timestamp cycle. The test-specific
// 1000 ms continuity limit reduces replay cost without changing feature lifetimes.
static void advanceWithoutNewFeatures(Recording& recording, uint64_t duration, float acceleration) {
  const uint32_t interval = recording.rule.settings().max_sample_gap_ms;
  while (duration) {
    const uint32_t step = duration > interval ? interval : uint32_t(duration);
    recording.vector(acceleration, 0, 0, step);
    duration -= step;
  }
}

TEST(MotionRule, RecoveryRequiresFreshCoverageButNotStillness) {
  Recording r; r.sequence(); EXPECT_EQ(0u,r.alerts);
  r.hold(240,1.4f,40); EXPECT_TRUE(r.rule.available());
  r.sequence(); EXPECT_EQ(1u,r.alerts);
}
TEST(MotionRule, StrongSequenceAlertsDespiteContinuedMovement) {
  Recording r;r.arm();r.sequence();r.hold(6000,1.5f,40);
  EXPECT_EQ(1u,r.alerts); EXPECT_EQ(MotionRule::StrongSequence,r.rule.eventKind());
}
TEST(MotionRule, SeparatePeaksEitherOrderAndTogether) {
  for(int order=0;order<3;++order) {
    Recording r;r.arm();r.sample(.3f);
    if(order==0){r.sample(3);r.sample(1,300);}
    if(order==1){r.sample(1,300);r.sample(3);}
    if(order==2)r.sample(3,300);
    EXPECT_EQ(1u,r.alerts);
  }
}
TEST(MotionRule, MissingFeaturesWithoutContextDoNotInventFall) {
  for(int missing=0;missing<3;++missing) {
    Recording r;r.arm();r.sample(missing==0?1:.3f);
    r.sample(missing==1?1:3,missing==2?0:300);r.hold(7000);
    EXPECT_EQ(0u,r.alerts);
  }
}
TEST(MotionRule, GyroJustBeforeTroughIsRetainedButStalePeakIsNot) {
  for(bool stale:{false,true}) {
    Recording r;r.arm();r.sample(1,300);if(stale)r.hold(320);
    r.sample(.3f);r.sample(3);EXPECT_EQ(stale?0u:1u,r.alerts);
  }
}
TEST(MotionRule, FixedStrongWindowBoundary) {
  for(bool late:{false,true}) {
    Recording r;r.arm();r.sample(.3f);r.hold(late?500:480);r.sample(3,300);
    EXPECT_EQ(late?0u:1u,r.alerts);
  }
}
TEST(MotionRule, ANewLowEpisodeHasItsOwnBoundedWindow) {
  for (bool vectors : {false, true}) {
    Recording r;
    if (vectors) r.torso(); else r.arm();
    auto sample = [&](float a, float rotation=0) {
      if (vectors) return r.vector(a,0,rotation);
      return r.sample(a,rotation);
    };
    sample(.3f);
    for (unsigned i=0;i<22;++i) sample(1);
    sample(.3f);
    EXPECT_EQ(MotionRule::LowEpisodeRenewed,r.rule.lastReason());
    EXPECT_FALSE(r.rule.decision());
    sample(1); sample(1);
    EXPECT_TRUE(sample(3,300)); // 60 ms after the renewed trough, 520 ms after the first.
    EXPECT_EQ(1u,r.alerts);
  }
}
TEST(MotionRule, SustainedLowDoesNotRestartOrExtendTheStrongWindow) {
  for (bool context : {false, true}) {
    FallDetectionConfig cfg; cfg.context_paths=context;
    Recording r(0,cfg); r.torso(); r.vector(.3f,0,20);
    for (unsigned i=0;i<350;++i) r.vector(.3f,0,20);
    EXPECT_EQ(MotionRule::Armed,r.rule.currentState());
    r.vector(3,0,300); r.quietTilt(0,1100);
    EXPECT_EQ(0u,r.alerts);
    r.vector(.3f,0); EXPECT_TRUE(r.vector(3,0,300));
  }
}
TEST(MotionRule, ReboundCanCompleteTheOriginalLowImpactSequence) {
  for (uint32_t start : {0u,UINT32_MAX-299}) {
    Recording r(start); r.arm(); r.sample(.3f); r.sample(3); r.sample(.3f);
    EXPECT_EQ(MotionRule::LowEpisodeRenewed,r.rule.lastReason());
    EXPECT_TRUE(r.sample(1,300));
    EXPECT_EQ(1u,r.alerts); EXPECT_EQ(MotionRule::StrongSequence,r.rule.eventKind());
    EXPECT_FLOAT_EQ(3,r.rule.peakAcceleration()); EXPECT_FLOAT_EQ(300,r.rule.peakRotation());
  }
}
TEST(MotionRule, RenewedLowCannotExtendAnEarlierPairDeadline) {
  for (bool late : {false,true}) {
    Recording r; r.arm(); r.sample(.3f); r.sample(3); r.hold(420); r.sample(.3f);
    EXPECT_EQ(MotionRule::LowEpisodeRenewed,r.rule.lastReason());
    r.hold(late ? 40 : 20); r.sample(1,300);
    EXPECT_EQ(late ? 0u : 1u,r.alerts);
  }
}
TEST(MotionRule, FaultClearsRetainedPairBeforeRecovery) {
  for (bool gap : {false,true}) {
    Recording r; r.arm(); r.sample(.3f); r.sample(3); r.sample(.3f);
    if (gap) r.sample(1,0,true,101); else r.sample(1,0,false);
    r.hold(240); r.sample(1,300);
    EXPECT_EQ(0u,r.alerts); r.sequence(); EXPECT_EQ(1u,r.alerts);
  }
}
TEST(MotionRule, FiringClearsPairsThroughRefractory) {
  FallDetectionConfig cfg; cfg.refractory_ms=100;
  Recording r(0,cfg); r.arm(); r.sample(.3f); r.sample(3); r.sample(.3f);
  ASSERT_TRUE(r.sample(1,300));
  r.hold(120); r.sample(1,300); EXPECT_EQ(1u,r.alerts);
  r.sequence(); EXPECT_EQ(2u,r.alerts);
}
TEST(MotionRule, APlateauFromRecoveryOrRefractoryNeedsAFreshLowEpisode) {
  for (bool refractory : {false, true}) {
    Recording r;
    if (refractory) { r.arm(); r.sequence(); }
    r.hold(1600,.3f,20); r.sample(3,300);
    EXPECT_EQ(refractory ? 1u : 0u,r.alerts);
    r.sequence();
    EXPECT_EQ(refractory ? 2u : 1u,r.alerts);
  }
}
TEST(MotionRule, ExpiredCandidateDoesNotRequireAnotherQuietPeriod) {
  Recording r;r.arm();r.sample(.3f);r.hold(520,1.4f,40);
  EXPECT_TRUE(r.rule.available());r.sequence();EXPECT_EQ(1u,r.alerts);
}
TEST(MotionRule, CandidateExpiryPreservesRecentIndependentRotation) {
  Recording r;r.arm();r.sample(.3f);r.hold(460);r.sample(1,300);r.sample();
  r.sample(.3f);r.sample(3);EXPECT_EQ(1u,r.alerts);
}
TEST(MotionRule, BriefFaultDuringMovementAllowsNewIndependentEvent) {
  Recording r;r.arm();r.sample(.3f);r.sample(1,0,false);
  r.sample(3,300);EXPECT_EQ(0u,r.alerts);
  r.hold(240,1.4f,40);r.sequence();EXPECT_EQ(1u,r.alerts);
}
TEST(MotionRule, GapAndInvalidValuesBreakEvidence) {
  for(float bad:{NAN,INFINITY,-1.0f}) {
    Recording r;r.arm();r.sample(.3f);r.sample(bad);r.sample(3,300);
    EXPECT_EQ(0u,r.alerts);r.hold(240);r.sequence();EXPECT_EQ(1u,r.alerts);
  }
  Recording r;r.arm();r.sample(.3f);r.sample(3,300,true,101);EXPECT_EQ(0u,r.alerts);
}
TEST(MotionRule, DuplicateTimestampCannotCompleteSequence) {
  Recording r;r.arm();r.sample(.3f);r.sample(3,300,true,0);EXPECT_EQ(0u,r.alerts);
}
TEST(MotionRule, DataLossAfterDeclarationDoesNotUndoEvent) {
  Recording r;r.arm();r.sequence();r.sample(1,0,false);EXPECT_EQ(1u,r.alerts);
}
TEST(MotionRule, NewFallsWithinSixtySecondsAreRecognized) {
  for(unsigned wait:{5000u,15000u,45000u}) {
    Recording r;r.arm();r.sequence();r.hold(wait);r.sequence();EXPECT_EQ(2u,r.alerts);
  }
}
TEST(MotionRule, BriefRefractorySuppressesImpactTrainWithoutQuietRequirement) {
  Recording r;r.arm();r.sequence();r.sequence();EXPECT_EQ(1u,r.alerts);
  r.hold(1300,1.4f,40);r.sequence();EXPECT_EQ(2u,r.alerts);
}
TEST(MotionRule, BriefFaultCannotBypassTheOriginalRefractoryDeadline) {
  Recording r;r.arm();r.sequence();r.sample(1,0,false);r.hold(240,1.4f,40);
  EXPECT_TRUE(r.rule.cooldownActive());r.sequence();EXPECT_EQ(1u,r.alerts);
  r.hold(1000);r.sequence();EXPECT_EQ(2u,r.alerts);
}
TEST(MotionRule, RefractoryCollectsOrientationHistoryForTheNextEvent) {
  Recording r;r.torso();r.vector(.3f,0);r.vector(3,0,300);
  r.quietTilt(0,1180);r.vector(3,0);r.quietTilt(70,1100);
  EXPECT_EQ(2u,r.alerts);EXPECT_EQ(MotionRule::ImpactOrientation,r.rule.eventKind());
}
TEST(MotionRule, DetectionRecoveryAndRefractoryAcrossWrap) {
  Recording r(UINT32_MAX-260);r.arm();r.sequence();EXPECT_EQ(1u,r.alerts);
  r.hold(1400);r.sequence();EXPECT_EQ(2u,r.alerts);
}
TEST(MotionRule, StaleRotationCannotReviveAfterAFullTimestampCycle) {
  FallDetectionConfig cfg; cfg.max_sample_gap_ms=cfg.recovery_ms=1000;
  Recording r(0,cfg); r.torso(); r.vector(1,0,300);
  ASSERT_TRUE(r.rule.available());
  advanceWithoutNewFeatures(r,(uint64_t(1)<<32)-40,1);
  ASSERT_TRUE(r.rule.available());
  r.vector(.3f,0); r.vector(3,0);
  EXPECT_EQ(0u,r.alerts);
}
TEST(MotionRule, StaleOrientationCannotReviveAfterAFullTimestampCycle) {
  FallDetectionConfig cfg; cfg.max_sample_gap_ms=cfg.recovery_ms=1000;
  Recording r(0,cfg); r.torso();
  ASSERT_TRUE(r.rule.available());
  advanceWithoutNewFeatures(r,(uint64_t(1)<<32)+180,1.4f);
  ASSERT_TRUE(r.rule.available());
  r.vector(3,0);
  EXPECT_FALSE(r.rule.referenceValid());
  r.quietTilt(70,1100);
  EXPECT_EQ(0u,r.alerts);
}
TEST(MotionRule, RetainedPairCannotReviveAfterAFullTimestampCycle) {
  FallDetectionConfig cfg; cfg.max_sample_gap_ms=cfg.recovery_ms=1000;
  Recording r(0,cfg); r.torso(); r.vector(.3f,0); r.vector(3,0); r.vector(.3f,0);
  ASSERT_TRUE(r.rule.available());
  advanceWithoutNewFeatures(r,(uint64_t(1)<<32)-20,1);
  ASSERT_TRUE(r.rule.available()); r.vector(1,0,300);
  EXPECT_EQ(0u,r.alerts);
}
TEST(MotionRule, RotationAtTheFullAssociationBoundaryRemainsUsable) {
  Recording r; r.arm(); r.sample(1,300); r.hold(280); r.sample(.3f);
  r.hold(480); EXPECT_TRUE(r.sample(3));
  EXPECT_EQ(1u,r.alerts);
}
TEST(MotionRule, OrientationReferenceIncludesItsMaximumAge) {
  Recording r; r.torso(); const uint32_t reference=r.now;
  for (unsigned i=0;i<49;++i) r.vector(1.4f,0,40);
  r.vector(3,0);
  EXPECT_TRUE(r.rule.referenceValid()); EXPECT_EQ(reference,r.rule.referenceTimeMs());
  r.quietTilt(70,1100); EXPECT_EQ(1u,r.alerts);
}
TEST(MotionRule, ImpactWithOrientationCanDetectWithoutLowGOrFastRotation) {
  Recording r;r.torso();r.vector(3,0,20);r.quietTilt(70,1100);
  EXPECT_EQ(1u,r.alerts);EXPECT_EQ(MotionRule::ImpactOrientation,r.rule.eventKind());
}
TEST(MotionRule, FirstLowKeepsAnExistingImpactOrientationCandidate) {
  Recording r; r.torso(); r.vector(3,0); r.vector(.3f,0); r.quietTilt(70,1100);
  EXPECT_EQ(1u,r.alerts); EXPECT_EQ(MotionRule::ImpactOrientation,r.rule.eventKind());
  EXPECT_FLOAT_EQ(3,r.rule.peakAcceleration());
}
TEST(MotionRule, ReboundKeepsTheExistingImpactOrientationCandidate) {
  Recording r; r.torso(); r.vector(.3f,0); r.vector(3,0); r.vector(.3f,0);
  EXPECT_EQ(MotionRule::LowEpisodeRenewed,r.rule.lastReason());
  r.quietTilt(70,1100);
  EXPECT_EQ(1u,r.alerts); EXPECT_EQ(MotionRule::ImpactOrientation,r.rule.eventKind());
  EXPECT_FLOAT_EQ(3,r.rule.peakAcceleration());
}
TEST(MotionRule, RenewedLowsDoNotMoveTheContextDeadline) {
  Recording r; r.torso(); r.vector(.3f,0); r.vector(3,0); r.quietTilt(0,5300);
  r.vector(.3f,0); r.vector(2.8f,0); r.quietTilt(70,1200);
  EXPECT_EQ(0u,r.alerts); EXPECT_EQ(MotionRule::Armed,r.rule.currentState());
}
TEST(MotionRule, FreshLowSurvivesContextExpirationWithinItsOwnWindow) {
  Recording r; r.torso(); r.vector(1.8f,0); r.quietTilt(0,5940);
  r.vector(.3f,0); r.quietTilt(0,40);
  EXPECT_TRUE(r.vector(3,0,300)); EXPECT_EQ(MotionRule::StrongSequence,r.rule.eventKind());
}
TEST(MotionRule, LowAndImpactCanDetectWithNoGyroPeak) {
  Recording r;r.torso();r.vector(.3f,0);r.vector(3,0);r.quietTilt(70,1100);
  EXPECT_EQ(1u,r.alerts);
}
TEST(MotionRule, SupportedDescentUsesLowerImpactWithContext) {
  Recording r;r.torso();r.vector(.8f,0,100);r.vector(1.8f,40,100);r.quietTilt(60,1100);
  EXPECT_EQ(1u,r.alerts);EXPECT_EQ(MotionRule::SupportedDescent,r.rule.eventKind());
}
TEST(MotionRule, SlowDescentCanDetectWithoutHighImpactOrRapidRotation) {
  Recording r;r.torso();r.vector(.8f,0,30);r.vector(1.2f,10,30);
  for(int angle=15;angle<=70;++angle)r.vector(1,angle,30);
  r.quietTilt(70,2100);EXPECT_EQ(1u,r.alerts);EXPECT_EQ(MotionRule::SlowDescent,r.rule.eventKind());
}
TEST(MotionRule, UnsupportedSlowTiltAndOrdinaryLyingDoNotAlert) {
  Recording r;r.torso();
  for(int angle=0;angle<90;++angle)r.vector(1,angle,20);
  r.quietTilt(90,7000);EXPECT_EQ(0u,r.alerts);
}
TEST(MotionRule, SittingBendingAndIsolatedDeviceBumpsDoNotAlert) {
  Recording r;r.torso();r.vector(1.8f,0,90);r.quietTilt(20,7000);
  r.vector(.8f,20,20);r.vector(1.2f,30,20);r.quietTilt(20,7000);
  EXPECT_EQ(0u,r.alerts);
}
TEST(MotionRule, ContextConfirmationRequiresContinuousEvidenceAndChangedOrientation) {
  Recording r;r.torso();r.vector(3,0);r.quietTilt(70,800);
  r.vector(1,0);r.quietTilt(0,7000);EXPECT_EQ(0u,r.alerts);
}
TEST(MotionRule, StaleOrientationCannotConfirmUsingOnlyQuietMagnitude) {
  Recording r;r.torso();r.vector(3,0);r.vector(1,70);
  for(unsigned i=0;i<60;++i)r.vector(1.2f,0);
  EXPECT_EQ(0u,r.alerts);
}
TEST(MotionRule, OldBumpCannotCombineWithLaterLyingPosture) {
  Recording r;r.torso();r.vector(3,0);r.quietTilt(0,4000);r.quietTilt(70,1100);
  EXPECT_EQ(0u,r.alerts);
}
TEST(MotionRule, OldLargerBumpCannotHideANewerIndependentContextEvent) {
  Recording r;r.torso();r.vector(3,0);r.quietTilt(0,3000);
  r.vector(2.8f,0);r.quietTilt(70,1100);EXPECT_EQ(1u,r.alerts);
}
TEST(MotionRule, LostSamplesCannotSupplyContextConfirmation) {
  Recording r;r.torso();r.vector(3,0);r.quietTilt(70,800);
  r.sample(1,0,false);r.quietTilt(70,7000);EXPECT_EQ(0u,r.alerts);
}
TEST(MotionRule, ContextPathsAreExplicitlyConfigurable) {
  FallDetectionConfig cfg;cfg.context_paths=false;Recording r(0,cfg);r.torso();
  r.vector(3,0);r.quietTilt(70,7000);EXPECT_EQ(0u,r.alerts);
}
TEST(MotionRule, InvalidConfigurationIsVisibleAndCannotProduceEvents) {
  for(int mode=0;mode<8;++mode) {
    FallDetectionConfig cfg;
    if(mode==0)cfg.low_g=NAN;
    if(mode==1)cfg.confirm_quiet_ms=cfg.confirm_timeout_ms+1;
    if(mode==2)cfg.context_angle_deg=181;
    if(mode==3)cfg.max_sample_gap_ms=0;
    if(mode==4)cfg.recovery_samples=1;
    if(mode==5)cfg.max_sample_gap_ms=1;
    if(mode==6)cfg.event_window_ms=1;
    if(mode==7)cfg.context_feature_ms=cfg.context_window_ms;
    Recording r(0,cfg);r.arm();r.sequence();EXPECT_EQ(0u,r.alerts);
    EXPECT_FALSE(r.rule.configured());EXPECT_STREQ("config-fault",r.rule.stateName());
  }
}
TEST(MotionRule, DeviceDropMatchingStrongSequenceRemainsAnExplicitAmbiguity) {
  Recording r;r.arm();r.sequence();r.hold(3000);
  EXPECT_EQ(1u,r.alerts); // A single IMU cannot establish who or what fell.
}
TEST(MotionRule, FalseAlarmDismissalDoesNotImposeMinuteLongBlindness) {
  Recording r;r.arm();r.sequence();
  FallResponse response;ASSERT_TRUE(response.onFall(r.now,false));
  auto click=[&](uint32_t t){response.updateButton(t,true);response.updateButton(t+25,true);
    response.updateButton(t+100,false);response.updateButton(t+125,false);};
  click(r.now+100);click(r.now+300);response.updateButton(r.now+3101,false);
  ASSERT_FALSE(response.alarmActive());r.hold(5000);r.sequence();
  EXPECT_EQ(2u,r.alerts);EXPECT_TRUE(response.onFall(r.now,false));
}
struct TraceReading {
  float ax=0,ay=0,az=1,gx=0,gy=0,gz=0,temperature=25;
  float acceleration()const{return sqrtf(ax*ax+ay*ay+az*az);}
  float rotation()const{return sqrtf(gx*gx+gy*gy+gz*gz);}
};
// Exercise the classifier-to-recorder reference identity with actual vectors.
struct TracedRecording {
  Recording motion;
  MotionTrace trace;
  explicit TracedRecording(const FallDetectionConfig& cfg=FallDetectionConfig()) : motion(0,cfg) {}
  void sample(float a=1,float angle=0,float rotation=0) {
    motion.vector(a,angle,rotation);
    TraceReading value; const float rad=angle*.0174532925f;
    value.ax=a*sinf(rad); value.az=a*cosf(rad); value.gx=rotation;
    trace.observe(motion.now,value,value,motion.rule.currentState(),motion.rule.lastReason(),1,false,
        motion.rule.decision(),motion.rule.referenceValid(),motion.rule.referenceTimeMs(),
        motion.rule.settings().pretrigger_ms);
  }
  void hold(unsigned ms,float a=1,float angle=0,float rotation=0) {
    for (unsigned i=0;i<ms;i+=20) sample(a,angle,rotation);
  }
};
TEST(MotionTrace, BoundedSnapshotKeepsDecisionAndRawCorrectedAxes) {
  MotionTrace trace;TraceReading raw,corrected;raw.gx=3;corrected.gx=1;
  for(unsigned i=0;i<200;++i)trace.observe(i*20,raw,corrected,1,i==120?6:0,1,true,i==120);
  ASSERT_EQ(MotionTrace::capacity,trace.sampleCount());
  bool found=false;for(unsigned i=0;i<trace.sampleCount();++i) {
    const auto s=trace.sample(i);if(s.at==2400){found=true;EXPECT_EQ(3,s.raw[3]);EXPECT_EQ(1,s.gyro[0]);}
  }
  EXPECT_TRUE(found);EXPECT_EQ(1u,trace.eventCount());
}
TEST(MotionTrace, EventRingIsBoundedAndKeepsRejectionsAndAcceptances) {
  MotionTrace trace;for(unsigned i=0;i<100;++i)trace.record(i,1,2,1,i%2?3:6,0);
  EXPECT_EQ(MotionTrace::event_capacity,trace.eventCount());
  EXPECT_EQ(68u,trace.event(0).at);EXPECT_EQ(99u,trace.event(31).at);
}
TEST(MotionTrace, SlowEventKeepsOriginalTroughAndPeakAfterRecentRingWraps) {
  Recording r;MotionTrace trace;TraceReading v;
  auto sample=[&](float a,float angle,float gyro=0) {
    r.vector(a,angle,gyro);const float rad=angle*.0174532925f;
    v.ax=a*sinf(rad);v.az=a*cosf(rad);v.gx=gyro;
    trace.observe(r.now,v,v,r.rule.currentState(),r.rule.lastReason(),1,false,r.rule.decision(),
        r.rule.referenceValid(),r.rule.referenceTimeMs());
  };
  for(unsigned i=0;i<60;++i)sample(1,0);
  sample(.8f,0,30);const uint32_t trough_at=r.now;
  sample(1.2f,10,30);const uint32_t peak_at=r.now;
  for(int angle=15;angle<=70;++angle)sample(1,angle,30);
  for(unsigned i=0;i<110;++i)sample(1,70);
  ASSERT_EQ(1u,r.alerts);ASSERT_EQ(5u,trace.evidenceCount());
  EXPECT_EQ(trough_at,trace.evidence(1).at);EXPECT_EQ(peak_at,trace.evidence(2).at);
  EXPECT_LT(trace.evidence(1).at,trace.sample(0).at);
}
TEST(MotionTrace, DataLossFreezesTheRejectedCandidateWithoutInventingAReading) {
  MotionTrace trace;TraceReading v;v.az=.2f;
  trace.observe(100,v,v,MotionRule::Tracking,MotionRule::CandidateStarted,1,true,false);
  trace.record(120,0,0,MotionRule::Recovering,MotionRule::SampleGap,0x100,true);
  ASSERT_EQ(1u,trace.sampleCount());EXPECT_EQ(100u,trace.sample(0).at);
  ASSERT_EQ(4u,trace.evidenceCount());EXPECT_EQ(100u,trace.evidence(0).at);
  EXPECT_TRUE(trace.evidence(0).buzzer);
}
TEST(MotionTrace, ReferenceMatchesTheClassifierInsteadOfTheNearestOlderSample) {
  TracedRecording r; r.hold(1000); const uint32_t reference=r.motion.now;
  r.hold(200,1.4f,90,40); r.sample(3,90); r.hold(1100,1,70);
  ASSERT_EQ(1u,r.motion.alerts); ASSERT_EQ(5u,r.trace.evidenceCount());
  EXPECT_EQ(reference,r.trace.evidence(4).at);
  EXPECT_FLOAT_EQ(1,r.trace.evidence(4).raw[2]);
  EXPECT_FLOAT_EQ(0,r.trace.evidence(4).gyro[0]);
}
TEST(MotionTrace, DiscardedHistoryCannotInventAReferenceCheckpoint) {
  TracedRecording r; r.hold(1200);
  r.motion.sample(1,0,false);
  r.trace.record(r.motion.now,0,0,r.motion.rule.currentState(),r.motion.rule.lastReason(),0x100,true);
  r.hold(240,1.4f,0,40); r.sample(.3f); r.sample(3,0,300);
  ASSERT_EQ(1u,r.motion.alerts); EXPECT_EQ(4u,r.trace.evidenceCount());
}
TEST(MotionTrace, AReferenceAtTimestampZeroRemainsAvailable) {
  TracedRecording r; r.motion.now=UINT32_MAX-219;
  r.hold(220); ASSERT_EQ(0u,r.motion.now);
  r.hold(180,1.4f,0,40); r.sample(3); r.hold(1100,1,70);
  ASSERT_EQ(1u,r.motion.alerts); ASSERT_EQ(5u,r.trace.evidenceCount());
  EXPECT_EQ(0u,r.trace.evidence(4).at);
}
TEST(MotionTrace, ConfiguredPretriggerWindowRetainsTheQualifyingGyroPeak) {
  FallDetectionConfig cfg; cfg.pretrigger_ms=500;
  TracedRecording r(cfg); r.hold(1200); r.sample(1,0,300);
  const uint32_t gyro_at=r.motion.now;
  r.hold(380); r.sample(.3f); r.sample(3);
  ASSERT_EQ(1u,r.motion.alerts); ASSERT_EQ(5u,r.trace.evidenceCount());
  EXPECT_EQ(gyro_at,r.trace.evidence(3).at);
  EXPECT_FLOAT_EQ(300,r.trace.evidence(3).gyro[0]);
}
TEST(MotionTrace, LowRenewalKeepsContextCheckpointsAndRecordsTheTransition) {
  TracedRecording r; r.hold(1200); r.sample(.3f); const uint32_t first=r.motion.now;
  r.hold(440); r.sample(.3f); const uint32_t renewed=r.motion.now;
  ASSERT_EQ(MotionRule::LowEpisodeRenewed,r.motion.rule.lastReason());
  EXPECT_EQ(0u,r.trace.evidenceCount());
  EXPECT_EQ(MotionRule::LowEpisodeRenewed,r.trace.event(r.trace.eventCount()-1).reason);
  r.hold(40); r.sample(3,0,300);
  ASSERT_EQ(1u,r.motion.alerts); ASSERT_EQ(5u,r.trace.evidenceCount());
  EXPECT_EQ(first,r.trace.evidence(0).at); EXPECT_EQ(first,r.trace.evidence(1).at);
  bool found=false;
  for (unsigned i=0;i<r.trace.sampleCount();++i) {
    const auto sample=r.trace.sample(i);
    if (sample.at==renewed && sample.reason==MotionRule::LowEpisodeRenewed) found=true;
  }
  EXPECT_TRUE(found);
}
TEST(MotionTrace, PretriggerGyroCannotCrossARejectedContinuityBoundary) {
  for (uint16_t quality : {0x100,0x200}) {
    TracedRecording r; r.hold(1200); r.sample(1,0,300);
    r.motion.sample(1,0,false);
    r.trace.record(r.motion.now,0,0,r.motion.rule.currentState(),r.motion.rule.lastReason(),quality,true);
    r.hold(220); r.sample(.3f); const uint32_t start=r.motion.now;
    r.sample(3); EXPECT_EQ(0u,r.motion.alerts);
    r.trace.record(r.motion.now+20,0,0,MotionRule::Recovering,MotionRule::SampleGap,0x100,true);
    ASSERT_GE(r.trace.evidenceCount(),4u);
    EXPECT_EQ(start,r.trace.evidence(3).at);
    EXPECT_FLOAT_EQ(0,r.trace.evidence(3).gyro[0]);
  }
}
TEST(MotionTrace, UnavailableRowsCannotBecomePretriggerEvidence) {
  for (uint16_t quality : {0x100,0x200}) {
    MotionTrace trace; TraceReading value; value.gx=300;
    trace.observe(100,value,value,MotionRule::Armed,MotionRule::None,1,false,false);
    trace.record(120,0,0,MotionRule::Recovering,MotionRule::SampleGap,quality,true);
    trace.observe(140,value,value,MotionRule::Recovering,MotionRule::None,quality,false,false);
    value.gx=0;
    for (uint32_t now=160;now<=360;now+=20)
      trace.observe(now,value,value,MotionRule::Recovering,MotionRule::None,1,false,false);
    value.az=.3f;
    trace.observe(380,value,value,MotionRule::Tracking,MotionRule::CandidateStarted,1,false,false);
    trace.record(400,0,0,MotionRule::Recovering,MotionRule::SampleGap,0x100,true);
    ASSERT_EQ(4u,trace.evidenceCount());
    EXPECT_EQ(380u,trace.evidence(3).at);
    EXPECT_FLOAT_EQ(0,trace.evidence(3).gyro[0]);
  }
}
TEST(MotionTrace, StartupFaultWithoutACandidateKeepsOrdinarySamplesRolling) {
  for (bool sampled_fault : {false,true}) {
    MotionTrace trace; TraceReading value;
    if (sampled_fault)
      trace.observe(0,value,value,MotionRule::Recovering,MotionRule::SampleGap,1,false,true);
    else
      trace.record(0,0,0,MotionRule::Recovering,MotionRule::SampleGap,0x100,true);
    for (unsigned i=1;i<=200;++i)
      trace.observe(i*20,value,value,MotionRule::Armed,MotionRule::None,1,false,false);
    ASSERT_EQ(MotionTrace::capacity,trace.sampleCount());
    EXPECT_EQ(4000u,trace.sample(trace.sampleCount()-1).at);
    EXPECT_EQ(0u,trace.evidenceCount());
    ASSERT_EQ(1u,trace.eventCount()); EXPECT_EQ(MotionRule::SampleGap,trace.event(0).reason);
  }
}
TEST(MotionTrace, FaultWithoutANewCandidatePreservesTheAcceptedCapture) {
  MotionTrace trace; TraceReading value;
  for (unsigned i=0;i<140;++i)
    trace.observe(i*20,value,value,MotionRule::Refractory,i==100 ? MotionRule::StrongSequence : MotionRule::None,
        1,false,i==100);
  const uint32_t first=trace.sample(0).at, last=trace.sample(trace.sampleCount()-1).at;
  trace.record(3000,0,0,MotionRule::Recovering,MotionRule::SampleGap,0x100,true);
  for (unsigned i=151;i<=200;++i)
    trace.observe(i*20,value,value,MotionRule::Armed,MotionRule::None,1,false,false);
  EXPECT_EQ(first,trace.sample(0).at);
  EXPECT_EQ(last,trace.sample(trace.sampleCount()-1).at);
}
int main(int argc,char**argv){::testing::InitGoogleTest(&argc,argv);return RUN_ALL_TESTS();}
