#pragma once
#include <stdint.h>
#include <stddef.h>
#include "MotionRule.h"

// Fixed-size RAM flight recorder. No serial, filesystem, allocation, or sensor calls.
// The latest decision retains its preceding samples and a short trailing window;
// a separate event ring retains older decisions even when the sample capture changes.
class MotionTrace {
public:
  static constexpr unsigned capacity = 96, event_capacity = 32, trailing_samples = 16;
  static constexpr unsigned evidence_capacity = 5;
  struct Sample {
    uint32_t at = 0;
    float raw[7] = {}; // ax, ay, az, gx, gy, gz, chip temperature
    float gyro[3] = {}; // Bias-corrected gyro; acceleration retains gravity.
    uint16_t quality = 0;
    uint8_t state = 0, reason = 0;
    bool buzzer = false;
  };
  struct Event {
    uint32_t at = 0;
    float acceleration = 0, rotation = 0;
    uint16_t quality = 0;
    uint8_t state = 0, reason = 0;
  };
  // Candidate reference identity comes from the classifier; the recorder does
  // not infer eligibility from its independently retained diagnostic samples.
  template<class Reading> void observe(uint32_t at, const Reading& raw, const Reading& corrected,
      uint8_t state, uint8_t reason, uint16_t quality, bool buzzer, bool decision,
      bool reference_valid = false, uint32_t reference_at = 0,
      uint32_t pretrigger_ms = FallDetectionConfig{}.pretrigger_ms) {
    Sample sample;
    sample.at = at; sample.state = state; sample.reason = reason; sample.quality = quality;
    sample.buzzer = buzzer;
    const float axes[] = {raw.ax, raw.ay, raw.az, raw.gx, raw.gy, raw.gz, raw.temperature};
    for (unsigned i = 0; i < 7; ++i) sample.raw[i] = axes[i];
    sample.gyro[0] = corrected.gx; sample.gyro[1] = corrected.gy; sample.gyro[2] = corrected.gz;
    const bool unavailable = (quality & 0x300) != 0 || reason == MotionRule::InvalidSample;
    const bool fault_decision = reason == MotionRule::InvalidSample || reason == MotionRule::SampleGap;
    if (unavailable || fault_decision) continuity_count = 0;
    const bool starts = state == MotionRule::Tracking &&
        (!candidate_open || reason == MotionRule::CandidateStarted || reason == MotionRule::CandidateExpired);
    if (starts && !decision) beginCandidate(sample, reference_valid, reference_at, pretrigger_ms);
    if (candidate_open && !starts && !unavailable && !fault_decision) updateEvidence(sample);
    ring[head] = sample; head = (head + 1) % capacity;
    if (count < capacity) ++count;
    if (!unavailable && continuity_count < capacity) ++continuity_count;
    if (decision) {
      if (!fault_decision || candidate_open) freeze();
      candidate_open = false;
      if (starts) beginCandidate(sample, reference_valid, reference_at, pretrigger_ms);
    } else if (post_remaining) {
      if (captured == capacity) {
        for (unsigned i = 1; i < captured; ++i) frozen[i - 1] = frozen[i];
        --captured;
      }
      frozen[captured++] = sample; --post_remaining;
    }
    if (reason) record(at, corrected.acceleration(), corrected.rotation(), state, reason, quality);
  }
  // Faults are events, never synthetic zero-motion samples.
  void record(uint32_t at, float acceleration, float rotation, uint8_t state, uint8_t reason,
      uint16_t quality, bool rejected = false) {
    Event& entry=events[event_head];
    entry.at=at; entry.acceleration=acceleration; entry.rotation=rotation;
    entry.quality=quality; entry.state=state; entry.reason=reason;
    event_head = (event_head + 1) % event_capacity;
    if (event_count < event_capacity) ++event_count;
    if (rejected || (quality & 0x300) != 0) continuity_count = 0;
    if (rejected) {
      if (candidate_open) freeze();
      candidate_open = false;
    }
  }
  unsigned sampleCount() const { return captured ? captured : count; }
  unsigned eventCount() const { return event_count; }
  unsigned evidenceCount() const { return frozen_evidence_count; }
  Sample evidence(unsigned index) const { return frozen_evidence[index]; }
  Sample sample(unsigned index) const { return captured ? frozen[index] : recent(index); }
  Event event(unsigned index) const { return events[(event_head + event_capacity - event_count + index) % event_capacity]; }
private:
  Sample ring[capacity], frozen[capacity];
  Event events[event_capacity];
  // Start, acceleration minimum/maximum, gyro maximum (including the configured
  // pre-trigger window), and the classifier's reference, when available. These
  // survive a multi-second candidate.
  Sample candidate[evidence_capacity], frozen_evidence[evidence_capacity];
  bool candidate_open = false;
  unsigned candidate_count = 0, frozen_evidence_count = 0;
  unsigned head = 0, count = 0, captured = 0, post_remaining = 0, event_head = 0, event_count = 0;
  // Only these newest rows can contribute pre-trigger evidence. Older rows
  // remain available in diagnostic captures after a data or maintenance break.
  unsigned continuity_count = 0;
  Sample recent(unsigned index) const { return ring[(head + capacity - count + index) % capacity]; }
  static float magnitude(const float* axes) {
    return sqrtf(axes[0]*axes[0] + axes[1]*axes[1] + axes[2]*axes[2]);
  }
  void beginCandidate(const Sample& sample, bool reference_valid, uint32_t reference_at,
      uint32_t pretrigger_ms) {
    candidate_open = true; candidate_count = 4;
    for (unsigned i = 0; i < 4; ++i) candidate[i] = sample;
    for (unsigned i = 0; i < continuity_count; ++i) {
      const Sample previous = recent(count-1-i);
      const uint32_t age = uint32_t(sample.at-previous.at);
      if (age <= pretrigger_ms && magnitude(previous.gyro) > magnitude(candidate[3].gyro)) candidate[3]=previous;
      if (reference_valid && previous.at == reference_at && candidate_count == 4) {
        candidate[4]=previous; candidate_count=5;
      }
    }
  }
  void updateEvidence(const Sample& sample) {
    const float acceleration=magnitude(sample.raw), rotation=magnitude(sample.gyro);
    if (acceleration < magnitude(candidate[1].raw)) candidate[1]=sample;
    if (acceleration > magnitude(candidate[2].raw)) candidate[2]=sample;
    if (rotation > magnitude(candidate[3].gyro)) candidate[3]=sample;
  }
  void freeze() {
    captured=count; post_remaining=trailing_samples;
    for (unsigned i=0; i<count; ++i) frozen[i]=recent(i);
    frozen_evidence_count=candidate_open ? candidate_count : 0;
    for (unsigned i=0; i<frozen_evidence_count; ++i) frozen_evidence[i]=candidate[i];
  }
};
