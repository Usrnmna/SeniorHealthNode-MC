#pragma once
#include <Mesh.h>
#include <HealthNodeConfig.h>
#include <helpers/SensorManager.h>
#include "FallAckMessage.h"
#include "FallAckLocation.h"

// Call only after FallResponse accepts a post-fall triple click. The standard
// MeshCore group text format is compatible with companion channel messages.
namespace FallAckSender {
enum Result { Queued, NoPacket, InvalidText };
inline Result send(mesh::Mesh& network, SensorManager& sensors, const char* sender_name) {
  using namespace HealthNodeConfig;
  double latitude, longitude;
  const bool has_fix = FallAckLocation::read(sensors, latitude, longitude);
  uint8_t payload[FallAckMessage::max_payload_bytes];
  const unsigned len = FallAckMessage::encodeWithLocation(payload, network.getRTCClock()->getCurrentTimeUnique(),
                          sender_name, ack_message, has_fix, latitude, longitude, DefaultLocation);
  if (!len) return InvalidText;
  mesh::GroupChannel channel = {};
  memcpy(channel.secret, ack_channel_key, sizeof(ack_channel_key));
  mesh::Utils::sha256(channel.hash, sizeof(channel.hash), ack_channel_key, sizeof(ack_channel_key));
  auto packet = network.createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, channel, payload, len);
  if (!packet) return NoPacket;
  network.sendFlood(packet, uint32_t(0), ack_path_hash_size);
  return Queued; // Queued for RF transmission, NOT proof of recipient delivery.
}
}
