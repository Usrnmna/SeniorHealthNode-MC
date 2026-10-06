#include <gtest/gtest.h>
#include <Arduino.h>
#include "../../src/helpers/ui/MomentaryButton.cpp"
#include "../../examples/companion_radio/health/FallResponse.h"

class CompanionButton : public testing::Test {
protected:
  MomentaryButton ui{0, 1000, true, true};
  FallResponse fall;
  void SetUp() override {
    g_mock_millis = 100;
    g_mock_button_level = HIGH;
    ui.begin();
    ui.check();
  }
  int sample(uint32_t at, bool pressed, bool suppress) {
    g_mock_millis = at;
    g_mock_button_level = pressed ? LOW : HIGH;
    return ui.check(false, suppress);
  }
};

TEST_F(CompanionButton, IdleClickStillNavigates) {
  EXPECT_EQ(sample(200, true, false), BUTTON_EVENT_NONE);
  EXPECT_EQ(sample(300, false, false), BUTTON_EVENT_NONE);
  EXPECT_EQ(sample(600, false, false), BUTTON_EVENT_CLICK);
}

TEST_F(CompanionButton, AlarmDiscardsAnAlreadyPendingUiClick) {
  sample(200, true, false);
  sample(300, false, false);
  EXPECT_EQ(sample(350, false, true), BUTTON_EVENT_NONE);
  EXPECT_EQ(sample(700, false, false), BUTTON_EVENT_NONE);
}

TEST_F(CompanionButton, ThreeAlarmClicksAcknowledgeWithoutUiEvents) {
  ASSERT_TRUE(fall.onFall(100, false));
  int acknowledgements = 0;
  for (uint32_t t = 110; t <= 2100; t += 10) {
    const bool pressed = (t >= 300 && t < 500) ||
                         (t >= 800 && t < 1000) ||
                         (t >= 1300 && t < 1500);
    const bool owned = fall.alarmActive();
    if (fall.updateButton(t, pressed)) ++acknowledgements;
    EXPECT_EQ(sample(t, pressed, owned), BUTTON_EVENT_NONE);
  }
  EXPECT_EQ(acknowledgements, 1);
  EXPECT_FALSE(fall.alarmActive());
  EXPECT_TRUE(fall.messagePending());
  sample(2300, true, false);
  sample(2400, false, false);
  EXPECT_EQ(sample(2700, false, false), BUTTON_EVENT_CLICK);
}

TEST_F(CompanionButton, HeldAlarmPressCannotBecomeUiShutdown) {
  EXPECT_EQ(sample(200, true, true), BUTTON_EVENT_NONE);
  EXPECT_EQ(sample(1500, true, true), BUTTON_EVENT_NONE);
  EXPECT_EQ(sample(1600, true, false), BUTTON_EVENT_NONE);
  EXPECT_EQ(sample(1700, false, false), BUTTON_EVENT_NONE);
  EXPECT_EQ(sample(2200, false, false), BUTTON_EVENT_NONE);
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
