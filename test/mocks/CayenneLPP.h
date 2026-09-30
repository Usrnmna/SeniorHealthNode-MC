#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class CayenneLPP {
public:
  explicit CayenneLPP(size_t capacity) : bytes(capacity) {}
  uint8_t* getBuffer() { return bytes.data(); }
  const uint8_t* getBuffer() const { return bytes.data(); }
  uint16_t getSize() const { return cursor; }
  // Only capacity reservation is modeled; tests replay pre-encoded real fields.
  void addRelativeHumidity(uint8_t, float) { reserve(3); }
  void addTemperature(uint8_t, float) { reserve(4); }
  void addGenericSensor(uint8_t, float) { reserve(6); }
protected:
  uint8_t getTypeSize(uint8_t type) {
    switch (type) {
      case 104: case 120: return 1;
      case 103: case 115: case 116: case 117: case 121: case 128: return 2;
      case 100: case 118: case 130: return 4;
      default: return 0;
    }
  }
private:
  std::vector<uint8_t> bytes;
  size_t cursor = 0;
  void reserve(unsigned count) { if (cursor + count <= bytes.size()) cursor += count; }
};
