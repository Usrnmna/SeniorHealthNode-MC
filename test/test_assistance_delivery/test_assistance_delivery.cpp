#include <gtest/gtest.h>
#include <Mesh.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/SimpleMeshTables.h>
#include "../../examples/simple_sensor/FallAckSender.h"
#include "../../examples/simple_sensor/MotionRule.h"
#include <deque>
#include <vector>
#include <string>

// Real packet creation, encryption, dispatch, Mesh receive/decryption and
// duplicate tables; only clock/radio transport and deterministic entropy are fakes.
struct ChannelClock : mesh::MillisecondClock, mesh::RTCClock {
  uint32_t now = 1000, epoch = 1700000000;
  explicit ChannelClock(uint32_t start = 1000) : now(start) {}
  unsigned long getMillis() override { return now; }
  uint32_t getCurrentTime() override { return epoch; }
  void setCurrentTime(uint32_t value) override { epoch = value; }
};

struct ChannelRng : mesh::RNG {
  uint32_t state = 17;
  void random(uint8_t* bytes, size_t len) override {
    while (len--) { state = state * 1664525U + 1013904223U; *bytes++ = state >> 24; }
  }
};

struct ChannelRadio : mesh::Radio {
  bool starts = true, completes = true, busy = false;
  unsigned attempts = 0;
  std::deque<std::vector<uint8_t>> inbound;
  std::vector<std::vector<uint8_t>> frames;
  int recvRaw(uint8_t* bytes, int capacity) override {
    if (inbound.empty()) return 0;
    auto frame = inbound.front(); inbound.pop_front();
    if (frame.size() > unsigned(capacity)) return 0;
    memcpy(bytes, frame.data(), frame.size());
    return frame.size();
  }
  uint32_t getEstAirtimeFor(int) override { return 100; }
  float packetScore(float, int) override { return 1; }
  bool startSendRaw(const uint8_t* bytes, int len) override {
    ++attempts;
    if (starts) frames.emplace_back(bytes, bytes + len);
    return starts;
  }
  bool isSendComplete() override { return completes; }
  void onSendFinished() override {}
  bool isInRecvMode() const override { return true; }
  bool isReceiving() override { return busy; }
};

class ChannelMesh : public mesh::Mesh, public AssistanceTransport {
public:
  AssistanceDelivery delivery{0}, other_delivery{1};
  ChannelClock& clock;
  bool forward = false;
  unsigned callbacks = 0, pre_dedup_floods = 0;
  unsigned long budget_window = 3600000;
  std::vector<std::vector<uint8_t>> plaintexts;
  std::vector<std::vector<uint8_t>> filtered_hashes;
  mesh::GroupChannel channel = {};
  ChannelMesh(ChannelRadio& radio, ChannelClock& ms, ChannelRng& rng,
              StaticPoolPacketManager& pool, SimpleMeshTables& tables)
      : Mesh(radio, ms, rng, ms, pool, tables), clock(ms) {
    self_id = mesh::LocalIdentity(&rng);
    memcpy(channel.secret, HealthNodeConfig::ack_channel_key, sizeof(HealthNodeConfig::ack_channel_key));
    mesh::Utils::sha256(channel.hash, sizeof(channel.hash), HealthNodeConfig::ack_channel_key,
                        sizeof(HealthNodeConfig::ack_channel_key));
    begin();
  }
  uint32_t uniqueTimestamp() override { return clock.getCurrentTimeUnique(); }
  Result queueAssistance(const uint8_t* bytes, unsigned len, uint32_t tag, uint8_t fingerprint[8]) override {
    return FallAckSender::queueGroup(*this, bytes, len, tag, fingerprint);
  }
protected:
  void onPacketTxComplete(uint32_t tag, bool sent) override {
    ++callbacks;
    delivery.txComplete(clock.now, tag, sent);
    other_delivery.txComplete(clock.now, tag, sent);
  }
  bool allowPacketForward(const mesh::Packet*) override { return forward; }
  uint32_t getRetransmitDelay(const mesh::Packet*) override { return 0; }
  int calcRxDelay(float, uint32_t) const override { return 0; }
  unsigned long getDutyCycleWindowMs() const override { return budget_window; }
  bool filterRecvFloodPacket(mesh::Packet* packet) override {
    ++pre_dedup_floods;
    uint8_t hash[MAX_HASH_SIZE];
    packet->calculatePacketHash(hash);
    filtered_hashes.emplace_back(hash, hash + sizeof(hash));
    for (auto* pending : {&delivery, &other_delivery}) {
      if (FallAckSender::observeRepeat(*packet, *pending)) cancelQueuedPacket(pending->queuedTag());
    }
    return false;
  }
  int searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel* result, int capacity) override {
    if (!capacity || memcmp(hash, channel.hash, sizeof(channel.hash))) return 0;
    result[0] = channel; return 1;
  }
  void onGroupDataRecv(mesh::Packet*, uint8_t type, const mesh::GroupChannel&,
                       uint8_t* bytes, size_t len) override {
    EXPECT_EQ(PAYLOAD_TYPE_GRP_TXT, type);
    plaintexts.emplace_back(bytes, bytes + len);
  }
};

struct ChannelNode {
  ChannelClock clock;
  ChannelRng rng;
  ChannelRadio radio;
  StaticPoolPacketManager pool{8};
  SimpleMeshTables tables;
  ChannelMesh mesh{radio, clock, rng, pool, tables};
  explicit ChannelNode(uint32_t start = 1000) : clock(start) {}
  void step(uint32_t elapsed = 20, bool service = true) {
    clock.now += elapsed;
    if (service) {
      mesh.delivery.poll(clock.now, mesh);
      mesh.other_delivery.poll(clock.now, mesh);
    }
    mesh.loop();
  }
};

class AssistanceChannel : public ::testing::Test {
protected:
  ChannelNode sender, receiver;
  std::vector<uint8_t> text;
  void SetUp() override {
    uint8_t data[FallAckMessage::max_payload_bytes];
    const unsigned len = FallAckMessage::encodeWithLocation(data, 0, "Node", HealthNodeConfig::ack_message,
        false, 0, 0, HealthNodeConfig::DefaultLocation);
    ASSERT_GT(len, 5u);
    text.assign(data, data + len);
  }
  void begin() { ASSERT_TRUE(sender.mesh.delivery.begin(sender.clock.now, sender.mesh, text.data(), text.size())); }
  void finishLocal() {
    sender.step(); // enqueue/start
    sender.step(); // radio completion
    ASSERT_EQ(AssistanceDelivery::Transmitted, sender.mesh.delivery.state());
  }
  void deliver(const std::vector<uint8_t>& frame) {
    receiver.radio.inbound.push_back(frame);
    receiver.step();
  }
};

TEST(ProductionCrypto, Sha256UsesRealImplementation) {
  const uint8_t input[] = {'a', 'b', 'c'};
  uint8_t hash[32];
  mesh::Utils::sha256(hash, sizeof(hash), input, sizeof(input));
  const uint8_t expected[] = {0xba, 0x78, 0x16, 0xbf};
  EXPECT_EQ(0, memcmp(hash, expected, sizeof(expected)));
}

TEST_F(AssistanceChannel, PublicCiphertextDecryptsToConfiguredTextOnRealMesh) {
  begin(); finishLocal();
  ASSERT_EQ(1u, sender.radio.frames.size());
  mesh::Packet frame;
  ASSERT_TRUE(frame.readFrom(sender.radio.frames[0].data(), sender.radio.frames[0].size()));
  EXPECT_EQ(PAYLOAD_TYPE_GRP_TXT, frame.getPayloadType());
  EXPECT_TRUE(frame.isRouteFlood());
  EXPECT_EQ(HealthNodeConfig::ack_path_hash_size, frame.getPathHashSize());
  deliver(sender.radio.frames[0]);
  ASSERT_EQ(1u, receiver.mesh.plaintexts.size());
  const auto& plain = receiver.mesh.plaintexts[0];
  EXPECT_EQ(0, memcmp(plain.data() + 5, text.data() + 5, text.size() - 5));
  EXPECT_EQ(0, plain[4]);
  uint32_t timestamp = 0; memcpy(&timestamp, plain.data(), 4);
  EXPECT_EQ(sender.clock.epoch, timestamp);
  EXPECT_STREQ("transmitted; waiting for repeat", sender.mesh.delivery.status());
  EXPECT_EQ(1u, sender.mesh.callbacks);
}

TEST_F(AssistanceChannel, StartFailureRetriesAndRecoversWithoutLosingMessage) {
  sender.radio.starts = false;
  begin(); sender.step();
  EXPECT_EQ(AssistanceDelivery::Pending, sender.mesh.delivery.state());
  sender.step(HealthNodeConfig::ack_retry_ms - 1);
  EXPECT_EQ(1u, sender.radio.attempts);
  sender.radio.starts = true;
  sender.step(1); sender.step();
  EXPECT_EQ(AssistanceDelivery::Transmitted, sender.mesh.delivery.state());
  EXPECT_EQ(2u, sender.radio.attempts);
  deliver(sender.radio.frames.back());
  EXPECT_EQ(1u, receiver.mesh.plaintexts.size());
}

TEST_F(AssistanceChannel, TimeoutRetriesWithFreshTimestampAndPreservesText) {
  sender.radio.completes = false;
  begin(); sender.step();
  ASSERT_EQ(1u, sender.radio.frames.size());
  deliver(sender.radio.frames[0]); // Radio timeout can happen after recipient heard the frame.
  sender.step(151);
  ASSERT_EQ(AssistanceDelivery::Pending, sender.mesh.delivery.state());
  sender.radio.completes = true;
  sender.step(HealthNodeConfig::ack_retry_ms); sender.step();
  ASSERT_EQ(AssistanceDelivery::Transmitted, sender.mesh.delivery.state());
  ASSERT_EQ(2u, sender.radio.frames.size());
  EXPECT_NE(sender.radio.frames[0], sender.radio.frames[1]);
  deliver(sender.radio.frames[1]);
  ASSERT_EQ(2u, receiver.mesh.plaintexts.size());
  EXPECT_EQ(0, memcmp(receiver.mesh.plaintexts[0].data() + 4,
                      receiver.mesh.plaintexts[1].data() + 4, text.size() - 4));
}

TEST_F(AssistanceChannel, LocalCompletionDoesNotClaimRecipientAcknowledgment) {
  begin(); finishLocal();
  // Deliberately drop every RF frame; local completion cannot detect this loss.
  sender.step(HealthNodeConfig::channel_repeat_wait_ms - 1);
  EXPECT_EQ(1u, sender.radio.frames.size());
  EXPECT_EQ(0u, receiver.mesh.plaintexts.size());
  EXPECT_STREQ("transmitted; waiting for repeat", sender.mesh.delivery.status());
}

TEST_F(AssistanceChannel, AllocationExhaustionRecoversWithoutDiscardingRequest) {
  std::vector<mesh::Packet*> held;
  while (auto* packet = sender.mesh.obtainNewPacket()) held.push_back(packet);
  begin(); sender.step();
  EXPECT_EQ(AssistanceDelivery::Pending, sender.mesh.delivery.state());
  EXPECT_EQ(1, sender.mesh.delivery.attemptCount());
  EXPECT_EQ(0u, sender.radio.attempts);
  for (auto* packet : held) sender.mesh.releasePacket(packet);
  sender.step(HealthNodeConfig::ack_retry_ms); sender.step();
  EXPECT_EQ(AssistanceDelivery::Transmitted, sender.mesh.delivery.state());
  EXPECT_EQ(8, sender.pool.getFreeCount());
}

TEST_F(AssistanceChannel, AllocationFailuresStopAtConfiguredAttemptLimit) {
  std::vector<mesh::Packet*> held;
  while (auto* packet = sender.mesh.obtainNewPacket()) held.push_back(packet);
  begin();
  for (unsigned i = 0; i < HealthNodeConfig::ack_max_attempts; ++i) sender.step(HealthNodeConfig::ack_retry_ms);
  EXPECT_EQ(AssistanceDelivery::Failed, sender.mesh.delivery.state());
  EXPECT_EQ(HealthNodeConfig::ack_max_attempts, sender.mesh.delivery.attemptCount());
  sender.step(HealthNodeConfig::ack_retry_ms);
  EXPECT_EQ(HealthNodeConfig::ack_max_attempts, sender.mesh.delivery.attemptCount());
  EXPECT_STREQ("failed: retries exhausted", sender.mesh.delivery.status());
  for (auto* packet : held) sender.mesh.releasePacket(packet);
}

TEST_F(AssistanceChannel, RadioFailuresStopAtConfiguredAttemptLimit) {
  sender.radio.starts = false;
  begin();
  for (unsigned i = 0; i < HealthNodeConfig::ack_max_attempts; ++i) sender.step(HealthNodeConfig::ack_retry_ms);
  EXPECT_EQ(AssistanceDelivery::Failed, sender.mesh.delivery.state());
  EXPECT_EQ(HealthNodeConfig::ack_max_attempts, sender.radio.attempts);
  EXPECT_EQ(AssistanceDelivery::AttemptsExhausted, sender.mesh.delivery.failure());
  EXPECT_EQ(8, sender.pool.getFreeCount());
}

TEST_F(AssistanceChannel, CadBlockedQueueExpiresAndLaterRecovers) {
  sender.radio.busy = true;
  begin(); sender.step();
  ASSERT_EQ(AssistanceDelivery::Queued, sender.mesh.delivery.state());
  sender.step(HealthNodeConfig::assistance_queue_timeout_ms);
  EXPECT_EQ(AssistanceDelivery::Pending, sender.mesh.delivery.state());
  EXPECT_EQ(0u, sender.radio.attempts);
  sender.radio.busy = false;
  sender.step(HealthNodeConfig::ack_retry_ms); sender.step();
  EXPECT_EQ(AssistanceDelivery::Transmitted, sender.mesh.delivery.state());
}

TEST_F(AssistanceChannel, BudgetBlockedQueueExpiresWithoutUnboundedQueueRetention) {
  sender.mesh.budget_window = 2;
  sender.mesh.begin();
  begin(); sender.step();
  ASSERT_EQ(AssistanceDelivery::Queued, sender.mesh.delivery.state());
  sender.step(HealthNodeConfig::assistance_queue_timeout_ms);
  EXPECT_EQ(AssistanceDelivery::Pending, sender.mesh.delivery.state());
  EXPECT_EQ(0, sender.pool.getOutboundTotal());
  EXPECT_EQ(8, sender.pool.getFreeCount());
}

TEST_F(AssistanceChannel, TwoMessageOwnersUseIndependentCallbacksAndTimestamps) {
  begin();
  ASSERT_TRUE(sender.mesh.other_delivery.begin(sender.clock.now, sender.mesh, text.data(), text.size()));
  for (unsigned i = 0; i < 5; ++i) sender.step();
  EXPECT_EQ(AssistanceDelivery::Transmitted, sender.mesh.delivery.state());
  EXPECT_EQ(AssistanceDelivery::Transmitted, sender.mesh.other_delivery.state());
  ASSERT_EQ(2u, sender.radio.frames.size());
  EXPECT_NE(sender.radio.frames[0], sender.radio.frames[1]);
  for (const auto& frame : sender.radio.frames) deliver(frame);
  EXPECT_EQ(2u, receiver.mesh.plaintexts.size());
}

TEST_F(AssistanceChannel, RepeatedEventCannotResetPendingRetryBudget) {
  sender.radio.starts = false;
  begin(); sender.step();
  EXPECT_FALSE(sender.mesh.delivery.begin(sender.clock.now, sender.mesh, text.data(), text.size()));
  EXPECT_EQ(1, sender.mesh.delivery.attemptCount());
  EXPECT_EQ(AssistanceDelivery::Pending, sender.mesh.delivery.state());
}

TEST_F(AssistanceChannel, InvalidPayloadIsVisibleAndNeverQueued) {
  EXPECT_TRUE(sender.mesh.delivery.begin(sender.clock.now, sender.mesh, text.data(), 4));
  sender.step();
  EXPECT_EQ(AssistanceDelivery::Failed, sender.mesh.delivery.state());
  EXPECT_EQ(AssistanceDelivery::InvalidText, sender.mesh.delivery.failure());
  uint8_t fingerprint[8];
  EXPECT_EQ(AssistanceTransport::InvalidPayload, FallAckSender::queueGroup(sender.mesh, text.data(), 4, 2, fingerprint));
  EXPECT_EQ(0u, sender.radio.attempts);
  EXPECT_EQ(8, sender.pool.getFreeCount());
}

TEST_F(AssistanceChannel, RetryTimingSurvivesMillisRollover) {
  ChannelNode wrapping(UINT32_MAX - 100);
  wrapping.radio.starts = false;
  ASSERT_TRUE(wrapping.mesh.delivery.begin(wrapping.clock.now, wrapping.mesh, text.data(), text.size()));
  wrapping.step();
  ASSERT_EQ(1u, wrapping.radio.attempts);
  wrapping.radio.starts = true;
  wrapping.step(HealthNodeConfig::ack_retry_ms - 1);
  EXPECT_EQ(1u, wrapping.radio.attempts);
  wrapping.step(1); wrapping.step();
  EXPECT_EQ(AssistanceDelivery::Transmitted, wrapping.mesh.delivery.state());
}

TEST_F(AssistanceChannel, MotionRuleContinuesToDetectDuringRadioFailures) {
  MotionRule motion;
  unsigned detected = 0, samples = 0;
  sender.radio.starts = false;
  begin();
  auto sample = [&](float acceleration, float rotation) {
    sender.step(20);
    if (motion.update(sender.clock.now, acceleration, rotation, true)) ++detected;
    ++samples;
  };
  for (unsigned i = 0; i < 101; ++i) sample(1, 0);
  sample(0.3f, 0); sample(3, 100); sample(1, 300);
  for (unsigned i = 0; i < 151; ++i) sample(1, 0);
  EXPECT_EQ(1u, detected);
  EXPECT_EQ(255u, samples);
  EXPECT_GE(sender.radio.attempts, 2u);
  EXPECT_EQ(AssistanceDelivery::Pending, sender.mesh.delivery.state());
}

TEST_F(AssistanceChannel, RealRepeaterEchoPreservesPayloadHashAndReachesPreDedupHook) {
  receiver.mesh.forward = true;
  begin(); finishLocal();
  deliver(sender.radio.frames[0]);
  receiver.step();
  ASSERT_EQ(1u, receiver.radio.frames.size());
  mesh::Packet original, echo;
  ASSERT_TRUE(original.readFrom(sender.radio.frames[0].data(), sender.radio.frames[0].size()));
  ASSERT_TRUE(echo.readFrom(receiver.radio.frames[0].data(), receiver.radio.frames[0].size()));
  EXPECT_EQ(1, echo.getPathHashCount());
  uint8_t original_hash[MAX_HASH_SIZE], echo_hash[MAX_HASH_SIZE];
  original.calculatePacketHash(original_hash); echo.calculatePacketHash(echo_hash);
  EXPECT_EQ(0, memcmp(original_hash, echo_hash, sizeof(original_hash)));
  sender.radio.inbound.push_back(receiver.radio.frames[0]); sender.step();
  EXPECT_EQ(1u, sender.mesh.pre_dedup_floods);
  EXPECT_TRUE(sender.mesh.plaintexts.empty()); // Already seen at sendFlood.
  EXPECT_GT(sender.tables.getNumFloodDups(), 0u);
  EXPECT_EQ(std::vector<uint8_t>(original_hash, original_hash + MAX_HASH_SIZE), sender.mesh.filtered_hashes[0]);
  EXPECT_EQ(AssistanceDelivery::Repeated, sender.mesh.delivery.state());
  EXPECT_STREQ("repeater echo heard", sender.mesh.delivery.status());
}

TEST_F(AssistanceChannel, LostRepeatsTriggerFreshBroadcastsThenFiniteUnconfirmedState) {
  begin(); finishLocal();
  for (unsigned attempt = 1; attempt < HealthNodeConfig::channel_transmit_attempts; ++attempt) {
    sender.step(HealthNodeConfig::channel_repeat_wait_ms);
    sender.step();
    ASSERT_EQ(AssistanceDelivery::Transmitted, sender.mesh.delivery.state());
  }
  EXPECT_EQ(HealthNodeConfig::channel_transmit_attempts, sender.radio.frames.size());
  sender.step(HealthNodeConfig::channel_repeat_wait_ms);
  EXPECT_EQ(AssistanceDelivery::Failed, sender.mesh.delivery.state());
  EXPECT_EQ(AssistanceDelivery::NoRepeat, sender.mesh.delivery.failure());
  EXPECT_STREQ("unconfirmed: no repeat heard", sender.mesh.delivery.status());
  sender.step(HealthNodeConfig::channel_repeat_wait_ms);
  EXPECT_EQ(HealthNodeConfig::channel_transmit_attempts, sender.radio.frames.size());
  for (unsigned i = 1; i < sender.radio.frames.size(); ++i) EXPECT_NE(sender.radio.frames[i-1], sender.radio.frames[i]);
}

TEST_F(AssistanceChannel, LateEchoCancelsQueuedRetryAndCannotBeUndoneByFailureCallback) {
  receiver.mesh.forward = true;
  begin(); finishLocal();
  deliver(sender.radio.frames[0]); receiver.step();
  ASSERT_EQ(1u, receiver.radio.frames.size());
  sender.radio.busy = true;
  sender.step(HealthNodeConfig::channel_repeat_wait_ms);
  ASSERT_EQ(AssistanceDelivery::Queued, sender.mesh.delivery.state());
  ASSERT_EQ(1, sender.pool.getOutboundTotal());
  sender.radio.inbound.push_back(receiver.radio.frames[0]); sender.step();
  EXPECT_EQ(AssistanceDelivery::Repeated, sender.mesh.delivery.state());
  EXPECT_EQ(0u, sender.mesh.delivery.queuedTag());
  EXPECT_FALSE(sender.mesh.delivery.busy());
  EXPECT_EQ(0, sender.pool.getOutboundTotal());
  EXPECT_EQ(1u, sender.radio.frames.size());
  EXPECT_EQ(2u, sender.mesh.callbacks); // Successful original + cancelled retry.
  sender.radio.busy = false; sender.step(HealthNodeConfig::channel_repeat_wait_ms);
  EXPECT_EQ(1u, sender.radio.frames.size());
}

TEST_F(AssistanceChannel, LateEchoFromFirstAttemptConfirmsAfterRetryExhaustion) {
  receiver.mesh.forward = true;
  begin(); finishLocal();
  deliver(sender.radio.frames[0]); receiver.step();
  ASSERT_EQ(1u, receiver.radio.frames.size());
  for (unsigned i = 1; i < HealthNodeConfig::channel_transmit_attempts; ++i) {
    sender.step(HealthNodeConfig::channel_repeat_wait_ms); sender.step();
  }
  sender.step(HealthNodeConfig::channel_repeat_wait_ms);
  ASSERT_EQ(AssistanceDelivery::Failed, sender.mesh.delivery.state());
  sender.radio.inbound.push_back(receiver.radio.frames[0]); sender.step();
  EXPECT_EQ(AssistanceDelivery::Repeated, sender.mesh.delivery.state());
  EXPECT_EQ(AssistanceDelivery::None, sender.mesh.delivery.failure());
}

TEST_F(AssistanceChannel, WrongPayloadTypeRouteAndZeroHopCannotConfirmRepeat) {
  begin(); finishLocal();
  mesh::Packet packet;
  ASSERT_TRUE(packet.readFrom(sender.radio.frames[0].data(), sender.radio.frames[0].size()));
  EXPECT_FALSE(FallAckSender::observeRepeat(packet, sender.mesh.delivery)); // Zero hops.
  packet.setPathHashCount(1); packet.path[0] = 0xAB;
  const uint8_t header = packet.header;
  packet.header = (packet.header & ~PH_ROUTE_MASK) | ROUTE_TYPE_DIRECT;
  EXPECT_FALSE(FallAckSender::observeRepeat(packet, sender.mesh.delivery));
  packet.header = (PAYLOAD_TYPE_GRP_DATA << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD;
  EXPECT_FALSE(FallAckSender::observeRepeat(packet, sender.mesh.delivery));
  packet.header = header;
  packet.payload[packet.payload_len - 1] ^= 1;
  EXPECT_FALSE(FallAckSender::observeRepeat(packet, sender.mesh.delivery));
  EXPECT_EQ(AssistanceDelivery::Transmitted, sender.mesh.delivery.state());
}

TEST_F(AssistanceChannel, DuplicateEchoIsIdempotentAndDoesNotQueueMoreMessages) {
  receiver.mesh.forward = true;
  begin(); finishLocal(); deliver(sender.radio.frames[0]); receiver.step();
  ASSERT_EQ(1u, receiver.radio.frames.size());
  for (unsigned i = 0; i < 3; ++i) {
    sender.radio.inbound.push_back(receiver.radio.frames[0]); sender.step();
    EXPECT_EQ(AssistanceDelivery::Repeated, sender.mesh.delivery.state());
  }
  EXPECT_EQ(1u, sender.radio.frames.size());
  EXPECT_EQ(1u, sender.mesh.callbacks);
}

TEST_F(AssistanceChannel, EchoOnlyConfirmsItsOwnMessageOwner) {
  receiver.mesh.forward = true;
  begin();
  ASSERT_TRUE(sender.mesh.other_delivery.begin(sender.clock.now, sender.mesh, text.data(), text.size()));
  for (unsigned i = 0; i < 5; ++i) sender.step();
  ASSERT_EQ(2u, sender.radio.frames.size());
  deliver(sender.radio.frames[0]); receiver.step();
  ASSERT_EQ(1u, receiver.radio.frames.size());
  sender.radio.inbound.push_back(receiver.radio.frames[0]); sender.step();
  EXPECT_EQ(AssistanceDelivery::Repeated, sender.mesh.delivery.state());
  EXPECT_EQ(AssistanceDelivery::Transmitted, sender.mesh.other_delivery.state());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
