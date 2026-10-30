#include <gtest/gtest.h>
#include "../../examples/companion_radio/health/IncidentReadIO.h"
#include "../mocks/DurableIncidentFS.h"
#include <array>
#include <map>
#include <vector>

namespace {
using namespace FallIncidentStore;

// Descriptor zero is deliberately valid. This fake records the native boundary
// operations without reproducing SPIFFS or the production record policy.
struct ReadIO {
  struct Fault {
    bool open = false, stat = false, read = false, close = false;
    int open_error = EACCES, stat_error = EBADF, read_error = EIO, close_error = EIO;
    size_t read_limit = record_size;
  };
  std::map<unsigned, std::vector<uint8_t>> files;
  std::array<Fault, 2> faults;
  bool is_mounted = true, unmount_on_open = false, unmount_on_close = false;
  unsigned active_slot = 0, opens = 0, stats = 0, reads = 0, closes = 0;
  unsigned mounts = 0;
  bool handle_open = false;
  std::vector<unsigned> written_slots;
  bool fail_verification_close = false;

  bool mounted() { ++mounts; return is_mounted; }
  int open(unsigned slot) {
    ++opens;
    EXPECT_FALSE(handle_open);
    active_slot = slot;
    if (unmount_on_open) is_mounted = false;
    if (faults[slot].open) { errno = faults[slot].open_error; return -1; }
    if (!files.count(slot)) { errno = ENOENT; return -1; }
    handle_open = true;
    return 0;
  }
  int size(int fd, size_t& length) {
    ++stats; EXPECT_EQ(0, fd); EXPECT_TRUE(handle_open);
    if (faults[active_slot].stat) { errno = faults[active_slot].stat_error; return -1; }
    length = files.at(active_slot).size();
    return 0;
  }
  int read(int fd, uint8_t* bytes, size_t length) {
    ++reads; EXPECT_EQ(0, fd); EXPECT_TRUE(handle_open);
    EXPECT_EQ(record_size, length);
    const auto& fault = faults[active_slot];
    if (fault.read) { errno = fault.read_error; return -1; }
    const size_t count = std::min(length, std::min(fault.read_limit, files.at(active_slot).size()));
    memcpy(bytes, files.at(active_slot).data(), count);
    return int(count);
  }
  int close(int fd) {
    ++closes; EXPECT_EQ(0, fd); EXPECT_TRUE(handle_open); handle_open = false;
    if (unmount_on_close) is_mounted = false;
    if (faults[active_slot].close) { errno = faults[active_slot].close_error; return -1; }
    return 0;
  }
};

// Encoding remains in saveWithReader. The writer captures its bytes and can
// arrange a close failure only on the subsequent readback, after commitment.
struct CaptureWriter {
  ReadIO& io;
  bool apiChecked() const { return true; }
  SaveResult operator()(unsigned slot, const uint8_t* bytes, size_t length) {
    io.written_slots.push_back(slot);
    io.files[slot].assign(bytes, bytes + length);
    if (io.fail_verification_close) io.faults[slot].close = true;
    SaveResult result; result.stage = SaveStage::Success; result.api_checked = true;
    return result;
  }
};

Incident activeIncident() {
  Incident value; value.incident = 17; value.active = true; value.origin_epoch = 1800000000;
  return value;
}
std::vector<uint8_t> encodedIncident() {
  DurableIncidentFS fs;
  EXPECT_TRUE(save(fs, activeIncident()));
  return fs.volatile_files.at(path(0));
}
void rechecksum(std::vector<uint8_t>& bytes) {
  mesh::Utils::sha256(bytes.data() + checksum_offset, 16, bytes.data(), checksum_offset);
}

TEST(CheckedIncidentRead, MountedAbsenceNeedsNoDescriptorAndHasNoInventedError) {
  ReadIO io; CheckedIncidentReader<ReadIO> reader(io);
  const auto record = reader(1);
  EXPECT_EQ(ReadStage::Missing, record.io.stage); EXPECT_EQ(1u, record.io.slot);
  EXPECT_TRUE(record.io.api_checked); EXPECT_FALSE(record.io.uncertain());
  EXPECT_FALSE(record.exists); EXPECT_FALSE(record.valid);
  EXPECT_EQ(0, record.io.error); EXPECT_EQ(2u, io.mounts);
  EXPECT_EQ(1u, io.opens); EXPECT_EQ(0u, io.stats); EXPECT_EQ(0u, io.reads); EXPECT_EQ(0u, io.closes);
}
TEST(CheckedIncidentRead, UnmountedStorageIsUnknownRatherThanAbsent) {
  ReadIO io; io.is_mounted = false; CheckedIncidentReader<ReadIO> reader(io);
  const auto record = reader(0);
  EXPECT_EQ(ReadStage::Mount, record.io.stage); EXPECT_EQ(ENODEV, record.io.error);
  EXPECT_TRUE(record.io.uncertain()); EXPECT_FALSE(record.readable);
  EXPECT_EQ(0u, io.opens); EXPECT_EQ(0u, io.closes);
  EXPECT_EQ(Corrupt, loadWithReader(reader, 0).outcome);
}
TEST(CheckedIncidentRead, OpenErrorsAndMissingErrnoRemainUncertainWithoutClose) {
  for (int error : {EACCES, EMFILE, EIO, 0}) {
    SCOPED_TRACE(error); ReadIO io; io.faults[0].open = true; io.faults[0].open_error = error;
    CheckedIncidentReader<ReadIO> reader(io); const auto record = reader(0);
    EXPECT_EQ(ReadStage::Open, record.io.stage); EXPECT_EQ(error ? error : EIO, record.io.error);
    EXPECT_TRUE(record.io.uncertain()); EXPECT_FALSE(record.valid); EXPECT_EQ(0u, io.closes);
  }
}
TEST(CheckedIncidentRead, OpenFailureSurvivesSubsequentUnmount) {
  for (int error : {EACCES, EMFILE, EIO, 0}) {
    SCOPED_TRACE(error); ReadIO io; io.unmount_on_open = true;
    io.faults[0].open = true; io.faults[0].open_error = error;
    CheckedIncidentReader<ReadIO> reader(io); const auto record = reader(0);
    EXPECT_EQ(ReadStage::Open, record.io.stage);
    EXPECT_EQ(error ? error : EIO, record.io.error);
    EXPECT_TRUE(record.io.uncertain()); EXPECT_EQ(0u, io.closes);
  }
}
TEST(CheckedIncidentRead, DescriptorZeroReadsOneBoundedRecordAndClosesExactlyOnce) {
  ReadIO io; io.files[0] = encodedIncident(); CheckedIncidentReader<ReadIO> reader(io);
  errno = ENOSPC; // Stale errno must not poison successful API results.
  const auto record = reader(0);
  EXPECT_EQ(ReadStage::Success, record.io.stage); EXPECT_TRUE(record.valid); EXPECT_TRUE(record.readable);
  EXPECT_EQ(17u, record.value.incident); EXPECT_EQ(0, record.io.error);
  EXPECT_EQ(1u, io.opens); EXPECT_EQ(1u, io.stats); EXPECT_EQ(1u, io.reads); EXPECT_EQ(1u, io.closes);
  EXPECT_FALSE(io.handle_open);
}
TEST(CheckedIncidentRead, StatFailureClosesWithoutReadingAndRetainsFallbackError) {
  for (int error : {EBADF, 0}) {
    ReadIO io; io.files[0] = encodedIncident(); io.faults[0].stat = true; io.faults[0].stat_error = error;
    CheckedIncidentReader<ReadIO> reader(io); const auto record = reader(0);
    EXPECT_EQ(ReadStage::Stat, record.io.stage); EXPECT_EQ(error ? error : EIO, record.io.error);
    EXPECT_FALSE(record.readable); EXPECT_EQ(0u, io.reads); EXPECT_EQ(1u, io.closes);
  }
}
TEST(CheckedIncidentRead, ReadErrorsIncludingMissingErrnoCloseAndNeverDecode) {
  for (int error : {EIO, ENODEV, 0}) {
    ReadIO io; io.files[0] = encodedIncident(); io.faults[0].read = true; io.faults[0].read_error = error;
    CheckedIncidentReader<ReadIO> reader(io); const auto record = reader(0);
    EXPECT_EQ(ReadStage::Read, record.io.stage); EXPECT_EQ(error ? error : EIO, record.io.error);
    EXPECT_FALSE(record.valid); EXPECT_FALSE(record.readable); EXPECT_EQ(1u, io.reads); EXPECT_EQ(1u, io.closes);
  }
}
TEST(CheckedIncidentRead, ShortReadsAreUnknownRatherThanRepairableCorruption) {
  for (size_t count : {size_t(0), size_t(1), size_t(99)}) {
    SCOPED_TRACE(count); ReadIO io; io.files[0] = encodedIncident(); io.faults[0].read_limit = count;
    CheckedIncidentReader<ReadIO> reader(io); const auto record = reader(0);
    EXPECT_EQ(ReadStage::Read, record.io.stage); EXPECT_EQ(EIO, record.io.error);
    EXPECT_TRUE(record.io.uncertain()); EXPECT_FALSE(record.readable); EXPECT_EQ(1u, io.closes);
  }
}
TEST(CheckedIncidentRead, InspectedWrongLengthsAreCorruptWithoutReadingArbitrarySizes) {
  for (size_t length : {size_t(0), size_t(99), size_t(101), size_t(65536)}) {
    SCOPED_TRACE(length); ReadIO io; io.files[0].resize(length);
    CheckedIncidentReader<ReadIO> reader(io); const auto record = reader(0);
    EXPECT_EQ(ReadStage::Invalid, record.io.stage); EXPECT_TRUE(record.exists); EXPECT_TRUE(record.readable);
    EXPECT_FALSE(record.valid); EXPECT_FALSE(record.io.uncertain());
    EXPECT_EQ(0u, io.reads); EXPECT_EQ(1u, io.closes);
  }
}
TEST(CheckedIncidentRead, CompleteCorruptChecksumAndMetadataUseSharedValidation) {
  for (unsigned corruption = 0; corruption < 3; ++corruption) {
    ReadIO io; io.files[0] = encodedIncident(); auto& bytes = io.files[0];
    if (corruption == 0) bytes[checksum_offset] ^= 1;
    else if (corruption == 1) { bytes[17] = 2; rechecksum(bytes); }
    else { put32(bytes.data() + 8, 0); rechecksum(bytes); }
    CheckedIncidentReader<ReadIO> reader(io); const auto record = reader(0);
    EXPECT_EQ(ReadStage::Invalid, record.io.stage); EXPECT_TRUE(record.readable); EXPECT_FALSE(record.valid);
    EXPECT_EQ(0, record.io.error); EXPECT_EQ(1u, io.closes);
  }
}
TEST(CheckedIncidentRead, CloseErrorAfterValidBytesPreventsSuccessfulDecode) {
  for (int error : {EIO, 0}) {
    ReadIO io; io.files[0] = encodedIncident(); io.faults[0].close = true; io.faults[0].close_error = error;
    CheckedIncidentReader<ReadIO> reader(io); const auto record = reader(0);
    EXPECT_EQ(ReadStage::Close, record.io.stage); EXPECT_EQ(EIO, record.io.error);
    EXPECT_EQ(EIO, record.io.close_error); EXPECT_FALSE(record.valid); EXPECT_FALSE(record.readable);
    EXPECT_EQ(1u, io.reads); EXPECT_EQ(1u, io.closes); EXPECT_FALSE(io.handle_open);
  }
}
TEST(CheckedIncidentRead, FirstIoErrorSurvivesCleanupCloseFailure) {
  for (bool stat_error : {false, true}) {
    ReadIO io; io.files[0] = encodedIncident(); auto& fault = io.faults[0];
    fault.stat = stat_error; fault.read = !stat_error; fault.stat_error = fault.read_error = EACCES;
    fault.close = true; fault.close_error = EBADF;
    CheckedIncidentReader<ReadIO> reader(io); const auto record = reader(0);
    EXPECT_EQ(stat_error ? ReadStage::Stat : ReadStage::Read, record.io.stage);
    EXPECT_EQ(EACCES, record.io.error); EXPECT_EQ(EBADF, record.io.close_error); EXPECT_EQ(1u, io.closes);
  }
}
TEST(CheckedIncidentRead, WrongLengthWithFailedCloseIsNotRepairable) {
  ReadIO io; io.files[0] = {1, 2, 3}; io.faults[0].close = true;
  CheckedIncidentReader<ReadIO> reader(io); const auto record = reader(0);
  EXPECT_EQ(ReadStage::Close, record.io.stage); EXPECT_TRUE(record.io.uncertain());
  EXPECT_FALSE(record.readable); EXPECT_EQ(0u, io.reads); EXPECT_EQ(1u, io.closes);
}
TEST(CheckedIncidentRead, MountLossAroundMissingOpenCannotBecomeQualifiedAbsence) {
  ReadIO io; io.unmount_on_open = true; CheckedIncidentReader<ReadIO> reader(io);
  const auto record = reader(0);
  EXPECT_EQ(ReadStage::Mount, record.io.stage); EXPECT_EQ(ENODEV, record.io.error);
  EXPECT_TRUE(record.io.uncertain()); EXPECT_EQ(1u, io.opens); EXPECT_EQ(0u, io.closes);
}
TEST(CheckedIncidentRead, MountLossAfterValidReadCannotBecomeSuccessfulRecovery) {
  ReadIO io; io.files[0] = encodedIncident(); io.unmount_on_close = true;
  CheckedIncidentReader<ReadIO> reader(io); const auto record = reader(0);
  EXPECT_EQ(ReadStage::Mount, record.io.stage); EXPECT_EQ(ENODEV, record.io.error);
  EXPECT_FALSE(record.valid); EXPECT_FALSE(record.readable); EXPECT_EQ(1u, io.closes);
}
TEST(CheckedIncidentRead, BothQualifiedMissingSlotsRemainMissingWithoutWrites) {
  ReadIO io; CheckedIncidentReader<ReadIO> reader(io);
  const auto recovery = loadWithReader(reader, 0);
  EXPECT_EQ(Missing, recovery.outcome); EXPECT_FALSE(recovery.shouldResume());
  for (const auto& result : recovery.reads) {
    EXPECT_EQ(ReadStage::Missing, result.stage); EXPECT_TRUE(result.api_checked);
  }
  EXPECT_TRUE(io.written_slots.empty());
}
TEST(CheckedIncidentRead, UnknownPeerBlocksOlderActiveRecoveryAndAllWrites) {
  for (unsigned unknown_slot : {0u, 1u}) {
    SCOPED_TRACE(unknown_slot); ReadIO io; io.files[unknown_slot ^ 1u] = encodedIncident();
    io.faults[unknown_slot].open = true; io.faults[unknown_slot].open_error = EIO;
    CheckedIncidentReader<ReadIO> reader(io); CaptureWriter writer{io}; const auto before = io.files;
    const auto recovery = loadWithReader(reader, 0);
    EXPECT_EQ(Corrupt, recovery.outcome); EXPECT_FALSE(recovery.shouldResume());
    EXPECT_EQ(ReadStage::Open, recovery.reads[unknown_slot].stage);
    const auto result = saveWithReader(reader, activeIncident(), writer);
    EXPECT_EQ(SaveStage::ReadExisting, result.stage); EXPECT_EQ(unknown_slot, result.slot);
    EXPECT_EQ(ReadStage::Open, result.read_result.stage); EXPECT_TRUE(result.read_result.api_checked);
    EXPECT_TRUE(io.written_slots.empty()); EXPECT_EQ(before, io.files);
  }
}
TEST(CheckedIncidentRead, BothUnknownSlotsCannotBeInitializedAsEmpty) {
  ReadIO io; io.is_mounted = false; CheckedIncidentReader<ReadIO> reader(io); CaptureWriter writer{io};
  EXPECT_EQ(Corrupt, loadWithReader(reader, 0).outcome);
  EXPECT_EQ(SaveStage::ReadExisting, saveWithReader(reader, activeIncident(), writer).stage);
  EXPECT_EQ(0u, io.opens); EXPECT_TRUE(io.written_slots.empty());
}
TEST(CheckedIncidentRead, VerificationCloseFailureRetainsCommittedDataWithoutClaimingSuccess) {
  ReadIO io; io.fail_verification_close = true; CheckedIncidentReader<ReadIO> reader(io); CaptureWriter writer{io};
  const auto result = saveWithReader(reader, activeIncident(), writer);
  EXPECT_EQ(SaveStage::Verify, result.stage); EXPECT_EQ(ReadStage::Close, result.read_result.stage);
  ASSERT_EQ(1u, io.written_slots.size()); EXPECT_EQ(0u, io.written_slots[0]);
  io.faults[0].close = false;
  EXPECT_EQ(UncertainClock, loadWithReader(reader, 0).outcome);
}
TEST(CheckedIncidentRead, CheckedReadSaveSequenceMatchesLegacyEncodingAndSlotSelection) {
  ReadIO io; CheckedIncidentReader<ReadIO> reader(io); CaptureWriter writer{io}; DurableIncidentFS legacy;
  auto next = activeIncident();
  for (unsigned i = 0; i != 6; ++i) {
    next.active = (i % 2) == 0; next.recovery_count = uint8_t(i);
    ASSERT_TRUE(saveWithReader(reader, next, writer).success()); ASSERT_TRUE(save(legacy, next));
    for (const auto& file : io.files) EXPECT_EQ(legacy.volatile_files.at(path(file.first)), file.second);
    const auto checked = loadWithReader(reader, next.origin_epoch + 1, true);
    const auto previous = load(legacy, next.origin_epoch + 1, true);
    EXPECT_EQ(previous.outcome, checked.outcome); EXPECT_EQ(previous.value.active, checked.value.active);
    EXPECT_EQ(previous.value.recovery_count, checked.value.recovery_count);
  }
  EXPECT_EQ((std::vector<unsigned>{0, 1, 0, 1, 0, 1}), io.written_slots);
  EXPECT_EQ(Resolved, loadWithReader(reader, 0).outcome);
}
TEST(CheckedIncidentRead, RepairWaitsForSuccessfulCheckedReadbackAndThenRepairsBothSlots) {
  ReadIO io; io.files[0] = {1, 2, 3}; io.fail_verification_close = true;
  CheckedIncidentReader<ReadIO> reader(io); CaptureWriter writer{io};
  EXPECT_EQ(SaveStage::Verify, saveWithReader(reader, activeIncident(), writer).stage);
  EXPECT_EQ((std::vector<unsigned>{1}), io.written_slots); EXPECT_EQ(3u, io.files[0].size());
  io.fail_verification_close = false; io.faults[1].close = false;
  ASSERT_TRUE(saveWithReader(reader, activeIncident(), writer).success());
  EXPECT_EQ((std::vector<unsigned>{1, 0}), io.written_slots);
  EXPECT_TRUE(reader(0).valid); EXPECT_TRUE(reader(1).valid);
  EXPECT_EQ(UncertainClock, loadWithReader(reader, 0).outcome);
}
TEST(CheckedIncidentRead, GenerationWrapAndTombstonePrecedenceRemainUnchanged) {
  ReadIO io; io.files[0] = encodedIncident(); put32(io.files[0].data() + 4, UINT32_MAX); rechecksum(io.files[0]);
  CheckedIncidentReader<ReadIO> reader(io); CaptureWriter writer{io}; auto resolved = activeIncident(); resolved.active = false;
  ASSERT_TRUE(saveWithReader(reader, resolved, writer).success());
  EXPECT_EQ(0u, reader(1).generation); EXPECT_EQ(Resolved, loadWithReader(reader, 0).outcome);
  io.faults[1].read = true;
  EXPECT_EQ(Corrupt, loadWithReader(reader, 0).outcome); // Never fall back to the older active slot.
}
TEST(CheckedIncidentRead, TwoCorruptSlotsRepairInLegacyOrderWithIdenticalRecords) {
  ReadIO io; io.files[0] = {1}; io.files[1] = {2, 3};
  DurableIncidentFS legacy;
  legacy.volatile_files[path(0)] = io.files[0]; legacy.volatile_files[path(1)] = io.files[1];
  CheckedIncidentReader<ReadIO> reader(io); CaptureWriter writer{io};
  ASSERT_TRUE(saveWithReader(reader, activeIncident(), writer).success());
  ASSERT_TRUE(save(legacy, activeIncident()));
  EXPECT_EQ((std::vector<unsigned>{1, 0}), io.written_slots);
  for (unsigned slot : {0u, 1u}) EXPECT_EQ(legacy.volatile_files.at(path(slot)), io.files.at(slot));
  EXPECT_EQ(reader(0).generation, reader(1).generation);
  EXPECT_EQ(UncertainClock, loadWithReader(reader, 0).outcome);
}
TEST(CheckedIncidentRead, QualifiedAndUnknownClockPoliciesUseTheSameRecordOwner) {
  ReadIO io; io.files[0] = encodedIncident(); CheckedIncidentReader<ReadIO> reader(io); CaptureWriter writer{io};
  EXPECT_EQ(Resume, loadWithReader(reader, activeIncident().origin_epoch + 1, true).outcome);
  EXPECT_EQ(UncertainClock, loadWithReader(reader, activeIncident().origin_epoch + 1, false).outcome);
  EXPECT_EQ(Expired, loadWithReader(reader, activeIncident().origin_epoch + maximum_age_seconds, true).outcome);
  auto limited = activeIncident(); limited.recovery_count = uncertain_recovery_limit;
  ASSERT_TRUE(saveWithReader(reader, limited, writer).success());
  EXPECT_EQ(RecoveryLimit, loadWithReader(reader, 0).outcome);
}
}
