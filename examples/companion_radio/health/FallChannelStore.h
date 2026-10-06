#pragma once
#include "FallChannel.h"

// Two alternating checked records keep the previous committed selection intact
// during a failed write. This uses the node's existing filesystem, independently
// of ordinary preferences. The template also permits host fault-injection tests.
namespace FallChannelStore {
constexpr size_t record_size = 8 + FallChannel::value_capacity + 16;
struct Record {
  uint32_t generation = 0;
  FallChannel channel;
  bool valid = false;
};
inline const char* path(unsigned slot) { return slot ? "/fall_ch1" : "/fall_ch0"; }
inline bool newer(uint32_t a, uint32_t b) { return a != b && uint32_t(a - b) < 0x80000000u; }

template<class FS> Record read(FS& fs, unsigned slot) {
  Record result;
  auto file = fs.open(path(slot), "r");
  if (!file) return result;
  uint8_t bytes[record_size] = {};
  const bool complete = file.size() == sizeof(bytes) && file.read(bytes, sizeof(bytes)) == sizeof(bytes);
  file.close();
  if (!complete || memcmp(bytes, "FCH1", 4)) return result;
  uint8_t hash[16];
  mesh::Utils::sha256(hash, sizeof(hash), bytes, sizeof(bytes) - sizeof(hash));
  if (memcmp(hash, bytes + sizeof(bytes) - sizeof(hash), sizeof(hash))) return result;
  const char* value = reinterpret_cast<const char*>(bytes + 8);
  if (!memchr(value, 0, FallChannel::value_capacity) || !FallChannel::parse(value, result.channel)) return result;
  for (unsigned i = 0; i < 4; ++i) result.generation |= uint32_t(bytes[4 + i]) << (8 * i);
  result.valid = true;
  return result;
}

// Missing settings use the compiled default. Unreadable/corrupt existing settings
// disable sending instead of silently disclosing private alerts on the default.
template<class FS> bool load(FS& fs, FallChannel& channel) {
  const auto a = read(fs, 0), b = read(fs, 1);
  if (a.valid || b.valid) {
    channel = b.valid && (!a.valid || newer(b.generation, a.generation)) ? b.channel : a.channel;
    return true;
  }
  channel = FallChannel{};
  if (fs.exists(path(0)) || fs.exists(path(1))) { channel.key_length = 0; return false; }
  return true;
}

// A successful write is read back before the in-memory selection may change.
template<class FS> bool save(FS& fs, const FallChannel& channel) {
  if (!channel.valid()) return false;
  const auto a = read(fs, 0), b = read(fs, 1);
  const bool use_b = b.valid && (!a.valid || newer(b.generation, a.generation));
  const unsigned slot = a.valid || b.valid ? (use_b ? 0 : 1) : 0;
  const uint32_t generation = (use_b ? b.generation : a.generation) + 1;
  uint8_t bytes[record_size] = {};
  memcpy(bytes, "FCH1", 4);
  for (unsigned i = 0; i < 4; ++i) bytes[4 + i] = uint8_t(generation >> (8 * i));
  char value[FallChannel::value_capacity] = {};
  channel.storageValue(value);
  memcpy(bytes + 8, value, sizeof(value));
  mesh::Utils::sha256(bytes + sizeof(bytes) - 16, 16, bytes, sizeof(bytes) - 16);
  auto file = fs.open(path(slot), "w");
  if (!file) return false;
  const bool written = file.write(bytes, sizeof(bytes)) == sizeof(bytes);
  file.flush(); file.close();
  if (!written) return false;
  const auto check = read(fs, slot);
  char saved[FallChannel::value_capacity] = {};
  check.channel.storageValue(saved);
  return check.valid && check.generation == generation && strcmp(saved, value) == 0;
}
}
