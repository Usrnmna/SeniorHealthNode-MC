#include <gtest/gtest.h>
#include "../../examples/companion_radio/health/IncidentWriteIO.h"
#include "../mocks/DurableIncidentFS.h"

namespace {
using namespace FallIncidentStore;

// Each checked call operates on the existing separate visible/durable model.
// Failures can be returned after its data committed, as real APIs may do.
struct CheckedIO {
  DurableIncidentFS& fs;
  DurableIncidentFS::File file{nullptr, "", false, false};
  bool fail_flush = false, fail_close = false;
  unsigned close_calls = 0;
  unsigned fail_close_number = 0;
  explicit CheckedIO(DurableIncidentFS& value) : fs(value) {}
  auto open(unsigned slot) -> DurableIncidentFS::File* {
    file = fs.open(path(slot), "w");
    if (!file) { errno = EACCES; return nullptr; }
    return &file;
  }
  size_t write(DurableIncidentFS::File* f, const uint8_t* bytes, size_t length) {
    return f->write(bytes, length);
  }
  int flush(DurableIncidentFS::File* f) {
    f->flush();
    if (fail_flush) { errno = ENOSPC; return -1; }
    return 0;
  }
  int close(DurableIncidentFS::File* f) {
    ++close_calls; f->close();
    if (fail_close || close_calls == fail_close_number) { errno = EIO; return -1; }
    return 0;
  }
};
Incident value() { Incident result; result.incident = 17; result.active = true; return result; }
SaveResult checkedSave(DurableIncidentFS& fs, const Incident& incident, CheckedIO& io) {
  CheckedIncidentWriter<CheckedIO> writer(io);
  return saveChecked(fs, incident, writer);
}

TEST(CheckedIncidentIO, IdenticalEncodingAndSlotOrderToExistingWriter) {
  DurableIncidentFS checked, legacy; CheckedIO io(checked); auto next = value();
  for (unsigned i = 0; i != 5; ++i) {
    next.active = (i % 2) == 0; next.recovery_count = uint8_t(i);
    const auto result = checkedSave(checked, next, io);
    ASSERT_TRUE(result.success()); EXPECT_TRUE(result.api_checked);
    ASSERT_TRUE(save(legacy, next)); EXPECT_EQ(legacy.volatile_files, checked.volatile_files);
    EXPECT_EQ(legacy.durable_files, checked.durable_files);
  }
  EXPECT_EQ(5u, io.close_calls);
}
TEST(CheckedIncidentIO, InvalidInputDoesNotOpenOrWrite) {
  DurableIncidentFS fs; CheckedIO io(fs); auto next = value(); next.incident = 0;
  EXPECT_EQ(SaveStage::Invalid, checkedSave(fs, next, io).stage);
  EXPECT_TRUE(fs.events.empty()); EXPECT_EQ(0u, io.close_calls);
}
TEST(CheckedIncidentIO, OpenFailureRetainsNativeErrorWithoutClosingInvalidHandle) {
  DurableIncidentFS fs; CheckedIO io(fs); fs.fail_write_open = true;
  const auto result = checkedSave(fs, value(), io);
  EXPECT_EQ(SaveStage::Open, result.stage); EXPECT_EQ(EACCES, result.error);
  EXPECT_EQ(0u, io.close_calls); EXPECT_EQ(0u, fs.writes);
}
TEST(CheckedIncidentIO, PartialWriteClosesExactlyOnceAndDoesNotVerify) {
  DurableIncidentFS fs; CheckedIO io(fs); fs.write_limit = 5;
  const auto result = checkedSave(fs, value(), io);
  EXPECT_EQ(SaveStage::Write, result.stage); EXPECT_EQ(EIO, result.error);
  EXPECT_EQ(1u, io.close_calls); EXPECT_EQ(1u, fs.flushes);
  for (const auto& event : fs.events) EXPECT_NE(DurableIncidentFS::Operation::Read, event.operation);
}
TEST(CheckedIncidentIO, FlushFailureIsReportedEvenWhenResolutionAlreadyCommitted) {
  DurableIncidentFS fs; CheckedIO io(fs); auto next = value();
  ASSERT_TRUE(checkedSave(fs, next, io).success()); next.active = false; io.fail_flush = true;
  const auto result = checkedSave(fs, next, io);
  EXPECT_EQ(SaveStage::Flush, result.stage); EXPECT_EQ(ENOSPC, result.error); EXPECT_EQ(1u, result.slot);
  fs.reboot(); EXPECT_EQ(Resolved, load(fs, 0).outcome);
  io.fail_flush = false; EXPECT_TRUE(checkedSave(fs, next, io).success());
  fs.reboot(); EXPECT_EQ(Resolved, load(fs, 0).outcome);
}
TEST(CheckedIncidentIO, CloseFailureIsReportedEvenWhenRecoveryCountCommitted) {
  DurableIncidentFS fs; CheckedIO io(fs); auto next = value();
  ASSERT_TRUE(checkedSave(fs, next, io).success()); countRecovery(next); io.fail_close = true;
  const auto result = checkedSave(fs, next, io);
  EXPECT_EQ(SaveStage::Close, result.stage); EXPECT_EQ(EIO, result.error); EXPECT_EQ(EIO, result.close_error);
  fs.reboot(); EXPECT_EQ(1u, load(fs, 0).value.recovery_count);
  io.fail_close = false; EXPECT_TRUE(checkedSave(fs, next, io).success());
  fs.reboot(); EXPECT_EQ(1u, load(fs, 0).value.recovery_count); // Retry does not consume another recovery.
}
TEST(CheckedIncidentIO, FirstErrorSurvivesCleanupCloseFailure) {
  DurableIncidentFS fs; CheckedIO io(fs); io.fail_flush = io.fail_close = true;
  const auto result = checkedSave(fs, value(), io);
  EXPECT_EQ(SaveStage::Flush, result.stage); EXPECT_EQ(ENOSPC, result.error);
  EXPECT_EQ(EIO, result.close_error); EXPECT_EQ(1u, io.close_calls);
}
TEST(CheckedIncidentIO, FailedReadbackCannotReportSuccessAfterCommit) {
  DurableIncidentFS fs; CheckedIO io(fs); fs.fail_verification_open = true;
  const auto result = checkedSave(fs, value(), io);
  EXPECT_EQ(SaveStage::Verify, result.stage); EXPECT_EQ(1u, io.close_calls);
  fs.reboot(); fs.fail_verification_open = false;
  EXPECT_EQ(UncertainClock, load(fs, 0).outcome);
}
TEST(CheckedIncidentIO, UnreadableExistingSlotPreventsAnyWrite) {
  DurableIncidentFS fs; CheckedIO io(fs); ASSERT_TRUE(checkedSave(fs, value(), io).success());
  const unsigned before = fs.writes; fs.fail_read = true;
  EXPECT_EQ(SaveStage::ReadExisting, checkedSave(fs, value(), io).stage);
  EXPECT_EQ(before, fs.writes);
}
TEST(CheckedIncidentIO, DamagedSlotRepairOccursOnlyAfterFullyCheckedFirstWrite) {
  DurableIncidentFS fs; CheckedIO io(fs); fs.volatile_files[path(0)] = {1,2,3}; io.fail_close = true;
  const auto result = checkedSave(fs, value(), io);
  EXPECT_EQ(SaveStage::Close, result.stage); EXPECT_EQ(1u, result.slot);
  EXPECT_EQ(1u, fs.writes); EXPECT_EQ(3u, fs.volatile_files[path(0)].size());
  io.fail_close = false; EXPECT_TRUE(checkedSave(fs, value(), io).success());
  EXPECT_EQ(UncertainClock, load(fs, 0).outcome);
}
TEST(CheckedIncidentIO, CheckedSuccessStillDoesNotProvePhysicalDurability) {
  DurableIncidentFS fs; CheckedIO io(fs); fs.flush_mode = DurableIncidentFS::FlushMode::Ignore;
  ASSERT_TRUE(checkedSave(fs, value(), io).success());
  EXPECT_EQ(UncertainClock, load(fs, 0).outcome);
  fs.reboot(); EXPECT_EQ(Missing, load(fs, 0).outcome);
}
TEST(CheckedIncidentIO, RepairCloseFailureReportsRepairSlotEvenWhenBothRecordsCommitted) {
  DurableIncidentFS fs; CheckedIO io(fs); auto next = value();
  fs.volatile_files[path(0)] = {1,2,3};
  io.fail_close_number = 2;
  const auto result = checkedSave(fs, next, io);
  EXPECT_EQ(SaveStage::Close, result.stage); EXPECT_EQ(0u, result.slot);
  EXPECT_EQ(EIO, result.close_error); EXPECT_EQ(2u, io.close_calls);
  fs.reboot();
  EXPECT_TRUE(read(fs, 0).valid); EXPECT_TRUE(read(fs, 1).valid);
  EXPECT_EQ(UncertainClock, load(fs, 0).outcome);
  // A later resolution supersedes the failed result, even though its data
  // already committed; an old error must not force the old intent on retry.
  next.active = false;
  ASSERT_TRUE(checkedSave(fs, next, io).success());
  fs.reboot(); EXPECT_EQ(Resolved, load(fs, 0).outcome);
}
TEST(CheckedIncidentIO, GenerationRolloverSelectsAndResolvesNewestSlot) {
  DurableIncidentFS fs; CheckedIO io(fs); auto next = value(); ASSERT_TRUE(checkedSave(fs, next, io).success());
  auto& bytes = fs.volatile_files[path(0)]; put32(bytes.data()+4, UINT32_MAX);
  mesh::Utils::sha256(bytes.data()+checksum_offset, 16, bytes.data(), checksum_offset);
  fs.durable_files = fs.volatile_files; next.active = false;
  ASSERT_TRUE(checkedSave(fs, next, io).success());
  EXPECT_EQ(0u, read(fs, 1).generation); fs.reboot(); EXPECT_EQ(Resolved, load(fs, 0).outcome);
}
TEST(CheckedIncidentIO, EveryWriteBoundaryCutPreservesKnownOrUncertainRecovery) {
  DurableIncidentFS original; CheckedIO first(original); auto next = value();
  ASSERT_TRUE(checkedSave(original, next, first).success()); next.active = false;
  auto completed = original; CheckedIO complete_io(completed); completed.beginTrace();
  ASSERT_TRUE(checkedSave(completed, next, complete_io).success());
  for (size_t cut=0; cut<completed.events.size(); ++cut) {
    SCOPED_TRACE(cut); auto fs = original; CheckedIO io(fs); fs.beginTrace(cut);
    EXPECT_THROW(checkedSave(fs, next, io), DurableIncidentFS::PowerLoss);
    const bool resolved_committed = fs.durable_files.count(path(1)) != 0;
    EXPECT_EQ(resolved_committed ? Resolved : UncertainClock, load(fs, 0).outcome);
  }
}
}
