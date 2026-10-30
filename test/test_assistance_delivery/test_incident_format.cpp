#include <gtest/gtest.h>
#include "../../examples/companion_radio/health/IncidentReadIO.h"
#include <algorithm>
#include <array>
#include <map>
#include <vector>

namespace {
using namespace FallIncidentStore;

// native_delivery links real Utils.cpp and rweather Crypto. Only filesystem
// transport is replaced here; FIN1 decoding, encoding and SHA-256 are production.
struct ReadIO {
  std::map<unsigned, std::vector<uint8_t>> files;
  std::vector<unsigned> written_slots;
  bool mounted() const { return true; }
  int open(unsigned slot) {
    if (!files.count(slot)) { errno = ENOENT; return -1; }
    return int(slot);
  }
  int size(int fd, size_t& length) { length = files.at(unsigned(fd)).size(); return 0; }
  int read(int fd, uint8_t* bytes, size_t length) {
    const auto& data = files.at(unsigned(fd));
    length = std::min(length, data.size());
    memcpy(bytes, data.data(), length);
    return int(length);
  }
  int close(int) { return 0; }
};
struct CaptureWriter {
  ReadIO& io;
  bool apiChecked() const { return true; }
  SaveResult operator()(unsigned slot, const uint8_t* bytes, size_t length) {
    io.written_slots.push_back(slot);
    io.files[slot].assign(bytes, bytes + length);
    SaveResult result; result.stage = SaveStage::Success; result.api_checked = true;
    return result;
  }
};

// Literal FIN1 fixtures follow the pre-batch layout at f33dd01b2ec8791cd10ec64f41112e1f51a28ac6.
// Their truncated SHA-256 checksums were independently calculated with Python
// hashlib over bytes 0..83. Expected bytes never use the current serializer.
// The private key below is synthetic test data, not a device credential.
void expectGoldenRoundTrip(const std::array<uint8_t, 100>& golden, const Incident& expected) {
  ReadIO input; input.files[0].assign(golden.begin(), golden.end());
  CheckedIncidentReader<ReadIO> reader(input);
  const auto record = reader(0);
  ASSERT_TRUE(record.valid); EXPECT_EQ(1u, record.generation);
  EXPECT_EQ(ReadStage::Success, record.io.stage); EXPECT_TRUE(record.io.api_checked);
  const auto recovered = loadWithReader(reader, 1800000001, true);
  EXPECT_EQ(expected.active ? Resume : Resolved, recovered.outcome);
  EXPECT_EQ(expected.incident, recovered.value.incident);
  EXPECT_EQ(expected.origin_epoch, recovered.value.origin_epoch);
  EXPECT_EQ(expected.recovery_count, recovered.value.recovery_count);
  EXPECT_EQ(expected.active, recovered.value.active);
  EXPECT_EQ(expected.channel.public_channel, recovered.value.channel.public_channel);
  EXPECT_EQ(expected.channel.key_length, recovered.value.channel.key_length);
  EXPECT_EQ(0, memcmp(expected.channel.name, recovered.value.channel.name, 32));
  EXPECT_EQ(0, memcmp(expected.channel.key, recovered.value.channel.key, 32));

  // Encode an independently constructed value into an empty store, not the
  // decoder's output, so matching encode/decode errors cannot cancel each other.
  ReadIO output; CheckedIncidentReader<ReadIO> output_reader(output); CaptureWriter writer{output};
  ASSERT_TRUE(saveWithReader(output_reader, expected, writer).success());
  ASSERT_EQ((std::vector<unsigned>{0}), output.written_slots);
  EXPECT_EQ(std::vector<uint8_t>(golden.begin(), golden.end()), output.files.at(0));
  EXPECT_STREQ("/fall_inc0", path(0)); EXPECT_STREQ("/fall_inc1", path(1));

  // Corrupt payload and checksum independently. The real digest must reject
  // both without turning a complete record into an I/O failure or absence.
  for (unsigned offset : {8u, 84u}) {
    auto corrupt = golden; corrupt[offset] ^= 1;
    input.files[0].assign(corrupt.begin(), corrupt.end());
    const auto rejected = reader(0);
    EXPECT_FALSE(rejected.valid); EXPECT_TRUE(rejected.readable);
    EXPECT_EQ(ReadStage::Invalid, rejected.io.stage);
    EXPECT_EQ(Corrupt, loadWithReader(reader, 1800000001, true).outcome);
  }
}
TEST(IncidentFormat, PublicActiveRecordMatchesIndependentFin1GoldenBytes) {
  const std::array<uint8_t, 100> golden = {
    0x46, 0x49, 0x4e, 0x31, 0x01, 0x00, 0x00, 0x00, 0x78, 0x56,
    0x34, 0x12, 0x00, 0xd2, 0x49, 0x6b, 0x01, 0x01, 0x01, 0x10,
    0x23, 0x67, 0x6f, 0x6c, 0x64, 0x65, 0x6e, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x44, 0x95, 0x4e, 0xe0, 0xe5, 0xf0, 0x85, 0x57,
    0x8b, 0x1a, 0xf9, 0x22, 0x6d, 0xd3, 0x73, 0xfc, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xc6, 0x37, 0xce, 0x7f, 0xfa, 0x22,
    0x71, 0x2f, 0xb9, 0x34, 0xd4, 0x24, 0xe7, 0xab, 0xf6, 0xe3,
  };
  Incident expected; expected.incident = 0x12345678; expected.origin_epoch = 1800000000;
  expected.recovery_count = 1; expected.active = true;
  memset(expected.channel.name, 0, sizeof(expected.channel.name));
  memcpy(expected.channel.name, "#golden", 7);
  const uint8_t key[32] = {0x44, 0x95, 0x4e, 0xe0, 0xe5, 0xf0, 0x85, 0x57,
      0x8b, 0x1a, 0xf9, 0x22, 0x6d, 0xd3, 0x73, 0xfc};
  memcpy(expected.channel.key, key, sizeof(key));
  expected.channel.key_length = 16; expected.channel.public_channel = true;
  expectGoldenRoundTrip(golden, expected);
}
TEST(IncidentFormat, PrivateTombstoneMatchesIndependentFin1GoldenBytes) {
  const std::array<uint8_t, 100> golden = {
    0x46, 0x49, 0x4e, 0x31, 0x01, 0x00, 0x00, 0x00, 0xef, 0xcd,
    0xab, 0x89, 0x00, 0xd2, 0x49, 0x6b, 0x02, 0x00, 0x00, 0x20,
    0x47, 0x6f, 0x6c, 0x64, 0x65, 0x6e, 0x43, 0x61, 0x72, 0x65,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11,
    0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b,
    0x1c, 0x1d, 0x1e, 0x1f, 0x13, 0x8a, 0x2b, 0x45, 0x68, 0x07,
    0x19, 0x13, 0xdf, 0x41, 0x0c, 0xcc, 0x06, 0x92, 0xdd, 0x02,
  };
  Incident expected; expected.incident = 0x89abcdef; expected.origin_epoch = 1800000000;
  expected.recovery_count = 2; expected.active = false;
  memset(expected.channel.name, 0, sizeof(expected.channel.name));
  memcpy(expected.channel.name, "GoldenCare", 10);
  for (unsigned i = 0; i < sizeof(expected.channel.key); ++i) expected.channel.key[i] = uint8_t(i);
  expected.channel.key_length = 32; expected.channel.public_channel = false;
  expectGoldenRoundTrip(golden, expected);
}

}
