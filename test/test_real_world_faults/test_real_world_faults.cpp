#include <gtest/gtest.h>
#define ESP32 // Exercise the production HAL-buffer path with transaction fakes.
#include <helpers/sensors/MPU6050.h>
#include <helpers/sensors/BoundedAHT.h>
#include <helpers/ui/BuzzerPattern.h>
#include <helpers/ui/OledFrameTransfer.h>
#include <helpers/StagedBatteryRead.h>
#include "../../examples/simple_sensor/MotionRule.h"
#include "../../examples/simple_sensor/FallResponse.h"
#include "../../examples/simple_sensor/LowBatteryAlert.h"
#include "../../examples/simple_sensor/FallAckMessage.h"
#include <array>
#include <string>

// Deterministic device-input harness. Production classes own all state transitions.
// The glue follows main.cpp; this does NOT run MyMesh, an RTOS, or physical buses.
struct FaultRig {
  MPU6050Sensor sensor;
  MotionRule motion;
  FallResponse response;
  BuzzerPattern buzzer{HealthNodeConfig::morse_dot_ms, HealthNodeConfig::sos_silence_ms};
  LowBatteryAlert battery;
  uint32_t now;
  unsigned falls = 0;
  float acceleration = 1, rotation = 0;
  int read_length = 14;
  uint8_t identity = 0x68;
  bool pressed = false;

  explicit FaultRig(uint32_t start = 0) : now(start) {
    Wire1 = TwoWire{};
    Wire1.response = [this](uint8_t reg, uint8_t count) {
      if (reg == 0x75) return std::vector<uint8_t>{identity};
      std::vector<uint8_t> bytes(14, 0);
      auto word = [&](unsigned offset, int16_t value) {
        bytes[offset] = uint16_t(value) >> 8; bytes[offset + 1] = uint8_t(value);
      };
      word(4, int16_t(acceleration * 4096));
      word(8, int16_t(rotation * 65.5f));
      bytes.resize(read_length);
      return bytes;
    };
  }
  ~FaultRig() { Wire1 = TwoWire{}; } // Do not leave a callback to a destroyed rig.
  void tick(uint32_t step = 20) {
    now += step;
    const bool fresh = sensor.poll(now);
    if (!sensor.valid) motion.update(now, 0, 0, false);
    else if (fresh && motion.update(now, sensor.values.acceleration(), sensor.values.rotation(), true)) {
      ++falls;
      if (response.onFall(now, pressed)) buzzer.startSOS(now);
    }
    if (response.updateButton(now, pressed)) buzzer.stop();
    buzzer.update(now);
  }
  void sample(float a, float g = 0, uint32_t step = 20) {
    acceleration = a; rotation = g; tick(step);
  }
  void quiet(unsigned samples = 110) {
    for (unsigned i = 0; i < samples; ++i) sample(1);
  }
  void fall() { sample(.3f); sample(3, 300); quiet(52); }
  void click(bool bounce = false) {
    if (bounce) {
      pressed = true; tick(5); pressed = false; tick(5);
    }
    pressed = true; tick(20); tick(30);
    if (bounce) { pressed = false; tick(5); pressed = true; tick(5); }
    tick(50); pressed = false; tick(20); tick(30);
  }
  void acknowledge() { click(); click(); click(); }
  bool voltage(uint16_t mv, bool charging = false) {
    const bool due = battery.update(now, mv, charging);
    if (!battery.isActive()) buzzer.stopLowBattery();
    else if (due && buzzer.startLowBattery(now)) { battery.markPlayed(now); return true; }
    return false;
  }
};

class ShortMpuRead : public ::testing::TestWithParam<int> {};
TEST_P(ShortMpuRead, CancelsConfirmationThenRecoversForANewFall) {
  FaultRig r; r.quiet(120); r.sample(.3f); r.sample(3, 300); r.quiet(20);
  r.read_length = GetParam(); r.tick();
  EXPECT_FALSE(r.sensor.valid); EXPECT_EQ(r.falls, 0u);
  r.read_length = 14; r.quiet(160);
  EXPECT_EQ(r.falls, 0u); EXPECT_FALSE(r.response.alarmActive());
  r.fall(); EXPECT_EQ(r.falls, 1u); EXPECT_TRUE(r.buzzer.alarmActive());
}
INSTANTIATE_TEST_SUITE_P(AllTruncatedLengths, ShortMpuRead, ::testing::Range(0, 14));

class LoopStall : public ::testing::TestWithParam<uint32_t> {};
TEST_P(LoopStall, CannotJoinFallEvidenceAcrossMissingTime) {
  FaultRig r; r.quiet(120); r.sample(.3f); r.sample(3, 300, GetParam());
  r.quiet(160); EXPECT_EQ(r.falls, 0u);
  r.fall(); EXPECT_EQ(r.falls, 1u);
}
INSTANTIATE_TEST_SUITE_P(OverGapLimit, LoopStall, ::testing::Values(101u, 250u, 1000u, 5000u, 60000u));

class FaultStage : public ::testing::TestWithParam<int> {};
TEST_P(FaultStage, DisconnectNeverCompletesAnOldCandidate) {
  FaultRig r; r.quiet(120);
  if (GetParam() >= 1) r.sample(.3f);
  if (GetParam() >= 2) { r.sample(3, 300); r.quiet(20); }
  r.read_length = 0; r.identity = 0;
  r.quiet(400); EXPECT_FALSE(r.sensor.valid); EXPECT_EQ(r.falls, 0u);
  r.read_length = 14; r.identity = 0x68;
  r.quiet(400); EXPECT_TRUE(r.sensor.valid); EXPECT_EQ(r.falls, 0u);
  r.fall(); EXPECT_EQ(r.falls, 1u);
}
INSTANTIATE_TEST_SUITE_P(ArmedPeaksConfirming, FaultStage, ::testing::Values(0, 1, 2));

TEST(RealWorldFaults, WrongDeviceIdentityNeverProducesMotionEvidence) {
  FaultRig r; r.identity = 0x69;
  for (unsigned i = 0; i < 10; ++i) { r.quiet(260); r.fall(); }
  EXPECT_FALSE(r.sensor.valid); EXPECT_EQ(r.falls, 0u);
  r.identity = 0x68; r.quiet(400); r.fall(); EXPECT_EQ(r.falls, 1u);
}

TEST(RealWorldFaults, ConfigurationWriteFailureRecoversAfterBackoff) {
  FaultRig r; Wire1.status = 2; r.quiet(300);
  EXPECT_FALSE(r.sensor.valid); EXPECT_EQ(r.falls, 0u);
  Wire1.status = 0; r.quiet(400); r.fall(); EXPECT_EQ(r.falls, 1u);
}

TEST(RealWorldFaults, AlternatingReadFailuresNeverAccumulateQuietEvidence) {
  FaultRig r;
  for (unsigned i = 0; i < 3000; ++i) {
    r.read_length = (i % 2) ? 14 : 0; r.sample(i % 7 == 0 ? .3f : 1);
  }
  EXPECT_EQ(r.falls, 0u);
  r.read_length = 14; r.quiet(400); r.fall(); EXPECT_EQ(r.falls, 1u);
}

TEST(RealWorldFaults, DisconnectDuringSosPreservesAlarmAndCooldown) {
  FaultRig r; r.quiet(120); r.fall(); ASSERT_EQ(r.falls, 1u);
  r.read_length = 0; r.identity = 0; r.quiet(300);
  EXPECT_TRUE(r.response.alarmActive()); EXPECT_TRUE(r.buzzer.alarmActive());
  r.read_length = 14; r.identity = 0x68; r.quiet(400); r.fall();
  EXPECT_EQ(r.falls, 1u); // Reconnection must not bypass the 60-second cooldown.
  r.quiet(3100); r.fall();
  EXPECT_EQ(r.falls, 2u); EXPECT_TRUE(r.buzzer.alarmActive());
}

TEST(RealWorldFaults, BouncingClicksSilenceSosDespiteDisconnectedSensor) {
  FaultRig r; r.quiet(120); r.fall();
  r.read_length = 0;
  r.click(true); r.click(true); EXPECT_TRUE(r.buzzer.alarmActive());
  r.click(true); EXPECT_FALSE(r.buzzer.alarmActive());
  EXPECT_TRUE(r.response.messagePending());
}

TEST(RealWorldFaults, HeldButtonAtFallNeedsThreeNewClicks) {
  FaultRig r; r.quiet(120); r.pressed = true; r.fall();
  r.pressed = false; r.tick(); r.tick(30);
  r.click(); r.click(); EXPECT_TRUE(r.response.alarmActive());
  r.click(); EXPECT_FALSE(r.response.alarmActive());
}

TEST(RealWorldFaults, LoopStallDuringPressDoesNotCountAsShortClick) {
  FaultRig r; r.quiet(120); r.fall(); r.click(); r.click();
  r.pressed = true; r.tick(); r.tick(30); r.tick(1500);
  r.pressed = false; r.tick(); r.tick(30);
  EXPECT_TRUE(r.response.alarmActive());
  r.click(); r.click(); EXPECT_TRUE(r.response.alarmActive());
  r.click(); EXPECT_FALSE(r.response.alarmActive());
}

TEST(RealWorldFaults, BatteryReminderDeferredBySosIsStillDueAfterAcknowledgment) {
  FaultRig r; r.quiet(120); r.fall();
  EXPECT_FALSE(r.voltage(3300)); EXPECT_TRUE(r.battery.isActive());
  r.acknowledge(); EXPECT_TRUE(r.response.messagePending());
  EXPECT_TRUE(r.voltage(3300));
  EXPECT_EQ(r.buzzer.frequencyHz(), 2093u);
  EXPECT_FALSE(r.voltage(3300)); // Accepted playback consumes only one reminder.
}

TEST(RealWorldFaults, ChargingDuringSosClearsBatteryPolicyWithoutStoppingAlarm) {
  FaultRig r; EXPECT_TRUE(r.voltage(3300));
  r.quiet(120); r.fall();
  EXPECT_FALSE(r.voltage(3300, true)); EXPECT_FALSE(r.battery.isActive());
  EXPECT_TRUE(r.buzzer.alarmActive());
  r.acknowledge(); EXPECT_FALSE(r.voltage(3300, true));
  EXPECT_TRUE(r.voltage(3300, false));
}

TEST(RealWorldFaults, MissingAdcReadingsDoNotInventRecoveryOrStartANewWarning) {
  FaultRig r; EXPECT_FALSE(r.voltage(0));
  EXPECT_TRUE(r.voltage(3499)); r.quiet(100);
  EXPECT_FALSE(r.voltage(0)); EXPECT_TRUE(r.battery.isActive());
  EXPECT_FALSE(r.voltage(3500)); EXPECT_TRUE(r.battery.isActive());
  EXPECT_FALSE(r.voltage(3501)); EXPECT_FALSE(r.battery.isActive());
  EXPECT_FALSE(r.voltage(0)); EXPECT_FALSE(r.battery.isActive());
}

class RetryRecovery : public ::testing::TestWithParam<unsigned> {};
TEST_P(RetryRecovery, AllocationRecoversOnceWithoutRestartingSos) {
  FaultRig r(UINT32_MAX - 4000); r.quiet(120); r.fall(); r.acknowledge();
  ASSERT_TRUE(r.response.messagePending());
  for (unsigned attempt = 0; attempt <= GetParam(); ++attempt) {
    ASSERT_TRUE(r.response.messageDue(r.now));
    r.response.messageAttempted(r.now, attempt == GetParam());
    EXPECT_FALSE(r.response.messageDue(r.now));
    EXPECT_FALSE(r.buzzer.alarmActive());
    r.tick(HealthNodeConfig::ack_retry_ms - 1);
    EXPECT_FALSE(r.response.messageDue(r.now));
    r.tick(1);
  }
  EXPECT_FALSE(r.response.messagePending());
  r.tick(600000); EXPECT_FALSE(r.response.messageDue(r.now));
}
INSTANTIATE_TEST_SUITE_P(FirstMiddleLastAttempt, RetryRecovery, ::testing::Values(0u, 1u, 29u, 59u));

TEST(RealWorldFaults, RetryExhaustionDoesNotBlockLaterDetectionOrAcknowledgment) {
  FaultRig r; r.quiet(120); r.fall(); r.acknowledge();
  for (unsigned i = 0; i < HealthNodeConfig::ack_max_attempts; ++i) {
    ASSERT_TRUE(r.response.messageDue(r.now));
    r.response.messageAttempted(r.now, false);
    r.quiet(250); // Motion continues during the five-second retry interval.
  }
  EXPECT_FALSE(r.response.messagePending()); EXPECT_FALSE(r.response.messageDue(r.now));
  r.fall(); EXPECT_EQ(r.falls, 2u); EXPECT_TRUE(r.buzzer.alarmActive());
  r.acknowledge(); EXPECT_TRUE(r.response.messageDue(r.now));
}

TEST(RealWorldFaults, RebootClearsPendingRequestAndRequiresFreshFallEvidence) {
  {
    FaultRig r; r.quiet(120); r.fall(); r.acknowledge();
    ASSERT_TRUE(r.response.messagePending()); r.voltage(3300);
  }
  FaultRig reboot;
  EXPECT_FALSE(reboot.sensor.valid); EXPECT_FALSE(reboot.response.messagePending());
  EXPECT_FALSE(reboot.buzzer.sounding()); EXPECT_FALSE(reboot.battery.isActive());
  reboot.fall(); EXPECT_EQ(reboot.falls, 0u);
  reboot.quiet(120); reboot.fall(); EXPECT_EQ(reboot.falls, 1u);
}

TEST(RealWorldFaults, JitteredHourOfOrdinaryMotionThenFallRemainsResponsive) {
  FaultRig r(UINT32_MAX - 1800000); uint32_t random = 0x6050;
  uint32_t elapsed = 0;
  while (elapsed < 3600000) {
    random = random * 1664525u + 1013904223u;
    const uint32_t step = 20 + random % 31; elapsed += step;
    // Reproducible bounded ordinary movement, not a recorded human dataset.
    r.sample(.75f + (random % 50) / 100.f, float(random % 25), step);
    if (random % 97 == 0) { r.read_length = 0; r.tick(); r.read_length = 14; }
    if (random % 211 == 0) r.tick(250);
  }
  EXPECT_EQ(r.falls, 0u); EXPECT_FALSE(r.buzzer.alarmActive());
  r.quiet(400); r.fall(); EXPECT_EQ(r.falls, 1u);
}

class OledFault : public ::testing::TestWithParam<unsigned> {};
TEST_P(OledFault, EveryChunkCanFailAndNextFrameRestartsAtZero) {
  OledFrameTransfer frame; frame.start();
  for (unsigned i = 0; i < GetParam(); ++i) frame.completed(true);
  ASSERT_EQ(frame.position(), GetParam() * frame.chunk_bytes);
  frame.completed(false); EXPECT_FALSE(frame.pending());
  frame.start(); EXPECT_EQ(frame.position(), 0);
  unsigned chunks = 0;
  while (frame.pending() && chunks < 33) { frame.completed(true); ++chunks; }
  EXPECT_EQ(chunks, 32u); EXPECT_FALSE(frame.pending());
}
INSTANTIATE_TEST_SUITE_P(All32Chunks, OledFault, ::testing::Range(0u, 32u));

TEST(RealWorldFaults, AdcLongStallTakesOneConversionPerCallAndFinishesPowerCycle) {
  StagedBatteryRead adc; unsigned conversions = 0; std::vector<bool> power;
  auto poll = [&](uint32_t now) {
    adc.poll(now, 1000, 10, 8, [&](bool on) { power.push_back(on); },
      [&]() { ++conversions; return conversions % 2 ? 0u : 4095u; });
  };
  poll(0); poll(9); EXPECT_EQ(conversions, 0u);
  poll(60000); EXPECT_EQ(conversions, 1u); EXPECT_FALSE(adc.hasReading());
  for (unsigned i = 1; i < 8; ++i) poll(60000 + i);
  EXPECT_EQ(conversions, 8u); EXPECT_EQ(adc.rawAverage(), 2047);
  EXPECT_EQ(power, (std::vector<bool>{true, false}));
  poll(61006); EXPECT_EQ(power.size(), 2u);
  poll(61007); EXPECT_EQ(power, (std::vector<bool>{true, false, true}));
}

TEST(RealWorldFaults, AhtBusyTimeoutWhileMotionAndSosContinueThenRecovers) {
  FaultRig r; r.quiet(120);
  TwoWire environmentalBus; BoundedAHT aht;
  environmentalBus.response = [](uint8_t, uint8_t) { return std::vector<uint8_t>{0x88}; };
  const uint32_t start = r.now; aht.begin(environmentalBus, 0x38, start);
  r.sample(.3f); r.sample(3, 300);
  for (unsigned i = 0; i < 60; ++i) {
    r.sample(1);
    auto result = aht.poll(r.now, 1000);
    EXPECT_EQ(result, r.now - start < 1000 ? BoundedAHT::Pending : BoundedAHT::Failed);
  }
  EXPECT_EQ(r.falls, 1u); EXPECT_TRUE(r.buzzer.alarmActive());
  environmentalBus.response = [](uint8_t, uint8_t) { return std::vector<uint8_t>{0x08}; };
  aht.begin(environmentalBus, 0x38, r.now); r.tick(); aht.poll(r.now, 1000);
  r.tick(); EXPECT_EQ(aht.poll(r.now, 1000), BoundedAHT::Complete);
  EXPECT_TRUE(r.buzzer.alarmActive());
}

TEST(RealWorldFaults, Utf8OversizePayloadLeavesDestinationAndGuardsUntouched) {
  std::array<uint8_t, FallAckMessage::max_payload_bytes + 2> storage;
  storage.fill(0xa5);
  std::string text;
  for (unsigned i = 0; i < 60; ++i) text += u8"\u2713";
  EXPECT_EQ(FallAckMessage::encodeWithLocation(storage.data() + 1, 0, "Node",
    text.c_str(), false, 0, 0, "HOME"), 0u);
  for (auto byte : storage) EXPECT_EQ(byte, 0xa5);
  auto size = FallAckMessage::encodeWithLocation(storage.data() + 1, 0, "Node",
    u8"\u2713 Help", true, -90, -180, "HOME");
  ASSERT_GT(size, 0u); EXPECT_EQ(storage.front(), 0xa5); EXPECT_EQ(storage.back(), 0xa5);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(storage.data() + 6), size - 5),
    u8"Node: \u2713 Help | lat=-90.000000, lon=-180.000000");
}


class HalReadFault : public ::testing::TestWithParam<int> {};
TEST_P(HalReadFault, BusErrorInvalidatesEvenACompleteBufferedResponse) {
  FaultRig r; r.quiet(120); r.sample(.3f); r.sample(3, 300);
  Wire1.read_status = GetParam(); r.tick();
  EXPECT_FALSE(r.sensor.valid); EXPECT_EQ(r.falls, 0u);
  EXPECT_EQ(Wire1.last_timeout_ms, HealthNodeConfig::sensor_i2c_timeout_ms);
  Wire1.read_status = 0; r.quiet(160); EXPECT_EQ(r.falls, 0u);
  r.fall(); EXPECT_EQ(r.falls, 1u);
}
INSTANTIATE_TEST_SUITE_P(NackTimeoutBusFailure, HalReadFault, ::testing::Values(2, 5, 263));

TEST(RealWorldFaults, NegativeSensorAxesStillDetectMagnitudesAcrossClockWrap) {
  FaultRig r(UINT32_MAX - 3000); r.quiet(120);
  r.sample(-.3f); r.sample(-3, -300); r.quiet(52);
  EXPECT_EQ(r.falls, 1u); EXPECT_TRUE(r.buzzer.alarmActive());
  r.acknowledge(); EXPECT_FALSE(r.buzzer.alarmActive());
}

TEST(RealWorldFaults, OversizedSensorResponseIsRejectedWithoutReusingOldValues) {
  FaultRig r; r.quiet(120); r.sample(.3f);
  r.read_length = 15; r.sample(3, 300);
  EXPECT_FALSE(r.sensor.valid);
  r.read_length = 14; r.quiet(160); EXPECT_EQ(r.falls, 0u);
  r.fall(); EXPECT_EQ(r.falls, 1u);
}

TEST(RealWorldFaults, DelayedSosPollsKeepNineMarksAndFullRepeatSilence) {
  BuzzerPattern buzzer(150, 5000); uint32_t now = UINT32_MAX - 500;
  buzzer.startSOS(now);
  for (unsigned mark = 0; mark < 9; ++mark) {
    ASSERT_TRUE(buzzer.sounding());
    now += 3000; buzzer.update(now); EXPECT_FALSE(buzzer.sounding());
    const uint32_t gap = mark == 8 ? 5000 : (mark == 2 || mark == 5 ? 450 : 150);
    buzzer.update(now + gap - 1); EXPECT_FALSE(buzzer.sounding());
    now += gap; buzzer.update(now); EXPECT_TRUE(buzzer.sounding());
  }
  // The next mark is the first dot of the next SOS, not a skipped phase.
  buzzer.update(now + 149); EXPECT_TRUE(buzzer.sounding());
  buzzer.update(now + 150); EXPECT_FALSE(buzzer.sounding());
}

class AhtShortRead : public ::testing::TestWithParam<int> {};
TEST_P(AhtShortRead, IncompleteMeasurementFailsAndCanBeReinitialized) {
  TwoWire bus; BoundedAHT aht;
  bus.response = [](uint8_t, uint8_t n) { return std::vector<uint8_t>(n, 0x08); };
  aht.begin(bus, 0x38, 0); aht.poll(20, 1000); aht.poll(40, 1000);
  ASSERT_TRUE(aht.start(50));
  bus.response = [this](uint8_t, uint8_t n) {
    return std::vector<uint8_t>(n == 1 ? 1 : GetParam(), 0x08);
  };
  EXPECT_EQ(aht.poll(130, 1000), BoundedAHT::Failed);
  EXPECT_FALSE(aht.start(150));
  bus.response = [](uint8_t, uint8_t n) { return std::vector<uint8_t>(n, 0x08); };
  aht.begin(bus, 0x38, 200); aht.poll(220, 1000);
  EXPECT_EQ(aht.poll(240, 1000), BoundedAHT::Complete);
  ASSERT_TRUE(aht.start(250)); EXPECT_EQ(aht.poll(330, 1000), BoundedAHT::Complete);
}
INSTANTIATE_TEST_SUITE_P(AllTruncatedMeasurements, AhtShortRead, ::testing::Range(0, 6));

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
