#include <gtest/gtest.h>
#include <Dispatcher.h>
#include <helpers/StaticPoolPacketManager.h>
#include <vector>
#include <utility>
#include <cstring>

// Exercise the production dispatcher, queue, pool and serialization. Only the
// hardware radio and millisecond clock are fakes; no RF delivery is asserted.
class DeliveryClock : public mesh::MillisecondClock {
public:
  uint32_t now = 1000;
  unsigned long getMillis() override { return now; }
};

class DeliveryRadio : public mesh::Radio {
public:
  bool starts = true, completes = false, busy = false;
  unsigned attempts = 0, finishes = 0;
  std::vector<uint8_t> received, transmitted;
  int recvRaw(uint8_t* bytes, int max_len) override {
    if (received.empty()) return 0;
    const int len = static_cast<int>(received.size());
    if (len > max_len) return 0;
    std::memcpy(bytes, received.data(), len);
    received.clear();
    return len;
  }
  uint32_t getEstAirtimeFor(int) override { return 100; }
  float packetScore(float, int) override { return 1.0f; }
  bool startSendRaw(const uint8_t* bytes, int len) override {
    ++attempts;
    transmitted.assign(bytes, bytes + len);
    return starts;
  }
  bool isSendComplete() override { return completes; }
  void onSendFinished() override { ++finishes; }
  bool isInRecvMode() const override { return true; }
  bool isReceiving() override { return busy; }
};

class DeliveryDispatcher : public mesh::Dispatcher {
public:
  std::vector<std::pair<uint32_t, bool>> outcomes;
  unsigned receives = 0;
  bool inbound_untracked = true, callback_tag_cleared = true;
  unsigned long budget_window = 3600000;
  mesh::Packet* observed = nullptr;
  DeliveryDispatcher(DeliveryRadio& radio, DeliveryClock& clock, StaticPoolPacketManager& pool)
      : Dispatcher(radio, clock, pool) {}
protected:
  mesh::DispatcherAction onRecvPacket(mesh::Packet* packet) override {
    ++receives;
    inbound_untracked = packet->tx_tag == 0 && packet->tx_deadline == 0;
    return ACTION_RELEASE;
  }
  void onPacketTxComplete(uint32_t tag, bool sent) override {
    outcomes.emplace_back(tag, sent);
    if (observed) callback_tag_cleared &= observed->tx_tag == 0;
  }
  unsigned long getDutyCycleWindowMs() const override { return budget_window; }
};

class DispatcherDelivery : public ::testing::Test {
protected:
  DeliveryClock clock;
  DeliveryRadio radio;
  StaticPoolPacketManager pool{4};
  DeliveryDispatcher dispatcher{radio, clock, pool};
  void SetUp() override { dispatcher.begin(); }
  mesh::Packet* packet(uint32_t tag = 42, uint32_t lifetime = 1000) {
    auto* p = dispatcher.obtainNewPacket();
    EXPECT_NE(nullptr, p);
    if (!p) return nullptr;
    p->header = ROUTE_TYPE_FLOOD | (PAYLOAD_TYPE_GRP_TXT << PH_TYPE_SHIFT);
    p->payload[0] = 0xA5;
    p->payload_len = 1;
    p->tx_tag = tag;
    p->tx_deadline = clock.now + lifetime;
    return p;
  }
  void start(mesh::Packet* p) {
    dispatcher.sendPacket(p, 0);
    ++clock.now; // Existing dispatcher schedules strictly after next_tx_time.
    dispatcher.loop();
  }
  void expectOutcome(bool sent, uint32_t tag = 42) {
    ASSERT_EQ(1u, dispatcher.outcomes.size());
    EXPECT_EQ(tag, dispatcher.outcomes[0].first);
    EXPECT_EQ(sent, dispatcher.outcomes[0].second);
    dispatcher.loop();
    EXPECT_EQ(1u, dispatcher.outcomes.size());
  }
};

TEST_F(DispatcherDelivery, SuccessReportsExactlyOnceAndClearsBeforeCallback) {
  dispatcher.observed = packet();
  start(dispatcher.observed);
  EXPECT_TRUE(dispatcher.outcomes.empty());
  radio.completes = true;
  clock.now += 20;
  dispatcher.loop();
  expectOutcome(true);
  EXPECT_TRUE(dispatcher.callback_tag_cleared);
  EXPECT_EQ(4, pool.getFreeCount());
}

TEST_F(DispatcherDelivery, StartFailureReportsFailureAndReturnsPacket) {
  radio.starts = false;
  start(packet());
  expectOutcome(false);
  EXPECT_EQ(1u, radio.attempts);
  EXPECT_EQ(4, pool.getFreeCount());
}

TEST_F(DispatcherDelivery, TimeoutReportsFailureAndCleansUpRadio) {
  start(packet());
  clock.now += 151;
  dispatcher.loop();
  expectOutcome(false);
  EXPECT_EQ(1u, radio.finishes);
  EXPECT_EQ(4, pool.getFreeCount());
}

TEST_F(DispatcherDelivery, InvalidPacketReportsFailure) {
  auto* p = packet();
  p->payload_len = MAX_PACKET_PAYLOAD + 1;
  dispatcher.sendPacket(p, 0);
  expectOutcome(false);
  EXPECT_EQ(0u, radio.attempts);
}

TEST_F(DispatcherDelivery, InvalidQueuedPacketReportsFailure) {
  auto* p = packet();
  p->payload_len = MAX_TRANS_UNIT;
  pool.queueOutbound(p, 0, clock.now);
  ++clock.now;
  dispatcher.loop();
  expectOutcome(false);
  EXPECT_EQ(0u, radio.attempts);
}

TEST_F(DispatcherDelivery, CadBlockedPacketExpiresAtExactDeadline) {
  radio.busy = true;
  start(packet(42, 100));
  EXPECT_TRUE(dispatcher.outcomes.empty());
  clock.now = 1100;
  dispatcher.loop();
  expectOutcome(false);
  EXPECT_EQ(0u, radio.attempts);
  EXPECT_EQ(0, pool.getOutboundTotal());
}

TEST_F(DispatcherDelivery, BudgetBlockedPacketExpiresWithoutSending) {
  dispatcher.budget_window = 2; // One ms budget cannot fund a 100 ms packet.
  dispatcher.begin();
  start(packet(42, 100));
  clock.now = 1100;
  dispatcher.loop();
  expectOutcome(false);
  EXPECT_EQ(0u, radio.attempts);
}

TEST_F(DispatcherDelivery, FutureScheduledPacketStillExpires) {
  dispatcher.sendPacket(packet(42, 100), 0, 10000);
  clock.now = 1100;
  dispatcher.loop();
  expectOutcome(false);
  EXPECT_EQ(0u, radio.attempts);
}

TEST_F(DispatcherDelivery, QueueDeadlineDoesNotAbortInFlightPacket) {
  start(packet(42, 10));
  clock.now += 20;
  dispatcher.loop();
  EXPECT_TRUE(dispatcher.outcomes.empty());
  EXPECT_EQ(0u, radio.finishes);
  radio.completes = true;
  dispatcher.loop();
  expectOutcome(true);
}

TEST_F(DispatcherDelivery, QueuedPacketExpiresWhileAnotherSendIsInFlight) {
  start(packet(0));
  dispatcher.sendPacket(packet(42, 10), 0);
  clock.now += 10;
  dispatcher.loop();
  expectOutcome(false);
  EXPECT_EQ(1u, radio.attempts);
  EXPECT_EQ(0u, radio.finishes);
}

TEST_F(DispatcherDelivery, UntrackedPacketRetainsNormalSendBehavior) {
  auto* p = packet(0);
  p->tx_deadline = clock.now - 1;
  start(p);
  radio.completes = true;
  ++clock.now;
  dispatcher.loop();
  EXPECT_EQ(1u, radio.attempts);
  EXPECT_TRUE(dispatcher.outcomes.empty());
  EXPECT_EQ(4, pool.getFreeCount());
}

TEST_F(DispatcherDelivery, DeadlineWrapsThroughZeroAndExpiresExactly) {
  clock.now = UINT32_MAX - 9;
  dispatcher.sendPacket(packet(42, 20), 0, 100);
  clock.now = 9;
  dispatcher.loop();
  EXPECT_TRUE(dispatcher.outcomes.empty());
  clock.now = 10;
  dispatcher.loop();
  expectOutcome(false);
  EXPECT_EQ(0u, radio.attempts);
}

TEST_F(DispatcherDelivery, ZeroDeadlineIsValidWhenTagIsNonzero) {
  clock.now = UINT32_MAX - 9;
  dispatcher.sendPacket(packet(42, 10), 0, 100);
  clock.now = UINT32_MAX;
  dispatcher.loop();
  EXPECT_TRUE(dispatcher.outcomes.empty());
  clock.now = 0;
  dispatcher.loop();
  expectOutcome(false);
}

TEST_F(DispatcherDelivery, ExpiryRemovesEveryExpiredEntryWithoutTouchingUntracked) {
  radio.busy = true;
  dispatcher.sendPacket(packet(1, 10), 0);
  dispatcher.sendPacket(packet(0, 10), 0);
  dispatcher.sendPacket(packet(2, 10), 0);
  dispatcher.sendPacket(packet(3, 10), 0);
  clock.now += 10;
  dispatcher.loop();
  EXPECT_EQ(3u, dispatcher.outcomes.size());
  EXPECT_EQ(1, pool.getOutboundTotal());
  EXPECT_EQ(0u, pool.getOutboundByIdx(0)->tx_tag);
}

TEST_F(DispatcherDelivery, AllocationResetsStaleMetadataIncludingInboundPath) {
  // Poison every free pool slot, bypassing the normal release path to exercise
  // defensive reset on allocation rather than just successful cleanup.
  std::vector<mesh::Packet*> packets;
  for (int i = 0; i < 4; ++i) {
    auto* p = pool.allocNew();
    p->tx_tag = 99;
    p->tx_deadline = 123;
    packets.push_back(p);
  }
  for (auto* p : packets) pool.free(p);
  auto* p = dispatcher.obtainNewPacket();
  EXPECT_EQ(0u, p->tx_tag);
  EXPECT_EQ(0u, p->tx_deadline);
  dispatcher.releasePacket(p);
  radio.received = {ROUTE_TYPE_DIRECT | (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT), 0, 0x12};
  dispatcher.loop();
  EXPECT_EQ(1u, dispatcher.receives);
  EXPECT_TRUE(dispatcher.inbound_untracked);
  EXPECT_TRUE(dispatcher.outcomes.empty());
}

TEST_F(DispatcherDelivery, MetadataNeverChangesWireBytes) {
  auto* p = packet();
  uint8_t bytes[MAX_TRANS_UNIT] = {};
  const uint8_t length = p->writeTo(bytes);
  EXPECT_EQ(3u, length);
  start(p);
  EXPECT_EQ(std::vector<uint8_t>(bytes, bytes + length), radio.transmitted);
}

TEST_F(DispatcherDelivery, CancelQueuedPacketReportsFailureExactlyOnce) {
  dispatcher.sendPacket(packet(), 0, 1000);
  EXPECT_FALSE(dispatcher.cancelQueuedPacket(0));
  EXPECT_FALSE(dispatcher.cancelQueuedPacket(99));
  EXPECT_TRUE(dispatcher.cancelQueuedPacket(42));
  EXPECT_FALSE(dispatcher.cancelQueuedPacket(42));
  expectOutcome(false);
  EXPECT_EQ(0, pool.getOutboundTotal());
  EXPECT_EQ(4, pool.getFreeCount());
}

TEST_F(DispatcherDelivery, CancelNeverAbortsInFlightPacket) {
  start(packet());
  EXPECT_FALSE(dispatcher.cancelQueuedPacket(42));
  EXPECT_TRUE(dispatcher.outcomes.empty());
  radio.completes = true;
  dispatcher.loop();
  expectOutcome(true);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
