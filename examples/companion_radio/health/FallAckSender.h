#pragma once
#include <Mesh.h>
#include <HealthNodeConfig.h>
#include "AssistanceDelivery.h"

// Public channel packet creation shared by firmware and native transport tests.
// Queued transfers ownership to Dispatcher; it never means delivered.
namespace FallAckSender {
// Called from Mesh's pre-dedup flood hook. A nonzero route shows forwarding;
// exact encrypted-payload hash matching ties the repeat to our actual message.
inline bool observeRepeat(const mesh::Packet& packet, AssistanceDelivery& delivery) {
  if (!packet.isRouteFlood() || packet.getPayloadType() != PAYLOAD_TYPE_GRP_TXT ||
      packet.getPathHashCount() == 0) return false;
  uint8_t fingerprint[8]; packet.calculatePacketHash(fingerprint);
  return delivery.confirmRepeat(fingerprint);
}
inline AssistanceTransport::Result queueGroup(mesh::Mesh& network, const uint8_t* payload,
                                             unsigned len, uint32_t tag, uint8_t fingerprint[8]) {
  using namespace HealthNodeConfig;
  if (len < 5 || len > FallAckMessage::max_payload_bytes) return AssistanceTransport::InvalidPayload;
  mesh::GroupChannel channel = {};
  memcpy(channel.secret, ack_channel_key, sizeof(ack_channel_key));
  mesh::Utils::sha256(channel.hash, sizeof(channel.hash), ack_channel_key, sizeof(ack_channel_key));
  auto packet = network.createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, channel, payload, len);
  if (!packet) return AssistanceTransport::NoPacket;
  packet->calculatePacketHash(fingerprint);
  packet->tx_tag = tag;
  packet->tx_deadline = network.futureMillis(assistance_queue_timeout_ms);
  network.sendFlood(packet, uint32_t(0), ack_path_hash_size);
  return AssistanceTransport::Queued;
}
}
