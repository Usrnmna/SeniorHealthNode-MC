#include <gtest/gtest.h>
#include "../../examples/companion_radio/health/FallIncidentStore.h"
#include "../mocks/DurableIncidentFS.h"

namespace {
using namespace FallIncidentStore;
using Operation = DurableIncidentFS::Operation;
using Phase = DurableIncidentFS::Phase;

Incident unresolved() {
  Incident value;
  value.incident = 17; value.active = true;
  value.origin_epoch = 0; // Current companion policy: downtime is not known.
  return value;
}

// Capture every before/after operation boundary once, then replay exactly the
// same production save with a power cut at each boundary. No save may return a
// success after its selected interruption, including cuts during verification.
template<class Check>
void sweepCuts(const DurableIncidentFS& initial, const Incident& next, Check check) {
  auto complete = initial;
  complete.beginTrace();
  ASSERT_TRUE(save(complete, next));
  ASSERT_FALSE(complete.events.empty());
  for (size_t cut = 0; cut < complete.events.size(); ++cut) {
    SCOPED_TRACE("cut event " + std::to_string(cut));
    auto fs = initial; fs.beginTrace(cut);
    EXPECT_THROW(save(fs, next), DurableIncidentFS::PowerLoss);
    EXPECT_EQ(fs.volatile_files, fs.durable_files);
    check(fs, complete.events[cut]);
  }
}

TEST(IncidentDurability, NormalSuccessSurvivesRebootAndLoadNeverWrites) {
  DurableIncidentFS fs; auto value = unresolved();
  ASSERT_TRUE(FallChannel::parse("private Care 00112233445566778899aabbccddeeff", value.channel));
  ASSERT_TRUE(save(fs, value));
  EXPECT_EQ(fs.volatile_files, fs.durable_files);
  fs.reboot();
  const auto result = load(fs, 0);
  EXPECT_EQ(UncertainClock, result.outcome);
  EXPECT_EQ(value.incident, result.value.incident);
  EXPECT_EQ(0, memcmp(value.channel.key, result.value.channel.key, sizeof(value.channel.key)));
  EXPECT_EQ(1u, fs.writes); EXPECT_EQ(1u, fs.flushes);
}

TEST(IncidentDurability, NewIncidentCutAtEveryOperationIsMissingOrFullyCommitted) {
  DurableIncidentFS initial;
  sweepCuts(initial, unresolved(), [](DurableIncidentFS& fs, const DurableIncidentFS::Event&) {
    const auto outcome = load(fs, 0).outcome;
    EXPECT_TRUE(outcome == Missing || outcome == UncertainClock);
    if (outcome == UncertainClock) EXPECT_EQ(17u, load(fs, 0).value.incident);
  });
}

TEST(IncidentDurability, ResolutionCutPointsNeverReviveACommittedTombstone) {
  DurableIncidentFS initial; auto value = unresolved();
  ASSERT_TRUE(save(initial, value)); value.active = false;
  sweepCuts(initial, value, [](DurableIncidentFS& fs, const DurableIncidentFS::Event&) {
    // Slot 1 exists durably only after this model commits the resolution flush.
    const bool resolution_committed = fs.durable_files.count(path(1)) != 0;
    const auto result = load(fs, 0);
    EXPECT_EQ(resolution_committed ? Resolved : UncertainClock, result.outcome);
    if (resolution_committed) EXPECT_FALSE(result.shouldResume());
  });
  ASSERT_TRUE(save(initial, value));
  for (unsigned reset = 0; reset < 12; ++reset) {
    initial.reboot(); EXPECT_EQ(Resolved, load(initial, 0).outcome);
  }
}

TEST(IncidentDurability, DurableTruncateMakesAnInterruptedResolutionExplicitlyUncertain) {
  DurableIncidentFS initial; auto value = unresolved();
  ASSERT_TRUE(save(initial, value)); value.active = false;
  initial.durable_truncate = true;
  sweepCuts(initial, value, [](DurableIncidentFS& fs, const DurableIncidentFS::Event&) {
    const auto found = fs.durable_files.find(path(1));
    const auto result = load(fs, 0);
    if (found == fs.durable_files.end()) EXPECT_EQ(UncertainClock, result.outcome);
    else if (found->second.empty()) {
      EXPECT_EQ(Corrupt, result.outcome); EXPECT_FALSE(result.shouldResume());
    } else EXPECT_EQ(Resolved, result.outcome);
  });
}

TEST(IncidentDurability, PartialWriteReturnsFailureAndDoesNotFallBackToOldAlarm) {
  for (size_t limit : {size_t(0), size_t(8), size_t(25), record_size - 1}) {
    SCOPED_TRACE(limit);
    DurableIncidentFS fs; auto value = unresolved(); ASSERT_TRUE(save(fs, value));
    value.active = false; fs.write_limit = limit;
    EXPECT_FALSE(save(fs, value)); fs.reboot();
    EXPECT_EQ(Corrupt, load(fs, 0).outcome);
    EXPECT_FALSE(load(fs, 0).shouldResume());
    fs.write_limit = size_t(-1); ASSERT_TRUE(save(fs, value)); fs.reboot();
    EXPECT_EQ(Resolved, load(fs, 0).outcome);
  }
}

TEST(IncidentDurability, InterruptedRepairRemainsUncertainUntilBothSlotsAreValid) {
  DurableIncidentFS initial;
  initial.durable_files[path(0)] = {1, 2, 3};
  initial.durable_files[path(1)] = {4, 5, 6};
  initial.reboot(); auto value = unresolved(); value.active = false;
  sweepCuts(initial, value, [](DurableIncidentFS& fs, const DurableIncidentFS::Event&) {
    const auto a = read(fs, 0), b = read(fs, 1);
    const auto result = load(fs, 0);
    EXPECT_EQ(a.valid && b.valid ? Resolved : Corrupt, result.outcome);
    EXPECT_FALSE(result.shouldResume());
  });
}

TEST(IncidentDurability, NewIncidentAfterResolutionCannotReviveTheResolvedIdentifier) {
  DurableIncidentFS initial; auto value = unresolved();
  ASSERT_TRUE(save(initial, value)); value.active = false; ASSERT_TRUE(save(initial, value));
  ++value.incident; value.active = true;
  sweepCuts(initial, value, [](DurableIncidentFS& fs, const DurableIncidentFS::Event&) {
    const auto result = load(fs, 0);
    EXPECT_TRUE(result.outcome == Resolved || result.outcome == UncertainClock);
    if (result.shouldResume()) EXPECT_EQ(18u, result.value.incident);
    else EXPECT_EQ(17u, result.value.incident);
  });
}

TEST(IncidentDurability, FailedOpenAndFailedReadDoNotOverwriteDurableState) {
  DurableIncidentFS fs; auto value = unresolved(); ASSERT_TRUE(save(fs, value));
  const auto before = fs.durable_files; value.active = false;
  fs.fail_write_open = true; EXPECT_FALSE(save(fs, value));
  EXPECT_EQ(before, fs.durable_files);
  fs.fail_write_open = false; fs.fail_read = true;
  EXPECT_FALSE(save(fs, value)); EXPECT_EQ(before, fs.durable_files);
  EXPECT_EQ(Corrupt, load(fs, 0).outcome);
  fs.fail_read = false; fs.reboot(); EXPECT_EQ(UncertainClock, load(fs, 0).outcome);
}

TEST(IncidentDurability, FailedVerificationCanHaveCommittedDataButNeverReportsSuccess) {
  DurableIncidentFS fs; fs.fail_verification_open = true;
  EXPECT_FALSE(save(fs, unresolved()));
  ASSERT_EQ(1u, fs.flushes); ASSERT_EQ(1u, fs.durable_files.size());
  fs.reboot(); fs.fail_verification_open = false;
  EXPECT_EQ(UncertainClock, load(fs, 0).outcome);
}

TEST(IncidentDurability, CheckedReadbackCannotProveAnIgnoredVoidFlushWasDurable) {
  DurableIncidentFS fs; auto value = unresolved(); ASSERT_TRUE(save(fs, value));
  value.active = false; fs.flush_mode = DurableIncidentFS::FlushMode::Ignore;
  // This is a limitation test, not a durability guarantee: the production bool
  // means checked readback. Both readback and close can succeed from the cache.
  EXPECT_TRUE(save(fs, value)); EXPECT_EQ(Resolved, load(fs, 0).outcome);
  EXPECT_NE(fs.volatile_files, fs.durable_files);
  fs.reboot(); EXPECT_EQ(UncertainClock, load(fs, 0).outcome);
}

TEST(IncidentDurability, TornDurableFlushIsUnknownEvenWhenCachedReadbackSucceeded) {
  DurableIncidentFS fs; auto value = unresolved(); ASSERT_TRUE(save(fs, value));
  value.active = false; fs.flush_mode = DurableIncidentFS::FlushMode::Torn;
  fs.torn_flush_bytes = 31;
  EXPECT_TRUE(save(fs, value)); EXPECT_EQ(Resolved, load(fs, 0).outcome);
  fs.reboot(); EXPECT_EQ(Corrupt, load(fs, 0).outcome);
  EXPECT_FALSE(load(fs, 0).shouldResume());
}

TEST(IncidentDurability, RecoveryCountCutsAdvanceOnlyTheCommittedBudget) {
  DurableIncidentFS initial; ASSERT_TRUE(save(initial, unresolved()));
  auto next = load(initial, 0).value; countRecovery(next);
  sweepCuts(initial, next, [](DurableIncidentFS& fs, const DurableIncidentFS::Event&) {
    const auto result = load(fs, 0);
    EXPECT_EQ(UncertainClock, result.outcome);
    EXPECT_EQ(fs.durable_files.count(path(1)) ? 1u : 0u, result.value.recovery_count);
    EXPECT_EQ(HealthNodeConfig::response_timeout_ms, result.elapsed_ms);
  });
}

TEST(IncidentDurability, RapidResetsBeforePersistenceCannotConsumeTheSavedBudget) {
  DurableIncidentFS fs; ASSERT_TRUE(save(fs, unresolved()));
  for (unsigned reset = 0; reset < 12; ++reset) {
    auto result = load(fs, 0); ASSERT_TRUE(result.shouldResume());
    countRecovery(result.value); EXPECT_EQ(1u, result.value.recovery_count);
    // The companion has not saved this transition before the next reset.
    fs.reboot(); EXPECT_EQ(0u, load(fs, 0).value.recovery_count);
  }
  for (unsigned saved = 1; saved <= uncertain_recovery_limit; ++saved) {
    auto result = load(fs, 0); ASSERT_TRUE(result.shouldResume());
    countRecovery(result.value); ASSERT_TRUE(save(fs, result.value)); fs.reboot();
    EXPECT_EQ(saved, load(fs, 0).value.recovery_count);
  }
  auto limited = load(fs, 0); EXPECT_EQ(RecoveryLimit, limited.outcome);
  EXPECT_FALSE(limited.shouldResume());
  limited.value.active = false; ASSERT_TRUE(save(fs, limited.value)); fs.reboot();
  EXPECT_EQ(Resolved, load(fs, 0).outcome);
}

TEST(IncidentDurability, FailedRecoveryPersistenceCannotClaimBudgetProgress) {
  DurableIncidentFS fs; ASSERT_TRUE(save(fs, unresolved()));
  for (unsigned reset = 0; reset < 6; ++reset) {
    auto value = load(fs, 0).value; countRecovery(value);
    fs.fail_write_open = true; EXPECT_FALSE(save(fs, value)); fs.reboot();
    EXPECT_EQ(0u, load(fs, 0).value.recovery_count);
    fs.fail_write_open = false;
  }
}

TEST(IncidentDurability, DamagedDurableResolutionCannotReviveTheOlderActiveRecord) {
  DurableIncidentFS fs; auto value = unresolved(); ASSERT_TRUE(save(fs, value));
  value.active = false; ASSERT_TRUE(save(fs, value));
  // Native tests use the existing noncryptographic SHA256 stand-in. Damage the
  // stored checksum directly; this asserts rejection, not hash strength.
  fs.durable_files[path(1)][checksum_offset] ^= 1;
  fs.reboot(); EXPECT_EQ(Corrupt, load(fs, 0).outcome);
  EXPECT_FALSE(load(fs, 0).shouldResume());
}

TEST(IncidentDurability, GenerationRolloverResolutionSurvivesEveryCutPoint) {
  DurableIncidentFS initial; auto value = unresolved(); ASSERT_TRUE(save(initial, value));
  auto& bytes = initial.durable_files[path(0)];
  put32(bytes.data() + 4, UINT32_MAX);
  mesh::Utils::sha256(bytes.data() + checksum_offset, 16, bytes.data(), checksum_offset);
  initial.reboot(); value.active = false;
  sweepCuts(initial, value, [](DurableIncidentFS& fs, const DurableIncidentFS::Event&) {
    if (fs.durable_files.count(path(1))) {
      EXPECT_EQ(0u, read(fs, 1).generation);
      EXPECT_EQ(Resolved, load(fs, 0).outcome);
    } else EXPECT_EQ(UncertainClock, load(fs, 0).outcome);
  });
}
}
