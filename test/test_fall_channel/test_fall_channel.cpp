#include <gtest/gtest.h>
#include "../../examples/companion_radio/health/FallChannelStore.h"
#include "../../examples/companion_radio/health/FallResponse.h"
#include "../../examples/companion_radio/health/FallNoticeOutbox.h"
#include <map>
#include <string>
#include <vector>
#include <algorithm>

namespace {
constexpr const char* private_value = "private CareGroup 00112233445566778899AABBCCDDEEFF";
FallChannel parse(const char* value) {
  FallChannel result;
  EXPECT_TRUE(FallChannel::parse(value, result));
  return result;
}
std::string description(const FallChannel& channel) {
  char text[80]; channel.describe(text, sizeof(text)); return text;
}

// A byte-preserving filesystem fake with explicit open/write faults. Production
// storage code is exercised unchanged, including its readback and checksums.
struct MemoryFS {
  std::map<std::string, std::vector<uint8_t>> files;
  size_t write_limit = 10000;
  bool fail_open = false;
  struct File {
    std::vector<uint8_t>* bytes;
    size_t write_limit;
    explicit operator bool() const { return bytes; }
    size_t size() const { return bytes->size(); }
    size_t read(uint8_t* dest, size_t len) {
      len = std::min(len, bytes->size()); memcpy(dest, bytes->data(), len); return len;
    }
    size_t write(const uint8_t* src, size_t len) {
      len = std::min(len, write_limit); bytes->assign(src, src + len); return len;
    }
    void flush() {}
    void close() {}
  };
  bool exists(const char* name) { return files.count(name); }
  File open(const char* name, const char* mode) {
    if (fail_open) return {nullptr, write_limit};
    if (*mode == 'w') { files[name].clear(); return {&files[name], write_limit}; }
    const auto entry = files.find(name);
    return {entry == files.end() ? nullptr : &entry->second, write_limit};
  }
};

TEST(FallChannel, ValidatesCompleteValuesWithoutChangingCurrentSelection) {
  auto channel = parse(private_value);
  const char* invalid[] = {"", "public", "public name", "public #", "public #test extra",
    "public #bad,name", "public #bad:name", "public #bad\nname", "private CareGroup",
    "private CareGroup password", "private CareGroup 00112233445566778899AABBCCDDEEFG",
    "private CareGroup 00112233445566778899AABBCCDDEEFF extra",
    "public #abcdefghijklmnopqrstuvwxyz123456"};
  for (const char* value : invalid) {
    EXPECT_FALSE(FallChannel::parse(value, channel)) << value;
    EXPECT_EQ("private CareGroup", description(channel));
    EXPECT_EQ(0x11, channel.key[1]);
  }
}
TEST(FallChannel, AcceptsHexAndCanonicalBase64AndNeverDescribesKeys) {
  const auto hex = parse(private_value);
  const auto b64 = parse("private CareGroup ABEiM0RVZneImaq7zN3u/w==");
  EXPECT_EQ(16, b64.key_length); EXPECT_EQ(0, memcmp(hex.key, b64.key, 32));
  const auto long_key = parse("private CareGroup AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=");
  EXPECT_EQ(32, long_key.key_length); EXPECT_EQ(31, long_key.key[31]);
  EXPECT_EQ("private CareGroup", description(long_key));
  FallChannel bad;
  EXPECT_FALSE(FallChannel::parse("private CareGroup ABEiM0RVZneImaq7zN3u/x==", bad)); // Nonzero pad bits.
  EXPECT_FALSE(FallChannel::parse("private CareGroup ABEiM0RVZneImaq7zN3u/w=", bad));
  char stored[FallChannel::value_capacity]; long_key.storageValue(stored);
  auto restored = parse(stored);
  EXPECT_EQ(0, memcmp(long_key.key, restored.key, 32));
}
TEST(FallChannel, RejectsMalformedUtf8AndNormalizesPaddedKeys) {
  FallChannel channel;
  EXPECT_TRUE(FallChannel::parse("public #caf\xc3\xa9", channel));
  EXPECT_FALSE(FallChannel::parse("public #\xc0\xaf", channel));
  EXPECT_FALSE(FallChannel::parse("public #\xed\xa0\x80", channel));
  EXPECT_FALSE(FallChannel::parse("public #\xf4\x90\x80\x80", channel));
  EXPECT_FALSE(FallChannel::parse("public #\xe2\x82", channel));
  EXPECT_FALSE(FallChannel::parse("public #\xe2\x41\x80", channel));
  channel = parse("private CareGroup 00112233445566778899AABBCCDDEEFF00000000000000000000000000000000");
  EXPECT_EQ(16, channel.key_length);
}
TEST(FallChannelStorage, DefaultsPersistAndSurviveReboot) {
  MemoryFS fs; FallChannel loaded;
  EXPECT_TRUE(FallChannelStore::load(fs, loaded));
  EXPECT_EQ("public #falldetect", description(loaded));
  ASSERT_TRUE(FallChannelStore::save(fs, parse(private_value)));
  ASSERT_TRUE(FallChannelStore::load(fs, loaded));
  EXPECT_EQ("private CareGroup", description(loaded));
  ASSERT_TRUE(FallChannelStore::save(fs, parse("public #other")));
  ASSERT_TRUE(FallChannelStore::load(fs, loaded));
  EXPECT_EQ("public #other", description(loaded));
  ASSERT_TRUE(FallChannelStore::save(fs, parse(private_value)));
  ASSERT_TRUE(FallChannelStore::load(fs, loaded));
  EXPECT_EQ("private CareGroup", description(loaded));
}
TEST(FallChannelStorage, FailedWriteRetainsPreviousPrivateSelection) {
  MemoryFS fs; FallChannel loaded;
  ASSERT_TRUE(FallChannelStore::save(fs, parse(private_value)));
  fs.write_limit = 20;
  EXPECT_FALSE(FallChannelStore::save(fs, parse("public #other")));
  ASSERT_TRUE(FallChannelStore::load(fs, loaded));
  EXPECT_EQ("private CareGroup", description(loaded));
  fs.fail_open = true;
  EXPECT_FALSE(FallChannelStore::save(fs, parse("public #other")));
  EXPECT_FALSE(FallChannelStore::load(fs, loaded)); EXPECT_FALSE(loaded.valid());
}
TEST(FallChannelStorage, CorruptOrTruncatedSettingsNeverFallbackToPublicDefault) {
  MemoryFS fs; FallChannel loaded;
  ASSERT_TRUE(FallChannelStore::save(fs, parse(private_value)));
  fs.files[FallChannelStore::path(0)][15] ^= 1;
  EXPECT_FALSE(FallChannelStore::load(fs, loaded)); EXPECT_FALSE(loaded.valid());
  fs.files[FallChannelStore::path(0)].resize(10);
  EXPECT_FALSE(FallChannelStore::load(fs, loaded)); EXPECT_FALSE(loaded.valid());
  ASSERT_TRUE(FallChannelStore::save(fs, parse(private_value)));
  EXPECT_TRUE(FallChannelStore::load(fs, loaded)); EXPECT_TRUE(loaded.valid());
}
TEST(FallChannelStorage, GenerationComparisonHandlesWrap) {
  EXPECT_TRUE(FallChannelStore::newer(0, UINT32_MAX));
  EXPECT_FALSE(FallChannelStore::newer(UINT32_MAX, 0));
  EXPECT_FALSE(FallChannelStore::newer(10, 10));
}

struct ChannelLink : AssistanceTransport {
  std::vector<FallChannel> destinations;
  uint32_t tag = 0;
  uint32_t uniqueTimestamp() override { return 1; }
  void cancelAssistance(uint32_t) override {}
  Result queueAssistance(const uint8_t*, unsigned, uint32_t id, uint8_t hash[8], const FallChannel& channel) override {
    destinations.push_back(channel); tag = id; memset(hash, 0, 8); return Queued;
  }
};
bool click(FallResponse& response, uint32_t time) {
  response.updateButton(time, true); response.updateButton(time + 25, true);
  response.updateButton(time + 100, false); return response.updateButton(time + 125, false);
}
TEST(FallChannelSnapshot, IncidentWarningAndLateConfirmationRetainOriginalKey) {
  auto selected = parse(private_value); FallResponse response;
  ASSERT_TRUE(response.onFall(0, false, selected));
  selected = parse("public #other");
  EXPECT_FALSE(response.onFall(100, false, selected));
  response.updateDeadline(HealthNodeConfig::response_timeout_ms);
  EXPECT_EQ("private CareGroup", description(response.pendingChannel()));
  response.messageAccepted();
  click(response, 31000); click(response, 31300); ASSERT_TRUE(click(response, 31600));
  EXPECT_EQ(FallNotice::UserConfirmedFall, response.pendingNotice());
  ASSERT_TRUE(response.onFall(40000, false, selected));
  EXPECT_EQ("public #other", description(response.incidentChannel()));
  EXPECT_EQ("private CareGroup", description(response.pendingChannel()));
  response.updateDeadline(70000); // Backpressure protects the older confirmation and key.
  EXPECT_EQ("private CareGroup", description(response.pendingChannel()));
}
TEST(FallChannelSnapshot, QueuedPartsAndRetriesRetainOriginalChannel) {
  ChannelLink link; AssistanceDelivery delivery; FallNoticeOutbox outbox;
  auto selected = parse(private_value);
  const auto message = FallNoticeMessage::build(FallNotice::NoResidualMovement,
      "A moderately long node name", true, -90, -180);
  ASSERT_EQ(2u, message.count);
  ASSERT_TRUE(outbox.enqueue(FallNotice::NoResidualMovement, 1, message, selected));
  selected = parse("public #other");
  outbox.poll(0, link, delivery);
  delivery.txComplete(1, link.tag, false);
  outbox.poll(HealthNodeConfig::ack_retry_ms + 1, link, delivery);
  delivery.txComplete(6000, link.tag, true);
  uint8_t hash[8] = {}; ASSERT_TRUE(delivery.confirmRepeat(hash));
  outbox.poll(6001, link, delivery);
  ASSERT_EQ(3u, link.destinations.size());
  for (const auto& channel : link.destinations) EXPECT_EQ("private CareGroup", description(channel));
}
}

int main(int argc, char** argv) { ::testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
