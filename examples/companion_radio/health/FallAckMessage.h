#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// MeshCore group text body: little-endian time, plain-text flag, "sender: text".
// Reject oversize text instead of cutting through a UTF-8 code point.
namespace FallAckMessage {
constexpr unsigned max_text_bytes = 160; // BaseChatMesh's MAX_TEXT_LEN.
constexpr unsigned max_payload_bytes = 5 + max_text_bytes;
inline bool validCoordinates(double latitude, double longitude) {
  return isfinite(latitude) && isfinite(longitude) &&
         latitude >= -90.0 && latitude <= 90.0 && longitude >= -180.0 && longitude <= 180.0;
}
inline unsigned encode(uint8_t* dest, uint32_t timestamp, const char* sender, const char* text) {
  const unsigned sender_len = strlen(sender), text_len = strlen(text);
  if (sender_len + 2 + text_len > max_text_bytes) return 0;
  for (unsigned i = 0; i < 4; ++i) dest[i] = uint8_t(timestamp >> (8 * i));
  dest[4] = 0; // TXT_TYPE_PLAIN
  memcpy(dest + 5, sender, sender_len);
  memcpy(dest + 5 + sender_len, ": ", 2);
  memcpy(dest + 7 + sender_len, text, text_len);
  return 7 + sender_len + text_len;
}

// Append to a fresh buffer each attempt; never modify the configured UTF-8 string.
inline unsigned encodeWithLocation(uint8_t* dest, uint32_t timestamp, const char* sender,
                                   const char* text, bool has_fix, double latitude,
                                   double longitude, const char* fallback) {
  char message[max_text_bytes + 1];
  const int size = has_fix && validCoordinates(latitude, longitude)
    ? snprintf(message, sizeof(message), "%s | lat=%.6f, lon=%.6f", text, latitude, longitude)
    : snprintf(message, sizeof(message), "%s | %s", text, fallback);
  if (size < 0 || unsigned(size) >= sizeof(message)) return 0;
  return encode(dest, timestamp, sender, message); // Includes sender prefix in the limit.
}
}
