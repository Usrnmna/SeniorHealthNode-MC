#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// One pending command reply, not a sample history. No heap allocation or waits.
class UsbReply {
  char bytes[2048] = {};
  size_t length = 0, offset = 0;
public:
  bool pending() const { return offset < length; }
  void clear() { length = offset = 0; bytes[0] = 0; }
  void append(const char* format, ...) {
    // Input parsing and health messages continue during a partial drain. Reuse
    // the consumed prefix before appending instead of reporting false overflow.
    if (offset) {
      length -= offset; memmove(bytes, bytes + offset, length);
      offset = 0; bytes[length] = 0;
    }
    va_list args; va_start(args, format);
    const int written = vsnprintf(bytes + length, sizeof(bytes) - length, format, args);
    va_end(args);
    if (written < 0) return;
    if (size_t(written) >= sizeof(bytes) - length) {
      const char error[] = "\nError: reply capacity exceeded\n";
      length = sizeof(bytes) - sizeof(error);
      memcpy(bytes + length, error, sizeof(error)); length += sizeof(error) - 1;
    } else length += written;
  }
  // The stream must report immediate capacity. Each call writes at most 32 bytes;
  // a disconnected or stalled host cannot make this drain wait for buffer space.
  template<class Stream> void drain(Stream& stream) {
    if (!pending()) return;
    const int capacity = stream.availableForWrite();
    if (capacity <= 0) return;
    size_t count = length - offset;
    if (count > 32) count = 32;
    if (count > size_t(capacity)) count = capacity;
    offset += stream.write(reinterpret_cast<const uint8_t*>(bytes + offset), count);
    if (!pending()) clear();
  }
};
