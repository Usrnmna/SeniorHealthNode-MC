#ifdef LOW_BATTERY_STANDALONE
#include <stdio.h>
#include <stdlib.h>
#define TEST(suite, name) static void suite##_##name()
#define EXPECT_TRUE(value) do { if (!(value)) { fprintf(stderr, "Failed at line %d\n", __LINE__); exit(1); } } while (0)
#define EXPECT_FALSE(value) EXPECT_TRUE(!(value))
#else
#include <gtest/gtest.h>
#endif
#include <helpers/ui/BuzzerPattern.h>
#include "../../examples/simple_sensor/LowBatteryAlert.h"

TEST(LowBattery, ThresholdAndTenMinuteRepeat) {
  LowBatteryAlert alert;
  EXPECT_FALSE(alert.update(0, 3500, false));
  EXPECT_FALSE(alert.update(1, 0, false));
  EXPECT_TRUE(alert.update(2, 3499, false));
  alert.markPlayed(2);
  EXPECT_FALSE(alert.update(600001, 3499, false));
  EXPECT_TRUE(alert.update(600002, 3500, false)); // Equality retains low state.
  alert.markPlayed(600002);
  EXPECT_FALSE(alert.update(600003, 3501, false));
  EXPECT_FALSE(alert.isActive());
  EXPECT_TRUE(alert.update(600004, 3499, false));
}
TEST(LowBattery, ChargingClearsBeforeBatteryRecoversAndUnplugRearms) {
  LowBatteryAlert alert;
  EXPECT_TRUE(alert.update(0, 3300, false)); alert.markPlayed(0);
  EXPECT_FALSE(alert.update(1, 3300, true)); EXPECT_FALSE(alert.isActive());
  EXPECT_FALSE(alert.update(600001, 3300, true));
  EXPECT_TRUE(alert.update(600002, 3300, false));
}
TEST(LowBattery, DeferredPlaybackAndTimerRollover) {
  LowBatteryAlert alert;
  const uint32_t start = UINT32_MAX - 1000;
  EXPECT_TRUE(alert.update(start, 3400, false));
  EXPECT_TRUE(alert.update(start + 1, 3400, false)); // Busy buzzer did not consume reminder.
  alert.markPlayed(start + 1);
  EXPECT_FALSE(alert.update(start + 600000, 3400, false));
  EXPECT_TRUE(alert.update(start + 600001, 3400, false));
  EXPECT_TRUE(alert.update(start + 600002, 0, false)); // Bad reading cannot clear warning.
}
TEST(LowBattery, ThreeLongTonesInOrderWithGapsAndNoLoop) {
  BuzzerPattern p(150, 5000);
  EXPECT_TRUE(p.startLowBattery(0));
  const uint32_t frequencies[] = {2093, 1760, 1480};
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t start = i * 600;
    EXPECT_TRUE(p.sounding()); EXPECT_TRUE(p.frequencyHz() == frequencies[i]);
    p.update(start + 449); EXPECT_TRUE(p.sounding());
    p.update(start + 450); EXPECT_FALSE(p.sounding());
    if (i < 2) {
      p.update(start + 599); EXPECT_FALSE(p.sounding());
      p.update(start + 600);
    }
  }
  p.update(600000); EXPECT_FALSE(p.sounding());
  EXPECT_TRUE(p.frequencyHz() == HealthNodeConfig::buzzer_frequency_hz);
}
TEST(LowBattery, SosPreemptsAndBatteryCancellationCannotSilenceSos) {
  BuzzerPattern p(150, 5000);
  EXPECT_TRUE(p.startLowBattery(0)); EXPECT_FALSE(p.beep(1, 100));
  p.startSOS(2);
  EXPECT_TRUE(p.alarmActive()); EXPECT_TRUE(p.sounding());
  EXPECT_TRUE(p.frequencyHz() == HealthNodeConfig::buzzer_frequency_hz);
  EXPECT_FALSE(p.startLowBattery(3));
  p.stopLowBattery(); EXPECT_TRUE(p.alarmActive()); EXPECT_TRUE(p.sounding());
  p.stop(); EXPECT_TRUE(p.startLowBattery(4));
  p.stopLowBattery(); EXPECT_FALSE(p.sounding());
  EXPECT_TRUE(p.beep(5, 150)); EXPECT_TRUE(p.frequencyHz() == HealthNodeConfig::buzzer_frequency_hz);
  EXPECT_FALSE(p.startLowBattery(6)); // Let an existing ordinary beep finish.
}
TEST(LowBattery, LatePollsAndRolloverDoNotSkipTones) {
  BuzzerPattern p(150, 5000);
  const uint32_t start = UINT32_MAX - 200;
  EXPECT_TRUE(p.startLowBattery(start));
  p.update(start + 449); EXPECT_TRUE(p.sounding());
  p.update(start + 1000); EXPECT_FALSE(p.sounding());
  p.update(start + 5000); EXPECT_TRUE(p.sounding()); EXPECT_TRUE(p.frequencyHz() == 1760);
  p.update(start + 5449); EXPECT_TRUE(p.sounding());
  p.update(start + 5450); EXPECT_FALSE(p.sounding());
}

#ifdef LOW_BATTERY_STANDALONE
int main() {
  LowBattery_ThresholdAndTenMinuteRepeat();
  LowBattery_ChargingClearsBeforeBatteryRecoversAndUnplugRearms();
  LowBattery_DeferredPlaybackAndTimerRollover();
  LowBattery_ThreeLongTonesInOrderWithGapsAndNoLoop();
  LowBattery_SosPreemptsAndBatteryCancellationCannotSilenceSos();
  LowBattery_LatePollsAndRolloverDoNotSkipTones();
  puts("6 low battery tests passed (synthetic voltage, charging and timing only)");
}
#else
// PlatformIO's GoogleTest library does not supply main().
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
