#pragma once
#include <Stream.h>

namespace PreferenceStore {
// Detect every short write, including an intermediate failure followed by a
// successful closing brace. Verification reproduces the serialized bytes.
template<class File> class CheckedStream : public Stream {
  File& file;
  bool compare;
public:
  bool good = true;
  CheckedStream(File& destination, bool readback) : file(destination), compare(readback) {}
  size_t write(uint8_t value) override {
    const bool ok = compare ? file.read() == value : file.write(&value, 1) == 1;
    good = good && ok; return ok ? 1 : 0;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

// Stage and verify before replacing the committed preferences. A backup covers
// interruption between the two renames; startup restores it when needed.
template<class FS, class Prefs> bool save(FS& fs, Prefs& prefs) {
  const char* active = "/prefs.json";
  const char* backup = "/prefs.bak";
  const char* pending = "/prefs.tmp";
  if (!fs.exists(active) && fs.exists(backup) && !fs.rename(backup, active)) return false;
  auto file = fs.open(pending, "w");
  if (!file) return false;
  CheckedStream<decltype(file)> output(file, false);
  const bool written = prefs.saveSerial(output) && output.good;
  file.flush(); file.close();
  if (!written) { fs.remove(pending); return false; }
  file = fs.open(pending, "r");
  if (!file) return false;
  CheckedStream<decltype(file)> verify(file, true);
  const bool verified = prefs.saveSerial(verify) && verify.good && file.available() == 0;
  file.close();
  if (!verified) { fs.remove(pending); return false; }
  if (fs.exists(backup) && !fs.remove(backup)) return false;
  const bool had_active = fs.exists(active);
  if (had_active && !fs.rename(active, backup)) return false;
  if (!fs.rename(pending, active)) {
    if (had_active) fs.rename(backup, active);
    return false;
  }
  // A stale backup is harmless: the active file always takes precedence.
  if (had_active) fs.remove(backup);
  return true;
}
} // namespace PreferenceStore
