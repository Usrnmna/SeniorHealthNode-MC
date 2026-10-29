#pragma once
#include "FallChannel.h"
#include <Utils.h>
#include <string.h>

// Two versioned records on the existing node filesystem. Only incident open,
// resolution and explicit reboot recovery write records; motion stays in RAM.
namespace FallIncidentStore {
constexpr uint32_t minimum_epoch = 1704067200UL; // 2024-01-01; plausibility, not clock authentication.
constexpr uint32_t maximum_age_seconds = 24UL * 60 * 60;
constexpr uint8_t uncertain_recovery_limit = 3;
constexpr size_t record_size = 100, checksum_offset = 84;

struct Incident {
  uint32_t incident = 0, origin_epoch = 0;
  uint8_t recovery_count = 0;
  bool active = false;
  FallChannel channel;
};
enum Outcome { Missing, Resolved, Resume, UncertainClock, Expired, RecoveryLimit, Corrupt };
struct Recovery {
  Outcome outcome = Missing;
  Incident value;
  uint32_t elapsed_ms = 0;
  bool shouldResume() const { return outcome == Resume || outcome == UncertainClock; }
};
struct Record {
  Incident value;
  uint32_t generation = 0;
  bool exists = false, readable = false, valid = false;
};
inline const char* path(unsigned slot) { return slot ? "/fall_inc1" : "/fall_inc0"; }
inline bool newer(uint32_t a, uint32_t b) { return a != b && uint32_t(a - b) < 0x80000000u; }
inline void put32(uint8_t* dest, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) dest[i] = uint8_t(value >> (8*i));
}
inline uint32_t get32(const uint8_t* src) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value |= uint32_t(src[i]) << (8*i);
  return value;
}
inline bool validIncident(const Incident& value) {
  if (!value.incident || !value.channel.valid() || !value.channel.name[0] ||
      !memchr(value.channel.name, 0, sizeof(value.channel.name))) return false;
  char text[FallChannel::value_capacity];
  value.channel.storageValue(text);
  FallChannel parsed;
  return FallChannel::parse(text, parsed) && parsed.key_length == value.channel.key_length &&
      (value.channel.public_channel || !memcmp(parsed.key, value.channel.key, value.channel.key_length));
}
inline const char* outcomeName(Outcome result) {
  switch (result) {
    case Missing:return "none"; case Resolved:return "resolved"; case Resume:return "resumed";
    case UncertainClock:return "resumed-clock-uncertain"; case Expired:return "expired";
    case RecoveryLimit:return "recovery-limit"; default:return "storage-uncertain";
  }
}
// Saturation prevents a long sequence of known-clock reboots from wrapping the
// unknown-clock recovery budget back to zero.
inline void countRecovery(Incident& value) {
  if (value.recovery_count != UINT8_MAX) ++value.recovery_count;
}

template<class FS> Record read(FS& fs, unsigned slot) {
  Record result; result.exists = fs.exists(path(slot));
  if (!result.exists) return result;
  auto file = fs.open(path(slot), "r");
  if (!file) return result;
  uint8_t bytes[record_size] = {};
  const size_t size = file.size();
  const bool complete = size == record_size && file.read(bytes, sizeof(bytes)) == sizeof(bytes);
  result.readable = size != record_size || complete;
  file.close();
  if (!complete || memcmp(bytes, "FIN1", 4)) return result;
  uint8_t checksum[16];
  mesh::Utils::sha256(checksum, sizeof(checksum), bytes, checksum_offset);
  if (memcmp(checksum, bytes + checksum_offset, sizeof(checksum)) ||
      bytes[17] > 1 || bytes[18] > 1) return result;
  result.generation = get32(bytes + 4);
  result.value.incident = get32(bytes + 8);
  result.value.origin_epoch = get32(bytes + 12);
  result.value.recovery_count = bytes[16]; result.value.active = bytes[17] != 0;
  result.value.channel.public_channel = bytes[18] != 0; result.value.channel.key_length = bytes[19];
  memcpy(result.value.channel.name, bytes + 20, 32);
  memcpy(result.value.channel.key, bytes + 52, 32);
  result.valid = validIncident(result.value);
  return result;
}

// Read-only recovery decision. Age-based expiry also requires the caller to
// establish continuity of the stored/current clock across restart. Date plausibility
// alone is insufficient: fallback clocks may start at a fixed plausible epoch.
// Unqualified/rollback clocks get a finite reboot budget and an immediate warning.
template<class FS> Recovery load(FS& fs, uint32_t now_epoch, bool clock_continuity = false) {
  const auto a = read(fs, 0), b = read(fs, 1);
  Recovery result;
  if (!a.exists && !b.exists) return result;
  if (!a.valid && !b.valid) { result.outcome = Corrupt; return result; }
  const auto& latest = b.valid && (!a.valid || newer(b.generation, a.generation)) ? b : a;
  result.value = latest.value;
  // A damaged second record might have been a later resolution. Do not resurrect
  // an older active alarm when the latest durable intent cannot be established.
  if ((a.exists && !a.valid) || (b.exists && !b.valid)) { result.outcome = Corrupt; return result; }
  if (!latest.value.active) { result.outcome = Resolved; return result; }
  const bool age_known = clock_continuity && result.value.origin_epoch >= minimum_epoch &&
      now_epoch >= result.value.origin_epoch;
  if (age_known) {
    const uint32_t age = now_epoch - result.value.origin_epoch;
    if (age >= maximum_age_seconds) { result.outcome = Expired; return result; }
    result.elapsed_ms = age >= HealthNodeConfig::response_timeout_ms / 1000 ?
        HealthNodeConfig::response_timeout_ms : age * 1000;
    result.outcome = Resume;
  } else if (result.value.recovery_count >= uncertain_recovery_limit) {
    result.outcome = RecoveryLimit;
  } else {
    result.elapsed_ms = HealthNodeConfig::response_timeout_ms;
    result.outcome = UncertainClock;
  }
  return result;
}

// Write the older slot and verify its readback before reporting success. Arduino
// File flush/close provide no durability result; successful cached readback does
// not prove survival of a power cut. A resolved record is a tombstone, not
// deletion, so an earlier active record cannot normally win. The caller retains
// active local state and reports/retries observable persistence failures.
template<class FS> bool save(FS& fs, const Incident& value) {
  if (!validIncident(value)) return false;
  const auto a = read(fs, 0), b = read(fs, 1);
  if ((a.exists && !a.readable) || (b.exists && !b.readable)) return false;
  const bool use_b = b.valid && (!a.valid || newer(b.generation, a.generation));
  const unsigned slot = a.valid || b.valid ? (use_b ? 0 : 1) : (a.exists ? 1 : 0);
  const uint32_t generation = (use_b ? b.generation : a.generation) + 1;
  uint8_t bytes[record_size] = {};
  memcpy(bytes, "FIN1", 4); put32(bytes + 4, generation);
  put32(bytes + 8, value.incident); put32(bytes + 12, value.origin_epoch);
  bytes[16] = value.recovery_count; bytes[17] = value.active; bytes[18] = value.channel.public_channel;
  bytes[19] = value.channel.key_length;
  memcpy(bytes + 20, value.channel.name, 32); memcpy(bytes + 52, value.channel.key, 32);
  mesh::Utils::sha256(bytes + checksum_offset, 16, bytes, checksum_offset);
  const auto write_checked = [&](unsigned destination) {
    auto file = fs.open(path(destination), "w");
    if (!file) return false;
    const bool written = file.write(bytes, sizeof(bytes)) == sizeof(bytes);
    file.flush(); file.close();
    if (!written) return false;
    const auto check = read(fs, destination);
    return check.valid && check.generation == generation &&
        check.value.incident == value.incident && check.value.origin_epoch == value.origin_epoch &&
        check.value.recovery_count == value.recovery_count && check.value.active == value.active &&
        check.value.channel.key_length == value.channel.key_length &&
        check.value.channel.public_channel == value.channel.public_channel &&
        !memcmp(check.value.channel.name, value.channel.name, 32) &&
        !memcmp(check.value.channel.key, value.channel.key, 32);
  };
  if (!write_checked(slot)) return false;
  // Repair a previously damaged second slot only after the new intent is verified.
  // Equal generations are safe here: both records contain exactly the same intent.
  // A torn repair remains explicitly Corrupt rather than falsely claiming success.
  const auto& other = slot ? a : b;
  return !other.exists || other.valid || write_checked(slot ^ 1u);
}
}
