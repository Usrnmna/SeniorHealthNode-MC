#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Optional RAM-only recording. Whole records are admitted or dropped; acquisition
// never waits for the consumer. Every drain writes one complete 31-byte frame so
// ordinary serial diagnostics can appear between frames without corrupting them.
class MotionRecorder {
public:
  static constexpr size_t capacity = 8192, max_payload = 4096, frame_size = 31;
  static constexpr uint8_t begin_kind = 1, end_kind = 12;
  bool active() const { return recording; }
  bool pending() const { return used != 0 || closing; }
  bool stopping() const { return closing; }
  size_t bufferedBytes() const { return used; }
  size_t highWaterBytes() const { return high_water; }
  uint32_t issuedRecords() const { return issued; }
  uint32_t droppedRecords() const { return dropped; }
  uint32_t shortWrites() const { return short_writes; }
  uint32_t exportedPackets() const { return packets; }

  // A previous session must finish draining before another Begin is admitted.
  bool begin(const uint8_t* payload, size_t length) {
    if (recording || pending() || length > max_payload) return false;
    issued = dropped = short_writes = packets = 0; high_water = 0;
    recording = true;
    return append(begin_kind, payload, length);
  }
  bool append(uint8_t kind, const uint8_t* payload, size_t length) {
    if (!recording) return false;
    const uint32_t sequence = issued++;
    if (length > max_payload || length + header_size > capacity - used) {
      ++dropped; return false;
    }
    put(kind); put(uint8_t(length)); put(uint8_t(length >> 8));
    for (unsigned shift = 0; shift < 32; shift += 8) put(uint8_t(sequence >> shift));
    for (size_t i = 0; i < length; ++i) put(payload[i]);
    if (used > high_water) high_water = used;
    return true;
  }
  // Stop collection immediately. End waits in its own small slot for queue space,
  // so overload cannot silently turn a requested complete session into a prefix.
  bool stop(const uint8_t* payload, size_t length) {
    if (!recording || length > sizeof(end_payload)) return false;
    if (length) memcpy(end_payload, payload, length);
    end_size = length;
    recording = false; closing = true; return true;
  }
  template<class Stream> void drain(Stream& stream) {
    if (closing && end_size + header_size <= capacity - used) {
      recording = true; append(end_kind, end_payload, end_size);
      recording = false; closing = false;
    }
    if (!used || stream.availableForWrite() < int(frame_size)) return;
    const bool first = record_remaining == 0;
    const size_t remaining = first ? header_size + size_t(peek(1)) + (size_t(peek(2)) << 8) : record_remaining;
    const size_t count = remaining < 10 ? remaining : 10;
    char frame[frame_size]; frame[0] = '\x1e'; frame[1] = first ? 'S' : 'M';
    hex(frame + 2, packet_sequence, 4);
    for (size_t i = 0; i < 10; ++i) hex(frame + 6 + i * 2, i < count ? peek(i) : 0, 2);
    hex(frame + 26, crc16(reinterpret_cast<const uint8_t*>(frame + 1), 25), 4);
    frame[30] = '\n';
    const size_t written = stream.write(reinterpret_cast<const uint8_t*>(frame), sizeof(frame));
    // Another producer can consume the observed queue slot before admission.
    // Zero bytes means no frame escaped; retain it for a later bounded attempt.
    if (!written) return;
    ++packet_sequence; ++packets;
    if (written != sizeof(frame)) ++short_writes;
    // A short write is lost export, never a retry of a partial frame. The next
    // start marker permits recovery; strict replay rejects the damaged session.
    tail = (tail + count) % capacity; used -= count; record_remaining = remaining - count;
  }
  static uint16_t crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < length; ++i) {
      crc ^= uint16_t(data[i]) << 8;
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = crc & 0x8000 ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return crc;
  }
private:
  static constexpr size_t header_size = 7;
  uint8_t bytes[capacity] = {}, end_payload[64] = {};
  size_t head = 0, tail = 0, used = 0, high_water = 0, record_remaining = 0, end_size = 0;
  uint32_t issued = 0, dropped = 0, short_writes = 0, packets = 0;
  uint16_t packet_sequence = 0;
  bool recording = false, closing = false;
  void put(uint8_t value) { bytes[head] = value; head = (head + 1) % capacity; ++used; }
  uint8_t peek(size_t offset) const { return bytes[(tail + offset) % capacity]; }
  static void hex(char* out, uint32_t value, unsigned width) {
    static const char digits[] = "0123456789abcdef";
    while (width) { out[--width] = digits[value & 15]; value >>= 4; }
  }
};
