#pragma once
#include <HealthNodeConfig.h>
#include <math.h>

// Post-fall evidence only; independent of MotionRule's detection and cooldown.
// No allocations or hardware calls. Pass only fresh samples, or invalidate().
class ResidualMovement {
  friend struct MotionCaptureCheckpoint;
public:
  void reset() { have_sample = quiet_tracking = false; }
  void invalidate() { reset(); }

  // A fixed quiet baseline catches gradual vector drift as well as abrupt motion.
  // Invalid readings or excessive gaps break continuity; recovery starts anew.
  void observe(uint32_t now, float ax, float ay, float az, float rotation_dps) {
    using namespace HealthNodeConfig;
    if (!isfinite(ax) || !isfinite(ay) || !isfinite(az) ||
        !isfinite(rotation_dps) || rotation_dps < 0) { invalidate(); return; }
    if (have_sample && now == last_sample) return;
    if (have_sample && uint32_t(now - last_sample) > residual_max_sample_gap_ms) reset();
    if (!have_sample) { base_x = ax; base_y = ay; base_z = az; }
    have_sample = true; last_sample = now;
    const float dx = ax - base_x, dy = ay - base_y, dz = az - base_z;
    const bool moving = rotation_dps >= residual_rotation_dps ||
        fabsf(sqrtf(ax*ax + ay*ay + az*az) - 1.0f) > residual_gravity_tolerance_g ||
        dx*dx + dy*dy + dz*dz > residual_acceleration_change_g * residual_acceleration_change_g;
    if (moving) {
      quiet_tracking = false; base_x = ax; base_y = ay; base_z = az;
    } else if (!quiet_tracking) {
      quiet_tracking = true; quiet_since = now;
    }
  }

  // Evidence must cover the final quiet window and still be fresh when serviced.
  // At most max_sample_gap_ms is tolerated between the last sample and deadline.
  bool quietAt(uint32_t deadline, uint32_t now) const {
    using namespace HealthNodeConfig;
    return have_sample && quiet_tracking &&
        uint32_t(now - last_sample) <= residual_max_sample_gap_ms &&
        uint32_t(deadline - last_sample) <= residual_max_sample_gap_ms &&
        uint32_t(deadline - quiet_since) >= residual_quiet_required_ms;
  }

private:
  bool have_sample = false, quiet_tracking = false;
  uint32_t last_sample = 0, quiet_since = 0;
  float base_x = 0, base_y = 0, base_z = 0;
};
