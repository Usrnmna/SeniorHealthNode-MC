#include "MyMesh.h"
#include "CompanionSettings.h"

// Apply only changed peripherals. A failed operation is rolled back by the
// caller before another command is accepted; the saved preferences remain old.
bool MyMesh::applySettings(const NodePrefs& next, const NodePrefs& previous) {
  const bool radio_changed = next.freq != previous.freq || next.bw != previous.bw ||
      next.sf != previous.sf || next.cr != previous.cr ||
      next.tx_power_dbm != previous.tx_power_dbm || next.rx_boosted_gain != previous.rx_boosted_gain ||
      next.radio_fem_rxgain != previous.radio_fem_rxgain;
  if (radio_changed) {
    radio_driver.prepareConfiguration();
#if ENV_INCLUDE_MPU6050
    if (!radio_driver.setParamsChecked(next.freq, next.bw, next.sf, next.cr)) return false;
#else
    radio_driver.setParams(next.freq, next.bw, next.sf, next.cr);
#endif
    if (!radio_driver.setTxPowerChecked(next.tx_power_dbm)) return false;
    if (next.rx_boosted_gain != previous.rx_boosted_gain &&
        !radio_driver.setRxBoostedGainMode(next.rx_boosted_gain)) return false;
    if (next.radio_fem_rxgain != previous.radio_fem_rxgain &&
        !board.setLoRaFemLnaEnabled(next.radio_fem_rxgain)) return false;
  }
  if (next.gps_enabled != previous.gps_enabled &&
      !sensors.setSettingValue("gps", next.gps_enabled ? "1" : "0")) return false;
  if (next.gps_interval != previous.gps_interval) {
    char interval[16]; snprintf(interval, sizeof(interval), "%lu", (unsigned long)next.gps_interval);
    if (!sensors.setSettingValue("gps_interval", interval)) return false;
  }
  if (next.adc_multiplier != previous.adc_multiplier && !board.setAdcMultiplier(next.adc_multiplier)) return false;
  return true;
}

uint8_t MyMesh::commitSettings(NodePrefs& candidate, char* reply) {
  if (!CompanionSettings::valid(candidate, MAX_LORA_TX_POWER) ||
      (candidate.isRepeatEn() && !isValidClientRepeatFreq(uint32_t(lroundf(candidate.freq * 1000))))) {
    strcpy(reply, "Error: invalid companion settings or disallowed client-repeat frequency"); return 1;
  }
#if ENV_INCLUDE_MPU6050
  if (calibrationIncidentBusy() || gyro_calibration.active()) {
    strcpy(reply, "Error: fall/help incident or calibration active; retry when idle"); return 3;
  }
#endif
  if (_mgr->getOutboundTotal() || radio_driver.configurationBusy()) {
    strcpy(reply, "Error: radio/queue busy; retry when idle"); return 3;
  }
  const NodePrefs previous(_prefs);
  if (!applySettings(candidate, previous)) {
    const bool restored = applySettings(previous, candidate);
    strcpy(reply, restored ? "Error: apply failed; previous settings restored" :
        "Error: apply and rollback failed; hardware state uncertain; reboot required");
    return 4;
  }
  if (!_store->savePrefs(candidate)) {
    const bool restored = applySettings(previous, candidate);
    strcpy(reply, restored ? "Error: save failed; previous settings restored" :
        "Error: save and rollback failed; hardware state uncertain; reboot required");
    return restored ? 2 : 4;
  }
  _prefs = candidate;
  if (candidate.node_lat != previous.node_lat) sensors.node_lat = candidate.node_lat;
  if (candidate.node_lon != previous.node_lon) sensors.node_lon = candidate.node_lon;
  strcpy(reply, candidate.ble_pin != previous.ble_pin ? "OK: saved; PIN applies after reboot" : "OK: saved and applied");
  return 0;
}

bool MyMesh::handleSettingsCommand(const char* command, char* reply) {
  if (!strcmp(command, "help") || !strncmp(command, "help ", 5)) {
    const char* topic = command[4] ? command + 5 : "";
    if (!*topic) {
      usb_reply.append("Commands: get <key>, set <key> <value>, health, motion, calibrate, battery, beep\n"
          "Help topics: get, set, health, motion, calibrate\n");
    } else if (!strcmp(topic, "get") || !strcmp(topic, "set")) {
      usb_reply.append("get/set: name (username), radio, freq, bw, sf, cr, tx, repeat, radio.rxgain, radio.fem.rxgain, af, dutycycle, rxdelay, multi.acks\n"
          "get/set: lat, lon, gps, gps_interval, advert.loc, telemetry.base, telemetry.loc, telemetry.env\n"
          "get/set: manual.add.contacts, autoadd.config, autoadd.max.hops, path.hash.mode, default.scope, pin, adc.multiplier, fall.channel, calibrate\n"
          "Read only: public.key, role. Use help set <key> for bounds and units.\n");
    } else if (!strncmp(topic, "set ", 4)) {
      NodePrefs candidate(_prefs); char detail[160] = {};
      const char* key = topic + 4;
      if (!strcmp(key, "name") || !strcmp(key, "username"))
        strcpy(detail, "Name: 1..31 UTF-8 bytes; spaces allowed; optional enclosing double quotes");
      else if (!strcmp(key, "adc.multiplier")) strcpy(detail, "Multiplier: 0..10; 0 restores board default");
      else if (!strcmp(key, "calibrate")) strcpy(detail, "Actions: start, save, cancel, reset; see help calibrate");
      else if (!strcmp(key, "fall.channel")) strcpy(detail, "Channel: public #name or private Name <16/32-byte hex or Base64 key>");
      else CompanionSettings::access(candidate, key, "?", detail, MAX_LORA_TX_POWER);
      usb_reply.append("%s\nUnits: freq=MHz, bw=kHz, tx=dBm, gps_interval=s, lat/lon=degrees. radio=MHz,kHz,SF,CR\n", detail);
    } else if (!strcmp(topic, "health")) {
      usb_reply.append("health [detail|timing|errors|radio|stats reset]\nReset affects motion/timing/error statistics only; faults, alarms and since-startup max_gap_ms remain.\n");
    } else if (!strcmp(topic, "motion")) {
      usb_reply.append("motion [raw|stats|stats raw|stats reset]\nStatistics: fresh samples since startup/reset; min/max/mean/sample standard deviation. No trailing-window history.\n");
    } else if (!strcmp(topic, "calibrate")) {
      usb_reply.append("calibrate [start|status|show|show saved|save|cancel|reset]\nStart pauses detection for countdown, stationary 15s measurement and validation. Save is explicit.\n");
    } else usb_reply.append("Error: unknown help topic\n");
    reply[0] = 0; return true;
  }
  const bool set = !strncmp(command, "set ", 4), get = !strncmp(command, "get ", 4);
  if (!set && !get) return false;
  // Health-specific commands retain their shared custom-variable implementation.
  const char* start = command + 4;
  const char* space = strchr(start, ' ');
  const size_t key_size = space ? size_t(space - start) : strlen(start);
  char key[40];
  if (!key_size || key_size >= sizeof(key)) { strcpy(reply, "Error: invalid setting name"); return true; }
  memcpy(key, start, key_size); key[key_size] = 0;
  if (!strcmp(key, "adc.multiplier") || !strcmp(key, "fall.channel") || !strcmp(key, "calibrate")) return false;
  if ((get && space) || (set && (!space || !space[1]))) {
    strcpy(reply, "Error: use get <key> or set <key> <value>"); return true;
  }
  if (!strcmp(key, "role") || !strcmp(key, "public.key")) {
    if (set) strcpy(reply, "Error: read-only setting");
    else if (!strcmp(key, "role")) strcpy(reply, "companion (BLE + health monitoring)");
    else mesh::Utils::toHex(reply, self_id.pub_key, PUB_KEY_SIZE);
    return true;
  }
  NodePrefs candidate(_prefs);
  if (!strcmp(key, "radio.fem.rxgain") && !board.canControlLoRaFemLna()) {
    strcpy(reply, "Error: external LNA control unavailable on this board"); return true;
  }
  if (!CompanionSettings::access(candidate, key, set ? space + 1 : nullptr, reply, MAX_LORA_TX_POWER)) return true;
  if (set) commitSettings(candidate, reply);
  return true;
}
