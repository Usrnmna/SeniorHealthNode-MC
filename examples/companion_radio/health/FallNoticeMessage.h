#pragma once
#include "FallAckMessage.h"
#include "FallNotice.h"

// Format an event once; the outbox retains these bytes for all local retries.
namespace FallNoticeMessage {
struct Bundle {
  uint8_t payload[2][FallAckMessage::max_payload_bytes] = {};
  unsigned length[2] = {};
  unsigned count = 0;
};

// Separators belong to composition, never to the stored warning text.
inline bool join(char* dest, unsigned capacity, const char* base, const char* extra) {
  const int n = snprintf(dest, capacity, "%s%s%s", base,
      base[0] && extra[0] ? " | " : "", extra);
  return n >= 0 && unsigned(n) < capacity;
}

inline unsigned labelled(uint8_t* dest, const char* label, const char* text) {
  char part[FallAckMessage::max_text_bytes + 1];
  const int n = snprintf(part, sizeof(part), "%s%s", label, text);
  return n >= 0 && unsigned(n) < sizeof(part) ? FallAckMessage::encode(dest, 0, part) : 0;
}

// Automatic warnings append to the existing assistance+location when it fits.
// Otherwise send the complete warning first, then assistance+location, labelled
// [1/2] and [2/2]. Never truncate UTF-8 or change the MeshCore payload limit.
// Fall confirmation is standalone; false-alarm confirmation includes location.
inline Bundle build(FallNotice notice, bool fix, double latitude,
                    double longitude, const char* fallback = HealthNodeConfig::DefaultLocation) {
  Bundle result;
  if (notice == FallNotice::None) return result;
  if (!isAutomaticWarning(notice)) {
    result.length[0] = notice == FallNotice::UserConfirmedFall
        ? FallAckMessage::encode(result.payload[0], 0, fallNoticeText(notice))
        : FallAckMessage::encodeWithLocation(result.payload[0], 0,
              fallNoticeText(notice), fix, latitude, longitude, fallback);
    result.count = result.length[0] ? 1 : 0;
    return result;
  }
  char location[FallAckMessage::max_text_bytes + 1];
  const int n = fix && FallAckMessage::validCoordinates(latitude, longitude)
      ? snprintf(location, sizeof(location), "lat=%.6f, lon=%.6f", latitude, longitude)
      : snprintf(location, sizeof(location), "%s", fallback);
  char base[FallAckMessage::max_text_bytes + 1], combined[FallAckMessage::max_text_bytes + 1];
  if (n < 0 || unsigned(n) >= sizeof(location) ||
      !join(base, sizeof(base), HealthNodeConfig::ack_message, location)) return result;
  if (join(combined, sizeof(combined), base, fallNoticeText(notice)))
    result.length[0] = FallAckMessage::encode(result.payload[0], 0, combined);
  if (result.length[0]) { result.count = 1; return result; }
  result.length[0] = labelled(result.payload[0], "[1/2] ", fallNoticeText(notice));
  result.length[1] = labelled(result.payload[1], "[2/2] ", base);
  if (result.length[0] && result.length[1]) result.count = 2;
  return result; // count=0 is a visible invalid-text failure; no partial event is sent.
}
}
