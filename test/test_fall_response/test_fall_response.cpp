#ifdef FALL_RESPONSE_STANDALONE
#include <stdio.h>
#include <stdlib.h>
#define TEST(suite, name) static void suite##_##name()
#define EXPECT_TRUE(value) do { if (!(value)) { fprintf(stderr, "Failed at line %d\n", __LINE__); exit(1); } } while (0)
#define EXPECT_FALSE(value) EXPECT_TRUE(!(value))
#else
#include <gtest/gtest.h>
#endif
#include <helpers/ui/BuzzerPattern.h>
#include "../../examples/simple_sensor/FallResponse.h"
#include "../../examples/simple_sensor/FallAckMessage.h"
#include "../../examples/simple_sensor/FallAckLocation.h"
#include "../../examples/simple_sensor/MotionRule.h"

static bool click(TripleClick& button, uint32_t start, uint32_t duration = 100) {
  EXPECT_FALSE(button.update(start, true));
  EXPECT_FALSE(button.update(start + 25, true));
  EXPECT_FALSE(button.update(start + duration, false));
  return button.update(start + duration + 25, false);
}
static bool click(FallResponse& response, uint32_t start, uint32_t duration = 100) {
  EXPECT_FALSE(response.updateButton(start, true));
  EXPECT_FALSE(response.updateButton(start + 25, true));
  EXPECT_FALSE(response.updateButton(start + duration, false));
  return response.updateButton(start + duration + 25, false);
}

TEST(FallResponse, ThreeSeparateClicksAndReleaseRequired) {
  TripleClick b(3000, 1000, 25); b.reset(0, false);
  EXPECT_FALSE(click(b, 100)); EXPECT_FALSE(click(b, 500));
  EXPECT_FALSE(b.update(900, true)); EXPECT_FALSE(b.update(925, true));
  EXPECT_FALSE(b.update(1000, false)); EXPECT_FALSE(b.update(1024, false));
  EXPECT_TRUE(b.update(1025, false)); EXPECT_FALSE(b.update(1050, false));
}
TEST(FallResponse, FirstDownToThirdUpWindowInclusive) {
  TripleClick b(3000, 1000, 25); b.reset(0, false);
  EXPECT_FALSE(click(b, 100)); EXPECT_FALSE(click(b, 1400));
  EXPECT_TRUE(click(b, 3000)); // Final raw release at 3100; debounce ends at 3125.
  EXPECT_FALSE(click(b, 4000)); EXPECT_FALSE(click(b, 5000));
  EXPECT_FALSE(click(b, 6901)); // 3001 ms is too late.
}
TEST(FallResponse, LongPressInAnyPositionCancelsSet) {
  for (unsigned held = 0; held < 3; ++held) {
    TripleClick b(3000, 1000, 25); b.reset(0, false);
    uint32_t now = 100;
    for (unsigned i = 0; i < 3; ++i) {
      EXPECT_FALSE(click(b, now, i == held ? 1000 : 100)); now += i == held ? 1100 : 200;
    }
  }
  TripleClick b(5000, 1000, 25); b.reset(0, false);
  EXPECT_FALSE(click(b, 100, 999)); EXPECT_FALSE(click(b, 1200, 999));
  EXPECT_TRUE(click(b, 2300, 999));
}
TEST(FallResponse, BounceCannotSupplyExtraClicks) {
  TripleClick b(3000, 1000, 25); b.reset(0, false);
  EXPECT_FALSE(b.update(100, true)); EXPECT_FALSE(b.update(110, false));
  EXPECT_FALSE(b.update(120, true)); EXPECT_FALSE(b.update(145, true));
  EXPECT_FALSE(b.update(150, false)); EXPECT_FALSE(b.update(160, true));
  EXPECT_FALSE(b.update(200, false)); EXPECT_FALSE(b.update(225, false));
  EXPECT_FALSE(click(b, 500)); EXPECT_TRUE(click(b, 900));
}
TEST(FallResponse, ExpiredGestureCannotReviveAfterFullTimerCycle) {
  TripleClick b(3000, 1000, 25); b.reset(0, false);
  EXPECT_FALSE(click(b, 100)); EXPECT_FALSE(b.update(4000, false));
  // An entire millis cycle later: expired first click must not be reused.
  EXPECT_FALSE(click(b, 500)); EXPECT_FALSE(click(b, 900)); EXPECT_TRUE(click(b, 1300));
}
TEST(FallResponse, HeldButtonAtFallIsIgnored) {
  FallResponse r; r.onFall(100, true);
  EXPECT_FALSE(r.updateButton(500, false)); EXPECT_FALSE(r.updateButton(525, false));
  EXPECT_FALSE(click(r, 600)); EXPECT_FALSE(click(r, 900)); EXPECT_TRUE(click(r, 1200));
}
TEST(FallResponse, ClicksBeforeFallNeverAcknowledge) {
  FallResponse r;
  EXPECT_FALSE(click(r, 100)); EXPECT_FALSE(click(r, 400)); EXPECT_FALSE(click(r, 700));
  EXPECT_FALSE(r.messagePending()); EXPECT_FALSE(r.alarmActive());
  r.onFall(1000, false);
  EXPECT_FALSE(click(r, 1100)); EXPECT_FALSE(click(r, 1400));
  EXPECT_TRUE(r.alarmActive()); EXPECT_FALSE(r.messagePending());
  EXPECT_TRUE(click(r, 1700)); EXPECT_FALSE(r.alarmActive()); EXPECT_TRUE(r.messagePending());
  EXPECT_FALSE(click(r, 2100)); EXPECT_FALSE(click(r, 2400)); EXPECT_FALSE(click(r, 2700));
}
TEST(FallResponse, DuplicateFallDoesNotRestartGesture) {
  FallResponse r; r.onFall(0, false);
  EXPECT_FALSE(click(r, 100)); r.onFall(300, false);
  EXPECT_FALSE(click(r, 400)); EXPECT_TRUE(click(r, 700));
}
TEST(FallResponse, PacketAllocationRetriesBoundedAndStopOnSuccess) {
  FallResponse r; r.onFall(0, false);
  click(r, 100); click(r, 400); EXPECT_TRUE(click(r, 700));
  EXPECT_TRUE(r.messageDue(825)); r.messageAttempted(825, false);
  EXPECT_FALSE(r.messageDue(825 + HealthNodeConfig::ack_retry_ms - 1));
  EXPECT_TRUE(r.messageDue(825 + HealthNodeConfig::ack_retry_ms));
  r.messageAttempted(825 + HealthNodeConfig::ack_retry_ms, true); EXPECT_FALSE(r.messagePending());
  r.onFall(2000, false); click(r, 2100); click(r, 2400); click(r, 2700);
  uint32_t now = 2825;
  for (unsigned i = 0; i < HealthNodeConfig::ack_max_attempts; ++i) {
    EXPECT_TRUE(r.messageDue(now)); r.messageAttempted(now, false); now += HealthNodeConfig::ack_retry_ms;
  }
  EXPECT_FALSE(r.messagePending()); EXPECT_FALSE(r.alarmActive());
}
TEST(FallResponse, RebootClearsAlarmMessageAndMotionEvidence) {
  FallResponse r; r.onFall(0, false); EXPECT_TRUE(r.alarmActive());
  FallResponse reboot; EXPECT_FALSE(reboot.alarmActive()); EXPECT_FALSE(reboot.messagePending());
  BuzzerPattern buzzer(150, 5000); buzzer.startSOS(0);
  BuzzerPattern reboot_buzzer(150, 5000);
  EXPECT_TRUE(buzzer.alarmActive()); EXPECT_FALSE(reboot_buzzer.sounding());
  EXPECT_FALSE(reboot_buzzer.alarmActive());
  click(r, 100); click(r, 400); click(r, 700); EXPECT_TRUE(r.messagePending());
  EXPECT_FALSE(reboot.messageDue(10000));
  MotionRule motion;
  for (uint32_t now = 0; now <= 2000; now += 20) motion.update(now, 1, 0, true);
  motion.update(2020, 0.3f, 0, true); motion.update(2040, 3, 300, true);
  MotionRule restarted;
  for (uint32_t now = 0; now <= 3000; now += 20) EXPECT_FALSE(restarted.update(now, 1, 0, true));
}
TEST(FallResponse, TimersSurviveMillisRollover) {
  const uint32_t start = UINT32_MAX - 400;
  FallResponse r; r.onFall(start, false);
  EXPECT_FALSE(click(r, start + 10)); EXPECT_FALSE(click(r, start + 310));
  EXPECT_TRUE(click(r, start + 610));
  r.messageAttempted(start + 735, false);
  EXPECT_FALSE(r.messageDue(start + 735 + HealthNodeConfig::ack_retry_ms - 1));
  EXPECT_TRUE(r.messageDue(start + 735 + HealthNodeConfig::ack_retry_ms));
  BuzzerPattern p(150, 5000); p.startSOS(start);
  p.update(start + 149); EXPECT_TRUE(p.sounding());
  p.update(start + 150); EXPECT_FALSE(p.sounding());
  p.update(start + 300); EXPECT_TRUE(p.sounding());
  p.update(start + 450); EXPECT_FALSE(p.sounding());
}
TEST(FallResponse, SosHasNineMarksAndFiveSecondsSilence) {
  BuzzerPattern p(150, 5000); p.startSOS(0);
  const uint32_t phases[] = {150,150,150,150,150,450,450,150,450,150,450,450,150,150,150,150,150,5000};
  uint32_t now = 0;
  for (unsigned repeat = 0; repeat < 3; ++repeat) {
    for (unsigned i = 0; i < 18; ++i) {
      EXPECT_TRUE(p.sounding() == (i % 2 == 0));
      p.update(now + phases[i] - 1); EXPECT_TRUE(p.sounding() == (i % 2 == 0));
      now += phases[i]; p.update(now);
    }
  }
  EXPECT_TRUE(p.sounding()); p.stop(); EXPECT_FALSE(p.sounding()); EXPECT_FALSE(p.alarmActive());
  p.update(now + 100000); EXPECT_FALSE(p.sounding());
}
TEST(FallResponse, BeepCannotOverrideSosAndLatePollsDoNotSkipMarks) {
  BuzzerPattern p(150, 5000);
  EXPECT_TRUE(p.beep(0, 100)); p.update(99); EXPECT_TRUE(p.sounding());
  p.update(100); EXPECT_FALSE(p.sounding());
  p.startSOS(200); EXPECT_FALSE(p.beep(201, 1)); p.startSOS(300);
  p.update(350); EXPECT_FALSE(p.sounding());
  p.update(10000); EXPECT_TRUE(p.sounding()); // Next dot is audible, not skipped.
  p.update(10149); EXPECT_TRUE(p.sounding()); p.update(10150); EXPECT_FALSE(p.sounding());
}
TEST(FallResponse, Utf8WireFormatAndByteLimit) {
  uint8_t data[FallAckMessage::max_payload_bytes] = {};
  const auto len = FallAckMessage::encode(data, 0x12345678, "Node", u8"\u2713");
  EXPECT_TRUE(len == 14);
  EXPECT_TRUE(data[0] == 0x78 && data[1] == 0x56 && data[2] == 0x34 && data[3] == 0x12 && data[4] == 0);
  EXPECT_TRUE(memcmp(data + 5, "Node: \xe2\x9c\x93", 9) == 0);
  char text[160]; memset(text, 'a', sizeof(text)); text[154] = 0;
  EXPECT_TRUE(FallAckMessage::encode(data, 0, "Node", text) == sizeof(data));
  text[154] = 'a'; text[155] = 0;
  EXPECT_TRUE(FallAckMessage::encode(data, 0, "Node", text) == 0);
}

static void expectLocationText(bool valid, double latitude, double longitude,
                               const char* fallback, const char* expected) {
  uint8_t data[FallAckMessage::max_payload_bytes];
  const unsigned len = FallAckMessage::encodeWithLocation(data, 1, "Node", u8"Help",
                                                         valid, latitude, longitude, fallback);
  EXPECT_TRUE(len == 5 + strlen(expected));
  EXPECT_TRUE(memcmp(data + 5, expected, strlen(expected)) == 0);
}
TEST(FallResponse, GpsLocationAppendedInDegrees) {
  expectLocationText(true, 37.123456, -122.654321, u8"HOME",
                     "Node: Help | lat=37.123456, lon=-122.654321");
}
TEST(FallResponse, NoFixUsesConfiguredUnicodeFallback) {
  expectLocationText(false, 37.123456, -122.654321, HealthNodeConfig::DefaultLocation,
                     "Node: Help | HOME");
  expectLocationText(false, 0, 0, u8"Maison \u00e9t\u00e9", u8"Node: Help | Maison \u00e9t\u00e9");
}
TEST(FallResponse, InvalidCoordinatesUseFallback) {
  expectLocationText(true, 90.000001, 0, "HOME", "Node: Help | HOME");
  expectLocationText(true, -90.000001, 0, "HOME", "Node: Help | HOME");
  expectLocationText(true, 0, 180.000001, "HOME", "Node: Help | HOME");
  expectLocationText(true, 0, -180.000001, "HOME", "Node: Help | HOME");
  expectLocationText(true, NAN, 0, "HOME", "Node: Help | HOME");
  expectLocationText(true, 0, INFINITY, "HOME", "Node: Help | HOME");
}
TEST(FallResponse, GeographicBoundariesAndZeroAreValidWithFix) {
  expectLocationText(true, -90, 180, "HOME", "Node: Help | lat=-90.000000, lon=180.000000");
  expectLocationText(true, 0, 0, "HOME", "Node: Help | lat=0.000000, lon=0.000000");
}
TEST(FallResponse, LocationIncludedInUtf8ByteLimit) {
  uint8_t data[FallAckMessage::max_payload_bytes];
  char fallback[150]; memset(fallback, 'a', sizeof(fallback)); fallback[147] = 0;
  // "Node: " + "Help | " + 147 bytes = 160.
  EXPECT_TRUE(FallAckMessage::encodeWithLocation(data, 1, "Node", "Help", false, 0, 0, fallback) == sizeof(data));
  fallback[147] = 'a'; fallback[148] = 0;
  EXPECT_TRUE(FallAckMessage::encodeWithLocation(data, 1, "Node", "Help", false, 0, 0, fallback) == 0);
  EXPECT_TRUE(FallAckMessage::encodeWithLocation(data, 1, "Node", fallback, true, 90, 180, "HOME") == 0);
}

struct FakeLocationProvider {
  bool enabled = true, valid = true;
  long latitude = 37123456, longitude = -122654321;
  unsigned polls = 0, reads = 0;
  bool isEnabled() { return enabled; }
  void loop() { ++polls; }
  bool isValid() { return valid; }
  long getLatitude() { EXPECT_TRUE(polls > 0); ++reads; return latitude; }
  long getLongitude() { EXPECT_TRUE(polls > 0); ++reads; return longitude; }
};
struct FakeLocationSource {
  FakeLocationProvider* provider;
  FakeLocationProvider* getLocationProvider() { return provider; }
};
TEST(FallResponse, LocationQueriedAgainOnEachAttempt) {
  FakeLocationProvider provider;
  FakeLocationSource source{&provider};
  double latitude, longitude;
  EXPECT_TRUE(FallAckLocation::read(source, latitude, longitude));
  EXPECT_TRUE(fabs(latitude - 37.123456) < 1e-9 && fabs(longitude + 122.654321) < 1e-9);
  provider.latitude = -45000000; provider.longitude = 90000000;
  EXPECT_TRUE(FallAckLocation::read(source, latitude, longitude));
  EXPECT_TRUE(latitude == -45 && longitude == 90 && provider.polls == 2);
  provider.valid = false;
  EXPECT_FALSE(FallAckLocation::read(source, latitude, longitude));
  EXPECT_TRUE(latitude == 0 && longitude == 0 && provider.reads == 4);
  provider.valid = true; provider.latitude = 91000000;
  EXPECT_FALSE(FallAckLocation::read(source, latitude, longitude));
}
TEST(FallResponse, MissingOrDisabledGpsDoesNotReturnLocation) {
  double latitude, longitude;
  FakeLocationSource missing{nullptr};
  EXPECT_FALSE(FallAckLocation::read(missing, latitude, longitude));
  FakeLocationProvider provider; provider.enabled = false;
  FakeLocationSource source{&provider};
  EXPECT_FALSE(FallAckLocation::read(source, latitude, longitude));
  EXPECT_TRUE(provider.polls == 0 && provider.reads == 0);
}

#ifdef FALL_RESPONSE_STANDALONE
int main() {
  FallResponse_ThreeSeparateClicksAndReleaseRequired();
  FallResponse_FirstDownToThirdUpWindowInclusive();
  FallResponse_LongPressInAnyPositionCancelsSet();
  FallResponse_BounceCannotSupplyExtraClicks();
  FallResponse_ExpiredGestureCannotReviveAfterFullTimerCycle();
  FallResponse_HeldButtonAtFallIsIgnored();
  FallResponse_ClicksBeforeFallNeverAcknowledge();
  FallResponse_DuplicateFallDoesNotRestartGesture();
  FallResponse_PacketAllocationRetriesBoundedAndStopOnSuccess();
  FallResponse_RebootClearsAlarmMessageAndMotionEvidence();
  FallResponse_TimersSurviveMillisRollover();
  FallResponse_SosHasNineMarksAndFiveSecondsSilence();
  FallResponse_BeepCannotOverrideSosAndLatePollsDoNotSkipMarks();
  FallResponse_Utf8WireFormatAndByteLimit();
  FallResponse_GpsLocationAppendedInDegrees();
  FallResponse_NoFixUsesConfiguredUnicodeFallback();
  FallResponse_InvalidCoordinatesUseFallback();
  FallResponse_GeographicBoundariesAndZeroAreValidWithFix();
  FallResponse_LocationIncludedInUtf8ByteLimit();
  FallResponse_LocationQueriedAgainOnEachAttempt();
  FallResponse_MissingOrDisabledGpsDoesNotReturnLocation();
  puts("21 fall response tests passed (synthetic timing, location providers and payloads only)");
}
#else
// PlatformIO's GoogleTest library does not supply main().
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
