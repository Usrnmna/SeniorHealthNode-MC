#pragma once

// Preserve ESP32 startup behavior and ordering, including format-on-failure.
// The Boolean result does not reveal whether formatting occurred. Tests call
// this production boundary; they do not simulate the SDK's mount implementation.
template<class FS, class Store, class Network>
void startCompanionStorage(FS& fs, Store& store, Network& network, bool has_display) {
  network.setStorageStartupResult(fs.begin(true));
  store.begin();
  network.begin(has_display);
}
