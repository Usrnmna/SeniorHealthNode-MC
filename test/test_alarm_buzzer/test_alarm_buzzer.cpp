#include <gtest/gtest.h>
#include <Arduino.h>
#include <vector>

// Record requests at the GPIO/LEDC boundary; these fakes do not simulate waveforms.
#define OUTPUT 1
struct BuzzerOutputEvent {
  char operation;
  uint32_t value;
};
static std::vector<BuzzerOutputEvent> output_events;
static bool setup_succeeds = true;
static uint32_t output_duty = 0;
static uint32_t output_frequency = 0;
static int output_level = LOW;

void digitalWrite(int, int level) {
  output_level = level;
  output_events.push_back({'G', static_cast<uint32_t>(level)});
}
uint32_t ledcSetup(uint8_t, uint32_t frequency, uint8_t resolution) {
  EXPECT_EQ(resolution, 8);
  output_frequency = frequency;
  output_events.push_back({'F', frequency});
  return setup_succeeds ? frequency : 0;
}
void ledcWrite(uint8_t, uint32_t duty) {
  output_duty = duty;
  output_events.push_back({'W', duty});
}
void ledcAttachPin(uint8_t, uint8_t) {
  output_events.push_back({'A', output_duty});
}

#include <helpers/ui/AlarmBuzzer.h>
#include "../../examples/companion_radio/health/FallResponse.h"

class AlarmBuzzerPolarity : public testing::TestWithParam<bool> {
protected:
  void SetUp() override {
    output_events.clear();
    setup_succeeds = true;
    output_duty = output_frequency = 0;
    output_level = LOW;
    g_mock_millis = 0;
  }
  uint32_t silentDuty() const { return GetParam() ? 255 : 0; }
  void advance(AlarmBuzzer& buzzer, uint32_t elapsed) {
    g_mock_millis += elapsed;
    buzzer.loop();
  }
};

TEST_P(AlarmBuzzerPolarity, InitializesInactiveBeforeAttachingPwm) {
  AlarmBuzzer buzzer(GetParam(), true);
  ASSERT_TRUE(buzzer.begin());
  EXPECT_EQ(output_level, GetParam() ? HIGH : LOW);
  ASSERT_EQ(output_events.size(), 4u);
  EXPECT_EQ(output_events.front().operation, 'G');
  EXPECT_EQ(output_events[2].operation, 'W');
  EXPECT_EQ(output_events[3].operation, 'A');
  EXPECT_EQ(output_events[3].value, silentDuty());
  EXPECT_EQ(output_duty, silentDuty());
}

TEST_P(AlarmBuzzerPolarity, FailedInitializationLeavesGpioInactive) {
  setup_succeeds = false;
  AlarmBuzzer buzzer(GetParam(), true);
  EXPECT_FALSE(buzzer.begin());
  EXPECT_FALSE(buzzer.beep());
  buzzer.startSOS();
  buzzer.loop();
  buzzer.stop();
  EXPECT_EQ(output_level, GetParam() ? HIGH : LOW);
  ASSERT_EQ(output_events.size(), 2u); // GPIO level and failed setup; no PWM attach.
}

TEST_P(AlarmBuzzerPolarity, BeepUsesHalfDutyAndReturnsToInactive) {
  AlarmBuzzer buzzer(GetParam(), true);
  ASSERT_TRUE(buzzer.begin());
  ASSERT_TRUE(buzzer.beep());
  EXPECT_EQ(output_duty, 128u);
  EXPECT_EQ(output_frequency, HealthNodeConfig::buzzer_frequency_hz);
  advance(buzzer, HealthNodeConfig::beep_ms);
  EXPECT_EQ(output_duty, silentDuty());
}

TEST_P(AlarmBuzzerPolarity, SosGapsAndExplicitStopAreInactive) {
  AlarmBuzzer buzzer(GetParam(), true);
  ASSERT_TRUE(buzzer.begin());
  buzzer.startSOS();
  EXPECT_EQ(output_duty, 128u);
  EXPECT_FALSE(buzzer.beep());
  advance(buzzer, HealthNodeConfig::morse_dot_ms);
  EXPECT_EQ(output_duty, silentDuty());
  advance(buzzer, HealthNodeConfig::morse_dot_ms);
  EXPECT_EQ(output_duty, 128u);
  buzzer.stop();
  EXPECT_EQ(output_duty, silentDuty());
}

TEST_P(AlarmBuzzerPolarity, BatteryTonesRetuneWhileInactiveAndFinishInactive) {
  AlarmBuzzer buzzer(GetParam(), true);
  ASSERT_TRUE(buzzer.begin());
  ASSERT_TRUE(buzzer.startLowBattery());
  for (unsigned tone = 0; tone < 3; ++tone) {
    EXPECT_EQ(output_frequency, HealthNodeConfig::low_battery_tones_hz[tone]);
    EXPECT_EQ(output_duty, 128u);
    advance(buzzer, HealthNodeConfig::long_beep_ms);
    EXPECT_EQ(output_duty, silentDuty());
    if (tone < 2) advance(buzzer, HealthNodeConfig::low_battery_gap_ms);
  }
  EXPECT_EQ(output_frequency, HealthNodeConfig::buzzer_frequency_hz);
  // Every frequency change after initialization must be preceded by inactive duty.
  unsigned frequency_changes = 0;
  for (size_t i = 2; i < output_events.size(); ++i) {
    if (output_events[i].operation != 'F') continue;
    ++frequency_changes;
    EXPECT_EQ(output_events[i - 1].operation, 'W');
    EXPECT_EQ(output_events[i - 1].value, silentDuty());
  }
  EXPECT_EQ(frequency_changes, 4u); // Three battery pitches, then ordinary pitch.
}

TEST_P(AlarmBuzzerPolarity, BatteryCancellationPreservesSosPriority) {
  AlarmBuzzer buzzer(GetParam(), true);
  ASSERT_TRUE(buzzer.begin());
  ASSERT_TRUE(buzzer.startLowBattery());
  buzzer.stopLowBattery();
  EXPECT_EQ(output_duty, silentDuty());
  ASSERT_TRUE(buzzer.startLowBattery());
  buzzer.startSOS();
  buzzer.stopLowBattery();
  EXPECT_EQ(output_duty, 128u);
  EXPECT_EQ(output_frequency, HealthNodeConfig::buzzer_frequency_hz);
  EXPECT_FALSE(buzzer.startLowBattery());
  buzzer.stop();
  EXPECT_EQ(output_duty, silentDuty());
}

TEST_P(AlarmBuzzerPolarity, DisabledOutputNeverCallsGpioOrPwm) {
  setup_succeeds = false; // Disabled operation must not require a working PWM peripheral.
  AlarmBuzzer buzzer(GetParam(), false);
  EXPECT_FALSE(buzzer.isEnabled());
  ASSERT_TRUE(buzzer.begin());
  EXPECT_FALSE(buzzer.beep());
  EXPECT_FALSE(buzzer.startLowBattery());
  buzzer.startSOS();
  advance(buzzer, HealthNodeConfig::morse_dot_ms);
  buzzer.stopLowBattery();
  buzzer.stop();
  advance(buzzer, HealthNodeConfig::sos_silence_ms);
  EXPECT_TRUE(output_events.empty());
}

TEST_P(AlarmBuzzerPolarity, DisabledOutputPreservesDeadlineAndButtonResponse) {
  AlarmBuzzer buzzer(GetParam(), false);
  ASSERT_TRUE(buzzer.begin());
  FallResponse response;
  ASSERT_TRUE(response.onFall(0, false));
  buzzer.startSOS();
  EXPECT_TRUE(response.alarmActive());
  g_mock_millis = HealthNodeConfig::response_timeout_ms;
  response.updateDeadline(g_mock_millis);
  buzzer.loop();
  ASSERT_TRUE(response.messagePending());
  EXPECT_EQ(response.pendingNotice(), FallNotice::UserCouldNotConfirm);
  response.messageAccepted();

  // Exercise the production button gate after escalation, independently of sound.
  for (unsigned click = 0; click < 3; ++click) {
    const uint32_t pressed_at = HealthNodeConfig::response_timeout_ms + 100 + click * 300;
    EXPECT_FALSE(response.updateButton(pressed_at, true));
    EXPECT_FALSE(response.updateButton(pressed_at + HealthNodeConfig::debounce_ms, true));
    EXPECT_FALSE(response.updateButton(pressed_at + 100, false));
    const bool accepted = response.updateButton(pressed_at + 100 + HealthNodeConfig::debounce_ms, false);
    EXPECT_EQ(accepted, click == 2);
    if (accepted) buzzer.stop();
  }
  EXPECT_FALSE(response.alarmActive());
  EXPECT_TRUE(response.messagePending());
  EXPECT_EQ(response.pendingNotice(), FallNotice::UserConfirmedFall);
  EXPECT_TRUE(output_events.empty());
}

INSTANTIATE_TEST_SUITE_P(BothPolarities, AlarmBuzzerPolarity, testing::Bool());

TEST(AlarmBuzzerDefault, UsesCompiledPolarity) {
  setup_succeeds = true;
  output_events.clear();
  AlarmBuzzer buzzer;
  EXPECT_EQ(buzzer.isEnabled(), HealthNodeConfig::buzzer_enabled);
  ASSERT_TRUE(buzzer.begin());
  if (HealthNodeConfig::buzzer_enabled) {
    EXPECT_EQ(output_level, HealthNodeConfig::buzzer_active_low ? HIGH : LOW);
    EXPECT_EQ(output_duty, HealthNodeConfig::buzzer_active_low ? 255u : 0u);
  } else {
    EXPECT_TRUE(output_events.empty());
  }
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
