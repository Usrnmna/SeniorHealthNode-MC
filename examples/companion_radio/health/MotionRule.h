#pragma once
#include <stdint.h>
#include <math.h>

// Acceleration includes gravity. Context thresholds are experimental engineering
// settings for firm torso attachment, not measured sensitivity/specificity.
struct FallDetectionConfig {
  static constexpr uint32_t sample_period_ms = 20;
  float low_g = 0.35f, impact_g = 2.4f, rotation_dps = 240.0f;
  uint32_t event_window_ms = 500, pretrigger_ms = 300;
  uint32_t recovery_ms = 200, recovery_samples = 6;
  uint32_t refractory_ms = 1200, max_sample_gap_ms = 100;
  float quiet_min_g = 0.7f, quiet_max_g = 1.3f, quiet_max_dps = 30;
  bool context_paths = true;
  float context_angle_deg = 55, slow_angle_deg = 65;
  float support_impact_g = 1.6f, support_rotation_dps = 80;
  float unload_g = 0.85f, slow_peak_g = 1.15f;
  uint32_t context_window_ms = 6000, confirm_quiet_ms = 1000;
  uint32_t slow_quiet_ms = 2000, confirm_timeout_ms = 5000;
  uint32_t context_feature_ms = 2000; // Peak/trough must precede orientation support closely.
};

// Pure streaming classifier. Gaps break evidence; candidate expiration does not
// disarm the stream. Delivery suppression and user confirmation are external.
class MotionRule {
  // Explicit field codec for development capture; no raw object/padding export.
  friend struct MotionCaptureCheckpoint;
public:
  enum State : uint8_t { Recovering, Armed, Tracking, Refractory, ConfigurationFault };
  enum Reason : uint8_t { None, Ready, CandidateStarted, CandidateExpired, InvalidSample,
    SampleGap, InvalidConfiguration, StrongSequence, ImpactOrientation, SupportedDescent,
    SlowDescent, RefractoryEnded, LowEpisodeRenewed };
  explicit MotionRule(const FallDetectionConfig& settings = FallDetectionConfig())
      : cfg(settings), settings_valid(validConfig(settings)) {
    if (!settings_valid) state = ConfigurationFault;
  }

  static bool validConfig(const FallDetectionConfig& c) {
    const float numbers[] = {c.low_g,c.impact_g,c.rotation_dps,c.quiet_min_g,c.quiet_max_g,
      c.quiet_max_dps,c.context_angle_deg,c.slow_angle_deg,c.support_impact_g,
      c.support_rotation_dps,c.unload_g,c.slow_peak_g};
    for (float x : numbers) if (!isfinite(x) || x <= 0) return false;
    const uint32_t times[] = {c.event_window_ms,c.pretrigger_ms,c.recovery_ms,c.refractory_ms,
      c.max_sample_gap_ms,c.context_window_ms,c.confirm_quiet_ms,c.slow_quiet_ms,c.confirm_timeout_ms,
      c.context_feature_ms};
    for (uint32_t t : times) if (!t || t >= 0x80000000u) return false;
    return c.low_g < c.unload_g && c.unload_g < 1 && c.slow_peak_g > 1 &&
      c.slow_peak_g < c.support_impact_g && c.support_impact_g <= c.impact_g &&
      c.support_rotation_dps < c.rotation_dps && c.quiet_min_g < 1 && c.quiet_max_g > 1 &&
      c.context_angle_deg < 180 && c.slow_angle_deg >= c.context_angle_deg && c.slow_angle_deg < 180 &&
      c.recovery_samples >= 2 && c.recovery_samples <= 100 &&
      c.pretrigger_ms <= 1000 && c.max_sample_gap_ms >= c.sample_period_ms &&
      c.event_window_ms >= c.sample_period_ms && c.max_sample_gap_ms <= c.recovery_ms &&
      c.context_feature_ms >= c.event_window_ms && c.context_feature_ms < c.context_window_ms &&
      c.confirm_quiet_ms <= c.confirm_timeout_ms && c.slow_quiet_ms <= c.confirm_timeout_ms &&
      c.context_window_ms >= c.confirm_timeout_ms && c.event_window_ms < c.context_window_ms;
  }

  // Magnitude-only callers exercise the original sequence; they cannot invent
  // orientation evidence. The companion supplies all three acceleration axes.
  bool update(uint32_t now, float acceleration, float rotation, bool valid) {
    return process(now, 0, 0, acceleration, acceleration, rotation, valid, false);
  }
  bool update(uint32_t now, float ax, float ay, float az, float rotation, bool valid) {
    return process(now, ax, ay, az, sqrtf(ax*ax + ay*ay + az*az), rotation, valid, true);
  }
  float peakAcceleration() const { return reported_accel; }
  float peakRotation() const { return reported_rotation; }
  State currentState() const { return state; }
  Reason lastReason() const { return reason; }
  Reason eventKind() const { return event_kind; }
  bool configured() const { return settings_valid; }
  const FallDetectionConfig& settings() const { return cfg; }
  bool referenceValid() const { return reference_valid; }
  // Meaningful only while referenceValid(); zero is a valid timestamp after rollover.
  uint32_t referenceTimeMs() const { return reference_at; }
  bool available() const { return state == Armed || state == Tracking; }
  bool cooldownActive() const { return state == Refractory; } // Short event refractory only.
  bool decision() const { return reason == CandidateExpired || reason == InvalidSample ||
      reason == SampleGap || isFallReason(reason); }
  static bool isFallReason(Reason r) { return r >= StrongSequence && r <= SlowDescent; }
  const char* stateName() const {
    switch (state) { case Armed:return "armed"; case Tracking:return "candidate";
      case Refractory:return "refractory"; case ConfigurationFault:return "config-fault";
      default:return "recovering"; }
  }
  static const char* reasonName(Reason r) {
    switch (r) {
      case Ready:return "fresh-data-ready"; case CandidateStarted:return "candidate-start";
      case CandidateExpired:return "candidate-expired"; case InvalidSample:return "invalid-sample";
      case LowEpisodeRenewed:return "low-episode-renewed";
      case SampleGap:return "sample-gap"; case InvalidConfiguration:return "invalid-config";
      case StrongSequence:return "strong-sequence"; case ImpactOrientation:return "impact-orientation";
      case SupportedDescent:return "supported-descent"; case SlowDescent:return "slow-descent";
      case RefractoryEnded:return "refractory-ended"; default:return "none";
    }
  }
private:
  struct History { uint32_t at; float x,y,z; };
  // A qualified low/impact pair retains its original deadline while a rebound
  // opens another low episode. Repeated impacts preserve the pair's largest peak.
  struct StrongPair {
    bool seen = false;
    uint32_t low_at = 0, impact_at = 0;
    float impact_peak = 0;
    void observe(uint32_t low, uint32_t now, float acceleration) {
      if (!seen || low_at != low) {
        seen=true; low_at=low; impact_at=now; impact_peak=acceleration;
      } else if (acceleration > impact_peak) {
        impact_at=now; impact_peak=acceleration;
      }
    }
  } strong_pair;
  struct Feature {
    uint32_t at = 0;
    bool seen = false;
    void observe(uint32_t now, bool present) { if (present) { at=now; seen=true; } }
    bool recent(uint32_t now, uint32_t limit) const { return seen && uint32_t(now-at)<=limit; }
  } context_impact, context_support, context_unload, context_rotation, context_slow_peak;
  static constexpr unsigned history_capacity = 64;
  History history[history_capacity] = {};
  unsigned head = 0, history_count = 0;
  FallDetectionConfig cfg;
  bool settings_valid, have_sample = false, recovery_started = false, refractory_pending = false;
  State state = Recovering;
  Reason reason = None, event_kind = None;
  uint32_t last_sample = 0, recovery_at = 0, recovery_count = 0, fired_at = 0;
  bool low_seen = false, gyro_seen = false, low_active = false, candidate_low_seen = false;
  uint32_t low_at = 0, gyro_at = 0;
  uint32_t candidate_at = 0, support_at = 0, quiet_at = 0, reference_at = 0;
  bool reference_valid = false, supported = false, quiet_tracking = false;
  float ref_x = 0,ref_y = 0,ref_z = 1, peak_accel = 0, peak_rotation = 0, trough = 100;
  float reported_accel = 0, reported_rotation = 0;
  float preceding_rotation = 0;
  Reason support_kind = None;

  void clearCandidate() {
    reference_valid = supported = quiet_tracking = false;
    peak_accel = peak_rotation = 0; trough = 100; support_kind = None;
    candidate_low_seen = false;
    context_impact=Feature{}; context_support=Feature{}; context_unload=Feature{};
    context_rotation=Feature{}; context_slow_peak=Feature{};
  }
  void breakEvidence(Reason why) {
    clearCandidate(); gyro_seen = low_seen = low_active = strong_pair.seen = false;
    head = history_count = 0; recovery_started = false; recovery_count = 0;
    state = Recovering; reason = why;
  }
  // Retire finite-lived evidence before its 32-bit timestamp can wrap and appear
  // recent again. History remains chronological because gaps reset the stream.
  void expireStoredEvidence(uint32_t now) {
    if (low_seen && uint32_t(now - low_at) > cfg.event_window_ms) low_seen = false;
    if (strong_pair.seen && uint32_t(now - strong_pair.low_at) > cfg.event_window_ms)
      strong_pair.seen = false;
    if (gyro_seen && uint32_t(now - gyro_at) > cfg.event_window_ms + cfg.pretrigger_ms)
      gyro_seen = false;
    while (history_count) {
      const unsigned oldest = (head + history_capacity - history_count) % history_capacity;
      if (uint32_t(now - history[oldest].at) <= 1000) break;
      --history_count;
    }
  }
  // Use an earlier gravity-like vector, not the accelerating trigger sample.
  void referenceAt(uint32_t now) {
    reference_valid = false;
    for (unsigned i = 0; i < history_count; ++i) {
      const auto& h = history[(head + history_capacity - 1 - i) % history_capacity];
      const uint32_t age = uint32_t(now - h.at);
      if (age >= 200 && age <= 1000) {
        ref_x=h.x; ref_y=h.y; ref_z=h.z; reference_at=h.at; reference_valid=true; return;
      }
    }
  }
  void remember(uint32_t now, float ax, float ay, float az, float a, float r, bool vector) {
    if (!vector || a < 0.85f || a > 1.15f || r >= cfg.quiet_max_dps) return;
    history[head] = {now,ax/a,ay/a,az/a}; head = (head+1)%history_capacity;
    if (history_count < history_capacity) ++history_count;
  }
  bool fire(uint32_t now, Reason kind) {
    reported_accel = peak_accel; reported_rotation = peak_rotation;
    if (kind == StrongSequence) {
      reported_accel=fmaxf(reported_accel,strong_pair.impact_peak);
      reported_rotation=fmaxf(reported_rotation,preceding_rotation);
    }
    event_kind = reason = kind; fired_at=now; state=Refractory; refractory_pending=true;
    clearCandidate(); gyro_seen=low_seen=strong_pair.seen=false; history_count=head=0;
    return true;
  }
  bool process(uint32_t now, float ax, float ay, float az, float a, float r, bool valid, bool vector) {
    reason = None;
    if (!settings_valid) { reason=InvalidConfiguration; return false; }
    if (!valid || !isfinite(a) || !isfinite(r) || a < 0 || r < 0 ||
        !isfinite(ax) || !isfinite(ay) || !isfinite(az)) {
      if (have_sample || recovery_started || state != Recovering) breakEvidence(InvalidSample);
      have_sample=false; return false;
    }
    if (have_sample && now == last_sample) return false;
    if (have_sample && uint32_t(now-last_sample) > cfg.max_sample_gap_ms) breakEvidence(SampleGap);
    have_sample=true; last_sample=now;
    expireStoredEvidence(now);
    const bool low_sample = a <= cfg.low_g;
    const bool new_low_episode = low_sample && !low_active;
    low_active = low_sample;
    if (refractory_pending && uint32_t(now-fired_at) >= cfg.refractory_ms) refractory_pending=false;
    if (state == Recovering) {
      if (!recovery_started) { recovery_started=true; recovery_at=now; recovery_count=0; }
      ++recovery_count;
      remember(now,ax,ay,az,a,r,vector);
      if (uint32_t(now-recovery_at) >= cfg.recovery_ms && recovery_count >= cfg.recovery_samples) {
        state=refractory_pending ? Refractory : Armed; reason=Ready;
      }
      return false;
    }
    if (state == Refractory) {
      if (refractory_pending) { remember(now,ax,ay,az,a,r,vector); return false; }
      state=Armed; reason=RefractoryEnded;
    }
    if (state == Tracking && uint32_t(now-candidate_at) >
        (cfg.context_paths && reference_valid ? cfg.context_window_ms : cfg.event_window_ms)) {
      clearCandidate(); state=Armed; reason=CandidateExpired;
    }

    // A renewed trough opens a strong window without moving the existing
    // context deadline or discarding an earlier bounded low/impact pair.
    if (new_low_episode) {
      if (state == Tracking && candidate_low_seen) reason=LowEpisodeRenewed;
      low_seen=true; low_at=now; candidate_low_seen=true;
    }
    if (a >= cfg.impact_g && low_seen) strong_pair.observe(low_at,now,a);
    if (r >= cfg.rotation_dps) { gyro_seen=true; gyro_at=now; preceding_rotation=r; }

    if (state == Armed && (new_low_episode || (cfg.context_paths && vector &&
        (a >= cfg.support_impact_g || (!low_sample && a <= cfg.unload_g && r >= 10))))) {
      state=Tracking; candidate_at=now; referenceAt(now);
      candidate_low_seen=low_seen;
      peak_accel=a; peak_rotation=gyro_seen && uint32_t(now-gyro_at)<=cfg.pretrigger_ms ?
          fmaxf(r,preceding_rotation) : r; trough=a;
      if (peak_rotation>r) context_rotation.observe(gyro_at, peak_rotation>=cfg.support_rotation_dps);
      // Retain the previous rejection when this sample opens a fresh candidate.
      if (reason != CandidateExpired) reason=CandidateStarted;
    }
    if (state == Tracking) {
      peak_accel=fmaxf(peak_accel,a); peak_rotation=fmaxf(peak_rotation,r); trough=fminf(trough,a);
      // Keep the latest qualifying feature times independently of diagnostic
      // maxima: an old larger bump must neither support nor hide a newer event.
      context_impact.observe(now,a>=cfg.impact_g);
      context_support.observe(now,a>=cfg.support_impact_g);
      context_unload.observe(now,a<=cfg.unload_g);
      context_rotation.observe(now,r>=cfg.support_rotation_dps);
      context_slow_peak.observe(now,a>=cfg.slow_peak_g);
    }
    // Strong evidence has its own lifetime; context expiry or a rebound cannot
    // cancel a qualified pair while its original low/impact window remains open.
    const uint32_t pair_age=uint32_t(now-strong_pair.low_at);
    if (strong_pair.seen && pair_age <= cfg.event_window_ms &&
        uint32_t(now-strong_pair.impact_at) <= pair_age && gyro_seen &&
        uint32_t(now-gyro_at) <= pair_age + cfg.pretrigger_ms) {
      return fire(now,StrongSequence); // Continued motion never vetoes this evidence.
    }
    if (state == Tracking) {
      const bool quiet = a>=cfg.quiet_min_g && a<=cfg.quiet_max_g && r<cfg.quiet_max_dps;
      bool orientation_supported=false;
      if (cfg.context_paths && vector && reference_valid && a>=0.85f && a<=1.15f) {
        const float dot=fmaxf(-1.0f,fminf(1.0f,(ax*ref_x+ay*ref_y+az*ref_z)/a));
        const float angle=acosf(dot)*57.2957795f;
        Reason kind=None;
        if (angle>=cfg.context_angle_deg && context_impact.recent(now,cfg.context_feature_ms)) kind=ImpactOrientation;
        else if (angle>=cfg.context_angle_deg && context_support.recent(now,cfg.context_feature_ms) &&
                 context_unload.recent(now,cfg.context_feature_ms) && context_rotation.recent(now,cfg.context_feature_ms))
          kind=SupportedDescent;
        else if (angle>=cfg.slow_angle_deg && context_unload.recent(now,cfg.context_feature_ms) &&
                 context_slow_peak.recent(now,cfg.context_feature_ms))
          kind=SlowDescent;
        if (kind != None && !supported) { supported=true; support_at=now; support_kind=kind; }
        // Returning to the prior orientation breaks context confirmation.
        if (angle<cfg.context_angle_deg) { supported=false; quiet_tracking=false; }
        orientation_supported=supported && angle >=
            (support_kind==SlowDescent ? cfg.slow_angle_deg : cfg.context_angle_deg);
      }
      if (supported) {
        if (uint32_t(now-support_at)>cfg.confirm_timeout_ms) {
          clearCandidate(); state=Armed; reason=CandidateExpired;
        } else {
          // Quiet magnitude alone cannot extend a stale orientation observation.
          if (!quiet || !orientation_supported) quiet_tracking=false;
          else {
            if (!quiet_tracking) { quiet_tracking=true; quiet_at=now; }
            const auto duration=support_kind==SlowDescent ? cfg.slow_quiet_ms : cfg.confirm_quiet_ms;
            if (uint32_t(now-quiet_at)>=duration) return fire(now,support_kind);
          }
        }
      }
    }
    remember(now,ax,ay,az,a,r,vector);
    return false;
  }
};
