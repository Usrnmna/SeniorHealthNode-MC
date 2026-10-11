#include <AssistanceTestLink.h>
#include <gtest/gtest.h>
#define ESP32 // Exercise the same local-buffer HAL branch as the combined firmware.
#include <helpers/sensors/MPU6050.h>
#include <helpers/sensors/BoundedAHT.h>
#include <helpers/sensors/TelemetrySnapshot.h>
#include <helpers/StagedBatteryRead.h>
#include <helpers/sensors/EnvironmentTelemetry.h>
#include <helpers/ui/OledFrameTransfer.h>
#include "../../examples/companion_radio/health/MotionRule.h"
#include "../../examples/companion_radio/health/FallResponse.h"

static std::vector<uint8_t> mpuResponse(uint8_t reg, uint8_t count) {
  if (reg == 0x75) return {0x68};
  if (count == 1) return {Wire1.registers[reg]};
  std::vector<uint8_t> bytes(count, 0);
  if (reg == 0x3A && count == 15) { bytes[0] = 1; bytes[5] = 0x10; } // New sample, 1 g.
  return bytes;
}

TEST(SensorTiming, SingleReadFailureRetriesIn20msWithoutReinitializing) {
  Wire1 = TwoWire{}; Wire1.response = mpuResponse;
  MPU6050Sensor sensor;
  EXPECT_FALSE(sensor.poll(0)); EXPECT_TRUE(sensor.poll(100));
  const auto writes = Wire1.writes.size();
  Wire1.response = [](uint8_t, uint8_t) { return std::vector<uint8_t>{}; };
  EXPECT_FALSE(sensor.poll(120)); EXPECT_FALSE(sensor.valid);
  Wire1.response = mpuResponse;
  EXPECT_FALSE(sensor.poll(139)); EXPECT_TRUE(sensor.poll(140));
  EXPECT_TRUE(sensor.valid);
  EXPECT_EQ(Wire1.writes.size(), writes + 2); // Only two data-register reads.
}

TEST(SensorTiming, RepeatedFailuresTryReinitializationBeforeSlowBackoff) {
  Wire1 = TwoWire{}; Wire1.response = mpuResponse;
  MPU6050Sensor sensor; sensor.poll(0); EXPECT_TRUE(sensor.poll(100));
  Wire1.response = [](uint8_t, uint8_t) { return std::vector<uint8_t>{}; };
  sensor.poll(120); sensor.poll(140); sensor.poll(160);
  sensor.poll(180); // Reinitialization also fails: now enter slow backoff.
  const auto reads = Wire1.requests;
  Wire1.response = mpuResponse;
  EXPECT_FALSE(sensor.poll(5179)); EXPECT_EQ(Wire1.requests, reads);
  EXPECT_FALSE(sensor.poll(5180)); EXPECT_GT(Wire1.requests, reads);
  EXPECT_TRUE(sensor.poll(5280));
}

TEST(SensorTiming, RecoveryTimersWorkAcrossMillisWrap) {
  Wire1 = TwoWire{}; Wire1.response = mpuResponse;
  MPU6050Sensor sensor; const uint32_t start = UINT32_MAX - 60;
  sensor.poll(start); EXPECT_TRUE(sensor.poll(start + 100));
  Wire1.response = [](uint8_t, uint8_t) { return std::vector<uint8_t>{}; };
  EXPECT_FALSE(sensor.poll(start + 120));
  Wire1.response = mpuResponse; EXPECT_TRUE(sensor.poll(start + 140));
}

TEST(SensorTiming, DataReadyGatesEvidenceEvenWhenAxesAreIdentical) {
  Wire1 = TwoWire{};
  bool data_ready = true;
  Wire1.response = [&](uint8_t reg, uint8_t count) {
    auto bytes = mpuResponse(reg, count);
    if (reg == 0x3A) bytes[0] = data_ready ? 1 : 0x10; // Other status is not data-ready.
    return bytes;
  };
  MPU6050Sensor sensor;
  sensor.poll(0); ASSERT_TRUE(sensor.poll(100));
  EXPECT_FLOAT_EQ(sensor.values.az, 1.0f);
  data_ready = false;
  EXPECT_FALSE(sensor.poll(120));
  EXPECT_TRUE(sensor.valid); // Cache remains available; not new detector evidence.
  data_ready = true;
  EXPECT_TRUE(sensor.poll(140)); // Identical stationary axes can still be a new sample.
  EXPECT_EQ(Wire1.writes.back(), std::vector<uint8_t>{0x3A});
}

TEST(SensorTiming, MissingDataReadyInvalidatesAndReinitializesAcrossWrap) {
  Wire1 = TwoWire{}; Wire1.response = mpuResponse;
  MPU6050Sensor sensor; const uint32_t start = UINT32_MAX - 200;
  sensor.poll(start); ASSERT_TRUE(sensor.poll(start + 100));
  Wire1.response = [](uint8_t reg, uint8_t count) {
    auto bytes = mpuResponse(reg, count);
    if (reg == 0x3A) bytes[0] = 0;
    return bytes;
  };
  sensor.poll(start + 100 + HealthNodeConfig::motion_data_timeout_ms - 1);
  EXPECT_TRUE(sensor.valid);
  for (unsigned i = 0; i < HealthNodeConfig::motion_fast_failures; ++i) {
    EXPECT_FALSE(sensor.poll(start + 120 + HealthNodeConfig::motion_data_timeout_ms + 20*i));
    EXPECT_FALSE(sensor.valid);
  }
  Wire1.response = mpuResponse;
  const auto recovery = start + 120 + HealthNodeConfig::motion_data_timeout_ms +
                        20*HealthNodeConfig::motion_fast_failures;
  EXPECT_FALSE(sensor.poll(recovery)); // Configuration must be verified again.
  EXPECT_TRUE(sensor.poll(recovery + 100));
}

class MpuConfigReadback : public ::testing::TestWithParam<uint8_t> {};
TEST_P(MpuConfigReadback, MismatchBlocksSamplesUntilSuccessfulRetry) {
  Wire1 = TwoWire{};
  Wire1.response = [&](uint8_t reg, uint8_t count) {
    auto bytes = mpuResponse(reg, count);
    if (reg == GetParam() && count == 1) {
      bytes[0] ^= (reg == 0x1B || reg == 0x1C) ? 0x08 : (reg == 0x37 ? 0x10 : 0x01);
    }
    return bytes;
  };
  MPU6050Sensor sensor;
  EXPECT_FALSE(sensor.poll(0)); EXPECT_FALSE(sensor.valid);
  const auto requests = Wire1.requests;
  EXPECT_FALSE(sensor.poll(4999)); EXPECT_EQ(Wire1.requests, requests);
  Wire1.response = mpuResponse;
  EXPECT_FALSE(sensor.poll(5000)); EXPECT_TRUE(sensor.poll(5100));
}
INSTANTIATE_TEST_SUITE_P(EveryRequiredRegister, MpuConfigReadback,
    ::testing::Values(0x6B, 0x6C, 0x1A, 0x19, 0x1B, 0x1C, 0x37, 0x38));

TEST(SensorTiming, ReadbackBusFailureCannotBeMistakenForCorrectConfiguration) {
  Wire1 = TwoWire{};
  Wire1.response = [](uint8_t reg, uint8_t count) {
    return reg == 0x1C ? std::vector<uint8_t>{} : mpuResponse(reg, count);
  };
  MPU6050Sensor sensor;
  sensor.poll(0); EXPECT_FALSE(sensor.poll(100)); EXPECT_FALSE(sensor.valid);
}

TEST(SensorTiming, ReadbackIgnoresReservedBitsButChecksFilterAndInterruptControl) {
  Wire1 = TwoWire{};
  Wire1.response = [](uint8_t reg, uint8_t count) {
    auto bytes = mpuResponse(reg, count);
    if (reg == 0x1A) bytes[0] |= 0xC0;
    if (reg == 0x38) bytes[0] |= 0xE6;
    return bytes;
  };
  MPU6050Sensor sensor; sensor.poll(0); EXPECT_TRUE(sensor.poll(100));
  EXPECT_EQ(Wire1.registers[0x37] & 0x10, 0); // Reads of other registers cannot clear status.
  EXPECT_EQ(Wire1.registers[0x38] & 0x01, 1); // Data-ready generation enabled.
}

TEST(SensorTiming, AhtStuckBusyHasBoundedInitializationAndMeasurement) {
  TwoWire bus; bus.response = [](uint8_t, uint8_t) { return std::vector<uint8_t>{0x88}; };
  BoundedAHT aht; aht.begin(bus, 0x38, 0);
  EXPECT_EQ(aht.poll(999, 1000), BoundedAHT::Pending);
  EXPECT_EQ(aht.poll(1000, 1000), BoundedAHT::Failed);
  bus.response = [](uint8_t, uint8_t) { return std::vector<uint8_t>{0x08}; };
  aht.begin(bus, 0x38, 1000); aht.poll(1020, 1000);
  EXPECT_EQ(aht.poll(1040, 1000), BoundedAHT::Complete);
  EXPECT_TRUE(aht.start(1100));
  bus.response = [](uint8_t, uint8_t) { return std::vector<uint8_t>{0x88}; };
  EXPECT_EQ(aht.poll(2099, 1000), BoundedAHT::Pending);
  EXPECT_EQ(aht.poll(2100, 1000), BoundedAHT::Failed);
}

TEST(SensorTiming, AhtDecodesCompleteReadingAndRejectsShortReads) {
  TwoWire bus; bus.response = [](uint8_t, uint8_t count) {
    return count == 1 ? std::vector<uint8_t>{0x08} :
                       std::vector<uint8_t>{0x08, 0x80, 0x00, 0x06, 0x00, 0x00};
  };
  BoundedAHT aht; aht.begin(bus, 0x38, 0); aht.poll(20, 1000); aht.poll(40, 1000);
  ASSERT_TRUE(aht.start(50)); EXPECT_EQ(aht.poll(129, 1000), BoundedAHT::Pending);
  EXPECT_EQ(aht.poll(130, 1000), BoundedAHT::Complete);
  EXPECT_FLOAT_EQ(aht.humidity, 50); EXPECT_FLOAT_EQ(aht.temperature, 25);
  ASSERT_TRUE(aht.start(200));
  bus.response = [](uint8_t, uint8_t) { return std::vector<uint8_t>{}; };
  EXPECT_EQ(aht.poll(280, 1000), BoundedAHT::Failed);
}

TEST(SensorTiming, CacheExpiresAcrossWrapAndFailureInvalidatesOldValues) {
  TelemetrySnapshot cache; const uint8_t bytes[] = {3, 103, 0, 250};
  EXPECT_FALSE(cache.fresh(0, 1000));
  const uint32_t start = UINT32_MAX - 30;
  cache.publish(bytes, sizeof(bytes), start);
  EXPECT_TRUE(cache.fresh(start + 999, 1000));
  EXPECT_FALSE(cache.fresh(start + 1000, 1000));
  cache.publish(bytes, sizeof(bytes), 500);
  cache.publish(nullptr, 0, 501); EXPECT_FALSE(cache.fresh(501, 1000));
  cache.publish(bytes, TelemetrySnapshot::capacity + 1, 600);
  EXPECT_FALSE(cache.fresh(600, 1000)); // Oversize never reads past input.
}

TEST(SensorTiming, BatterySettlesWithoutReadingAndAveragesOncePerInterval) {
  StagedBatteryRead battery; unsigned reads = 0; bool powered = false;
  auto poll = [&](uint32_t now) {
    battery.poll(now, 1000, 10, 8, [&](bool value) { powered = value; },
      [&]() { return 100 + reads++; });
  };
  const uint32_t start = UINT32_MAX - 5;
  poll(start); EXPECT_TRUE(powered); EXPECT_FALSE(battery.hasReading());
  poll(start + 9); EXPECT_EQ(reads, 0);
  for (unsigned i = 0; i < 8; ++i) poll(start + 10 + i);
  EXPECT_FALSE(powered); EXPECT_TRUE(battery.hasReading());
  EXPECT_EQ(reads, 8); EXPECT_EQ(battery.rawAverage(), 103);
  poll(start + 1016); EXPECT_FALSE(powered); EXPECT_EQ(reads, 8);
  poll(start + 1017); EXPECT_TRUE(powered); EXPECT_EQ(reads, 8);
}

TEST(SensorTiming, AnalysisDetectsAgainWhilePriorSosRemainsActive) {
  MotionRule rule; FallResponse response; uint32_t now = 0; unsigned detected = 0;
  auto sample = [&](float a, float g) {
    now += 20;
    if (rule.update(now, a, g, true)) {
      ++detected; response.onFall(now, false);
    }
  };
  auto fall = [&]() { sample(.3f, 0); sample(3, 300); for (int i=0;i<52;++i) sample(1,0); };
  for (int i=0;i<102;++i) sample(1,0);
  fall(); EXPECT_EQ(detected, 1); ASSERT_TRUE(response.alarmActive());
  for (int i=0;i<3100;++i) sample(1,0);
  fall(); EXPECT_EQ(detected, 2); EXPECT_TRUE(response.alarmActive());
}

TEST(SensorTiming, NewFallAndAcknowledgmentDoNotResetOlderMessageRetryBudget) {
  FallResponse response; ASSERT_TRUE(response.onFall(0, false));
  auto click = [&](uint32_t t) {
    response.updateButton(t, true); response.updateButton(t+25, true);
    response.updateButton(t+100, false); return response.updateButton(t+125, false);
  };
  click(100); click(300); ASSERT_TRUE(click(500));
  AssistanceDelivery delivery; AssistanceTestLink link; link.start(delivery, 700);
  delivery.poll(700, link); EXPECT_EQ(delivery.attemptCount(), 1);
  ASSERT_TRUE(response.onFall(1000, false));
  click(1100); click(1300); ASSERT_TRUE(click(1500));
  link.start(delivery, 1600); // Coalesced event cannot restart delivery budget.
  delivery.poll(1600, link); EXPECT_EQ(delivery.attemptCount(), 1);
  for (unsigned i=1;i<HealthNodeConfig::ack_max_attempts;++i)
    delivery.poll(700 + 5000*i, link);
  EXPECT_EQ(delivery.state(), AssistanceDelivery::Failed);
  response.messageAccepted();
  EXPECT_FALSE(response.messagePending());
}

TEST(SensorTiming, EnvironmentalReplayPreservesExactFieldsAndHonorsCapacity) {
  const uint8_t bytes[] = {3,103,0xff,0xfb, 4,104,99, 5,100,0xde,0xad,0xbe,0xef};
  TelemetrySnapshot snapshot; snapshot.publish(bytes, sizeof(bytes), 0);
  EnvironmentTelemetry decoder; CayenneLPP dest(64);
  decoder.append(snapshot, dest);
  ASSERT_EQ(dest.getSize(), sizeof(bytes));
  EXPECT_EQ(memcmp(dest.getBuffer(), bytes, sizeof(bytes)), 0);
  CayenneLPP small(6); decoder.append(snapshot, small);
  EXPECT_EQ(small.getSize(), 4); // No partial second field.
  EXPECT_EQ(memcmp(small.getBuffer(), bytes, 4), 0);
  snapshot.size = 2; CayenneLPP truncated(64); decoder.append(snapshot, truncated);
  EXPECT_EQ(truncated.getSize(), 0);
}

TEST(SensorTiming, OledChunksCoverFrameOnceWithoutCrossingPageAndAbortOnFault) {
  OledFrameTransfer frame; EXPECT_FALSE(frame.pending()); frame.start();
  unsigned total = 0, chunks = 0;
  while (frame.pending()) {
    EXPECT_EQ(frame.position(), total);
    EXPECT_LE(frame.position() % 128 + frame.chunk_bytes, 128);
    total += frame.chunk_bytes; ++chunks; frame.completed(true);
  }
  EXPECT_EQ(total, 1024); EXPECT_EQ(chunks, 32);
  frame.start(); frame.completed(true); frame.completed(false);
  EXPECT_FALSE(frame.pending()); frame.completed(true); EXPECT_FALSE(frame.pending());
  frame.start(); EXPECT_EQ(frame.position(), 0); frame.cancel(); EXPECT_FALSE(frame.pending());
}

int main(int argc, char** argv) { ::testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
