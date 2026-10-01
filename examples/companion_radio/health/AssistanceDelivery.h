#pragma once
#include <HealthNodeConfig.h>
#include "FallAckMessage.h"

// Public channel only. Repeat confirmation is a matching RF rebroadcast, not a DM ACK.
class AssistanceTransport {
public:
  enum Result { Queued, NoPacket, InvalidPayload };
  virtual uint32_t uniqueTimestamp() = 0;
  virtual Result queueAssistance(const uint8_t* data, unsigned len, uint32_t tag,
                                uint8_t fingerprint[8]) = 0;
};

// One volatile public channel event. No waits, heap allocation or UI changes.
// Two instances (fall and assistance) allow both essential texts to progress.
class AssistanceDelivery {
public:
  enum State { Idle, Pending, Queued, Transmitted, Repeated, Failed };
  enum Failure { None, InvalidText, AttemptsExhausted, NoRepeat };
  // Distinct seeds 0/1 allocate even/odd tags for two concurrent message owners.
  explicit AssistanceDelivery(uint32_t tag_seed = 0) : next_tag(tag_seed) {}
  bool begin(uint32_t now, AssistanceTransport&, const uint8_t* payload, unsigned len) {
    if (busy()) return false; // Coalesce without resetting an existing retry budget.
    attempts = transmissions = fingerprint_count = 0;
    attempted = false; failure_code = None; active_tag = 0; changed_at = now;
    if (len < 5 || len > sizeof(body)) {
      state_code = Failed; failure_code = InvalidText; return true;
    }
    memcpy(body, payload, len); body_len = len;
    body[4] = 0; // Standard plain public channel text.
    state_code = Pending;
    return true;
  }
  void poll(uint32_t now, AssistanceTransport& link) {
    if (state_code == Transmitted && uint32_t(now - changed_at) >= HealthNodeConfig::channel_repeat_wait_ms) {
      if (transmissions >= HealthNodeConfig::channel_transmit_attempts) {
        state_code = Failed; failure_code = NoRepeat;
      } else { state_code = Pending; attempted = false; }
    }
    if (state_code != Pending ||
        (attempted && uint32_t(now - changed_at) < HealthNodeConfig::ack_retry_ms)) return;
    if (attempts >= HealthNodeConfig::ack_max_attempts) {
      state_code = Failed; failure_code = AttemptsExhausted; return;
    }
    attempted = true; changed_at = now; ++attempts;
    // A fresh standard timestamp bypasses repeater packet dedup on retries.
    // Public receivers may display duplicate text; no recipient-level exactly-once claim.
    const uint32_t timestamp = link.uniqueTimestamp();
    for (unsigned b = 0; b < 4; ++b) body[b] = uint8_t(timestamp >> (8*b));
    next_tag += 2; if (next_tag == 0) next_tag += 2;
    active_tag = next_tag; state_code = Queued;
    uint8_t fingerprint[8];
    const auto result = link.queueAssistance(body, body_len, active_tag, fingerprint);
    if (result == AssistanceTransport::InvalidPayload) {
      active_tag = 0; state_code = Failed; failure_code = InvalidText;
    } else if (result == AssistanceTransport::NoPacket) {
      active_tag = 0; retryOrFail();
    } else {
      memcpy(fingerprints[fingerprint_count++], fingerprint, 8);
    }
    // A synchronous dispatcher failure callback may already have changed state.
  }
  void txComplete(uint32_t now, uint32_t tag, bool sent) {
    if (!tag || tag != active_tag) return;
    active_tag = 0;
    if (state_code == Repeated) return; // A late callback cannot undo RF evidence.
    if (state_code != Queued) return;
    changed_at = now;
    if (sent) { ++transmissions; state_code = Transmitted; }
    else retryOrFail();
  }
  bool confirmRepeat(const uint8_t fingerprint[8]) {
    for (unsigned i = 0; i < fingerprint_count; ++i) {
      if (memcmp(fingerprints[i], fingerprint, 8) != 0) continue;
      state_code = Repeated; failure_code = None; return true;
    }
    return false; // Retain old hashes until next event, accepting late repeats after exhaustion.
  }
  bool busy() const { return state_code == Pending || state_code == Queued || state_code == Transmitted || active_tag; }
  State state() const { return state_code; }
  Failure failure() const { return failure_code; }
  uint8_t attemptCount() const { return attempts; }
  uint8_t transmitCount() const { return transmissions; }
  uint32_t queuedTag() const { return active_tag; }
  const char* status() const {
    switch (state_code) {
      case Pending: return "pending"; case Queued: return "queued";
      case Transmitted: return "transmitted; waiting for repeat";
      case Repeated: return "repeater echo heard";
      case Failed: return failure_code == InvalidText ? "failed: invalid text" :
                          failure_code == NoRepeat ? "unconfirmed: no repeat heard" : "failed: retries exhausted";
      default: return "idle";
    }
  }
private:
  void retryOrFail() {
    if (attempts >= HealthNodeConfig::ack_max_attempts) {
      state_code = Failed; failure_code = AttemptsExhausted;
    } else state_code = Pending;
  }
  uint8_t body[FallAckMessage::max_payload_bytes] = {};
  uint8_t fingerprints[HealthNodeConfig::ack_max_attempts][8] = {};
  unsigned body_len = 0;
  uint32_t next_tag, active_tag = 0, changed_at = 0;
  uint8_t attempts = 0, transmissions = 0, fingerprint_count = 0;
  bool attempted = false;
  State state_code = Idle;
  Failure failure_code = None;
};
