#pragma once
#include <CayenneLPP.h>
#include "TelemetrySnapshot.h"

// Replays encoded scalar environmental fields without floating-point requantizing.
// Neither snapshot publication nor replay performs sensor I/O. GPS and MPU data
// are added separately by the manager, so telemetry permissions remain intact.
class EnvironmentTelemetry : public CayenneLPP {
public:
  EnvironmentTelemetry() : CayenneLPP(TelemetrySnapshot::capacity) { }
  void append(const TelemetrySnapshot& snapshot, CayenneLPP& dest) {
    for (unsigned i = 0; i + 2 <= snapshot.size;) {
      const unsigned field_start = i;
      const uint8_t channel = snapshot.bytes[i++], type = snapshot.bytes[i++];
      const uint8_t size = getTypeSize(type);
      if ((size != 1 && size != 2 && size != 4) || i + size > snapshot.size) return;
      const unsigned offset = dest.getSize();
      // CayenneLPP has no appendRaw API. Reserve the required scalar width via
      // its checked public API, then replace the complete field with its exact
      // channel/type/value bytes. Never change its cursor or bypass capacity.
      if (size == 1) dest.addRelativeHumidity(channel, 0);
      else if (size == 2) dest.addTemperature(channel, 0);
      else dest.addGenericSensor(channel, 0);
      if (dest.getSize() != offset + size + 2) return;
      memcpy(dest.getBuffer() + offset, snapshot.bytes + field_start, size + 2);
      i += size;
    }
  }
};
