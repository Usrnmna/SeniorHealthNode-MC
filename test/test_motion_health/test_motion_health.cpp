#include <gtest/gtest.h>
#include "../../examples/simple_sensor/MotionHealth.h"

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

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
