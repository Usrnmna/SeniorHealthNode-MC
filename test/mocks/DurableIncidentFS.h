#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// Host fault model for the filesystem interface used by FallIncidentStore.
// File data visible to readback and data surviving reboot are deliberately
// separate. A completed normal flush commits one file; ignored/torn flushes
// model failures that the Arduino void flush/close interface cannot report.
// These are selectable assumptions, not a model of a particular flash chip.
class DurableIncidentFS {
public:
  using Bytes = std::vector<uint8_t>;
  using Files = std::map<std::string, Bytes>;
  enum class Operation { OpenRead, OpenWrite, Read, Write, Flush, CloseRead, CloseWrite };
  enum class Phase { Before, After };
  enum class FlushMode { Commit, Ignore, Torn };
  struct Event {
    Operation operation;
    Phase phase;
    std::string path;
  };
  struct PowerLoss {};

  Files volatile_files, durable_files;
  std::vector<Event> events;
  FlushMode flush_mode = FlushMode::Commit;
  size_t write_limit = size_t(-1), torn_flush_bytes = 0;
  bool fail_write_open = false, fail_read_open = false, fail_read = false;
  bool fail_verification_open = false;
  // Some filesystems can make truncation durable before later data. The default
  // model keeps it volatile until flush; both alternatives are exercised.
  bool durable_truncate = false;
  unsigned writes = 0, flushes = 0;
  size_t cut_at_event = size_t(-1);

  // Start counting cut points without changing either storage layer.
  void beginTrace(size_t cut = size_t(-1)) {
    events.clear(); cut_at_event = cut;
  }

  // Discard all uncommitted state. Failure knobs are retained so tests can model
  // repeated faults explicitly; beginTrace() disables an already-used cut.
  void reboot() {
    volatile_files = durable_files;
    cut_at_event = size_t(-1);
  }

  bool exists(const char* path) const { return volatile_files.count(path) != 0; }

  struct File {
    DurableIncidentFS* fs;
    std::string path;
    bool writable, opened;
    explicit operator bool() const { return opened; }
    size_t size() const { return fs->volatile_files.at(path).size(); }
    size_t read(uint8_t* destination, size_t length) {
      fs->event(Operation::Read, Phase::Before, path);
      const auto& bytes = fs->volatile_files.at(path);
      length = fs->fail_read ? 0 : std::min(length, bytes.size());
      if (length) memcpy(destination, bytes.data(), length);
      fs->event(Operation::Read, Phase::After, path);
      return length;
    }
    size_t write(const uint8_t* source, size_t length) {
      fs->event(Operation::Write, Phase::Before, path);
      ++fs->writes;
      length = std::min(length, fs->write_limit);
      fs->volatile_files[path].assign(source, source + length);
      fs->event(Operation::Write, Phase::After, path);
      return length;
    }
    void flush() {
      fs->event(Operation::Flush, Phase::Before, path);
      ++fs->flushes;
      if (fs->flush_mode == FlushMode::Commit) {
        fs->durable_files[path] = fs->volatile_files.at(path);
      } else if (fs->flush_mode == FlushMode::Torn) {
        const auto& source = fs->volatile_files.at(path);
        const size_t count = std::min(source.size(), fs->torn_flush_bytes);
        fs->durable_files[path].assign(source.begin(), source.begin() + count);
      }
      fs->event(Operation::Flush, Phase::After, path);
    }
    void close() {
      const auto operation = writable ? Operation::CloseWrite : Operation::CloseRead;
      fs->event(operation, Phase::Before, path);
      opened = false;
      fs->event(operation, Phase::After, path);
    }
  };

  File open(const char* path, const char* mode) {
    const bool writable = mode[0] == 'w';
    const auto operation = writable ? Operation::OpenWrite : Operation::OpenRead;
    event(operation, Phase::Before, path);
    const bool failed = writable ? fail_write_open :
        fail_read_open || (fail_verification_open && writes != 0) || !exists(path);
    if (!failed && writable) {
      volatile_files[path].clear();
      if (durable_truncate) durable_files[path].clear();
    }
    event(operation, Phase::After, path);
    return {this, path, writable, !failed};
  }

private:
  void event(Operation operation, Phase phase, const std::string& path) {
    events.push_back({operation, phase, path});
    if (events.size() - 1 == cut_at_event) {
      reboot();
      throw PowerLoss{};
    }
  }
};
