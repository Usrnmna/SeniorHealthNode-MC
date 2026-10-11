#pragma once
#include "NodePrefs.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <type_traits>

// Shared parsing and limits for USB text and companion protocol preferences.
// Parsing changes only a candidate; hardware and persistence are separate steps.
namespace CompanionSettings {
inline bool number(const char* text, double& value) {
  if (!text || !*text) return false;
  // Decimal notation only; reject hexadecimal, NaN, infinity and hidden suffixes.
  for (const char* p = text; *p; ++p)
    if (!strchr("0123456789.+-eE", *p)) return false;
  char* end;
  value = strtod(text, &end);
  return end != text && !*end && isfinite(value);
}

// Validate complete UTF-8, including overlong encodings and surrogate codepoints.
// Control characters are excluded from names displayed in terminals and packets.
inline bool validName(const char* text, size_t size) {
  if (!size || size > 31) return false;
  bool visible = false;
  for (size_t i = 0; i < size;) {
    uint32_t cp = uint8_t(text[i++]);
    unsigned extra = 0; uint32_t minimum = 0;
    if (cp >= 0xC2 && cp <= 0xDF) { cp &= 31; extra = 1; minimum = 0x80; }
    else if (cp >= 0xE0 && cp <= 0xEF) { cp &= 15; extra = 2; minimum = 0x800; }
    else if (cp >= 0xF0 && cp <= 0xF4) { cp &= 7; extra = 3; minimum = 0x10000; }
    else if (cp >= 0x80) return false;
    if (i + extra > size) return false;
    while (extra--) {
      const uint8_t c = uint8_t(text[i++]);
      if ((c & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (c & 63);
    }
    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) ||
        cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return false;
    if (cp != ' ') visible = true;
  }
  return visible;
}

inline bool setName(NodePrefs& prefs, const char* text, size_t size) {
  if (!validName(text, size)) return false;
  memcpy(prefs.node_name, text, size); prefs.node_name[size] = 0;
  return true;
}

template<class T> bool scalar(T& field, const char* value, double low, double high, char* reply) {
  if (!value) { snprintf(reply, 160, "%.9g", double(field)); return true; }
  double parsed;
  if (!number(value, parsed) || parsed < low || parsed > high ||
      (std::is_integral<T>::value && floor(parsed) != parsed)) {
    snprintf(reply, 160, "Error: value must be %s in %.9g..%.9g",
        std::is_integral<T>::value ? "an integer" : "a finite number", low, high);
    return false;
  }
  field = T(parsed); return true;
}

inline bool boolean(uint8_t& field, const char* value, char* reply) {
  if (value && !strcmp(value, "on")) value = "1";
  if (value && !strcmp(value, "off")) value = "0";
  return scalar(field, value, 0, 1, reply);
}

inline bool bandwidth(double bw) {
  const double allowed[] = {7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125, 250, 500};
  for (double v : allowed) if (fabs(v - bw) < 0.0001) return true;
  return false;
}

// This target uses SX1262 limits. Client-repeat frequency policy is checked by
// MyMesh against the firmware's existing allowed ranges before committing.
inline bool validRadio(const NodePrefs& p, int max_tx) {
  return isfinite(p.freq) && p.freq >= 150 && p.freq <= 960 && bandwidth(p.bw) &&
      p.sf >= 5 && p.sf <= 12 && p.cr >= 5 && p.cr <= 8 &&
      p.tx_power_dbm >= -9 && p.tx_power_dbm <= max_tx && p.rx_boosted_gain <= 1 && p.radio_fem_rxgain <= 1;
}

inline bool valid(const NodePrefs& p, int max_tx) {
  return validRadio(p, max_tx) && validName(p.node_name, strnlen(p.node_name, 32)) &&
      isfinite(p.node_lat) && p.node_lat >= -90 && p.node_lat <= 90 &&
      isfinite(p.node_lon) && p.node_lon >= -180 && p.node_lon <= 180 &&
      isfinite(p.rx_delay_base) && p.rx_delay_base >= 0 && p.rx_delay_base <= 20 &&
      isfinite(p.airtime_factor) && p.airtime_factor >= 0 && p.airtime_factor <= 99 &&
      isfinite(p.adc_multiplier) && p.adc_multiplier >= 0 && p.adc_multiplier <= 10 &&
      p.telemetry_mode_base <= 2 && p.telemetry_mode_loc <= 2 && p.telemetry_mode_env <= 2 &&
      p.manual_add_contacts <= 1 && p.multi_acks <= 3 && p.advert_loc_policy <= 1 &&
      p.path_hash_mode <= 2 && p.autoadd_max_hops <= 64 && p.gps_enabled <= 1 &&
      p.gps_interval <= 86400 && (p.ble_pin == 0 || (p.ble_pin >= 100000 && p.ble_pin <= 999999));
}

// Return false for invalid/unsupported input with an explanatory reply. A null
// value requests a read. Units and bounds are kept beside each writable field.
inline bool access(NodePrefs& p, const char* key, const char* value, char* reply, int max_tx) {
  if (!strcmp(key, "username") || !strcmp(key, "name")) {
    if (!value) { snprintf(reply, 160, "%s", p.node_name); return true; }
    size_t size = strlen(value);
    if (size && value[0] == '"') {
      if (size < 2 || value[size-1] != '"') { strcpy(reply, "Error: unmatched name quotes"); return false; }
      ++value; size -= 2;
    }
    if (setName(p, value, size)) return true;
    strcpy(reply, "Error: name requires 1..31 UTF-8 bytes without control characters"); return false;
  }
  if (!strcmp(key, "radio")) {
    if (!value) { snprintf(reply, 160, "%.3f,%.3f,%u,%u", p.freq, p.bw, p.sf, p.cr); return true; }
    char parts[96];
    if (strlen(value) >= sizeof(parts)) { strcpy(reply, "Error: radio value too long"); return false; }
    strcpy(parts, value); char* part = parts; double v[4];
    for (unsigned i = 0; i < 4; ++i) {
      char* comma = strchr(part, ',');
      if ((i < 3) != (comma != nullptr)) { strcpy(reply, "Error: radio requires MHz,kHz,SF,CR"); return false; }
      if (comma) *comma = 0;
      if (!number(part, v[i])) { strcpy(reply, "Error: invalid radio number"); return false; }
      part = comma ? comma + 1 : part;
    }
    if (v[0] < 150 || v[0] > 960 || !bandwidth(v[1]) || v[2] < 5 || v[2] > 12 ||
        v[3] < 5 || v[3] > 8 || floor(v[2]) != v[2] || floor(v[3]) != v[3]) {
      strcpy(reply, "Error: unsupported SX1262 radio parameters"); return false;
    }
    p.freq = v[0]; p.bw = v[1]; p.sf = v[2]; p.cr = v[3]; return true;
  }
  if (!strcmp(key, "repeat")) {
    uint8_t enabled = p.isRepeatEn();
    if (!boolean(enabled, value, reply)) return false;
    if (value) p.setRepeatEn(enabled != 0);
    return true;
  }
  if (!strcmp(key, "dutycycle")) {
    double percent = 100.0 / (1 + p.airtime_factor);
    if (!scalar(percent, value, 1, 100, reply)) return false;
    if (value) p.airtime_factor = 100.0 / percent - 1;
    return true;
  }
  if (!strcmp(key, "pin")) {
    if (!scalar(p.ble_pin, value, 0, 999999, reply)) return false;
    if (!value || p.ble_pin == 0 || p.ble_pin >= 100000) return true;
    strcpy(reply, "Error: pin must be 0 or six digits (100000..999999)"); return false;
  }
  if (!strcmp(key, "default.scope")) {
    if (!value) {
      snprintf(reply, 160, "%s; key=%s", p.default_scope_name[0] ? p.default_scope_name : "none",
          p.default_scope_name[0] ? "configured (redacted)" : "none"); return true;
    }
    if (!strcmp(value, "none")) {
      memset(p.default_scope_name, 0, sizeof(p.default_scope_name));
      memset(p.default_scope_key, 0, sizeof(p.default_scope_key)); return true;
    }
    const char* split = strrchr(value, ' ');
    if (!split || split == value || size_t(split-value) > 30 || strlen(split+1) != 32 ||
        !validName(value, size_t(split-value))) {
      strcpy(reply, "Error: default.scope requires none or Name <16-byte hex key>"); return false;
    }
    uint8_t key_bytes[16];
    for (unsigned i = 0; i < 32; ++i) {
      const char c = split[1+i];
      const int digit = c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 :
          c >= 'A' && c <= 'F' ? c-'A'+10 : -1;
      if (digit < 0) { strcpy(reply, "Error: scope key must contain 32 hexadecimal digits"); return false; }
      if (!(i & 1)) key_bytes[i/2] = uint8_t(digit << 4);
      else key_bytes[i/2] |= uint8_t(digit);
    }
    memcpy(p.default_scope_name, value, split-value); p.default_scope_name[split-value] = 0;
    memcpy(p.default_scope_key, key_bytes, sizeof(key_bytes)); return true;
  }
  // Field mappings keep USB spelling independent of the on-disk schema.
#define NUM_SETTING(name, field, low, high) if (!strcmp(key, name)) return scalar(p.field, value, low, high, reply)
#define BOOL_SETTING(name, field) if (!strcmp(key, name)) return boolean(p.field, value, reply)
  NUM_SETTING("freq", freq, 150, 960);
  NUM_SETTING("bw", bw, 7.8, 500);
  NUM_SETTING("sf", sf, 5, 12);
  NUM_SETTING("cr", cr, 5, 8);
  NUM_SETTING("tx", tx_power_dbm, -9, max_tx);
  NUM_SETTING("af", airtime_factor, 0, 99);
  NUM_SETTING("rxdelay", rx_delay_base, 0, 20);
  NUM_SETTING("multi.acks", multi_acks, 0, 3);
  NUM_SETTING("lat", node_lat, -90, 90);
  NUM_SETTING("lon", node_lon, -180, 180);
  NUM_SETTING("path.hash.mode", path_hash_mode, 0, 2);
  NUM_SETTING("gps_interval", gps_interval, 1, 86400);
  NUM_SETTING("telemetry.base", telemetry_mode_base, 0, 2);
  NUM_SETTING("telemetry.loc", telemetry_mode_loc, 0, 2);
  NUM_SETTING("telemetry.env", telemetry_mode_env, 0, 2);
  NUM_SETTING("autoadd.config", autoadd_config, 0, 255);
  NUM_SETTING("autoadd.max.hops", autoadd_max_hops, 0, 64);
  BOOL_SETTING("radio.rxgain", rx_boosted_gain);
  BOOL_SETTING("radio.fem.rxgain", radio_fem_rxgain);
  BOOL_SETTING("gps", gps_enabled);
  BOOL_SETTING("advert.loc", advert_loc_policy);
  BOOL_SETTING("manual.add.contacts", manual_add_contacts);
#undef NUM_SETTING
#undef BOOL_SETTING
  const char* unsupported[] = {"int.thresh", "cad", "agc.reset.interval", "allow.read.only",
    "flood.advert.interval", "advert.interval", "guest.password", "txdelay", "direct.txdelay",
    "flood.max", "flood.max.advert", "flood.max.unscoped", "owner.info", "loop.detect",
    "extra.sf", "bootloader.ver"};
  for (const char* name : unsupported) if (!strcmp(key, name)) {
    strcpy(reply, "Error: unsupported by this companion firmware/board"); return false;
  }
  if (!strncmp(key, "bridge.", 7) || !strncmp(key, "pwrmgt.", 7)) {
    strcpy(reply, "Error: unsupported by this companion firmware/board"); return false;
  }
  if (!strcmp(key, "prv.key")) {
    strcpy(reply, "Error: private identity keys are not exposed through USB text commands"); return false;
  }
  strcpy(reply, "Error: unknown setting; use help get or help set"); return false;
}
} // namespace CompanionSettings
