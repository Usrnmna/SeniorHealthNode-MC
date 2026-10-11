#pragma once

#include <Arduino.h>
#include <helpers/RefCountedDigitalPin.h>
#include <helpers/ESP32Board.h>
#include "LoRaFEMControl.h"
#include <helpers/BatteryCalibration.h>
#if ENV_INCLUDE_MPU6050
#include <helpers/StagedBatteryRead.h>
#endif

#ifndef ADC_MULTIPLIER
  // Effective scaling for 10-bit analogRead() counts, not the resistor ratio.
  // The V4.3 schematic has R27=390k and R28=100k: actual VBAT = VADC * 4.9.
  // Preserve the existing raw-ADC default and adc.multiplier calibration values;
  // changing this to 4.9 alone would lower reported voltage by about 9.6%.
  // This default is not a claim of calibration on the individual board.
  // See docs/heltec_v4_spec_reference.md for the schematic and conversion units.
  #define ADC_MULTIPLIER 5.42
#endif

class HeltecV4Board : public ESP32Board {

protected:
  float adc_mult = ADC_MULTIPLIER; // Effective raw-count scale, including ADC correction.
#if ENV_INCLUDE_MPU6050
  StagedBatteryRead battery_reader;
#endif

public:
  RefCountedDigitalPin periph_power;
  LoRaFEMControl loRaFEMControl;
  HeltecV4Board() : periph_power(PIN_VEXT_EN,PIN_VEXT_EN_ACTIVE) { }

  void begin();
  void onBeforeTransmit(void) override;
  void onAfterTransmit(void) override;
  void powerOff() override;
  bool setLoRaFemLnaEnabled(bool enable) override;
  bool canControlLoRaFemLna() const override;
  bool isLoRaFemLnaEnabled() const override;
  uint16_t getBattMilliVolts() override;
#if ENV_INCLUDE_MPU6050
  void pollBattery();
  bool batteryReadingReady() const { return battery_reader.hasReading(); }
  uint16_t batteryRawAverage() const { return battery_reader.rawAverage(); }
  uint32_t batteryReadingAgeMs(uint32_t now) const { return battery_reader.readingAgeMs(now); }
#endif
  bool setAdcMultiplier(float multiplier) override {
    if (!BatteryCalibration::validMultiplier(multiplier)) return false;
    if (multiplier == 0.0f) {
      adc_mult = ADC_MULTIPLIER;
    } else {
      adc_mult = multiplier;
    }
    return true;
  }
  float getAdcMultiplier() const override { return adc_mult; }
  const char* getManufacturerName() const override;
};
