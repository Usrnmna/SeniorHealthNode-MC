#pragma once
#include "FallIncidentStore.h"

#if defined(ESP32) && !defined(HEALTH_MONITOR_HOST_HEADER)
#include <esp_spiffs.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace FallIncidentStore {
#if defined(ESP32) && !defined(HEALTH_MONITOR_HOST_HEADER)
// Read-only operations on the companion's existing default SPIFFS partition.
// No mount, formatting, retries or recovery decision is performed here.
struct IncidentReadPosix {
  bool mounted() const { return esp_spiffs_mounted(nullptr); }
  int open(unsigned slot) { return ::open(slot ? "/spiffs/fall_inc1" : "/spiffs/fall_inc0", O_RDONLY); }
  int size(int fd, size_t& length) {
    struct stat info;
    if (::fstat(fd, &info) != 0) return -1;
    if (!S_ISREG(info.st_mode) || info.st_size < 0) { errno = EIO; return -1; }
    length = size_t(info.st_size); return 0;
  }
  int read(int fd, uint8_t* bytes, size_t length) { return int(::read(fd, bytes, length)); }
  int close(int fd) { return ::close(fd); }
};
#endif

// One open/stat/read/close sequence per slot, with no internal retry. A short
// read is uncertain, while a successfully inspected wrong length is corrupt.
// Only ENOENT observed between mounted checks qualifies as API-level absence.
// Before/after checks do not make concurrent filesystem changes atomic.
template<class IO> class CheckedIncidentReader {
  IO& io;
public:
  explicit CheckedIncidentReader(IO& value) : io(value) {}
  Record operator()(unsigned slot) {
    Record result;
    auto& status = result.io;
    status.slot = uint8_t(slot); status.api_checked = true;
    if (!io.mounted()) { status.stage = ReadStage::Mount; status.error = ENODEV; return result; }
    errno = 0;
    const int fd = io.open(slot);
    if (fd < 0) {
      const int error = errno ? errno : EIO;
      // Mount qualification is needed for absence; other open failures retain
      // their first native error even if storage subsequently becomes unavailable.
      if (error != ENOENT) { status.stage = ReadStage::Open; status.error = error; }
      else if (!io.mounted()) { status.stage = ReadStage::Mount; status.error = ENODEV; }
      else status.stage = ReadStage::Missing;
      return result;
    }
    result.exists = true;
    uint8_t bytes[record_size] = {};
    size_t length = 0;
    status.stage = ReadStage::Success;
    errno = 0;
    if (io.size(fd, length) != 0) {
      status.stage = ReadStage::Stat; status.error = errno ? errno : EIO;
    } else if (length != record_size) {
      status.stage = ReadStage::Invalid;
    } else {
      errno = 0;
      if (io.read(fd, bytes, sizeof(bytes)) != int(sizeof(bytes))) {
        status.stage = ReadStage::Read; status.error = errno ? errno : EIO;
      }
    }
    errno = 0;
    if (io.close(fd) != 0) {
      status.close_error = errno ? errno : EIO;
      if (!status.uncertain()) { status.stage = ReadStage::Close; status.error = status.close_error; }
    }
    if (!io.mounted() && !status.uncertain()) {
      status.stage = ReadStage::Mount; status.error = ENODEV;
    }
    result.readable = !status.uncertain();
    if (status.stage == ReadStage::Success) {
      decodeRecord(bytes, result);
      if (!result.valid) status.stage = ReadStage::Invalid;
    }
    return result;
  }
};
}
