#include <gtest/gtest.h>
#include <AssistanceTestLink.h>
#include <map>
#include <string>
#include <vector>
#include <limits>
#define ESP32
#include <helpers/sensors/MPU6050.h>
#include <MpuFifoFake.h>
#include "../../examples/companion_radio/CompanionSettings.h"
#include "../../examples/companion_radio/PreferenceStore.h"
#include "../../examples/companion_radio/UsbReply.h"
#include "../../examples/companion_radio/health/MotionStatistics.h"
#include "../../examples/companion_radio/health/MotionRule.h"

namespace {
NodePrefs defaults() {
  NodePrefs p; strcpy(p.node_name, "Node"); p.freq = 869.618f; p.bw = 62.5f;
  p.sf = 8; p.cr = 5; p.tx_power_dbm = 22; return p;
}
TEST(CompanionSettings, NumericInputMustBeCompleteFiniteAndDecimal) {
  double n;
  for (auto invalid : {"", "nan", "inf", "1e999", "1.2x", "0x12", "1 2", "--1", "1\n", "1,2"})
    EXPECT_FALSE(CompanionSettings::number(invalid, n)) << invalid;
  EXPECT_TRUE(CompanionSettings::number("-1.25e2", n)); EXPECT_EQ(-125, n);
}
TEST(CompanionSettings, NamesHaveOneSharedValueAndQuotesAreOptional) {
  auto p = defaults(); char reply[160] = {};
  ASSERT_TRUE(CompanionSettings::access(p, "username", "\"Medical Node 2\"", reply, 22));
  ASSERT_TRUE(CompanionSettings::access(p, "name", nullptr, reply, 22));
  EXPECT_STREQ("Medical Node 2", reply);
  ASSERT_TRUE(CompanionSettings::access(p, "name", "Node 3", reply, 22));
  EXPECT_STREQ("Node 3", p.node_name);
}
TEST(CompanionSettings, RejectsOversizedOrMalformedUtf8WithoutTruncation) {
  auto p = defaults(); char reply[160];
  for (auto invalid : {"\"missing", "", "   ", "bad\nname", "\xC0\xAF", "\xED\xA0\x80",
                       "\xF4\x90\x80\x80", "\xE2\x82", "12345678901234567890123456789012"}) {
    EXPECT_FALSE(CompanionSettings::access(p, "username", invalid, reply, 22));
    EXPECT_STREQ("Node", p.node_name);
  }
  EXPECT_TRUE(CompanionSettings::setName(p, "Node \xC3\xA9", 7));
  const char embedded[] = {'a', 0, 'b'};
  EXPECT_FALSE(CompanionSettings::setName(p, embedded, sizeof(embedded)));
}
TEST(CompanionSettings, GroupRadioParsingIsAllOrNothing) {
  auto p = defaults(); char reply[160];
  for (auto invalid : {"900,125,7", "900,125,7,5,9", "900,125,7.5,5", "1000,125,7,5", "900,63,7,5"}) {
    EXPECT_FALSE(CompanionSettings::access(p, "radio", invalid, reply, 22));
    EXPECT_FLOAT_EQ(869.618f, p.freq); EXPECT_EQ(8, p.sf);
  }
  ASSERT_TRUE(CompanionSettings::access(p, "radio", "915.125,125,7,5", reply, 22));
  EXPECT_FLOAT_EQ(915.125f, p.freq); EXPECT_EQ(7, p.sf);
  EXPECT_TRUE(CompanionSettings::valid(p, 22));
}
TEST(CompanionSettings, IndependentRadioFieldsRetainOtherParameters) {
  auto p = defaults(); char reply[160];
  ASSERT_TRUE(CompanionSettings::access(p, "freq", "915.125", reply, 22));
  EXPECT_FLOAT_EQ(62.5f, p.bw); EXPECT_EQ(8, p.sf);
  EXPECT_FALSE(CompanionSettings::access(p, "sf", "6.5", reply, 22));
  EXPECT_FALSE(CompanionSettings::access(p, "tx", "23", reply, 22));
  ASSERT_TRUE(CompanionSettings::access(p, "bw", "63", reply, 22));
  EXPECT_FALSE(CompanionSettings::valid(p, 22));
}
TEST(CompanionSettings, DutycycleAndAirtimeFactorAgreeAcrossFullRange) {
  auto p = defaults(); char reply[160];
  ASSERT_TRUE(CompanionSettings::access(p, "dutycycle", "1", reply, 22));
  EXPECT_FLOAT_EQ(99, p.airtime_factor); EXPECT_TRUE(CompanionSettings::valid(p, 22));
  ASSERT_TRUE(CompanionSettings::access(p, "dutycycle", "100", reply, 22));
  EXPECT_FLOAT_EQ(0, p.airtime_factor);
  EXPECT_FALSE(CompanionSettings::access(p, "dutycycle", "0", reply, 22));
}
TEST(CompanionSettings, TypedRangesAndBooleansRejectSilentClamping) {
  auto p = defaults(); char reply[160];
  EXPECT_FALSE(CompanionSettings::access(p, "telemetry.base", "3", reply, 22));
  EXPECT_FALSE(CompanionSettings::access(p, "autoadd.max.hops", "65", reply, 22));
  EXPECT_FALSE(CompanionSettings::access(p, "gps_interval", "0", reply, 22));
  EXPECT_FALSE(CompanionSettings::access(p, "gps", "onward", reply, 22));
  EXPECT_FALSE(CompanionSettings::access(p, "lat", "90.01", reply, 22));
  EXPECT_TRUE(CompanionSettings::access(p, "repeat", "on", reply, 22)); EXPECT_TRUE(p.isRepeatEn());
  EXPECT_TRUE(CompanionSettings::access(p, "repeat", "off", reply, 22)); EXPECT_FALSE(p.isRepeatEn());
}
TEST(CompanionSettings, ScopeKeysAreValidatedAndRedacted) {
  auto p = defaults(); char reply[160];
  ASSERT_TRUE(CompanionSettings::access(p, "default.scope", "Local Area 00112233445566778899aabbccddeeff", reply, 22));
  ASSERT_TRUE(CompanionSettings::access(p, "default.scope", nullptr, reply, 22));
  EXPECT_NE(nullptr, strstr(reply, "Local Area")); EXPECT_EQ(nullptr, strstr(reply, "001122"));
  EXPECT_FALSE(CompanionSettings::access(p, "default.scope", "Bad 00xx", reply, 22));
  EXPECT_TRUE(CompanionSettings::access(p, "default.scope", "none", reply, 22));
  EXPECT_EQ(0, p.default_scope_name[0]); EXPECT_EQ(0, p.default_scope_key[15]);
}
TEST(CompanionSettings, UnsupportedCommandsAreExplicitAndNeverEchoSecrets) {
  auto p = defaults(); char reply[160];
  for (auto key : {"guest.password", "bridge.secret", "loop.detect", "prv.key", "nonsense"}) {
    EXPECT_FALSE(CompanionSettings::access(p, key, "SECRET", reply, 22));
    EXPECT_EQ(nullptr, strstr(reply, "SECRET")); EXPECT_NE(nullptr, strstr(reply, "Error:"));
  }
}
class TextInput : public Stream {
  const char* text;
public:
  explicit TextInput(const char* s) : text(s) {}
  int available() override { return strlen(text); }
  int read() override { return *text ? uint8_t(*text++) : -1; }
  int peek() override { return *text ? uint8_t(*text) : -1; }
};
TEST(CompanionSettings, CopiedSerializerSectionsRemainBoundToTheirOwnObject) {
  auto original = defaults(); original.gps_interval = 12; original.setRepeatEn(true);
  NodePrefs copied(original), assigned; assigned = original;
  for (NodePrefs* p : {&copied, &assigned}) {
    TextInput input("{name:\"Changed\",radio:{freq:915.125},gps:{int:22},repeat:{disable:1},comp:{pin:123456}}");
    ASSERT_TRUE(p->loadSerial(input));
    EXPECT_FLOAT_EQ(915.125f, p->freq); EXPECT_EQ(22u, p->gps_interval);
    EXPECT_EQ(123456u, p->ble_pin); EXPECT_FALSE(p->isRepeatEn());
  }
  EXPECT_STREQ("Node", original.node_name); EXPECT_FLOAT_EQ(869.618f, original.freq);
  EXPECT_EQ(12u, original.gps_interval); EXPECT_TRUE(original.isRepeatEn());
}

using Reading = MPU6050Sensor::Reading;
const Reading still{0, 0, 1, 1, 2, 2, 25};
TEST(MotionStatistics, CountsFreshSamplesAndCalculatesSampleMoments) {
  MotionStatistics s; auto second = still; second.ax = 2;
  s.observe(100, still, still, true, true, 100);
  s.observe(120, second, second, true, true, 100);
  EXPECT_EQ(2u, s.samples); EXPECT_FLOAT_EQ(1, s.fields[0].mean);
  EXPECT_FLOAT_EQ(0, s.fields[0].minimum); EXPECT_FLOAT_EQ(2, s.fields[0].maximum);
  EXPECT_NEAR(sqrtf(2), s.fields[0].standardDeviation(s.samples), 0.00001);
  EXPECT_EQ(20u, s.coverage_ms); EXPECT_FLOAT_EQ(20, s.intervals.mean);
}
TEST(MotionStatistics, DoesNotDuplicateCacheOrInsertInvalidZeros) {
  MotionStatistics s;
  s.observe(100, still, still, true, true, 100);
  s.observe(100, still, still, true, true, 100);
  s.observe(110, still, still, false, true, 100);
  s.observe(120, still, still, true, false, 100);
  auto bad = still; bad.ax = std::numeric_limits<float>::quiet_NaN();
  s.observe(130, bad, bad, true, true, 100);
  EXPECT_EQ(1u, s.samples); EXPECT_EQ(1u, s.rejected);
  EXPECT_EQ(0u, s.interval_count); EXPECT_FLOAT_EQ(1, s.fields[MotionStatistics::Az].mean);
}
TEST(MotionStatistics, GapsAndMillisWrapRemainVisible) {
  MotionStatistics s; const uint32_t start = UINT32_MAX - 50;
  s.observe(start, still, still, true, true, 100);
  s.observe(start + 20, still, still, true, true, 100);
  s.observe(start + 300, still, still, true, true, 100);
  EXPECT_EQ(300u, s.coverage_ms); EXPECT_EQ(1u, s.gaps);
  EXPECT_FLOAT_EQ(280, s.intervals.maximum); EXPECT_FLOAT_EQ(150, s.intervals.mean);
}
TEST(MotionStatistics, CorrectedAndRawGyrosAreSeparateAndResetCleanly) {
  MotionStatistics s; auto corrected = still; corrected.gx = corrected.gy = corrected.gz = 0;
  s.observe(100, corrected, still, true, true, 100);
  EXPECT_FLOAT_EQ(0, s.fields[MotionStatistics::Rotation].mean);
  EXPECT_FLOAT_EQ(3, s.fields[MotionStatistics::RawRotation].mean);
  s.reset(120, "calibration-changed"); EXPECT_EQ(0u, s.samples); EXPECT_EQ(120u, s.reset_at);
  s.observe(200, corrected, still, true, true, 100);
  EXPECT_EQ(0u, s.interval_count); EXPECT_EQ(200u, s.first_at);
}
TEST(MotionStatistics, CollectionAndResetDoNotChangeDetectorDecisions) {
  MotionRule baseline, monitored; MotionStatistics s;
  std::vector<uint32_t> falls;
  for (uint32_t now = 0; now < 7000; now += 20) {
    auto v = still; v.gx = v.gy = v.gz = 0;
    if (now == 2200) v.az = 0.2f;
    if (now == 2240) { v.az = 3; v.gx = 280; }
    const bool a = baseline.update(now, v.acceleration(), v.rotation(), true);
    const bool b = monitored.update(now, v.acceleration(), v.rotation(), true);
    EXPECT_EQ(a, b); if (a) falls.push_back(now);
    s.observe(now, v, v, true, true, 100);
    if (now % 200 == 0) s.reset(now, "test");
  }
  EXPECT_EQ(1u, falls.size()); EXPECT_FALSE(monitored.cooldownActive());
  EXPECT_TRUE(monitored.available());
}
struct SerialSink {
  int capacity = 0; size_t largest = 0, calls = 0; std::string bytes;
  int availableForWrite() { return capacity; }
  size_t write(const uint8_t* data, size_t size) {
    ++calls; largest = std::max(largest, size); bytes.append((const char*)data, size); return size;
  }
};
TEST(UsbReply, BackpressureNeverWritesAndChunksRespectCapacity) {
  UsbReply r; SerialSink sink; r.append("%0100d\n", 1);
  for (unsigned i = 0; i < 1000; ++i) r.drain(sink);
  EXPECT_EQ(0u, sink.calls); EXPECT_TRUE(r.pending());
  sink.capacity = 7; r.drain(sink); EXPECT_EQ(7u, sink.bytes.size());
  sink.capacity = 200;
  while (r.pending()) r.drain(sink);
  EXPECT_EQ(101u, sink.bytes.size()); EXPECT_EQ(32u, sink.largest);
}
TEST(UsbReply, OverflowIsReportedWithinFixedCapacity) {
  UsbReply r; SerialSink sink; sink.capacity = 100;
  const std::string oversized(3000, 'x'); r.append("%s", oversized.c_str());
  while (r.pending()) r.drain(sink);
  EXPECT_LE(sink.bytes.size(), 2047u); EXPECT_NE(std::string::npos, sink.bytes.find("capacity exceeded"));
}

struct MemoryFS {
  std::map<std::string, std::vector<uint8_t>> files;
  int fail_write_at = -1, writes = 0; bool fail_commit = false;
  struct File {
    MemoryFS* fs = nullptr; std::string path; size_t pos = 0;
    explicit operator bool() const { return fs != nullptr; }
    size_t write(const uint8_t* data, size_t count) {
      if (fs->writes++ == fs->fail_write_at) return 0;
      auto& out = fs->files[path]; out.insert(out.end(), data, data+count); return count;
    }
    int read() { auto& in = fs->files[path]; return pos < in.size() ? in[pos++] : -1; }
    int available() { return fs->files[path].size() - pos; }
    void flush() {} void close() {}
  };
  File open(const char* path, const char* mode) {
    if (*mode == 'w') files[path].clear();
    if (!exists(path)) return {};
    return {this, path, 0};
  }
  bool exists(const char* path) { return files.count(path) != 0; }
  bool remove(const char* path) { return files.erase(path) != 0; }
  bool rename(const char* from, const char* to) {
    if (!exists(from) || exists(to) || (fail_commit && !strcmp(from, "/prefs.tmp"))) return false;
    files[to] = files[from]; files.erase(from); return true;
  }
};
struct TextPrefs {
  const char* text = "{name:\"Node\"}";
  bool saveSerial(Stream& stream) {
    // Deliberately reports only the final byte, like a serializer that misses
    // intermediate short writes. CheckedStream must still reject the result.
    const size_t n = strlen(text);
    stream.write((const uint8_t*)text, n-1); return stream.write(uint8_t(text[n-1])) == 1;
  }
};
TEST(PreferenceStore, IntermediateWriteFailurePreservesCommittedPreferences) {
  MemoryFS fs; fs.files["/prefs.json"] = {'o','l','d'}; fs.fail_write_at = 3;
  TextPrefs p; EXPECT_FALSE(PreferenceStore::save(fs, p));
  EXPECT_EQ((std::vector<uint8_t>{'o','l','d'}), fs.files["/prefs.json"]);
}
TEST(PreferenceStore, FailedCommitRestoresBackup) {
  MemoryFS fs; fs.files["/prefs.json"] = {'o','l','d'}; fs.fail_commit = true;
  TextPrefs p; EXPECT_FALSE(PreferenceStore::save(fs, p));
  EXPECT_EQ((std::vector<uint8_t>{'o','l','d'}), fs.files["/prefs.json"]);
}
TEST(PreferenceStore, RecoveryThenSuccessfulSaveReplacesInterruptedTransaction) {
  MemoryFS fs; fs.files["/prefs.bak"] = {'o','l','d'};
  TextPrefs p; EXPECT_TRUE(PreferenceStore::save(fs, p));
  EXPECT_EQ(std::string(p.text), std::string(fs.files["/prefs.json"].begin(), fs.files["/prefs.json"].end()));
  EXPECT_FALSE(fs.exists("/prefs.bak")); EXPECT_FALSE(fs.exists("/prefs.tmp"));
}
TEST(SensorDiagnostics, ResetDoesNotClearFaultOrAlterRecoverySchedule) {
  MpuFifoFake hardware;
  MPU6050Sensor sensor; sensor.poll(0); EXPECT_FALSE(sensor.poll(100));
  hardware.push(); ASSERT_TRUE(sensor.poll(120));
  Wire1.read_status = 1; EXPECT_FALSE(sensor.poll(140));
  EXPECT_EQ(1u, sensor.diagnostics().bus_errors); EXPECT_FALSE(sensor.valid);
  const auto generation = sensor.discontinuityGeneration();
  sensor.resetDiagnostics(); EXPECT_FALSE(sensor.valid);
  EXPECT_EQ(generation, sensor.discontinuityGeneration());
  EXPECT_EQ(0u, sensor.diagnostics().bus_errors);
  Wire1.read_status = 0; EXPECT_FALSE(sensor.poll(159)); EXPECT_FALSE(sensor.poll(160));
  hardware.push(); EXPECT_TRUE(sensor.poll(180));
  EXPECT_EQ(1u, sensor.diagnostics().recoveries);
}
TEST(SensorDiagnostics, ShortReadAndFailedInitializationAreCounted) {
  Wire1 = TwoWire{};
  MPU6050Sensor sensor;
  EXPECT_FALSE(sensor.poll(0));
  EXPECT_EQ(1u, sensor.diagnostics().init_attempts);
  EXPECT_EQ(1u, sensor.diagnostics().init_failures);
  EXPECT_EQ(1u, sensor.diagnostics().short_reads);
  EXPECT_EQ(1u, sensor.diagnostics().bus_errors);
  sensor.resetDiagnostics();
  EXPECT_FALSE(sensor.poll(4999)); EXPECT_EQ(0u, sensor.diagnostics().init_attempts);
}
TEST(CompanionSettings, ExternalLnaRemainsIndependentOfRadioGainAfterCopy) {
  auto p = defaults(); p.rx_boosted_gain = 0; p.radio_fem_rxgain = 1;
  char reply[160];
  ASSERT_TRUE(CompanionSettings::access(p, "radio.fem.rxgain", "off", reply, 22));
  ASSERT_TRUE(CompanionSettings::access(p, "radio.rxgain", "on", reply, 22));
  NodePrefs copy(p);
  EXPECT_EQ(0, copy.radio_fem_rxgain); EXPECT_EQ(1, copy.rx_boosted_gain);
}
}
int main(int argc, char** argv) { ::testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
