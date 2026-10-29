#include <gtest/gtest.h>
#include "../../examples/companion_radio/health/MotionHealth.h"
#include "../../examples/companion_radio/health/DetectorAvailability.h"
#include "../../examples/companion_radio/health/MotionServiceTiming.h"

TEST(MotionHealth, InvalidReadFaultStartsAtBootTimeZeroAndRecovers) {
  MotionHealth health(100, 1000);
  health.observe(0, false, false);
  health.observe(999, false, true);
  EXPECT_FALSE(health.fault());
  health.observe(1000, false, true);
  EXPECT_TRUE(health.fault());
  health.observe(1010, true, false);
  EXPECT_TRUE(health.fault());
  health.observe(1020, true, true);
  EXPECT_FALSE(health.fault());
  EXPECT_EQ(0u, health.gapCount()); // No pre-first-sample interval invented.
}

TEST(MotionHealth, FrozenValidReadingsDoNotHideOutage) {
  MotionHealth health(100, 1000);
  health.observe(0, true, true);
  for (uint32_t now = 20; now < 1000; now += 20) {
    health.observe(now, true, false);
    EXPECT_FALSE(health.fault());
  }
  health.observe(1000, true, false);
  EXPECT_TRUE(health.fault());
  health.observe(1020, true, true);
  EXPECT_FALSE(health.fault());
  EXPECT_EQ(1020u, health.maxGapMs());
  EXPECT_EQ(1u, health.gapCount());
}

TEST(MotionHealth, GapThresholdIsStrictAndOutageCountsOnceOnRecovery) {
  MotionHealth health(100, 1000);
  health.observe(0, true, true);
  health.observe(100, true, true);
  EXPECT_EQ(100u, health.maxGapMs());
  EXPECT_EQ(0u, health.gapCount());
  health.observe(201, true, true);
  EXPECT_EQ(1u, health.gapCount());
  health.observe(500, false, true);
  health.observe(800, false, true);
  EXPECT_EQ(1u, health.gapCount());
  health.observe(1201, false, false);
  EXPECT_TRUE(health.fault());
  health.observe(1221, true, true);
  EXPECT_EQ(2u, health.gapCount());
  EXPECT_EQ(1020u, health.maxGapMs());
  health.observe(1241, true, true);
  EXPECT_EQ(2u, health.gapCount());
  EXPECT_EQ(1020u, health.maxGapMs());
}

TEST(MotionHealth, FaultAndGapTimersCrossMillisRollover) {
  MotionHealth health(100, 1000);
  const uint32_t start = UINT32_MAX - 200;
  health.observe(start, true, true);
  health.observe(uint32_t(start + 999), false, false);
  EXPECT_FALSE(health.fault());
  health.observe(uint32_t(start + 1000), true, false);
  EXPECT_TRUE(health.fault());
  health.observe(uint32_t(start + 1020), true, true);
  EXPECT_FALSE(health.fault());
  EXPECT_EQ(1020u, health.maxGapMs());
  EXPECT_EQ(1u, health.gapCount());
}

TEST(MotionHealth, ContinuousFreshSamplesStayHealthy) {
  MotionHealth health(100, 1000);
  for (uint32_t now = 0; now <= 10000; now += 20) {
    health.observe(now, true, true);
    EXPECT_FALSE(health.fault());
  }
  EXPECT_EQ(20u, health.maxGapMs());
  EXPECT_EQ(0u, health.gapCount());
}

TEST(DetectorAvailability, LongStallCountsTimeBeyondLastSampleFreshness) {
  DetectorAvailability availability;
  availability.observe(1000,true,20,100);
  EXPECT_EQ(0u,availability.unavailableMs(1080));
  EXPECT_EQ(920u,availability.unavailableMs(2000));
  availability.observe(2000,true,20,100);
  EXPECT_EQ(920u,availability.unavailableMs(2020));
}

TEST(DetectorAvailability, RecoveryCalibrationAndRefractoryRemainUnavailable) {
  DetectorAvailability availability;
  availability.observe(0,false,UINT32_MAX,100);
  availability.observe(200,true,20,100);
  availability.observe(220,false,20,100);
  EXPECT_EQ(1200u,availability.unavailableMs(1220));
  availability.observe(1220,true,20,100);
  EXPECT_EQ(1200u,availability.unavailableMs(1240));
}

TEST(DetectorAvailability, ReadOnlyQueriesDoNotDoubleCountAndRolloverIsBounded) {
  DetectorAvailability availability;const uint32_t start=UINT32_MAX-40;
  availability.observe(start,true,20,100);
  EXPECT_EQ(20u,availability.unavailableMs(start+100));
  EXPECT_EQ(20u,availability.unavailableMs(start+100));
  availability.observe(start+100,false,UINT32_MAX,100);
  EXPECT_EQ(40u,availability.unavailableMs(start+120));
}

TEST(DetectorAvailability, ExclusiveCausesIncludeStartupAndDoNotRewriteStalls) {
  DetectorAvailability a;
  a.observe(10, DetectorAvailability::Startup, UINT32_MAX, 100);
  a.observe(110, DetectorAvailability::Recovery, 20, 100);
  a.observe(210, DetectorAvailability::Available, 20, 100);
  a.observe(1210, DetectorAvailability::Available, 20, 100);
  const auto t = a.totals(1230);
  EXPECT_EQ(100u,t.ms[DetectorAvailability::Startup]);
  EXPECT_EQ(80u,t.ms[DetectorAvailability::Recovery]);
  EXPECT_EQ(940u,t.ms[DetectorAvailability::Stale]);
  EXPECT_EQ(100u,t.ms[DetectorAvailability::Available]);
  EXPECT_EQ(1220u,t.monitoredMs());
  EXPECT_EQ(1120u,t.unavailableMs());
  EXPECT_EQ(t.monitoredMs(),a.totals(1230).monitoredMs());
}
TEST(DetectorAvailability, ExplicitHigherPriorityCausesDoNotBecomeStale) {
  DetectorAvailability a;
  a.observe(0,DetectorAvailability::Calibration,0,100);
  a.observe(1000,DetectorAvailability::ConfigurationFault,0,100);
  a.observe(2000,DetectorAvailability::SensorUnavailable,0,100);
  auto t=a.totals(3000);
  EXPECT_EQ(1000u,t.ms[DetectorAvailability::Calibration]);
  EXPECT_EQ(1000u,t.ms[DetectorAvailability::ConfigurationFault]);
  EXPECT_EQ(1000u,t.ms[DetectorAvailability::SensorUnavailable]);
  EXPECT_EQ(0u,t.ms[DetectorAvailability::Stale]);
}
TEST(DetectorAvailability, RefractoryFreshnessExpiryAcrossRollover) {
  DetectorAvailability a;const uint32_t start=UINT32_MAX-50;
  a.observe(start,DetectorAvailability::Refractory,20,100);
  auto t=a.totals(start+150);
  EXPECT_EQ(80u,t.ms[DetectorAvailability::Refractory]);
  EXPECT_EQ(70u,t.ms[DetectorAvailability::Stale]);
  EXPECT_EQ(150u,t.monitoredMs());
}
TEST(MotionServiceTiming, ServiceDelayAndPersistenceAreSeparateAcrossRollover) {
  MotionServiceTiming t;const uint32_t start=UINT32_MAX-10;
  t.begin(start);t.sample(start-20,start+5);t.finish(start+15);
  t.begin(start+200);t.finish(start+207);t.persistence(40);
  EXPECT_EQ(2u,t.polls);EXPECT_EQ(22u,t.busy_ms);
  EXPECT_EQ(200u,t.max_service_gap_ms);EXPECT_EQ(15u,t.max_poll_ms);
  EXPECT_EQ(25u,t.max_sample_delay_ms);EXPECT_EQ(40u,t.persistence_ms);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
