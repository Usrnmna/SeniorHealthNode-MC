#pragma once
#include "FallAckMessage.h"

namespace FallAckLocation {
// Request the local MeshCore GPS provider's latest reported fix before each send.
// Drain available input, but do not enable/reset GPS or wait for a satellite fix.
// Templated only so the same query can be tested with a simulated provider.
template <typename SensorSource>
bool read(SensorSource& sensors, double& latitude, double& longitude) {
  latitude = longitude = 0;
  auto provider = sensors.getLocationProvider();
  if (!provider || !provider->isEnabled()) return false;
  provider->loop();
  if (!provider->isValid()) return false;
  // LocationProvider uses millionths of a degree, not degrees or telemetry floats.
  latitude = double(provider->getLatitude()) / 1000000.0;
  longitude = double(provider->getLongitude()) / 1000000.0;
  return FallAckMessage::validCoordinates(latitude, longitude);
}
}
