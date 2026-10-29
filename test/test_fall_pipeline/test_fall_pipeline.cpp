#include <gtest/gtest.h>
#include <array>
#include <string>
#include <vector>
#define ESP32
#include <helpers/sensors/MPU6050.h>
#include <helpers/ui/BuzzerPattern.h>
#include <MpuFifoFake.h>
#include "../../examples/companion_radio/health/MotionRule.h"
#include "../../examples/companion_radio/health/FallResponse.h"
#include "../../examples/companion_radio/health/FallInitialAlert.h"
#include "../../examples/companion_radio/health/FallNoticeOutbox.h"

// Production component integration with independently supplied FIFO bytes and
// transport outcomes. The bounded glue mirrors HealthMonitor's ordering but does
// not execute MyMesh, FreeRTOS, physical I2C/PWM, BLE, or the RF dispatcher.
struct PipelineLink : AssistanceTransport {
  struct Attempt {
    uint32_t tag;
    std::vector<uint8_t> body;
    std::array<uint8_t, 8> fingerprint;
    FallChannel channel;
    std::string text() const { return std::string(body.begin() + 5, body.end()); }
  };
  Result result = Queued;
  uint32_t timestamp = 0;
  std::vector<Attempt> attempts;
  std::vector<uint32_t> cancelled;
  std::vector<AssistanceDelivery*> owners;
  uint32_t uniqueTimestamp() override { return ++timestamp; }
  Result queueAssistance(const uint8_t* data, unsigned size, uint32_t tag,
                        uint8_t hash[8], const FallChannel& channel) override {
    std::array<uint8_t, 8> fingerprint{};
    memcpy(fingerprint.data(), &tag, 4); memcpy(fingerprint.data() + 4, &timestamp, 4);
    memcpy(hash, fingerprint.data(), 8);
    attempts.push_back({tag, {data, data + size}, fingerprint, channel});
    return result;
  }
  void cancelAssistance(uint32_t tag) override {
    cancelled.push_back(tag);
    for (auto* delivery : owners) delivery->txComplete(0, tag, false);
  }
  const Attempt* find(uint32_t tag) const {
    for (auto i = attempts.rbegin(); i != attempts.rend(); ++i) if (i->tag == tag) return &*i;
    return nullptr;
  }
};

struct FallPipeline {
  MpuFifoFake hardware;
  MPU6050Sensor sensor;
  MotionRule detector;
  FallResponse response;
  FallChannel channel;
  FallInitialAlert initial;
  AssistanceDelivery delivery{1}, assistance{0};
  FallNoticeOutbox notices;
  PipelineLink link;
  BuzzerPattern buzzer{HealthNodeConfig::morse_dot_ms, HealthNodeConfig::sos_silence_ms};
  uint32_t now, generation = 0;
  unsigned detections = 0, alarm_starts = 0, max_processed = 0;
  bool pressed = false;
  std::vector<uint32_t> captures, declarations;

  explicit FallPipeline(uint32_t start = 0) : now(start) {
    link.owners = {&delivery, &assistance};
    pollMotion(); now += 100; pollMotion(); // Configure, settle, then reset FIFO.
  }
  unsigned pollMotion() {
    unsigned processed = 0;
    for (unsigned budget = 0; budget < 4; ++budget) {
      const bool fresh = sensor.poll(now);
      if (generation != sensor.discontinuityGeneration()) {
        generation = sensor.discontinuityGeneration();
        response.motionUnavailable(); detector.update(now, 0, 0, false);
      }
      if (!sensor.valid) {
        response.motionUnavailable(); detector.update(now, 0, 0, false); break;
      }
      if (!fresh) break;
      ++processed;
      const auto at = sensor.sampleTimeMs();
      const auto& v = sensor.values;
      captures.push_back(at);
      response.observeMotion(at, v.ax, v.ay, v.az, v.rotation());
      if (detector.update(at, v.ax, v.ay, v.az, v.rotation(), true)) {
        ++detections;
        if (response.onFall(now, pressed, channel)) {
          ++alarm_starts; declarations.push_back(now); buzzer.startSOS(now);
        }
        char text[140];
        snprintf(text, sizeof(text), "Possible fall (%s): peak accel=%.2fg rotation=%.1fdeg/s",
            MotionRule::reasonName(detector.eventKind()), detector.peakAcceleration(), detector.peakRotation());
        initial.enqueue(response.incidentId(), response.incidentChannel(), text);
      }
      if (!sensor.queuedSamples()) break;
    }
    if (processed > max_processed) max_processed = processed;
    return processed;
  }
  void serviceResponse() {
    if (response.updateButton(now, pressed)) {
      buzzer.stop();
      if (response.falseAlarmConfirmed()) {
        delivery.supersedeIncident(link, response.incidentId());
        initial.cancel(response.incidentId());
        notices.cancelWarnings(response.incidentId(), link, assistance);
      }
    }
    response.updateDeadline(now);
    if (response.messagePending()) {
      const auto notice = response.pendingNotice();
      const auto message = FallNoticeMessage::build(notice, false, 0, 0);
      if (notices.enqueue(notice, response.pendingIncident(), message, response.pendingChannel()))
        response.messageAccepted();
    }
    notices.poll(now, link, assistance);
    delivery.poll(now, link);
    initial.poll(now, link, delivery);
    buzzer.update(now);
  }
  void sample(float acceleration = 1, float rotation = 0) {
    hardware.push(acceleration, rotation); now += 20; pollMotion(); serviceResponse();
  }
  void vector(float acceleration, float degrees, float rotation = 0) {
    const float angle = degrees * 0.0174532925f;
    hardware.pushRaw({{int16_t(acceleration * sinf(angle) * 4096), 0,
        int16_t(acceleration * cosf(angle) * 4096), 0, int16_t(rotation * 65.5f), 0, 0}});
    now += 20; pollMotion(); serviceResponse();
  }
  void hold(unsigned duration, float acceleration = 1, float rotation = 0) {
    for (unsigned elapsed = 0; elapsed < duration; elapsed += 20) sample(acceleration, rotation);
  }
  void arm() { hold(300); }
  void fall(float peak = 3) { sample(.3f); sample(peak, 300); }
  void click() { pressed = true; hold(60); pressed = false; hold(60); }
  void acknowledge() { click(); click(); click(); }
  void dismiss() { click(); click(); hold(3000); }
  void complete(AssistanceDelivery& owner) {
    const auto tag = owner.queuedTag();
    const auto* attempt = link.find(tag);
    ASSERT_NE(nullptr, attempt);
    const auto hash = attempt->fingerprint;
    owner.txComplete(now, tag, true); EXPECT_TRUE(owner.confirmRepeat(hash.data()));
  }
};

TEST(FallPipeline, BoundedBatchesRetainTimingAndUseWallTimeForTheIncidentAcrossWrap) {
  for (uint32_t origin : {0u, UINT32_MAX - 500}) {
    FallPipeline p(origin); p.arm();
    const auto previous = p.captures.back();
    const auto count = p.captures.size();
    for (unsigned i = 0; i < 10; ++i) p.hardware.push(i == 5 ? .3f : i == 6 ? 3 : 1, i == 6 ? 300 : 0);
    p.now += 200;
    EXPECT_EQ(4u, p.pollMotion()); EXPECT_EQ(0u, p.detections); EXPECT_EQ(6u, p.sensor.queuedSamples());
    p.serviceResponse();
    EXPECT_EQ(4u, p.pollMotion()); EXPECT_EQ(1u, p.detections); EXPECT_EQ(2u, p.sensor.queuedSamples());
    p.serviceResponse();
    EXPECT_EQ(2u, p.pollMotion()); p.serviceResponse();
    ASSERT_EQ(count + 10, p.captures.size());
    for (unsigned i = 0; i < 10; ++i) EXPECT_EQ(uint32_t(previous + (i + 1) * 20), p.captures[count + i]);
    ASSERT_EQ(1u, p.declarations.size()); EXPECT_EQ(p.now, p.declarations[0]);
    EXPECT_EQ(4u, p.max_processed); EXPECT_TRUE(p.buzzer.alarmActive());
    p.response.updateDeadline(p.now + HealthNodeConfig::response_timeout_ms - 1);
    EXPECT_FALSE(p.response.messagePending());
    p.response.updateDeadline(p.now + HealthNodeConfig::response_timeout_ms);
    EXPECT_EQ(FallNotice::UserCouldNotConfirm, p.response.pendingNotice());
  }
}

TEST(FallPipeline, LostFifoCannotJoinAnOldTroughToImpactAndWalkingRecoveryAllowsANewEvent) {
  for (unsigned failure = 0; failure < 3; ++failure) {
    FallPipeline p; p.arm(); p.sample(.3f);
    const auto generation = p.generation;
    if (failure == 0) { p.hardware.push(3, 300); p.hardware.short_bytes = 7; p.now += 20; }
    if (failure == 1) { for (unsigned i = 0; i < 74; ++i) p.hardware.push(3, 300); p.now += 20; }
    if (failure == 2) { p.hardware.push(3, 300); p.now += 1020; }
    EXPECT_EQ(0u, p.pollMotion()); p.serviceResponse();
    EXPECT_NE(generation, p.generation); EXPECT_EQ(0u, p.detections); EXPECT_FALSE(p.response.alarmActive());
    p.hardware.short_bytes = -1; p.now += 20; p.pollMotion(); p.serviceResponse();
    p.sample(3, 300); p.hold(300, 1.4f, 40);
    EXPECT_EQ(0u, p.detections); EXPECT_TRUE(p.detector.available());
    p.fall(); EXPECT_EQ(1u, p.detections); EXPECT_TRUE(p.buzzer.alarmActive());
  }
}

TEST(FallPipeline, OrientationPathUsesActualFifoAxesWithoutLowGOrRapidRotation) {
  FallPipeline p; p.arm(); p.vector(3, 0);
  for (unsigned i = 0; i < 60; ++i) p.vector(1, 70);
  EXPECT_EQ(1u, p.detections); EXPECT_EQ(MotionRule::ImpactOrientation, p.detector.eventKind());
  EXPECT_TRUE(p.response.alarmActive()); EXPECT_TRUE(p.buzzer.alarmActive());
}

TEST(FallPipeline, NewAlarmAfterAcknowledgementWaitsBehindAnImmutableBusyDelivery) {
  FallPipeline p; p.arm(); p.fall(); p.sample();
  ASSERT_NE(0u, p.delivery.queuedTag());
  const auto old_tag = p.delivery.queuedTag();
  const auto old_hash = p.link.find(old_tag)->fingerprint;
  p.acknowledge(); ASSERT_FALSE(p.response.alarmActive()); ASSERT_FALSE(p.buzzer.alarmActive());
  p.hold(1400);
  p.channel.public_channel = false; strcpy(p.channel.name, "Second"); p.channel.key[0] ^= 0x55;
  p.fall(4);
  EXPECT_EQ(2u, p.alarm_starts); EXPECT_EQ(2u, p.response.incidentId());
  EXPECT_TRUE(p.buzzer.alarmActive()); EXPECT_TRUE(p.initial.pending());
  EXPECT_EQ(old_tag, p.delivery.queuedTag()); EXPECT_EQ(1u, p.delivery.attemptCount());
  p.complete(p.delivery); p.serviceResponse(); p.serviceResponse();
  const auto new_tag = p.delivery.queuedTag();
  ASSERT_NE(0u, new_tag); EXPECT_NE(old_tag, new_tag); EXPECT_EQ(1u, p.delivery.attemptCount());
  const auto* next = p.link.find(new_tag); ASSERT_NE(nullptr, next);
  EXPECT_NE(std::string::npos, next->text().find("peak accel=4.00g"));
  EXPECT_EQ(0, memcmp(p.channel.key, next->channel.key, p.channel.key_length));
  p.delivery.txComplete(p.now, old_tag, false); EXPECT_FALSE(p.delivery.confirmRepeat(old_hash.data()));
  EXPECT_EQ(AssistanceDelivery::Queued, p.delivery.state());
}

TEST(FallPipeline, DismissalCancelsOnlyItsWaitingAlertAndAnotherFallStillStartsAnAlarm) {
  FallPipeline p; p.arm(); p.fall(); p.sample();
  const auto old_tag = p.delivery.queuedTag(); ASSERT_NE(0u, old_tag);
  p.acknowledge(); p.hold(1400); p.fall();
  ASSERT_TRUE(p.initial.pending()); ASSERT_EQ(2u, p.response.incidentId());
  p.dismiss();
  EXPECT_TRUE(p.response.falseAlarmConfirmed()); EXPECT_FALSE(p.response.alarmActive());
  EXPECT_FALSE(p.buzzer.alarmActive()); EXPECT_FALSE(p.initial.pending());
  EXPECT_EQ(old_tag, p.delivery.queuedTag()); EXPECT_TRUE(p.delivery.busy());
  EXPECT_TRUE(p.link.cancelled.empty());
  p.fall();
  EXPECT_EQ(3u, p.alarm_starts); EXPECT_EQ(3u, p.response.incidentId());
  EXPECT_TRUE(p.buzzer.alarmActive()); EXPECT_TRUE(p.initial.pending());
  EXPECT_EQ(old_tag, p.delivery.queuedTag()); EXPECT_EQ(1u, p.delivery.attemptCount());
}

TEST(FallPipeline, RepeatedDetectionPreservesTheUnresolvedDeadlineAndRetryOwner) {
  FallPipeline p; p.link.result = AssistanceTransport::NoPacket;
  p.arm(); p.fall(); p.sample();
  const auto declared = p.declarations.back();
  p.hold(1400, 1.4f, 40); p.fall();
  EXPECT_EQ(2u, p.detections); EXPECT_EQ(1u, p.alarm_starts); EXPECT_EQ(1u, p.response.incidentId());
  EXPECT_FALSE(p.initial.pending()); EXPECT_EQ(1u, p.delivery.attemptCount());
  p.response.updateDeadline(declared + HealthNodeConfig::response_timeout_ms - 1);
  EXPECT_FALSE(p.response.messagePending());
  p.response.updateDeadline(declared + HealthNodeConfig::response_timeout_ms);
  EXPECT_EQ(FallNotice::UserCouldNotConfirm, p.response.pendingNotice());
  EXPECT_TRUE(p.buzzer.alarmActive());
}

TEST(FallPipeline, MissingDataBreaksStillnessAndLaterFreshStillnessProducesAnUpdate) {
  FallPipeline p; p.arm(); p.fall(); p.sample();
  p.hold(20000, 1.1f, 10); p.hold(9000);
  p.hardware.short_bytes = 0; p.sample();
  p.hardware.short_bytes = -1; p.sample(); p.hold(1100);
  const auto warning_tag = p.assistance.queuedTag();
  const auto* warning = p.link.find(warning_tag); ASSERT_NE(nullptr, warning);
  EXPECT_NE(std::string::npos, warning->text().find("USER COULD NOT CONFIRM"));
  p.complete(p.assistance); p.serviceResponse(); p.hold(10000);
  const auto* later = p.link.find(p.assistance.queuedTag()); ASSERT_NE(nullptr, later);
  EXPECT_NE(warning_tag, later->tag);
  EXPECT_NE(std::string::npos, later->text().find("NO RESIDUAL MOVEMENTS"));
  EXPECT_TRUE(p.buzzer.alarmActive()); EXPECT_EQ(1u, p.alarm_starts);
}

TEST(FallInitialAlert, RepeatedIncidentCannotResetRetriesOrReplaceAcceptedText) {
  FallInitialAlert initial; AssistanceDelivery delivery{1}; PipelineLink link;
  link.result = AssistanceTransport::NoPacket;
  ASSERT_TRUE(initial.enqueue(1, FallChannel{}, "First")); initial.poll(0, link, delivery); delivery.poll(0, link);
  for (uint32_t now = 20; now < HealthNodeConfig::ack_retry_ms; now += 20) {
    EXPECT_FALSE(initial.enqueue(1, FallChannel{}, "Replacement"));
    initial.poll(now, link, delivery); delivery.poll(now, link);
  }
  EXPECT_EQ(1u, delivery.attemptCount());
  delivery.poll(HealthNodeConfig::ack_retry_ms, link);
  ASSERT_EQ(2u, link.attempts.size()); EXPECT_EQ(2u, delivery.attemptCount());
  EXPECT_EQ("First", link.attempts[0].text()); EXPECT_EQ("First", link.attempts[1].text());
  EXPECT_NE(link.attempts[0].tag, link.attempts[1].tag);
}

TEST(FallInitialAlert, WaitingIncidentsCoalesceWithoutChangingTheActiveOwner) {
  FallInitialAlert initial; AssistanceDelivery delivery{1}; PipelineLink link;
  const FallChannel original;
  ASSERT_TRUE(initial.enqueue(1, original, "First")); initial.poll(0, link, delivery); delivery.poll(0, link);
  const auto old_tag = delivery.queuedTag(); const auto hash = link.attempts.back().fingerprint;
  ASSERT_TRUE(initial.enqueue(2, original, "Second"));
  FallChannel third; third.key[0] ^= 0x55;
  ASSERT_TRUE(initial.enqueue(3, third, "Third")); initial.cancel(2);
  EXPECT_EQ(1u, initial.coalescedCount()); EXPECT_TRUE(initial.pending());
  initial.poll(100, link, delivery); EXPECT_EQ(old_tag, delivery.queuedTag()); EXPECT_EQ(1u, delivery.attemptCount());
  delivery.txComplete(100, old_tag, true); EXPECT_TRUE(delivery.confirmRepeat(hash.data()));
  initial.poll(101, link, delivery); delivery.poll(101, link);
  ASSERT_EQ(2u, link.attempts.size()); EXPECT_EQ("Third", link.attempts.back().text());
  EXPECT_EQ(0, memcmp(third.key, link.attempts.back().channel.key, third.key_length));
  EXPECT_FALSE(initial.pending()); EXPECT_EQ(1u, delivery.attemptCount());
}

TEST(FallInitialAlert, ExhaustedDeliveryReleasesTheWaitingIncidentWithItsOwnBudget) {
  FallInitialAlert initial; AssistanceDelivery delivery{1}; PipelineLink link;
  link.result = AssistanceTransport::NoPacket;
  ASSERT_TRUE(initial.enqueue(1, FallChannel{}, "First")); initial.poll(0, link, delivery);
  ASSERT_TRUE(initial.enqueue(2, FallChannel{}, "Second"));
  for (unsigned i = 0; i < HealthNodeConfig::ack_max_attempts; ++i) {
    delivery.poll(i * HealthNodeConfig::ack_retry_ms, link);
    if (i + 1 < HealthNodeConfig::ack_max_attempts) initial.poll(i * HealthNodeConfig::ack_retry_ms, link, delivery);
  }
  EXPECT_EQ(AssistanceDelivery::Failed, delivery.state());
  EXPECT_EQ(HealthNodeConfig::ack_max_attempts, delivery.attemptCount());
  ASSERT_EQ(HealthNodeConfig::ack_max_attempts, link.attempts.size());
  for (const auto& attempt : link.attempts) EXPECT_EQ("First", attempt.text());
  const auto now = HealthNodeConfig::ack_max_attempts * HealthNodeConfig::ack_retry_ms;
  link.result = AssistanceTransport::Queued; initial.poll(now, link, delivery); delivery.poll(now, link);
  EXPECT_EQ(1u, delivery.attemptCount()); EXPECT_EQ("Second", link.attempts.back().text());
}

int main(int argc, char** argv) { ::testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
