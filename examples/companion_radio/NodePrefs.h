#pragma once
#include <cstdint> // For uint8_t, uint32_t
#include <helpers/ConfigSerializer.h>

#define TELEM_MODE_DENY            0
#define TELEM_MODE_ALLOW_FLAGS     1     // use contact.flags
#define TELEM_MODE_ALLOW_ALL       2

#define ADVERT_LOC_NONE       0
#define ADVERT_LOC_SHARE      1

class NodePrefs : public ConfigSerializer {  // persisted to file
public:
  float airtime_factor = 0;
  char node_name[32];
  double node_lat = 0, node_lon = 0;
  float freq = 0;
  uint8_t sf = 0;
  uint8_t cr = 0;
  uint8_t multi_acks = 0;
  uint8_t manual_add_contacts = 0;
  float bw = 0;
  int8_t tx_power_dbm = 0;
  uint8_t telemetry_mode_base = 0;
  uint8_t telemetry_mode_loc = 0;
  uint8_t telemetry_mode_env = 0;
  float rx_delay_base = 0;
  uint32_t ble_pin = 0;
  float adc_multiplier = 0;    // Zero selects board default; absent in older preferences.
  uint8_t  advert_loc_policy = 0;
  uint8_t  buzzer_quiet = 0;
  uint8_t  vibe_quiet = 0;
  uint8_t  gps_enabled = 0;      // GPS enabled flag (0=disabled, 1=enabled)
  uint32_t gps_interval = 0;     // GPS read interval in seconds
  uint8_t autoadd_config = 0;    // bitmask for auto-add contacts config
  uint8_t rx_boosted_gain = 0; // SX126x RX boosted gain mode (0=power saving, 1=boosted)
  uint8_t radio_fem_rxgain = 1; // External LNA; independent of SX1262 boosted RX gain.
  uint8_t _client_repeat = 0;  // DEPRECATED -> use repeat.disable_fwd
  uint8_t path_hash_mode = 0;    // which path mode to use when sending
  uint8_t autoadd_max_hops = 0;  // 0 = no limit, 1 = direct (0 hops), N = up to N-1 hops (max 64)
  char default_scope_name[31];
  uint8_t default_scope_key[16];

private:
  class RadioPrefs : public ConfigSerializer {  // COPIED from CommonCLI (for now)
    NodePrefs* _parent;
  protected:
    void structure() override {
      def("freq", _parent->freq);
      def("bw", _parent->bw);
      def("sf", _parent->sf);
      def("cr", _parent->cr);
      //def("cad", _parent->cad_enabled);
      //def("int_thr", _parent->interference_threshold);
      def("rxgain", _parent->rx_boosted_gain);
      // Earlier records duplicated rxgain under fem_rxgain; fem_lna is independent.
      def("fem_lna", _parent->radio_fem_rxgain);
      def("tx", _parent->tx_power_dbm);
      def("af", _parent->airtime_factor);
      def("rxdelay", _parent->rx_delay_base);
      //def("f_txdelay", _parent->tx_delay_factor);   currently hard-coded
      //def("d_txdelay", _parent->direct_tx_delay_factor);  currently hard-coded
      //def("agc_int", _parent->agc_reset_interval);
      def("hash_mode", _parent->path_hash_mode);
      def("multi_ack", _parent->multi_acks);
    }
  public:
    RadioPrefs(NodePrefs* parent) : _parent(parent) { }
  };
  RadioPrefs radio;

  class GPSPrefs : public ConfigSerializer {  // COPIED from CommonCLI (for now)
    NodePrefs* _parent;
  protected:
    void structure() override {
      def("en", _parent->gps_enabled); // boolean
      def("int", _parent->gps_interval);   // interval in seconds
      def("adv_loc", _parent->advert_loc_policy);
    }
  public:
    GPSPrefs(NodePrefs* parent) : _parent(parent) { }
  };
  GPSPrefs gps;

  class RepeatPrefs : public ConfigSerializer {  // COPIED from CommonCLI (for now)
  public:
    uint8_t disable_fwd = 1;
  protected:
    void structure() override {
      def("disable", disable_fwd);
      //def("f_max", flood_max);
      //def("f_max_uns", flood_max_unscoped);
      //def("f_max_adv", flood_max_advert);
      //def("loop", loop_detect);
    }
  };
  RepeatPrefs repeat;

  class CompanionPrefs : public ConfigSerializer {
    NodePrefs* _parent;
  protected:
    void structure() override {
      def("auto_max", _parent->autoadd_max_hops);  // 0 = no limit, 1 = direct (0 hops), N = up to N-1 hops (max 64)
      def("defs_nm", _parent->default_scope_name, sizeof(_parent->default_scope_name));
      def("defs_key", (void *) _parent->default_scope_key, sizeof(_parent->default_scope_key));
      def("pin", _parent->ble_pin);
      def("buzz_q", _parent->buzzer_quiet);
      def("vibe_q", _parent->vibe_quiet);
      def("auto_add", _parent->autoadd_config);    // bitmask for auto-add contacts config
      def("man_add", _parent->manual_add_contacts);
      def("tel_base", _parent->telemetry_mode_base);
      def("tel_loc", _parent->telemetry_mode_loc);
      def("tel_env", _parent->telemetry_mode_env);
    }
  public:
    CompanionPrefs(NodePrefs* parent) : _parent(parent) { }
  };
  CompanionPrefs companion;

protected:
  void structure() override {
    def("name", node_name, sizeof(node_name));
    //def("adv_int", advert_interval);
    //def("f_adv_int", flood_advert_interval);
    def("lat", node_lat);
    def("lon", node_lon);
    def("adc_mult", adc_multiplier);
    def("radio", radio);
    def("gps", gps);
    def("repeat", repeat);
    def("comp", companion);
  }
public:
  NodePrefs() : radio(this), gps(this), companion(this) {
    node_name[0] = 0;
    default_scope_name[0] = 0;
    memset(default_scope_key, 0, sizeof(default_scope_key));
  }
  // Serializer sections retain pointers to their owning preferences. Copy only
  // values so a staged transaction cannot serialize or modify another instance.
  NodePrefs(const NodePrefs& other) : NodePrefs() { *this = other; }
  NodePrefs& operator=(const NodePrefs& other) {
    airtime_factor = other.airtime_factor;
    memcpy(node_name, other.node_name, sizeof(node_name));
    node_lat = other.node_lat; node_lon = other.node_lon;
    freq = other.freq; bw = other.bw; sf = other.sf; cr = other.cr;
    multi_acks = other.multi_acks; manual_add_contacts = other.manual_add_contacts;
    tx_power_dbm = other.tx_power_dbm;
    telemetry_mode_base = other.telemetry_mode_base;
    telemetry_mode_loc = other.telemetry_mode_loc; telemetry_mode_env = other.telemetry_mode_env;
    rx_delay_base = other.rx_delay_base; ble_pin = other.ble_pin;
    adc_multiplier = other.adc_multiplier; advert_loc_policy = other.advert_loc_policy;
    buzzer_quiet = other.buzzer_quiet; vibe_quiet = other.vibe_quiet;
    gps_enabled = other.gps_enabled; gps_interval = other.gps_interval;
    autoadd_config = other.autoadd_config; rx_boosted_gain = other.rx_boosted_gain;
    radio_fem_rxgain = other.radio_fem_rxgain;
    _client_repeat = other._client_repeat; path_hash_mode = other.path_hash_mode;
    autoadd_max_hops = other.autoadd_max_hops;
    memcpy(default_scope_name, other.default_scope_name, sizeof(default_scope_name));
    memcpy(default_scope_key, other.default_scope_key, sizeof(default_scope_key));
    setRepeatEn(other.isRepeatEn());
    return *this;
  }
  // new accessor methods
  bool isRepeatEn() const { return repeat.disable_fwd == 0; }
  void setRepeatEn(bool en) { repeat.disable_fwd = en ? 0 : 1; }
};
