#pragma once
#include "FallChannel.h"
#include <Utils.h>
#include <string.h>
#include <cerrno>

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
// Transport errors are distinct from a complete but invalid record. Missing is
// an API observation, never proof that a record did not exist before a failure.
enum class ReadStage : uint8_t { NotAttempted, Mount, Open, Stat, Read, Close, Missing, Invalid, Success };
struct ReadResult {
  ReadStage stage = ReadStage::NotAttempted;
  int error = 0, close_error = 0;
  uint8_t slot = 0;
  bool api_checked = false;
  bool uncertain() const {
    return stage != ReadStage::Missing && stage != ReadStage::Invalid && stage != ReadStage::Success;
  }
};
inline const char* readStageName(ReadStage stage) {
  switch (stage) {
    case ReadStage::NotAttempted:return "not-attempted";
    case ReadStage::Mount:return "mount";
    case ReadStage::Open:return "open";
    case ReadStage::Stat:return "stat";
    case ReadStage::Read:return "read";
    case ReadStage::Close:return "close";
    case ReadStage::Missing:return "missing";
    case ReadStage::Invalid:return "invalid-record";
    default:return "valid-record";
  }
}
struct Recovery {
  Outcome outcome = Missing;
  Incident value;
  uint32_t elapsed_ms = 0;
  ReadResult reads[2];
  bool shouldResume() const { return outcome == Resume || outcome == UncertainClock; }
};
struct Record {
  Incident value;
  uint32_t generation = 0;
  bool exists = false, readable = false, valid = false;
  ReadResult io;
};
// The first failed operation is retained, together with any cleanup-close error.
// Success describes the selected API and readback, never physical durability.
enum class SaveStage : uint8_t { NotAttempted, Invalid, ReadExisting, Open, Write, Flush, Close, Verify, Success };
struct SaveResult {
  SaveStage stage = SaveStage::NotAttempted;
  int error = 0, close_error = 0;
  uint8_t slot = 0;
  bool api_checked = false;
  ReadResult read_result; // Last inspection/verification; writer status stays separate.
  bool success() const { return stage == SaveStage::Success; }
};
inline const char* saveStageName(SaveStage stage) {
  switch (stage) {
    case SaveStage::NotAttempted:return "not-attempted";
    case SaveStage::Invalid:return "invalid-incident";
    case SaveStage::ReadExisting:return "read-existing";
    case SaveStage::Open:return "open";
    case SaveStage::Write:return "write";
    case SaveStage::Flush:return "stdio-flush";
    case SaveStage::Close:return "close";
    case SaveStage::Verify:return "verify";
    default:return "verified-readback";
  }
}
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

// Decode only a complete record after its I/O owner has closed successfully.
// All adapters share this format/checksum/channel-validation owner.
inline void decodeRecord(const uint8_t* bytes, Record& result) {
  if (memcmp(bytes, "FIN1", 4)) return;
  uint8_t checksum[16];
  mesh::Utils::sha256(checksum, sizeof(checksum), bytes, checksum_offset);
  if (memcmp(checksum, bytes + checksum_offset, sizeof(checksum)) ||
      bytes[17] > 1 || bytes[18] > 1) return;
  result.generation = get32(bytes + 4);
  result.value.incident = get32(bytes + 8);
  result.value.origin_epoch = get32(bytes + 12);
  result.value.recovery_count = bytes[16]; result.value.active = bytes[17] != 0;
  result.value.channel.public_channel = bytes[18] != 0; result.value.channel.key_length = bytes[19];
  memcpy(result.value.channel.name, bytes + 20, 32);
  memcpy(result.value.channel.key, bytes + 52, 32);
  result.valid = validIncident(result.value);
}

// Compatibility reader: Arduino existence/close results cannot establish the
// checked ESP32 contract. Kept for other targets and earlier fault models.
template<class FS> Record read(FS& fs, unsigned slot) {
  Record result; result.io.slot = uint8_t(slot);
  result.exists = fs.exists(path(slot));
  if (!result.exists) { result.io.stage = ReadStage::Missing; return result; }
  auto file = fs.open(path(slot), "r");
  if (!file) { result.io.stage = ReadStage::Open; result.io.error = EIO; return result; }
  uint8_t bytes[record_size] = {};
  const size_t size = file.size();
  const bool complete = size == record_size && file.read(bytes, sizeof(bytes)) == sizeof(bytes);
  result.readable = size != record_size || complete;
  file.close();
  if (complete) decodeRecord(bytes, result);
  result.io.stage = result.valid ? ReadStage::Success : result.readable ? ReadStage::Invalid : ReadStage::Read;
  if (!result.readable) result.io.error = EIO;
  return result;
}
template<class FS> struct ArduinoReader {
  FS& fs;
  Record operator()(unsigned slot) { return read(fs, slot); }
};

// Read-only recovery decision. Age-based expiry also requires the caller to
// establish continuity of the stored/current clock across restart. Date plausibility
// alone is insufficient: fallback clocks may start at a fixed plausible epoch.
// Unqualified/rollback clocks get a finite reboot budget and an immediate warning.
template<class Reader> Recovery loadWithReader(Reader& reader, uint32_t now_epoch, bool clock_continuity = false) {
  const auto a = reader(0), b = reader(1);
  Recovery result;
  result.reads[0] = a.io; result.reads[1] = b.io;
  const bool uncertain = a.io.uncertain() || b.io.uncertain();
  if (!a.exists && !b.exists && !uncertain) return result;
  if (!a.valid && !b.valid) { result.outcome = Corrupt; return result; }
  const auto& latest = b.valid && (!a.valid || newer(b.generation, a.generation)) ? b : a;
  result.value = latest.value;
  // A damaged second record might have been a later resolution. Do not resurrect
  // an older active alarm when the latest durable intent cannot be established.
  if (uncertain || (a.exists && !a.valid) || (b.exists && !b.valid)) { result.outcome = Corrupt; return result; }
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
template<class FS> Recovery load(FS& fs, uint32_t now_epoch, bool clock_continuity = false) {
  ArduinoReader<FS> reader{fs};
  return loadWithReader(reader, now_epoch, clock_continuity);
}

// The writer owns only one file operation; this function remains the sole owner
// of encoding, slot selection, verification and repair ordering. A failed write
// can already have committed, so the caller retains dirty state and retries.
template<class Reader, class Writer> SaveResult saveWithReader(Reader& reader, const Incident& value, Writer& writer) {
  SaveResult result;
  result.api_checked = writer.apiChecked();
  if (!validIncident(value)) { result.stage = SaveStage::Invalid; return result; }
  const auto a = reader(0), b = reader(1);
  result.read_result = b.io;
  if (a.io.uncertain() || b.io.uncertain()) {
    result.stage = SaveStage::ReadExisting; result.slot = a.io.uncertain() ? 0 : 1;
    result.read_result = result.slot ? b.io : a.io;
    return result;
  }
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
    const auto inspection = result.read_result;
    result = writer(destination, bytes, sizeof(bytes));
    result.read_result = inspection;
    result.slot = uint8_t(destination);
    if (!result.success()) return false;
    const auto check = reader(destination);
    result.read_result = check.io;
    const bool verified = check.valid && check.generation == generation &&
        check.value.incident == value.incident && check.value.origin_epoch == value.origin_epoch &&
        check.value.recovery_count == value.recovery_count && check.value.active == value.active &&
        check.value.channel.key_length == value.channel.key_length &&
        check.value.channel.public_channel == value.channel.public_channel &&
        !memcmp(check.value.channel.name, value.channel.name, 32) &&
        !memcmp(check.value.channel.key, value.channel.key, 32);
    if (!verified) result.stage = SaveStage::Verify;
    return verified;
  };
  if (!write_checked(slot)) return result;
  // Repair a previously damaged second slot only after the new intent is verified.
  // Equal generations are safe here: both records contain exactly the same intent.
  // A torn repair remains explicitly Corrupt rather than falsely claiming success.
  const auto& other = slot ? a : b;
  if (other.exists && !other.valid) write_checked(slot ^ 1u);
  return result;
}
template<class FS, class Writer> SaveResult saveChecked(FS& fs, const Incident& value, Writer& writer) {
  ArduinoReader<FS> reader{fs};
  return saveWithReader(reader, value, writer);
}

// Compatibility path for non-ESP32 filesystems and the earlier fault models.
// It deliberately does not claim checked synchronization or close results.
template<class FS> struct ArduinoWriter {
  FS& fs;
  bool apiChecked() const { return false; }
  SaveResult operator()(unsigned slot, const uint8_t* bytes, size_t length) {
    SaveResult result; result.slot = uint8_t(slot);
    auto file = fs.open(path(slot), "w");
    if (!file) { result.stage = SaveStage::Open; return result; }
    const bool written = file.write(bytes, length) == length;
    file.flush(); file.close();
    result.stage = written ? SaveStage::Success : SaveStage::Write;
    return result;
  }
};
template<class FS> bool save(FS& fs, const Incident& value) {
  ArduinoWriter<FS> writer{fs};
  return saveChecked(fs, value, writer).success();
}
}
