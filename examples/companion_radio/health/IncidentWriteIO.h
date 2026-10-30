#pragma once
#include "FallIncidentStore.h"
#include <cerrno>
#include <cstdio>

// Incident writes on the companion's existing /spiffs VFS mount. The pinned
// ESP-IDF 4.4.7 SPIFFS backend has no fsync callback. fflush checks the C stream
// buffer; fclose reaches SPIFFS_close, which reports its cache-flush failure.
// These API results and subsequent readback do not prove power-cut durability.
namespace FallIncidentStore {
struct IncidentStdio {
  FILE* open(unsigned slot) { return std::fopen(slot ? "/spiffs/fall_inc1" : "/spiffs/fall_inc0", "wb"); }
  size_t write(FILE* file, const uint8_t* bytes, size_t length) { return std::fwrite(bytes, 1, length, file); }
  int flush(FILE* file) { return std::fflush(file); }
  int close(FILE* file) { return std::fclose(file); }
};

// Narrow injected I/O boundary: tests execute this same sequencing with fake
// file handles. No retry occurs inside a write; every opened handle is closed
// once, even after a partial write or flush failure. Retrying a failed close is
// unsafe because the API may already have released its handle.
template<class IO> class CheckedIncidentWriter {
  IO& io;
public:
  explicit CheckedIncidentWriter(IO& value) : io(value) {}
  bool apiChecked() const { return true; }
  SaveResult operator()(unsigned slot, const uint8_t* bytes, size_t length) {
    SaveResult result; result.slot = uint8_t(slot); result.api_checked = true;
    errno = 0;
    auto file = io.open(slot);
    if (!file) { result.stage = SaveStage::Open; result.error = errno ? errno : EIO; return result; }
    result.stage = SaveStage::Success;
    errno = 0;
    if (io.write(file, bytes, length) != length) {
      result.stage = SaveStage::Write; result.error = errno ? errno : EIO;
    }
    errno = 0;
    if (io.flush(file) != 0 && result.success()) {
      result.stage = SaveStage::Flush; result.error = errno ? errno : EIO;
    }
    errno = 0;
    if (io.close(file) != 0) {
      result.close_error = errno ? errno : EIO;
      if (result.success()) { result.stage = SaveStage::Close; result.error = result.close_error; }
    }
    return result;
  }
};
}
