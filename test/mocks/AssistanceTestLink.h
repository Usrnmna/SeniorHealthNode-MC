#pragma once
#include "../../examples/simple_sensor/AssistanceDelivery.h"

// State-machine-only fake. Real packets/radio failures are covered separately by
// test_assistance_delivery using production Mesh/Dispatcher and actual Crypto.
struct AssistanceTestLink : AssistanceTransport {
  Result result = NoPacket;
  uint32_t tag = 0, timestamp = 0;
  uint8_t fingerprint[8] = {};
  uint32_t uniqueTimestamp() override { return ++timestamp; }
  Result queueAssistance(const uint8_t*, unsigned, uint32_t t, uint8_t hash[8]) override {
    tag = t; memcpy(fingerprint, &tag, sizeof(tag)); memcpy(hash, fingerprint, 8); return result;
  }
  void start(AssistanceDelivery& d, uint32_t now) {
    uint8_t payload[FallAckMessage::max_payload_bytes];
    const unsigned len = FallAckMessage::encode(payload, 0, "Node", "Help");
    d.begin(now, *this, payload, len);
  }
};
