#pragma once
#include <stdint.h>

// One ADC conversion per service call, after a nonblocking divider settle period.
// Hardware is injected to keep timing, rollover and averaging host-testable.
class StagedBatteryRead {
  bool started = false, collecting = false, valid = false;
  uint32_t started_at = 0, completed_at = 0, sum = 0;
  uint8_t count = 0;
  uint16_t average = 0;
public:
  template<class Power, class Read>
  void poll(uint32_t now, uint32_t interval_ms, uint32_t settle_ms,
            uint8_t samples, Power power, Read read) {
    if (!collecting) {
      if (started && uint32_t(now - completed_at) < interval_ms) return;
      started = collecting = true; started_at = now; sum = count = 0;
      power(true);
      return;
    }
    if (uint32_t(now - started_at) < settle_ms) return;
    sum += read();
    if (++count >= samples) {
      average = sum / count; valid = true; collecting = false;
      completed_at = now; power(false);
    }
  }
  bool hasReading() const { return valid; }
  uint16_t rawAverage() const { return average; }
};
