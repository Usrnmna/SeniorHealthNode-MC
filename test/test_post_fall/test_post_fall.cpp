#include <gtest/gtest.h>
#include <helpers/ui/BuzzerPattern.h>
#include "../../examples/companion_radio/health/FallResponse.h"
#include "../../examples/companion_radio/health/FallNoticeOutbox.h"
#include <string>
#include <vector>

// Production response/outbox logic with synthetic motion, time and transport.
static bool click(FallResponse& r, uint32_t t) {
  r.updateButton(t, true); r.updateButton(t + 25, true);
  r.updateButton(t + 100, false); return r.updateButton(t + 125, false);
}
static void quiet(FallResponse& r, uint32_t start, uint32_t duration) {
  for (uint32_t t = 0; t <= duration; t += 20) r.observeMotion(start + t, 0, 0, 1, 0);
}
static std::string body(const FallNoticeMessage::Bundle& m, unsigned part = 0) {
  return std::string(reinterpret_cast<const char*>(m.payload[part] + 5), m.length[part] - 5);
}

TEST(PostFall, FixedDeadlineIssuesOneWarningAndKeepsSos) {
  FallResponse r; BuzzerPattern buzzer(150, 5000);
  ASSERT_TRUE(r.onFall(100, false)); buzzer.startSOS(100);
  r.updateDeadline(30099); EXPECT_FALSE(r.messagePending());
  r.updateDeadline(30100);
  EXPECT_EQ(FallNotice::UserCouldNotConfirm, r.pendingNotice());
  EXPECT_TRUE(r.alarmActive()); EXPECT_TRUE(buzzer.alarmActive());
  r.messageAccepted();
  for (uint32_t t = 30100; t < 3600000; t += 150) { r.updateDeadline(t); buzzer.update(t); }
  EXPECT_FALSE(r.messagePending()); EXPECT_TRUE(r.alarmActive()); EXPECT_TRUE(buzzer.alarmActive());
}
TEST(PostFall, DuplicateFallAndPartialClicksDoNotExtendDeadline) {
  FallResponse r; r.onFall(0, false);
  click(r, 28000); EXPECT_FALSE(r.onFall(28500, false)); click(r, 29000);
  r.updateDeadline(30000); EXPECT_EQ(FallNotice::UserCouldNotConfirm, r.pendingNotice());
  EXPECT_TRUE(click(r, 30000)); EXPECT_EQ(FallNotice::UserConfirmedFall, r.pendingNotice());
  EXPECT_FALSE(r.alarmActive());
}
TEST(PostFall, CompletedGestureWinsExactDeadlineAndCancelsEscalation) {
  FallResponse r; r.onFall(0, false);
  click(r, 28000); click(r, 29000); ASSERT_TRUE(click(r, 29875));
  r.updateDeadline(30000); EXPECT_EQ(FallNotice::AssistanceRequested, r.pendingNotice());
  r.messageAccepted(); r.updateDeadline(60000); EXPECT_FALSE(r.messagePending());
}
TEST(PostFall, DebounceCompletingAfterDeadlineProducesFollowup) {
  FallResponse r; r.onFall(0, false); click(r, 28000); click(r, 29000);
  r.updateButton(29890, true); r.updateButton(29915, true); r.updateButton(29990, false);
  EXPECT_FALSE(r.updateButton(30000, false)); r.updateDeadline(30000);
  EXPECT_EQ(FallNotice::UserCouldNotConfirm, r.pendingNotice());
  EXPECT_TRUE(r.updateButton(30015, false)); EXPECT_EQ(FallNotice::UserConfirmedFall, r.pendingNotice());
}
TEST(PostFall, FinalTenSecondsQuietSufficeAfterEarlierMovement) {
  FallResponse r; r.onFall(0, false);
  for (uint32_t t = 0; t < 20000; t += 20) r.observeMotion(t, 0, 0, 1, 20);
  quiet(r, 20000, 10000); r.updateDeadline(30000);
  EXPECT_EQ(FallNotice::NoResidualMovement, r.pendingNotice()); EXPECT_TRUE(r.alarmActive());
}
TEST(PostFall, ShortQuietPeriodAndResumedMovementUseGeneralWarning) {
  for (bool resume : {false, true}) {
    FallResponse r; r.onFall(0, false);
    quiet(r, resume ? 10000 : 20020, resume ? 19980 : 9980);
    if (resume) r.observeMotion(30000, 0, 0, 1, 10);
    r.updateDeadline(30000); EXPECT_EQ(FallNotice::UserCouldNotConfirm, r.pendingNotice());
  }
}
TEST(PostFall, InvalidMissingAndNonfiniteSamplesCannotEstablishQuiet) {
  for (unsigned fault = 0; fault < 4; ++fault) {
    FallResponse r; r.onFall(0, false); quiet(r, 10000, 19000);
    if (fault == 0) r.motionUnavailable();
    if (fault == 1) r.observeMotion(30000, 0, 0, 1, 0); // One-second gap.
    if (fault == 2) r.observeMotion(30000, NAN, 0, 1, 0);
    if (fault == 3) r.observeMotion(30000, 0, 0, 1, INFINITY);
    r.updateDeadline(30000); EXPECT_EQ(FallNotice::UserCouldNotConfirm, r.pendingNotice());
  }
}
TEST(PostFall, CachedSamplesAndStalledLoopCannotExtendQuietEvidence) {
  FallResponse r; r.onFall(0, false); quiet(r, 10000, 19980);
  for (unsigned i = 0; i < 100; ++i) r.observeMotion(29980, 0, 0, 1, 0);
  r.observeMotion(60000, 0, 0, 1, 0); r.updateDeadline(60000);
  EXPECT_EQ(FallNotice::UserCouldNotConfirm, r.pendingNotice());
}
TEST(PostFall, RecoveryRequiresANewFullQuietWindow) {
  FallResponse r; r.onFall(0, false); quiet(r, 0, 10000); r.motionUnavailable();
  quiet(r, 20000, 10000); r.updateDeadline(30000);
  EXPECT_EQ(FallNotice::NoResidualMovement, r.pendingNotice());
}
TEST(PostFall, SlowVectorDriftBreaksQuietEvenWithSmallPerSampleChanges) {
  FallResponse r; r.onFall(0, false);
  for (uint32_t t = 0; t <= 30000; t += 20) {
    const float angle = t * 0.00005f;
    r.observeMotion(t, sinf(angle), 0, cosf(angle), 0);
  }
  r.updateDeadline(30000); EXPECT_EQ(FallNotice::UserCouldNotConfirm, r.pendingNotice());
}
TEST(PostFall, NonGravityAccelerationDoesNotCountAsQuiet) {
  FallResponse r; r.onFall(0, false);
  for (uint32_t t = 0; t <= 30000; t += 20) r.observeMotion(t, 0, 0, 0, 0);
  r.updateDeadline(30000); EXPECT_EQ(FallNotice::UserCouldNotConfirm, r.pendingNotice());
}
TEST(PostFall, BothWarningsAllowLateAcknowledgmentAndNewIncident) {
  for (bool still : {false, true}) {
    FallResponse r; r.onFall(0, false); if (still) quiet(r, 0, 30000);
    r.updateDeadline(30000); r.messageAccepted(); // Radio may finish or fail much later.
    click(r, 1000000); click(r, 1000400); ASSERT_TRUE(click(r, 1000800));
    EXPECT_EQ(FallNotice::UserConfirmedFall, r.pendingNotice()); EXPECT_FALSE(r.alarmActive());
    r.messageAccepted(); EXPECT_FALSE(click(r, 1001200));
    ASSERT_TRUE(r.onFall(1002000, false)); r.updateDeadline(1032000);
    EXPECT_EQ(FallNotice::UserCouldNotConfirm, r.pendingNotice());
  }
}
TEST(PostFall, DeadlineQuietAndLateClicksSurviveTimerRollover) {
  const uint32_t start = UINT32_MAX - 10000;
  FallResponse r; r.onFall(start, false); quiet(r, start, 30000);
  r.updateDeadline(start + 30000); EXPECT_EQ(FallNotice::NoResidualMovement, r.pendingNotice());
  click(r, start + 31000); click(r, start + 31400); EXPECT_TRUE(click(r, start + 31800));
  EXPECT_EQ(FallNotice::UserConfirmedFall, r.pendingNotice());
}
TEST(PostFall, RebootDoesNotRestoreIncidentOrQuietEvidence) {
  FallResponse r; r.onFall(0, false); quiet(r, 0, 30000); r.updateDeadline(30000);
  FallResponse reboot; reboot.updateDeadline(90000);
  EXPECT_FALSE(reboot.alarmActive()); EXPECT_FALSE(reboot.messagePending());
}

TEST(PostFall, FastDoubleWaitsFullWindowThenResolvesOnceWithoutEscalation) {
  FallResponse r; r.onFall(0, false);
  EXPECT_FALSE(click(r, 100)); EXPECT_FALSE(click(r, 400));
  EXPECT_FALSE(r.updateButton(3099, false)); EXPECT_TRUE(r.alarmActive());
  EXPECT_FALSE(r.updateButton(3100, false)); // Third release may still occur at this boundary.
  EXPECT_TRUE(r.updateButton(3101, false));
  EXPECT_EQ(FallNotice::UserConfirmedFalseAlarm, r.pendingNotice());
  EXPECT_TRUE(r.falseAlarmConfirmed()); EXPECT_FALSE(r.alarmActive());
  r.messageAccepted(); r.updateDeadline(30000);
  EXPECT_FALSE(r.messagePending()); EXPECT_FALSE(click(r, 31000));
}

TEST(PostFall, DoubleWindowUsesFirstDownToSecondReleaseInclusive) {
  for (uint32_t elapsed : {999u, 1000u, 1001u}) {
    FallResponse r; r.onFall(0, false); click(r, 100);
    EXPECT_FALSE(click(r, elapsed)); // Release at elapsed+100; first down at 100.
    EXPECT_EQ(elapsed <= 1000, r.updateButton(3101, false));
    EXPECT_EQ(elapsed > 1000, r.alarmActive());
  }
}

TEST(PostFall, FastDoubleNeverBeatsValidThirdReleaseIncludingExactBoundary) {
  for (uint32_t third_down : {800u, 3000u}) {
    FallResponse r; r.onFall(0, false); click(r, 100); click(r, 400);
    EXPECT_TRUE(click(r, third_down));
    EXPECT_EQ(FallNotice::AssistanceRequested, r.pendingNotice());
    EXPECT_FALSE(r.falseAlarmConfirmed());
    EXPECT_FALSE(r.updateButton(4000, false));
  }
}

TEST(PostFall, HeldOrLateThirdPressDoesNotTurnAttemptedConfirmationIntoFalseAlarm) {
  for (bool held : {false, true}) {
    FallResponse r; r.onFall(0, false); click(r, 100); click(r, 400);
    const uint32_t down = held ? 800 : 3000;
    r.updateButton(down, true); r.updateButton(down + 25, true);
    r.updateButton(held ? 1800 : 3101, true);
    r.updateButton(held ? 1900 : 3200, false);
    EXPECT_FALSE(r.updateButton(held ? 1925 : 3225, false));
    EXPECT_FALSE(r.updateButton(5000, false));
    EXPECT_TRUE(r.alarmActive()); EXPECT_FALSE(r.messagePending());
    click(r, 6000); click(r, 6400); EXPECT_TRUE(r.updateButton(9001, false));
  }
}

TEST(PostFall, DoublePressBounceDoesNotCreateThirdClickOrPrematureResponse) {
  FallResponse r; r.onFall(0, false); click(r, 100);
  r.updateButton(400, true); r.updateButton(410, false);
  r.updateButton(420, true); r.updateButton(445, true);
  r.updateButton(500, false); r.updateButton(510, true);
  r.updateButton(520, false); EXPECT_FALSE(r.updateButton(545, false));
  r.updateButton(3095, true); // Unsettled third-down bounce.
  EXPECT_TRUE(r.updateButton(3105, false)); EXPECT_FALSE(r.updateButton(3106, false));
  EXPECT_EQ(FallNotice::UserConfirmedFalseAlarm, r.pendingNotice());
}

TEST(PostFall, HeldAtDetectionAndSinglePressCannotConfirmFalseAlarm) {
  FallResponse r; r.onFall(0, true);
  r.updateButton(100, false); r.updateButton(125, false);
  click(r, 400); EXPECT_FALSE(r.updateButton(3401, false));
  EXPECT_TRUE(r.alarmActive()); EXPECT_FALSE(r.messagePending());
}

TEST(PostFall, LateDoubleResolvesBothWarningTypesWithoutRadioSuccess) {
  for (bool still : {false, true}) {
    FallResponse r; r.onFall(0, false); if (still) quiet(r, 0, 30000);
    r.updateDeadline(30000); // Warning still pending, not delivered.
    click(r, 40000); click(r, 40400);
    ASSERT_TRUE(r.updateButton(43001, false));
    EXPECT_EQ(FallNotice::UserConfirmedFalseAlarm, r.pendingNotice());
    EXPECT_FALSE(r.alarmActive());
    r.messageAccepted(); ASSERT_TRUE(r.onFall(70000, false));
    EXPECT_FALSE(r.falseAlarmConfirmed()); r.updateDeadline(100000);
    EXPECT_EQ(FallNotice::UserCouldNotConfirm, r.pendingNotice());
  }
}

TEST(PostFall, PendingDoubleDoesNotPostponeThirtySecondWarning) {
  FallResponse r; r.onFall(0, false); click(r, 28000); click(r, 28400);
  EXPECT_FALSE(r.updateButton(30000, false)); r.updateDeadline(30000);
  EXPECT_EQ(FallNotice::UserCouldNotConfirm, r.pendingNotice()); EXPECT_TRUE(r.alarmActive());
  EXPECT_TRUE(r.updateButton(31001, false));
  EXPECT_EQ(FallNotice::UserConfirmedFalseAlarm, r.pendingNotice());
}

TEST(PostFall, CompletedDoubleWinsSameLoopDeadline) {
  FallResponse r; r.onFall(0, false); click(r, 26999); click(r, 27400);
  EXPECT_TRUE(r.updateButton(30000, false)); r.updateDeadline(30000);
  EXPECT_EQ(FallNotice::UserConfirmedFalseAlarm, r.pendingNotice());
}

TEST(PostFall, DoubleSurvivesRolloverAndRebootClearsPendingCandidate) {
  const uint32_t start = UINT32_MAX - 200;
  FallResponse r; r.onFall(start, false); click(r, start + 10); click(r, start + 410);
  EXPECT_FALSE(r.updateButton(start + 3010, false));
  EXPECT_TRUE(r.updateButton(start + 3011, false));
  EXPECT_EQ(FallNotice::UserConfirmedFalseAlarm, r.pendingNotice());
  FallResponse reboot; EXPECT_FALSE(reboot.updateButton(start + 4000, false));
  EXPECT_FALSE(reboot.messagePending());
}

TEST(PostFall, PendingFalseAlarmRetainsItsOriginalIncidentAndChannelUnderBackpressure) {
  FallChannel first, next;
  ASSERT_TRUE(FallChannel::parse("public #first", first));
  ASSERT_TRUE(FallChannel::parse("public #next", next));
  FallResponse r; r.onFall(0, false, first); click(r, 100); click(r, 400);
  ASSERT_TRUE(r.updateButton(3101, false)); const auto incident = r.pendingIncident();
  r.onFall(4000, false, next); r.updateDeadline(34000);
  EXPECT_EQ(FallNotice::UserConfirmedFalseAlarm, r.pendingNotice());
  EXPECT_EQ(incident, r.pendingIncident()); EXPECT_NE(incident, r.incidentId());
  EXPECT_EQ(0, memcmp(first.key, r.pendingChannel().key, first.key_length));
}

TEST(FallNoticeText, JoinAddsSeparatorOnlyBetweenNonemptyTexts) {
  char dest[160];
  ASSERT_TRUE(FallNoticeMessage::join(dest, sizeof(dest), "", HealthNodeConfig::unconfirmed_message));
  EXPECT_STREQ("USER COULD NOT CONFIRM THEIR CONDITION", dest);
  ASSERT_TRUE(FallNoticeMessage::join(dest, sizeof(dest), "Help", HealthNodeConfig::unconfirmed_message));
  EXPECT_STREQ("Help | USER COULD NOT CONFIRM THEIR CONDITION", dest);
  ASSERT_TRUE(FallNoticeMessage::join(dest, sizeof(dest), "Help", "")); EXPECT_STREQ("Help", dest);
}
TEST(FallNoticeText, BothWarningsAppendExactTextAfterExistingLocation) {
  for (auto notice : {FallNotice::UserCouldNotConfirm, FallNotice::NoResidualMovement}) {
    const auto m = FallNoticeMessage::build(notice, false, 0, 0);
    ASSERT_EQ(1u, m.count);
    EXPECT_EQ(std::string(HealthNodeConfig::ack_message) + " | HOME | " + fallNoticeText(notice), body(m));
  }
}
TEST(FallNoticeText, LongFallbackUsesCompleteLabelledParts) {
  const std::string location(70, 'L');
  const auto m = FallNoticeMessage::build(FallNotice::NoResidualMovement, false, 0, 0, location.c_str());
  ASSERT_EQ(2u, m.count);
  EXPECT_EQ(std::string("[1/2] ") + HealthNodeConfig::no_movement_message, body(m));
  EXPECT_EQ(std::string("[2/2] ") + HealthNodeConfig::ack_message + " | " + location, body(m, 1));
  EXPECT_LE(m.length[0], 165u); EXPECT_LE(m.length[1], 165u);
}
TEST(FallNoticeText, ConfirmationIsExactIndependentText) {
  const auto m = FallNoticeMessage::build(FallNotice::UserConfirmedFall, true, 1, 2);
  ASSERT_EQ(1u, m.count); EXPECT_EQ("USER HAS CONFIRMED FALL", body(m));
}
TEST(FallNoticeText, FalseAlarmHasExactTextAndGpsOrFallbackLocation) {
  auto m = FallNoticeMessage::build(FallNotice::UserConfirmedFalseAlarm, true, 37.123456, -122.654321);
  ASSERT_EQ(1u, m.count);
  EXPECT_EQ("User Confirms False Alarm | lat=37.123456, lon=-122.654321", body(m));
  m = FallNoticeMessage::build(FallNotice::UserConfirmedFalseAlarm, false, 0, 0);
  ASSERT_EQ(1u, m.count); EXPECT_EQ("User Confirms False Alarm | HOME", body(m));
  m = FallNoticeMessage::build(FallNotice::UserConfirmedFalseAlarm, true, NAN, 0, u8"Maison \u00e9t\u00e9");
  ASSERT_EQ(1u, m.count); EXPECT_EQ(u8"User Confirms False Alarm | Maison \u00e9t\u00e9", body(m));
}
TEST(FallNoticeText, Utf8BytesRemainIntactAcrossMultipartAndFallback) {
  const std::string location = u8"Maison\u00e9Maison\u00e9Maison\u00e9Maison\u00e9Maison\u00e9Maison\u00e9Maison\u00e9Maison\u00e9";
  const auto m = FallNoticeMessage::build(FallNotice::NoResidualMovement, false, 0, 0, location.c_str());
  ASSERT_EQ(2u, m.count);
  EXPECT_EQ(std::string("[1/2] ") + HealthNodeConfig::no_movement_message, body(m));
  EXPECT_EQ(std::string("[2/2] ") + HealthNodeConfig::ack_message + " | " + location, body(m, 1));
  const auto fallback = FallNoticeMessage::build(FallNotice::UserCouldNotConfirm, true, NAN, 0, u8"Maison \u00e9t\u00e9");
  ASSERT_EQ(1u, fallback.count); EXPECT_NE(std::string::npos, body(fallback).find(u8" | Maison \u00e9t\u00e9 | USER"));
}
TEST(FallNoticeText, IndividuallyOversizedPartRejectsWholeEventWithoutTruncation) {
  const std::string location(150, 'L');
  EXPECT_EQ(0u, FallNoticeMessage::build(FallNotice::NoResidualMovement, false, 0, 0, location.c_str()).count);
}

struct NoticeLink : AssistanceTransport {
  AssistanceDelivery* delivery = nullptr;
  Result result = Queued;
  uint32_t timestamp = 0, tag = 0, cancelled = 0;
  bool cancel_callback = true;
  uint8_t fingerprint[8] = {};
  std::vector<std::string> sent;
  uint32_t uniqueTimestamp() override { return ++timestamp; }
  Result queueAssistance(const uint8_t* data, unsigned len, uint32_t id, uint8_t hash[8], const FallChannel& channel) override {
    tag = id; memcpy(fingerprint, &id, 4); memcpy(hash, fingerprint, 8);
    if (result == Queued) sent.emplace_back(reinterpret_cast<const char*>(data + 5), len - 5);
    return result;
  }
  void cancelAssistance(uint32_t id) override {
    cancelled = id; if (cancel_callback) delivery->txComplete(0, id, false);
  }
};
struct NoticeRig {
  AssistanceDelivery delivery;
  FallNoticeOutbox outbox;
  NoticeLink link;
  NoticeRig() { link.delivery = &delivery; }
  bool enqueue(FallNotice notice, uint32_t incident = 1, const char* location = "HOME") {
    return outbox.enqueue(notice, incident, FallNoticeMessage::build(notice, false, 0, 0, location));
  }
  void poll(uint32_t now) { outbox.poll(now, link, delivery); }
  void complete() {
    delivery.txComplete(0, link.tag, true); delivery.confirmRepeat(link.fingerprint);
  }
};
TEST(FallOutbox, QueuedWarningIsCancelledAndLateCallbackCannotCompleteConfirmation) {
  NoticeRig r; ASSERT_TRUE(r.enqueue(FallNotice::UserCouldNotConfirm)); r.poll(0);
  const uint32_t old_tag = r.link.tag;
  uint8_t old_hash[8]; memcpy(old_hash, r.link.fingerprint, 8);
  ASSERT_TRUE(r.enqueue(FallNotice::UserConfirmedFall)); r.poll(1);
  EXPECT_EQ(old_tag, r.link.cancelled); EXPECT_NE(old_tag, r.link.tag);
  EXPECT_EQ("USER HAS CONFIRMED FALL", r.link.sent.back());
  r.delivery.txComplete(2, old_tag, true); EXPECT_FALSE(r.delivery.confirmRepeat(old_hash));
  EXPECT_EQ(AssistanceDelivery::Queued, r.delivery.state());
}
TEST(FallOutbox, InFlightWarningCanFinishWithoutRestartingItsRetries) {
  NoticeRig r; r.link.cancel_callback = false;
  r.enqueue(FallNotice::NoResidualMovement); r.poll(0); const auto old_tag = r.link.tag;
  r.enqueue(FallNotice::UserConfirmedFall); r.poll(1);
  r.delivery.txComplete(2, old_tag, false); r.complete(); r.poll(3); r.poll(600000);
  EXPECT_EQ(2u, r.link.sent.size()); EXPECT_FALSE(r.outbox.busy());
}
TEST(FallOutbox, ConfirmationDiscardsUnsentSecondWarningPart) {
  NoticeRig r; const std::string location(70, 'L');
  r.enqueue(FallNotice::NoResidualMovement, 1, location.c_str()); r.poll(0); r.complete();
  r.enqueue(FallNotice::UserConfirmedFall); r.poll(1);
  ASSERT_EQ(2u, r.link.sent.size()); EXPECT_EQ("USER HAS CONFIRMED FALL", r.link.sent.back());
}
TEST(FallOutbox, FailedFirstPartStillAllowsSecondAndRetainsFailure) {
  NoticeRig r; const std::string location(70, 'L'); r.link.result = AssistanceTransport::InvalidPayload;
  r.enqueue(FallNotice::NoResidualMovement, 1, location.c_str()); r.poll(0);
  r.link.result = AssistanceTransport::Queued; r.poll(1);
  ASSERT_EQ(1u, r.link.sent.size()); EXPECT_NE(std::string::npos, r.link.sent[0].find("[2/2]"));
  EXPECT_TRUE(r.outbox.partFailed()); r.complete(); r.poll(2);
  EXPECT_TRUE(r.outbox.partFailed()); EXPECT_FALSE(r.outbox.busy());
}
TEST(FallOutbox, ConfirmationHasReservedSlotAndPriorityOverWaitingWarning) {
  NoticeRig r; r.enqueue(FallNotice::AssistanceRequested, 1); r.poll(0);
  ASSERT_TRUE(r.enqueue(FallNotice::UserCouldNotConfirm, 2));
  EXPECT_FALSE(r.enqueue(FallNotice::AssistanceRequested, 3));
  ASSERT_TRUE(r.enqueue(FallNotice::UserConfirmedFall, 2));
  r.complete(); r.poll(1);
  ASSERT_EQ(2u, r.link.sent.size()); EXPECT_EQ("USER HAS CONFIRMED FALL", r.link.sent.back());
  r.complete(); r.poll(2); EXPECT_FALSE(r.outbox.busy());
}
TEST(FallOutbox, ExhaustedWarningDoesNotBlockConfirmation) {
  NoticeRig r; r.link.result = AssistanceTransport::NoPacket;
  r.enqueue(FallNotice::UserCouldNotConfirm);
  for (unsigned n = 0; n <= HealthNodeConfig::ack_max_attempts; ++n) r.poll(n * 5000);
  EXPECT_EQ(AssistanceDelivery::Failed, r.delivery.state());
  r.link.result = AssistanceTransport::Queued; r.enqueue(FallNotice::UserConfirmedFall); r.poll(400000);
  ASSERT_EQ(1u, r.link.sent.size()); EXPECT_EQ("USER HAS CONFIRMED FALL", r.link.sent[0]);
}
TEST(FallOutbox, MultipartRetriesKeepFrozenTextAndUseDistinctTags) {
  NoticeRig r; const std::string location(70, 'L');
  r.enqueue(FallNotice::NoResidualMovement, 1, location.c_str()); r.poll(0);
  const auto first_tag = r.link.tag; r.delivery.txComplete(0, first_tag, false); r.poll(5000);
  ASSERT_EQ(2u, r.link.sent.size()); EXPECT_EQ(r.link.sent[0], r.link.sent[1]);
  EXPECT_NE(first_tag, r.link.tag); r.complete(); r.poll(5001);
  ASSERT_EQ(3u, r.link.sent.size()); EXPECT_NE(std::string::npos, r.link.sent[2].find("[2/2]"));
}

TEST(FallOutbox, FalseAlarmCancelsWarningPartsAndRejectsLateCallbacks) {
  NoticeRig r; const std::string location(70, 'L');
  ASSERT_TRUE(r.enqueue(FallNotice::NoResidualMovement, 1, location.c_str())); r.poll(0);
  const auto old_tag = r.link.tag; uint8_t old_hash[8]; memcpy(old_hash, r.link.fingerprint, 8);
  ASSERT_TRUE(r.enqueue(FallNotice::UserConfirmedFalseAlarm)); r.poll(1);
  EXPECT_EQ(old_tag, r.link.cancelled);
  EXPECT_EQ("User Confirms False Alarm | HOME", r.link.sent.back());
  r.delivery.txComplete(2, old_tag, true); EXPECT_FALSE(r.delivery.confirmRepeat(old_hash));
  r.complete(); r.poll(3); r.poll(600000);
  EXPECT_EQ(2u, r.link.sent.size()); EXPECT_FALSE(r.outbox.busy());
}

TEST(FallOutbox, FalseAlarmLeavesAnotherIncidentsWarningAlone) {
  NoticeRig r; r.enqueue(FallNotice::UserCouldNotConfirm, 1); r.poll(0);
  r.outbox.cancelWarnings(2, r.link, r.delivery);
  ASSERT_TRUE(r.enqueue(FallNotice::UserConfirmedFalseAlarm, 2)); r.poll(1);
  EXPECT_EQ(0u, r.link.cancelled); EXPECT_EQ(1u, r.link.sent.size());
  r.complete(); r.poll(2); EXPECT_EQ("User Confirms False Alarm | HOME", r.link.sent.back());
}

TEST(FallOutbox, CancellationWorksEvenWhileReservedConfirmationSlotIsOccupied) {
  NoticeRig r; r.enqueue(FallNotice::UserCouldNotConfirm, 2); r.poll(0);
  r.enqueue(FallNotice::UserConfirmedFall, 1);
  EXPECT_FALSE(r.enqueue(FallNotice::UserConfirmedFalseAlarm, 2));
  const auto warning_tag = r.link.tag;
  r.outbox.cancelWarnings(2, r.link, r.delivery); EXPECT_EQ(warning_tag, r.link.cancelled);
  r.poll(1); ASSERT_TRUE(r.enqueue(FallNotice::UserConfirmedFalseAlarm, 2));
  r.complete(); r.poll(2); EXPECT_EQ("User Confirms False Alarm | HOME", r.link.sent.back());
}

TEST(FallOutbox, FalseAlarmRetriesKeepFrozenLocation) {
  NoticeRig r; r.enqueue(FallNotice::UserConfirmedFalseAlarm, 1, "Workshop"); r.poll(0);
  r.delivery.txComplete(1, r.link.tag, false); r.poll(5001);
  ASSERT_EQ(2u, r.link.sent.size());
  EXPECT_EQ("User Confirms False Alarm | Workshop", r.link.sent[0]);
  EXPECT_EQ(r.link.sent[0], r.link.sent[1]);
}

TEST(FallOutbox, InitialAlertCancellationIsScopedToOwningIncidentAndRetiresCallbacks) {
  NoticeRig r; uint8_t payload[FallAckMessage::max_payload_bytes];
  const auto len = FallAckMessage::encode(payload, 0, "Possible fall");
  ASSERT_TRUE(r.delivery.begin(0, r.link, payload, len, FallChannel{}, 1)); r.delivery.poll(0, r.link);
  const auto old_tag = r.link.tag; uint8_t old_hash[8]; memcpy(old_hash, r.link.fingerprint, 8);
  EXPECT_FALSE(r.delivery.begin(1, r.link, payload, len, FallChannel{}, 2));
  EXPECT_FALSE(r.delivery.supersedeIncident(r.link, 2)); EXPECT_EQ(0u, r.link.cancelled);
  EXPECT_TRUE(r.delivery.supersedeIncident(r.link, 1)); EXPECT_EQ(old_tag, r.link.cancelled);
  r.delivery.txComplete(2, old_tag, true); EXPECT_FALSE(r.delivery.confirmRepeat(old_hash));
  r.delivery.poll(600000, r.link); EXPECT_FALSE(r.delivery.busy()); EXPECT_EQ(1u, r.link.sent.size());
}

int main(int argc, char** argv) { ::testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
