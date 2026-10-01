#pragma once
#include <HealthNodeConfig.h>

// Pure policy: charging clears immediately, even while the battery is below 3.5 V.
// A disconnected charger with a still-low battery starts a fresh alert immediately.
class LowBatteryAlert {
public:
  bool update(uint32_t now, uint16_t battery_mv, bool charging) {
    if (charging || battery_mv > HealthNodeConfig::low_battery_mv) {
      active = played = false;
    } else if (battery_mv > 0 && battery_mv < HealthNodeConfig::low_battery_mv) {
      active = true; // Zero is an unavailable ADC sample, not a measured battery.
    }
    return active && (!played || uint32_t(now - last_played) >= HealthNodeConfig::low_battery_repeat_ms);
  }
  // Commit the deadline only when the buzzer accepts playback (SOS may defer it).
  void markPlayed(uint32_t now) { played = true; last_played = now; }
  bool isActive() const { return active; }
private:
  bool active = false, played = false;
  uint32_t last_played = 0;
};
