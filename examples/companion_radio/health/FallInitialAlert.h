#pragma once
#include "AssistanceDelivery.h"

// One waiting initial alert beside the transport's active immutable event.
// Repeated observations of one incident cannot reset its retry budget. If several
// new incidents outpace delivery, retain the newest and expose the coalesced count.
class FallInitialAlert {
public:
  bool enqueue(uint32_t incident, const FallChannel& channel, const char* text) {
    if (!incident || incident == last_incident) return false;
    uint8_t encoded[FallAckMessage::max_payload_bytes];
    const unsigned size = FallAckMessage::encode(encoded, 0, text);
    if (!size || !channel.valid()) return false;
    if (waiting) ++coalesced;
    memcpy(payload, encoded, size); length=size; destination=channel;
    last_incident=pending_incident=incident; waiting=true; return true;
  }
  void cancel(uint32_t incident) { if (waiting && incident==pending_incident) waiting=false; }
  void poll(uint32_t now, AssistanceTransport& link, AssistanceDelivery& delivery) {
    if (waiting && !delivery.busy() &&
        delivery.begin(now,link,payload,length,destination,pending_incident)) waiting=false;
  }
  bool pending() const { return waiting; }
  uint32_t coalescedCount() const { return coalesced; }
private:
  uint8_t payload[FallAckMessage::max_payload_bytes] = {};
  unsigned length=0;
  uint32_t last_incident=0, pending_incident=0, coalesced=0;
  FallChannel destination;
  bool waiting=false;
};
