#include <gtest/gtest.h>
#include <AssistanceTestLink.h>
#define ESP32
#include <helpers/sensors/MPU6050.h>
#include "../../examples/companion_radio/health/GyroCalibrationStore.h"
#include "../../examples/companion_radio/health/MotionRule.h"
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <limits>

namespace {
using Sample = MPU6050Sensor::Reading;
const Sample stationary = {0.6f, 0, 0.8f, 1.2f, -2.4f, 0.5f, 30};

// Feed independent fresh samples at the configured rate; no hardware is involved.
uint32_t feed(GyroCalibration& cal, uint32_t start, uint32_t duration, const Sample& sample = stationary,
              uint32_t step = 20) {
  for (uint32_t offset = 0; offset <= duration; offset += step) {
    cal.tick(start + offset, true); cal.observe(start + offset, sample);
  }
  return start + duration;
}
GyroCalibration::Result calibrated() {
  GyroCalibration cal; cal.start(0); feed(cal, 0, 20100);
  EXPECT_TRUE(cal.pending()); return cal.result();
}

TEST(GyroCalibration, ArbitraryOrientationProducesBiasAfterIndependentValidation) {
  GyroCalibration cal; ASSERT_TRUE(cal.start(0));
  feed(cal, 0, 17980); EXPECT_EQ(GyroCalibration::Measuring, cal.state());
  cal.observe(18000, stationary); EXPECT_EQ(GyroCalibration::Validating, cal.state());
  feed(cal, 18020, 1980); EXPECT_FALSE(cal.pending());
  cal.observe(20020, stationary); ASSERT_TRUE(cal.pending());
  EXPECT_EQ(751u, cal.result().samples);
  EXPECT_FLOAT_EQ(1.2f, cal.result().bias[0]); EXPECT_FLOAT_EQ(-2.4f, cal.result().bias[1]);
  EXPECT_FLOAT_EQ(0.5f, cal.result().bias[2]); EXPECT_FLOAT_EQ(30, cal.result().temperature);
  EXPECT_TRUE(GyroCalibration::validResult(cal.result()));
}
TEST(GyroCalibration, HorizontalVerticalAndInvertedPositionsAreAccepted) {
  for (const auto& v : {Sample{0,0,1,2,3,4,25}, Sample{1,0,0,2,3,4,25}, Sample{0,0,-1,2,3,4,25}}) {
    GyroCalibration cal; cal.start(0); feed(cal, 0, 20100, v);
    EXPECT_TRUE(cal.pending()); EXPECT_FLOAT_EQ(4, cal.result().bias[2]);
  }
}
TEST(GyroCalibration, MovementRestartsACompleteCountdownAndMeasurement) {
  GyroCalibration cal; cal.start(0); feed(cal, 0, 5000);
  auto moved = stationary; moved.ax = 0; moved.az = 1;
  cal.observe(5020, moved); EXPECT_EQ(1u, cal.restarts());
  EXPECT_EQ(GyroCalibration::Preparing, cal.state());
  feed(cal, 5040, 20000); EXPECT_TRUE(cal.pending());
}
TEST(GyroCalibration, ValidationDriftCannotBeSavedAsSuccessfulCalibration) {
  GyroCalibration cal; cal.start(0); feed(cal, 0, 18000);
  auto shifted = stationary; shifted.gx += 0.5f;
  feed(cal, 18020, 2000, shifted);
  EXPECT_FALSE(cal.pending()); EXPECT_EQ(1u, cal.restarts());
  EXPECT_STREQ("validation-residual", cal.reason());
}
TEST(GyroCalibration, NoiseAndSlowOrientationDriftAreRejected) {
  GyroCalibration cal; cal.start(0);
  for (uint32_t now = 0; now <= 18000; now += 20) {
    auto noisy = stationary; noisy.gx += (now/20 % 2) ? 0.25f : -0.25f;
    cal.observe(now, noisy);
  }
  EXPECT_FALSE(cal.pending()); EXPECT_STREQ("gyro-noise", cal.reason());
  cal.cancel(); cal.start(20000); feed(cal, 20000, 4000);
  auto tilted = stationary; tilted.ax += 0.05f;
  cal.observe(24020, tilted); EXPECT_STREQ("movement", cal.reason());
}
TEST(GyroCalibration, SensorErrorStaleDataAndLargeGapsAbortImmediately) {
  GyroCalibration cal; cal.start(0); feed(cal, 0, 5000);
  cal.tick(5020, false); EXPECT_EQ(GyroCalibration::Failed, cal.state());
  EXPECT_STREQ("sensor-error", cal.reason());
  cal.start(6000); cal.tick(6100, true); EXPECT_TRUE(cal.active());
  cal.tick(6101, true); EXPECT_FALSE(cal.active()); EXPECT_STREQ("sample-gap", cal.reason());
  cal.start(7000); cal.observe(7101, stationary); EXPECT_STREQ("sample-gap", cal.reason());
}
TEST(GyroCalibration, BoundedBacklogDefersFreshnessButNeverOverallTimeout) {
  GyroCalibration cal;ASSERT_TRUE(cal.start(1000));cal.observe(1020,stationary);
  cal.tick(1140,true,true);ASSERT_TRUE(cal.active());
  for(uint32_t at=1040;at<=1120;at+=20)cal.observe(at,stationary);
  cal.tick(1140,true,false);EXPECT_TRUE(cal.active());
  cal.tick(1140+HealthNodeConfig::gyro_calibration_timeout_ms,true,true);
  EXPECT_FALSE(cal.active());EXPECT_STREQ("timeout",cal.reason());
}
TEST(GyroCalibration, BufferedSampleBeforeStartCannotEnterTheNewCollection) {
  GyroCalibration cal;ASSERT_TRUE(cal.start(1000));auto moving=stationary;moving.gz=400;
  cal.observe(980,moving);EXPECT_TRUE(cal.active());EXPECT_EQ(0u,cal.restarts());
  feed(cal,1000,20100);EXPECT_TRUE(cal.pending());
}
TEST(GyroCalibration, CachedSamplesDoNotAccumulateAndLowSamplingRateFails) {
  GyroCalibration cal; cal.start(0);
  for (uint32_t now = 0; now <= 100; ++now) cal.tick(now, true);
  cal.tick(101, true); EXPECT_FALSE(cal.pending());
  cal.start(1000); feed(cal, 1000, 18000, stationary, 100);
  EXPECT_EQ(GyroCalibration::Failed, cal.state()); EXPECT_STREQ("too-few-samples", cal.reason());
}
TEST(GyroCalibration, TemperatureDriftAndInvalidNumbersCannotProduceOffsets) {
  GyroCalibration cal; cal.start(0); feed(cal, 0, 5000);
  auto warm = stationary; warm.temperature += 2.1f;
  cal.observe(5020, warm); EXPECT_STREQ("temperature-drift", cal.reason());
  auto bad = stationary; bad.gy = std::numeric_limits<float>::quiet_NaN();
  cal.observe(5040, bad); EXPECT_EQ(GyroCalibration::Failed, cal.state());
  EXPECT_STREQ("invalid-sample", cal.reason());
}
TEST(GyroCalibration, OverallTimeoutBoundsRepeatedMovementAndHandlesClockWrap) {
  const uint32_t start = UINT32_MAX - 1000;
  GyroCalibration cal; cal.start(start); feed(cal, start, 20100); EXPECT_TRUE(cal.pending());
  cal.cancel(); cal.start(start);
  auto moving = stationary; moving.gz = 25;
  feed(cal, start, 60000, moving);
  EXPECT_EQ(GyroCalibration::Failed, cal.state()); EXPECT_STREQ("timeout", cal.reason());
}
TEST(GyroCalibration, PendingResultExpiresAndCancelDiscardsIt) {
  GyroCalibration cal; cal.start(0); feed(cal, 0, 20100);
  EXPECT_FALSE(cal.start(20120)); cal.tick(140020, true);
  EXPECT_FALSE(cal.pending()); EXPECT_STREQ("candidate-expired", cal.reason());
  ASSERT_TRUE(cal.start(150000)); feed(cal, 150000, 20100); ASSERT_TRUE(cal.pending());
  cal.cancel(); EXPECT_FALSE(cal.pending()); EXPECT_EQ(0u, cal.result().samples);
  cal.reset(); EXPECT_EQ(GyroCalibration::Idle, cal.state());
}

// Fault-injectable byte storage exercises the actual record format and recovery.
struct MemoryFS {
  std::map<std::string, std::vector<uint8_t>> files;
  size_t write_limit = 10000;
  bool fail_open = false, corrupt_write = false, fail_readback = false;
  unsigned writes = 0;
  struct File {
    MemoryFS* fs;
    std::vector<uint8_t>* bytes;
    explicit operator bool() const { return bytes; }
    size_t size() const { return bytes->size(); }
    size_t read(uint8_t* dest, size_t len) {
      len = std::min(len, bytes->size()); memcpy(dest, bytes->data(), len); return len;
    }
    size_t write(const uint8_t* src, size_t len) {
      ++fs->writes; len = std::min(len, fs->write_limit); bytes->assign(src, src + len);
      if (fs->corrupt_write && len) (*bytes)[0] ^= 1;
      return len;
    }
    void flush() {}
    void close() {}
  };
  bool exists(const char* name) { return files.count(name); }
  File open(const char* name, const char* mode) {
    if (fail_open || (*mode == 'r' && fail_readback && writes)) return {this, nullptr};
    if (*mode == 'w') { files[name].clear(); return {this, &files[name]}; }
    auto entry = files.find(name); return {this, entry == files.end() ? nullptr : &entry->second};
  }
};
TEST(GyroStorage, SavesLoadsAndResetsWithoutStructPaddingOrLostMetadata) {
  MemoryFS fs; GyroCalibration::Result loaded;
  ASSERT_TRUE(GyroCalibrationStore::load(fs, loaded)); EXPECT_EQ(0u, loaded.samples);
  auto value = calibrated(); ASSERT_TRUE(GyroCalibrationStore::save(fs, value));
  ASSERT_TRUE(GyroCalibrationStore::load(fs, loaded));
  EXPECT_FLOAT_EQ(value.bias[1], loaded.bias[1]); EXPECT_EQ(value.samples, loaded.samples);
  EXPECT_FLOAT_EQ(value.temperature, loaded.temperature);
  ASSERT_TRUE(GyroCalibrationStore::save(fs, GyroCalibration::Result{}));
  ASSERT_TRUE(GyroCalibrationStore::load(fs, loaded)); EXPECT_EQ(0u, loaded.samples);
  EXPECT_FLOAT_EQ(0, loaded.bias[1]);
}
TEST(GyroStorage, InterruptedAndCorruptWritesRetainPreviousCalibration) {
  MemoryFS fs; auto value = calibrated(); GyroCalibration::Result loaded;
  ASSERT_TRUE(GyroCalibrationStore::save(fs, value));
  fs.write_limit = 25;
  EXPECT_FALSE(GyroCalibrationStore::save(fs, GyroCalibration::Result{}));
  ASSERT_TRUE(GyroCalibrationStore::load(fs, loaded)); EXPECT_EQ(value.samples, loaded.samples);
  fs.write_limit = 10000; fs.corrupt_write = true;
  EXPECT_FALSE(GyroCalibrationStore::save(fs, GyroCalibration::Result{}));
  ASSERT_TRUE(GyroCalibrationStore::load(fs, loaded)); EXPECT_FLOAT_EQ(value.bias[0], loaded.bias[0]);
}
TEST(GyroStorage, UnreadableRecordsAreNotOverwrittenAndReadbackIsRequired) {
  MemoryFS fs; ASSERT_TRUE(GyroCalibrationStore::save(fs, calibrated()));
  auto before = fs.files; fs.fail_open = true;
  EXPECT_FALSE(GyroCalibrationStore::save(fs, GyroCalibration::Result{})); EXPECT_EQ(before, fs.files);
  MemoryFS readback; readback.fail_readback = true;
  EXPECT_FALSE(GyroCalibrationStore::save(readback, calibrated()));
}
TEST(GyroStorage, CorruptRecordsFallBackToZeroAndCanBeReplaced) {
  MemoryFS fs; auto value = calibrated(); GyroCalibration::Result loaded;
  ASSERT_TRUE(GyroCalibrationStore::save(fs, value)); ASSERT_TRUE(GyroCalibrationStore::save(fs, value));
  for (auto& entry : fs.files) entry.second[10] ^= 1;
  EXPECT_FALSE(GyroCalibrationStore::load(fs, loaded)); EXPECT_EQ(0u, loaded.samples);
  ASSERT_TRUE(GyroCalibrationStore::save(fs, value));
  ASSERT_TRUE(GyroCalibrationStore::load(fs, loaded)); EXPECT_EQ(value.samples, loaded.samples);
}
TEST(GyroStorage, InvalidCoefficientsAndMetadataAreRejected) {
  MemoryFS fs; auto value = calibrated(); value.bias[0] = 21;
  EXPECT_FALSE(GyroCalibrationStore::save(fs, value));
  value = calibrated(); value.noise[1] = std::numeric_limits<float>::infinity();
  EXPECT_FALSE(GyroCalibrationStore::save(fs, value));
  value = calibrated(); value.samples = 1; EXPECT_FALSE(GyroCalibrationStore::save(fs, value));
  EXPECT_TRUE(fs.files.empty());
  EXPECT_TRUE(GyroCalibrationStore::newer(0, UINT32_MAX));
  EXPECT_FALSE(GyroCalibrationStore::newer(UINT32_MAX, 0));
}
TEST(GyroSensor, CorrectionPreservesGravityRawReadingsAndFreshness) {
  Wire1 = TwoWire{};
  Wire1.response = [](uint8_t reg, uint8_t count) {
    if (reg == 0x75) return std::vector<uint8_t>{0x68};
    if (reg == 0x3A) return std::vector<uint8_t>{1};
    if (reg == 0x72) return std::vector<uint8_t>{0, 14};
    if (count == 1) return std::vector<uint8_t>{Wire1.registers[reg]};
    std::vector<uint8_t> bytes(count, 0);
    if (reg == 0x74 && count == 14) { bytes[4] = 0x10; bytes[9] = 131; }
    return bytes;
  };
  MPU6050Sensor sensor; sensor.poll(0); sensor.poll(100); ASSERT_TRUE(sensor.poll(120));
  auto value = calibrated(); value.bias[0] = 2;
  ASSERT_TRUE(sensor.setCalibration(value));
  EXPECT_FLOAT_EQ(2, sensor.raw_values.gx); EXPECT_FLOAT_EQ(0, sensor.values.gx);
  EXPECT_FLOAT_EQ(1, sensor.values.az); EXPECT_EQ(30u, sensor.sampleAgeMs(130));
  EXPECT_FALSE(sensor.poll(130)); ASSERT_TRUE(sensor.poll(140));
  EXPECT_FLOAT_EQ(0, sensor.values.gx); EXPECT_FLOAT_EQ(2, sensor.raw_values.gx);
  ASSERT_TRUE(sensor.setCalibration(value)); EXPECT_FLOAT_EQ(0, sensor.values.gx);
  ASSERT_TRUE(sensor.setCalibration(GyroCalibration::Result{})); EXPECT_FLOAT_EQ(2, sensor.values.gx);
}
}
int main(int argc, char** argv) { ::testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
