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

// Synthetic 50 Hz recordings test logic, NOT accuracy on real falls or wearers.
struct Recording {
  MotionRule rule;
  uint32_t now;
  unsigned alerts = 0;
  explicit Recording(uint32_t start = 0, const FallDetectionConfig& cfg = FallDetectionConfig())
    : rule(cfg), now(start) {}
  bool sample(float a = 1, float g = 0, bool valid = true, uint32_t step = 20) {
    now += step;
    const bool fired = rule.update(now, a, g, valid);
    if (fired) ++alerts;
    return fired;
  }
  void hold(unsigned ms, float a = 1, float g = 0) {
    for (unsigned elapsed = 0; elapsed < ms; elapsed += 20) sample(a, g);
  }
  void arm() { hold(2020); }
  void sequence() { sample(0.3f); sample(3, 100); sample(1, 300); }
  void fall() { sequence(); hold(1020); }
};

TEST(MotionRule, StartupRequiresSettling) {
  Recording r;
  r.fall();
  EXPECT_TRUE(r.alerts == 0);
  r.arm(); r.fall();
  EXPECT_TRUE(r.alerts == 1);
}

TEST(MotionRule, QuietAndOrdinaryMotionDoNotAlert) {
  Recording r; r.arm();
  for (int i = 0; i < 100; ++i) {
    r.sample(0.8f, 15); r.sample(1.5f, 60); r.sample(1, 10);
  }
  r.hold(6000);
  EXPECT_TRUE(r.alerts == 0);
}

TEST(MotionRule, IsolatedPeaksAndMissingLowAreRejected) {
  Recording r; r.arm();
  r.sample(4, 0); r.sample(1, 400); r.sample(4, 400);
  r.hold(6000);
  EXPECT_TRUE(r.alerts == 0);
}

TEST(MotionRule, BothPeaksAreRequired) {
  for (int missing = 0; missing < 2; ++missing) {
    Recording r; r.arm(); r.sample(0.3f);
    r.sample(missing == 0 ? 1 : 3, missing == 0 ? 300 : 0);
    r.hold(6000);
    EXPECT_TRUE(r.alerts == 0);
  }
}

TEST(MotionRule, SeparatePeaksInEitherOrderAndTogether) {
  for (int order = 0; order < 3; ++order) {
    Recording r; r.arm(); r.sample(0.3f);
    if (order == 0) { r.sample(3, 0); r.sample(1, 300); }
    if (order == 1) { r.sample(1, 300); r.sample(3, 0); }
    if (order == 2) r.sample(3, 300);
    r.hold(1000);
    EXPECT_TRUE(r.alerts == 0);
    EXPECT_TRUE(r.sample());
    EXPECT_TRUE(r.rule.peakAcceleration() == 3);
    EXPECT_TRUE(r.rule.peakRotation() == 300);
  }
}

TEST(MotionRule, EventWindowIncludesBoundaryButRejectsLatePeak) {
  for (int late = 0; late < 2; ++late) {
    Recording r; r.arm(); r.sample(0.35f);
    r.hold(late ? 500 : 480);
    r.sample(2.4f, 240);
    r.hold(1020);
    EXPECT_TRUE(r.alerts == (late ? 0U : 1U));
  }
}

TEST(MotionRule, RepeatedLowCannotExtendWindow) {
  Recording r; r.arm(); r.sample(0.3f);
  r.hold(520, 0.3f); r.sample(4, 400); r.hold(6000);
  EXPECT_TRUE(r.alerts == 0);
}

TEST(MotionRule, QuietMustBeContinuous) {
  Recording r; r.arm(); r.sequence(); r.hold(800);
  r.sample(1.5f, 40); r.hold(1000);
  EXPECT_TRUE(r.alerts == 0);
  EXPECT_TRUE(r.sample());
}

TEST(MotionRule, ContinuedMovementExpiresConfirmation) {
  Recording r; r.arm(); r.sequence(); r.hold(5020, 1.5f, 40);
  r.hold(3000);
  EXPECT_TRUE(r.alerts == 0);
  r.fall();
  EXPECT_TRUE(r.alerts == 1);
}

TEST(MotionRule, ConfirmationDeadlineIsBounded) {
  for (int late = 0; late < 2; ++late) {
    Recording r; r.arm(); r.sequence();
    r.hold(late ? 4000 : 3980, 1.5f, 40);
    r.hold(1020);
    EXPECT_TRUE(r.alerts == (late ? 0U : 1U));
  }
}

TEST(MotionRule, InvalidSamplesCancelAllEvidence) {
  const float bad[][2] = {{NAN, 0}, {1, NAN}, {float(INFINITY), 0}, {1, float(INFINITY)}, {-1, 0}, {1, -1}};
  for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
    Recording r; r.arm(); r.sequence(); r.hold(600);
    r.sample(bad[i][0], bad[i][1]); r.hold(3000);
    EXPECT_TRUE(r.alerts == 0);
    r.fall(); EXPECT_TRUE(r.alerts == 1);
  }
  Recording r; r.arm(); r.sequence(); r.sample(1, 0, false); r.hold(3000);
  EXPECT_TRUE(r.alerts == 0);
}

TEST(MotionRule, GapsCancelPeaksAndConfirmation) {
  for (int stage = 0; stage < 2; ++stage) {
    Recording r; r.arm();
    if (stage == 0) r.sample(0.3f); else r.sequence();
    r.sample(1, 0, true, 101); r.sample(4, 400); r.hold(3000);
    EXPECT_TRUE(r.alerts == 0);
    r.fall(); EXPECT_TRUE(r.alerts == 1);
  }
}

TEST(MotionRule, DuplicateTimestampsCannotSupplyAnotherPeak) {
  Recording r; r.arm(); r.sample(0.3f);
  r.sample(4, 400, true, 0); r.hold(6000);
  EXPECT_TRUE(r.alerts == 0);
}

TEST(MotionRule, GapBoundaryIsAllowed) {
  Recording r; r.arm(); r.sample(0.3f);
  r.sample(3, 300, true, 100); r.hold(1020);
  EXPECT_TRUE(r.alerts == 1);
}

TEST(MotionRule, CooldownAndQuietAreBothRequired) {
  Recording r; r.arm(); r.fall();
  EXPECT_TRUE(r.alerts == 1);
  r.hold(3000); r.fall(); // Quiet alone cannot bypass cooldown.
  EXPECT_TRUE(r.alerts == 1);
  r.hold(60000, 1.5f, 40); r.fall(); // Elapsed cooldown alone is insufficient.
  EXPECT_TRUE(r.alerts == 1);
  r.arm(); r.fall();
  EXPECT_TRUE(r.alerts == 2);
}

TEST(MotionRule, ReconnectionDoesNotBypassCooldown) {
  Recording r; r.arm(); r.fall();
  r.sample(1, 0, false); r.arm(); r.fall();
  EXPECT_TRUE(r.alerts == 1);
  r.hold(60000); r.fall();
  EXPECT_TRUE(r.alerts == 2);
}

TEST(MotionRule, DetectionAndCooldownSurviveMillisWrap) {
  const uint32_t starts[] = {UINT32_MAX - 2100U, UINT32_MAX - 4000U};
  for (uint32_t start : starts) {
    Recording r(start); r.arm(); r.fall();
    EXPECT_TRUE(r.alerts == 1);
    r.hold(59960); r.sequence(); r.hold(1020);
    EXPECT_TRUE(r.alerts == 1); // Low began before cooldown expired.
    r.arm(); r.fall(); EXPECT_TRUE(r.alerts == 2);
  }
}

TEST(MotionRule, CustomSettingsAndSequenceOnlyMode) {
  FallDetectionConfig cfg;
  cfg.low_g = 0.5f; cfg.impact_g = 2; cfg.rotation_dps = 200;
  cfg.confirm_quiet_ms = 0;
  Recording r(0, cfg); r.arm(); r.sample(0.45f);
  EXPECT_TRUE(r.sample(2.1f, 210));
  EXPECT_FALSE(r.sample(3, 400));
}

#ifdef MOTION_RULE_STANDALONE
int main() {
  MotionRule_StartupRequiresSettling();
  MotionRule_QuietAndOrdinaryMotionDoNotAlert();
  MotionRule_IsolatedPeaksAndMissingLowAreRejected();
  MotionRule_BothPeaksAreRequired();
  MotionRule_SeparatePeaksInEitherOrderAndTogether();
  MotionRule_EventWindowIncludesBoundaryButRejectsLatePeak();
  MotionRule_RepeatedLowCannotExtendWindow();
  MotionRule_QuietMustBeContinuous();
  MotionRule_ContinuedMovementExpiresConfirmation();
  MotionRule_ConfirmationDeadlineIsBounded();
  MotionRule_InvalidSamplesCancelAllEvidence();
  MotionRule_GapsCancelPeaksAndConfirmation();
  MotionRule_DuplicateTimestampsCannotSupplyAnotherPeak();
  MotionRule_GapBoundaryIsAllowed();
  MotionRule_CooldownAndQuietAreBothRequired();
  MotionRule_ReconnectionDoesNotBypassCooldown();
  MotionRule_DetectionAndCooldownSurviveMillisWrap();
  MotionRule_CustomSettingsAndSequenceOnlyMode();
  puts("18 motion rule tests passed (synthetic recordings only)");
}
#endif
