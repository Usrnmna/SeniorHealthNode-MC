#include <gtest/gtest.h>
#include <helpers/BatteryCalibration.h>
#include <helpers/StagedBatteryRead.h>
#include <HealthNodeConfig.h>
#include "../../examples/companion_radio/NodePrefs.h"
#include <string>
#include <stdio.h>

// Exercise the real preference serializer, including its four-decimal float
// storage format and legacy documents with no calibration field.
class CalibrationStream : public Stream {
public:
  std::string text;
  size_t position = 0;
  explicit CalibrationStream(const char* value = "") : text(value) {}
  size_t write(uint8_t value) override { text += char(value); return 1; }
  int available() override { return int(text.size() - position); }
  int read() override { return available() ? uint8_t(text[position++]) : -1; }
  int peek() override { return available() ? uint8_t(text[position]) : -1; }
  size_t print(double value, int digits = 2) override {
    char number[64]; snprintf(number, sizeof(number), "%.*f", digits, value);
    text += number; return strlen(number);
  }
  size_t print(unsigned char value, int = DEC) override { return integer(value); }
  size_t print(int value, int = DEC) override { return integer(value); }
  size_t print(unsigned int value, int = DEC) override { return integer(value); }
  size_t print(long value, int = DEC) override { return integer(value); }
  size_t print(unsigned long value, int = DEC) override { return integer(value); }
private:
  template<class T> size_t integer(T value) {
    const auto number = std::to_string(value); text += number; return number.size();
  }
};

TEST(BatteryCalibration, ExistingRawConversionRemainsUnchanged) {
  for (float multiplier : {4.9f, 5.42f, 6.0f, 10.0f})
    for (uint16_t raw = 0; raw <= 1023; ++raw)
      EXPECT_EQ(BatteryCalibration::milliVolts(raw, multiplier),
                uint16_t((multiplier * (3.3 / 1024.0) * raw) * 1000));
}

TEST(BatteryCalibration, MeterReadingProducesCorrectionWithoutChangingInputScale) {
  float suggested = 5.42f;
  ASSERT_TRUE(BatteryCalibration::suggest(3920, 224, 100, 2500, 3300, 4200, suggested));
  EXPECT_NEAR(suggested, 5.430303f, 0.00001f);
  EXPECT_NEAR(BatteryCalibration::milliVolts(224, suggested), 3920, 1);
  // A second voltage on the same assumed linear response uses the same scale.
  EXPECT_NEAR(BatteryCalibration::milliVolts(200, suggested), 3500, 1);
}

TEST(BatteryCalibration, RejectsUnavailableStaleSaturatedAndImplausibleInputs) {
  float result = 5.42f;
  for (uint16_t raw : {0, 1, 1023, 65535})
    EXPECT_FALSE(BatteryCalibration::suggest(3920, raw, 0, 2500, 3300, 4200, result));
  for (uint16_t mv : {0, 4, 3299, 4201, 65535})
    EXPECT_FALSE(BatteryCalibration::suggest(mv, 224, 0, 2500, 3300, 4200, result));
  EXPECT_FALSE(BatteryCalibration::suggest(3920, 224, 2501, 2500, 3300, 4200, result));
  EXPECT_FALSE(BatteryCalibration::suggest(3920, 224, UINT32_MAX, 2500, 3300, 4200, result));
  EXPECT_FLOAT_EQ(result, 5.42f); // A failed suggestion never overwrites the output.
  EXPECT_TRUE(BatteryCalibration::suggest(3920, 224, 2500, 2500, 3300, 4200, result));
}

TEST(BatteryCalibration, RejectsInvalidMultiplierWithoutSilentlyResettingIt) {
  float result = 5.42f;
  for (const char* text : {"", "nan", "inf", "-1", "10.01", "1e3", "0x5", "5.4 junk", "5.4.2", "."}) {
    EXPECT_FALSE(BatteryCalibration::parseMultiplier(text, result)) << text;
    EXPECT_FLOAT_EQ(result, 5.42f);
  }
  EXPECT_TRUE(BatteryCalibration::parseMultiplier("0", result)); EXPECT_EQ(result, 0);
  EXPECT_TRUE(BatteryCalibration::parseMultiplier("5.4303", result));
  EXPECT_FLOAT_EQ(result, 5.4303f);
  EXPECT_FALSE(BatteryCalibration::validMultiplier(NAN));
  EXPECT_FALSE(BatteryCalibration::validMultiplier(INFINITY));
}

TEST(BatteryCalibration, MeterInputRequiresWholeMillivolts) {
  uint16_t result = 3920;
  for (const char* text : {"", "3.92", "3920mV", "-3920", "nan", "65536", "99999999999999"}) {
    EXPECT_FALSE(BatteryCalibration::parseMillivolts(text, result));
    EXPECT_EQ(result, 3920);
  }
  EXPECT_TRUE(BatteryCalibration::parseMillivolts("4200", result)); EXPECT_EQ(result, 4200);
}

TEST(BatteryCalibration, CachedSampleAgeWorksAcrossClockWrap) {
  StagedBatteryRead battery;
  EXPECT_EQ(battery.readingAgeMs(0), UINT32_MAX);
  const uint32_t start = UINT32_MAX - 20;
  auto poll = [&](uint32_t now) {
    battery.poll(now, 1000, 10, 8, [](bool) {}, []() { return 224; });
  };
  poll(start);
  for (uint32_t i = 0; i < 8; ++i) poll(start + 10 + i);
  EXPECT_EQ(battery.rawAverage(), 224);
  EXPECT_EQ(battery.readingAgeMs(start + 17), 0);
  EXPECT_EQ(battery.readingAgeMs(start + 1017), 1000);
}

TEST(BatteryCalibration, MissingLegacyPreferenceUsesDefaultAndNewValueRoundTrips) {
  NodePrefs legacy;
  CalibrationStream old("{name:\"Existing node\",comp:{pin:12345}}");
  ASSERT_TRUE(legacy.loadSerial(old));
  EXPECT_EQ(legacy.adc_multiplier, 0);
  EXPECT_EQ(legacy.ble_pin, 12345u);
  legacy.adc_multiplier = 5.4303f;
  CalibrationStream saved;
  ASSERT_TRUE(legacy.saveSerial(saved));
  NodePrefs restored;
  ASSERT_TRUE(restored.loadSerial(saved));
  EXPECT_FLOAT_EQ(restored.adc_multiplier, 5.4303f);
  EXPECT_EQ(restored.ble_pin, 12345u);
  EXPECT_STREQ(restored.node_name, "Existing node");
}

int main(int argc, char** argv) { ::testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
