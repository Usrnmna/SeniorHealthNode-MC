#pragma once
#include <stdint.h>
#include <math.h>

#ifndef MOTION_ACCEL_THRESHOLD_G
#define MOTION_ACCEL_THRESHOLD_G 2.5f
#endif
#ifndef MOTION_GYRO_THRESHOLD_DPS
#define MOTION_GYRO_THRESHOLD_DPS 250.0f
#endif
#ifndef MOTION_COOLDOWN_MS
#define MOTION_COOLDOWN_MS 60000U
#endif

// Example motion rule, not a validated fall detector. Tune for sensor placement.
class MotionRule {
public:
  bool update(uint32_t now, float acceleration_g, float rotation_dps, bool valid) {
    if (!valid || !isfinite(acceleration_g) || !isfinite(rotation_dps)) {
      quiet = false; return false;
    }
    if (!armed) {
      if (acceleration_g < 1.3f && acceleration_g > 0.7f && rotation_dps < 30.0f) {
        if (!quiet) { quiet = true; quiet_since = now; }
        if (uint32_t(now - quiet_since) >= 2000 && uint32_t(now - fired_at) >= MOTION_COOLDOWN_MS)
          armed = true;
      } else { quiet = false; }
      return false;
    }
    if (acceleration_g >= MOTION_ACCEL_THRESHOLD_G || rotation_dps >= MOTION_GYRO_THRESHOLD_DPS) {
      armed = false;
      quiet = false;
      fired_at = now;
      return true;
    }
    return false;
  }
private:
  bool armed = true, quiet = false;
  uint32_t fired_at = 0, quiet_since = 0;
};
