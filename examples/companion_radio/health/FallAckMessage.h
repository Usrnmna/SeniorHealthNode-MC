#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// Fall-channel body: little-endian time, plain-text flag, message text only.
// Omit the node name and its separator from every fall notification.
// Reject oversize text instead of cutting through a UTF-8 code point.
namespace FallAckMessage {
constexpr unsigned max_text_bytes = 160; // BaseChatMesh's MAX_TEXT_LEN.
constexpr unsigned max_payload_bytes = 5 + max_text_bytes;
inline bool validCoordinates(double latitude, double longitude) {
  return isfinite(latitude) && isfinite(longitude) &&
         latitude >= -90.0 && latitude <= 90.0 && longitude >= -180.0 && longitude <= 180.0;
}
inline unsigned encode(uint8_t* dest, uint32_t timestamp, const char* text) {
  const unsigned text_len = strlen(text);
  if (text_len > max_text_bytes) return 0;
  for (unsigned i = 0; i < 4; ++i) dest[i] = uint8_t(timestamp >> (8 * i));
  dest[4] = 0; // TXT_TYPE_PLAIN
  memcpy(dest + 5, text, text_len);
  return 5 + text_len;
}

// Append to a fresh buffer each attempt; never modify the configured UTF-8 string.
inline unsigned encodeWithLocation(uint8_t* dest, uint32_t timestamp,
                                   const char* text, bool has_fix, double latitude,
                                   double longitude, const char* fallback) {
  char message[max_text_bytes + 1];
  const int size = has_fix && validCoordinates(latitude, longitude)
    ? snprintf(message, sizeof(message), "%s | lat=%.6f, lon=%.6f", text, latitude, longitude)
    : snprintf(message, sizeof(message), "%s | %s", text, fallback);
  if (size < 0 || unsigned(size) >= sizeof(message)) return 0;
  return encode(dest, timestamp, message); // Includes the location suffix in the limit.
}
}
