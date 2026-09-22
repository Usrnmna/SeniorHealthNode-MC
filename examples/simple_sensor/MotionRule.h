#pragma once
#include <stdint.h>
#include <math.h>

// EDIT HERE: all tunable settings. Units: g includes gravity; dps = degrees/s.
// Intended mounting: firmly attached to the torso/chest. Prototype only.
struct FallDetectionConfig {
  // STUDY [4], Huynh 2015: low acceleration -> impact AND rotation in 0.5 s.
  // https://doi.org/10.1155/2015/452078 (README bibliography uses these numbers).
  float low_g = 0.35f;          // Upper end of the study's 0.30-0.35 g range.
  float impact_g = 2.4f;
  float rotation_dps = 240.0f;
  uint32_t event_window_ms = 500;

  // PROJECT EXTENSION: require quiet AFTER the sequence; not a posture classifier.
  // Set confirm_quiet_ms = 0 to alert on the sequence alone for comparisons.
  float quiet_min_g = 0.7f;
  float quiet_max_g = 1.3f;
  float quiet_max_dps = 30.0f;
  uint32_t confirm_quiet_ms = 1000;
  uint32_t confirm_timeout_ms = 5000; // Measured from the second required peak.

  // PROJECT GUARDS: startup/recovery settling, duplicate suppression, missing data.
  uint32_t rearm_quiet_ms = 2000;
  uint32_t cooldown_ms = 60000;
  uint32_t max_sample_gap_ms = 100; // Nominal sample spacing is 20 ms.
};

class MotionRule {
public:
  explicit MotionRule(const FallDetectionConfig& settings = FallDetectionConfig()) : cfg(settings) {}

  // Call once per FRESH sample; valid=false breaks the sequence immediately.
  // Returns true ONCE per possible fall. No allocation, blocking, or Arduino dependency.
  bool update(uint32_t now, float acceleration_g, float rotation_dps, bool valid) {
    // STAGE 0 -- Reject missing/bad data; never join evidence across a read gap.
    if (!valid || !isfinite(acceleration_g) || !isfinite(rotation_dps)
        || acceleration_g < 0 || rotation_dps < 0) {
      resetEvidence();
      have_sample = false;
      return false;
    }
    if (have_sample && uint32_t(now - last_sample) > cfg.max_sample_gap_ms)
      resetEvidence();
    if (have_sample && now == last_sample) return false;
    last_sample = now;
    have_sample = true;
    const bool quiet = acceleration_g >= cfg.quiet_min_g
                    && acceleration_g <= cfg.quiet_max_g
                    && rotation_dps < cfg.quiet_max_dps;

    // STAGE 1 -- Require continuous quiet at startup, after a gap, and after alert.
    // Cooldown is latched as complete so a later millis() wrap cannot revive it.
    if (cooling && uint32_t(now - fired_at) >= cfg.cooldown_ms) cooling = false;
    if (state == Settling) {
      if (quietFor(now, quiet, cfg.rearm_quiet_ms) && !cooling) {
        state = Armed;
        quiet_tracking = false;
      }
      return false;
    }

    // STAGE 2 -- Low acceleration opens a FIXED window; repeated lows cannot extend it.
    if (state == Armed) {
      if (acceleration_g <= cfg.low_g) {
        state = Peaks;
        event_at = now;
        peak_accel = acceleration_g;
        peak_gyro = rotation_dps;
      }
      return false;
    }

    // STAGE 3 -- Study [4]: BOTH peaks must occur within the same low-started window.
    // The peaks can occur on different samples and in either order.
    if (state == Peaks) {
      if (uint32_t(now - event_at) > cfg.event_window_ms) {
        resetEvidence();
        return false;
      }
      peak_accel = fmaxf(peak_accel, acceleration_g);
      peak_gyro = fmaxf(peak_gyro, rotation_dps);
      if (peak_accel < cfg.impact_g || peak_gyro < cfg.rotation_dps) return false;
      if (cfg.confirm_quiet_ms == 0) return fire(now);
      state = Confirming;
      confirm_at = now;
      quiet_tracking = false;
      return false;
    }

    // STAGE 4 -- Project extension: continuous post-event quiet within a deadline.
    // Movement restarts the quiet timer. Continued movement or a gap cancels.
    // This may MISS falls where the wearer keeps moving; it does not prove lying.
    if (uint32_t(now - confirm_at) > cfg.confirm_timeout_ms) {
      resetEvidence();
      return false;
    }
    if (quietFor(now, quiet, cfg.confirm_quiet_ms)) return fire(now);
    return false;
  }

  // Retain event evidence for alert text; confirmation samples are usually near 1 g.
  float peakAcceleration() const { return peak_accel; }
  float peakRotation() const { return peak_gyro; }

private:
  enum State { Settling, Armed, Peaks, Confirming };
  const FallDetectionConfig cfg;
  State state = Settling;
  bool have_sample = false, quiet_tracking = false, cooling = false;
  uint32_t last_sample = 0, event_at = 0, confirm_at = 0, quiet_since = 0, fired_at = 0;
  float peak_accel = 0, peak_gyro = 0;

  // All time differences use unsigned subtraction to tolerate millis() rollover.
  bool quietFor(uint32_t now, bool quiet, uint32_t duration) {
    if (!quiet) { quiet_tracking = false; return false; }
    if (!quiet_tracking) { quiet_tracking = true; quiet_since = now; }
    return uint32_t(now - quiet_since) >= duration;
  }

  void resetEvidence() {
    state = Settling;
    quiet_tracking = false;
    // Preserve cooldown: unplugging the sensor must not bypass duplicate suppression.
  }

  // STAGE 5 -- Emit one possible-fall event, then require cooldown AND quiet to rearm.
  bool fire(uint32_t now) {
    fired_at = now;
    cooling = true;
    resetEvidence();
    return true;
  }
};
