#ifdef MOTION_RULE_STANDALONE
#include <stdio.h>
#include <stdlib.h>
#define TEST(suite, name) static void suite##_##name()
#define EXPECT_TRUE(value) do { if (!(value)) { fprintf(stderr, "Failed at line %d\n", __LINE__); exit(1); } } while (0)
#define EXPECT_FALSE(value) EXPECT_TRUE(!(value))
#else
#include <gtest/gtest.h>
#endif
#include "../../examples/simple_sensor/MotionRule.h"

TEST(MotionRule, RestAndInvalidReadingsDoNotTrigger) {
  MotionRule rule;
  EXPECT_FALSE(rule.update(0, 1, 0, true));
  EXPECT_FALSE(rule.update(20, 5, 500, false));
  EXPECT_FALSE(rule.update(40, NAN, 500, true));
  EXPECT_TRUE(rule.update(60, 2.5f, 0, true));
}

TEST(MotionRule, RotationCanTriggerIndependently) {
  MotionRule rule;
  EXPECT_TRUE(rule.update(0, 1, 250, true));
  EXPECT_FALSE(rule.update(60001, 1, 250, true));
}

TEST(MotionRule, RequiresCooldownAndContinuousQuietToRearm) {
  MotionRule rule;
  EXPECT_TRUE(rule.update(0, 3, 0, true));
  EXPECT_FALSE(rule.update(100, 1, 0, true));
  EXPECT_FALSE(rule.update(2100, 1, 0, true));
  EXPECT_FALSE(rule.update(59000, 3, 0, true));
  EXPECT_FALSE(rule.update(60000, 1, 0, true));
  EXPECT_FALSE(rule.update(61999, 1, 0, true));
  EXPECT_FALSE(rule.update(62000, 1, 0, true));
  EXPECT_TRUE(rule.update(62020, 3, 0, true));
}

TEST(MotionRule, ReadFailureBreaksQuietPeriod) {
  MotionRule rule;
  EXPECT_TRUE(rule.update(0, 3, 0, true));
  rule.update(60000, 1, 0, true);
  rule.update(61000, 0, 0, false);
  rule.update(62000, 1, 0, true);
  EXPECT_FALSE(rule.update(63000, 3, 0, true));
}

TEST(MotionRule, CooldownSurvivesMillisWrap) {
  MotionRule rule;
  const uint32_t start = UINT32_MAX - 1000;
  EXPECT_TRUE(rule.update(start, 3, 0, true));
  rule.update(start + 58000U, 1, 0, true);
  rule.update(start + 60000U, 1, 0, true);
  EXPECT_TRUE(rule.update(start + 60020U, 3, 0, true));
}

#ifdef MOTION_RULE_STANDALONE
int main() {
  MotionRule_RestAndInvalidReadingsDoNotTrigger();
  MotionRule_RotationCanTriggerIndependently();
  MotionRule_RequiresCooldownAndContinuousQuietToRearm();
  MotionRule_ReadFailureBreaksQuietPeriod();
  MotionRule_CooldownSurvivesMillisWrap();
  puts("5 motion rule tests passed");
}
#endif
