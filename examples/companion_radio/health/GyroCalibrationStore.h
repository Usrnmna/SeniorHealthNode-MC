#pragma once
#include <helpers/GyroCalibration.h>
#include <Utils.h>
#include <string.h>

// Versioned alternating records on the existing node filesystem. Explicit field
// encoding avoids struct padding; sensor configuration prevents incompatible reuse.
namespace GyroCalibrationStore {
constexpr size_t record_size = 68;
struct Record {
  uint32_t generation = 0;
  GyroCalibration::Result value;
  bool valid = false;
  bool readable = false;
};
inline const char* path(unsigned slot) { return slot ? "/gyro_cal1" : "/gyro_cal0"; }
inline bool newer(uint32_t a, uint32_t b) { return a != b && uint32_t(a - b) < 0x80000000u; }
inline void put32(uint8_t* p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i));
}
inline uint32_t get32(const uint8_t* p) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value |= uint32_t(p[i]) << (8 * i);
  return value;
}
inline void putFloat(uint8_t* p, float value) {
  static_assert(sizeof(float) == 4, "Calibration storage requires 32-bit float");
  uint32_t bits; memcpy(&bits, &value, 4); put32(p, bits);
}
inline float getFloat(const uint8_t* p) {
  const uint32_t bits = get32(p); float value; memcpy(&value, &bits, 4); return value;
}

template<class FS> Record read(FS& fs, unsigned slot) {
  Record result;
  auto file = fs.open(path(slot), "r");
  if (!file) return result;
  uint8_t bytes[record_size] = {};
  const size_t size = file.size();
  const bool complete = size == sizeof(bytes) && file.read(bytes, sizeof(bytes)) == sizeof(bytes);
  result.readable = size != sizeof(bytes) || complete;
  file.close();
  const uint8_t config[] = {0x68, 0x08, 0x10, 0x03, 19, 0, 0, 0};
  if (!complete || memcmp(bytes, "GYC1", 4) || memcmp(bytes + 8, config, sizeof(config))) return result;
  uint8_t hash[16];
  mesh::Utils::sha256(hash, sizeof(hash), bytes, sizeof(bytes) - sizeof(hash));
  if (memcmp(hash, bytes + sizeof(bytes) - sizeof(hash), sizeof(hash))) return result;
  result.generation = get32(bytes + 4);
  for (unsigned i = 0; i < 3; ++i) {
    result.value.bias[i] = getFloat(bytes + 16 + 4*i);
    result.value.noise[i] = getFloat(bytes + 28 + 4*i);
  }
  result.value.temperature = getFloat(bytes + 40);
  result.value.temperature_span = getFloat(bytes + 44);
  result.value.samples = get32(bytes + 48);
  result.valid = GyroCalibration::validResult(result.value);
  return result;
}

// Missing records select zero correction. Corrupt records are reported, with
// zero correction if neither record remains usable; detection is not disabled.
template<class FS> bool load(FS& fs, GyroCalibration::Result& value) {
  const auto a = read(fs, 0), b = read(fs, 1);
  value = GyroCalibration::Result{};
  if (a.valid || b.valid) {
    value = b.valid && (!a.valid || newer(b.generation, a.generation)) ? b.value : a.value;
    return true;
  }
  return !fs.exists(path(0)) && !fs.exists(path(1));
}

// Only overwrite the older slot, then read back the complete checked record.
// The caller applies the new coefficients only after successful persistence.
template<class FS> bool save(FS& fs, const GyroCalibration::Result& value) {
  if (!GyroCalibration::validResult(value)) return false;
  const auto a = read(fs, 0), b = read(fs, 1);
  // Do not overwrite existing records when neither can be read reliably.
  if ((!a.readable && fs.exists(path(0))) || (!b.readable && fs.exists(path(1)))) return false;
  const bool use_b = b.valid && (!a.valid || newer(b.generation, a.generation));
  const unsigned slot = a.valid || b.valid ? (use_b ? 0 : 1) : (fs.exists(path(0)) ? 1 : 0);
  const uint32_t generation = (use_b ? b.generation : a.generation) + 1;
  uint8_t bytes[record_size] = {};
  memcpy(bytes, "GYC1", 4); put32(bytes + 4, generation);
  const uint8_t config[] = {0x68, 0x08, 0x10, 0x03, 19, 0, 0, 0};
  memcpy(bytes + 8, config, sizeof(config));
  for (unsigned i = 0; i < 3; ++i) {
    putFloat(bytes + 16 + 4*i, value.bias[i]); putFloat(bytes + 28 + 4*i, value.noise[i]);
  }
  putFloat(bytes + 40, value.temperature); putFloat(bytes + 44, value.temperature_span);
  put32(bytes + 48, value.samples);
  mesh::Utils::sha256(bytes + 52, 16, bytes, 52);
  auto file = fs.open(path(slot), "w");
  if (!file) return false;
  const bool written = file.write(bytes, sizeof(bytes)) == sizeof(bytes);
  file.flush(); file.close();
  if (!written) return false;
  const auto check = read(fs, slot);
  if (!check.valid || check.generation != generation) return false;
  for (unsigned i = 0; i < 3; ++i)
    if (check.value.bias[i] != value.bias[i] || check.value.noise[i] != value.noise[i]) return false;
  return check.value.temperature == value.temperature &&
      check.value.temperature_span == value.temperature_span && check.value.samples == value.samples;
}
}
