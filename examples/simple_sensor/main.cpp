#include "SensorMesh.h"
#if ENV_INCLUDE_MPU6050
#include "MotionRule.h"
#include "FallResponse.h"
#include "FallAckSender.h"
#include "LowBatteryAlert.h"
#include <helpers/ui/AlarmBuzzer.h>

AlarmBuzzer alarm_buzzer; // Include AlarmBuzzer.h elsewhere to call alarm_buzzer.beep().
static_assert(HealthNodeConfig::buzzer_pin != ENV_PIN_SDA &&
              HealthNodeConfig::buzzer_pin != ENV_PIN_SCL &&
              HealthNodeConfig::buzzer_pin != PIN_USER_BTN, "Buzzer pin conflicts with sensor/PRG");
static_assert(HealthNodeConfig::charging_status_pin < 0 ||
              (HealthNodeConfig::charging_status_pin != ENV_PIN_SDA &&
               HealthNodeConfig::charging_status_pin != ENV_PIN_SCL &&
               HealthNodeConfig::charging_status_pin != PIN_USER_BTN), "Charging pin conflicts with sensor/PRG");
#endif

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(display);
#endif

class MyMesh : public SensorMesh {
#if ENV_INCLUDE_MPU6050
  MotionRule motion_rule;
  Trigger motion_alert;
  FallResponse fall_response;
  LowBatteryAlert battery_alert;
#endif
public:
  void pollLowBattery() {
#if ENV_INCLUDE_MPU6050
    using namespace HealthNodeConfig;
    board.pollBattery();
    if (!board.batteryReadingReady()) return; // Startup has no voltage yet, not 0 V.
    const uint32_t now = millis();
    const uint16_t battery_mv = board.getBattMilliVolts();
    const bool charging = charging_status_pin >= 0 &&
        digitalRead(charging_status_pin) == (charging_status_active_low ? LOW : HIGH);
    const bool due = battery_alert.update(now, battery_mv, charging);
    if (!battery_alert.isActive()) alarm_buzzer.stopLowBattery(); // Never stop SOS.
    else if (due && alarm_buzzer.startLowBattery()) battery_alert.markPlayed(now);
#endif
  }

  void pollFallResponse() {
#if ENV_INCLUDE_MPU6050
    const uint32_t now = millis();
    if (fall_response.updateButton(now, digitalRead(PIN_USER_BTN) == LOW)) {
      alarm_buzzer.stop();
      Serial.println("Fall acknowledged: SOS stopped; channel message pending");
    }
    if (fall_response.messageDue(now)) {
      const auto result = FallAckSender::send(*this, sensors, getNodeName());
      fall_response.messageAttempted(now, result != FallAckSender::NoPacket);
      if (result == FallAckSender::Queued)
        Serial.printf("Fall acknowledgement queued to %s (delivery unconfirmed)\n", HealthNodeConfig::ack_channel_name);
      else if (result == FallAckSender::InvalidText)
        Serial.println("Fall acknowledgement NOT queued: sender + message + location exceeds 160 UTF-8 bytes");
      else if (!fall_response.messagePending())
        Serial.println("Fall acknowledgement NOT queued: packet allocation retries exhausted");
    }
    alarm_buzzer.loop();
#endif
  }

  void pollMotion() {
#if ENV_INCLUDE_MPU6050
    const uint32_t now = millis();
    const bool fresh = sensors.motion.poll(now);
    // Sensor validity alone gates analysis; notification state cannot blind it.
    if (!sensors.motion.valid) {
      motion_rule.update(now, 0, 0, false);
      return;
    }
    if (!fresh) return;
    const auto& v = sensors.motion.values;
    if (motion_rule.update(now, v.acceleration(), v.rotation(), true)) {
      // Coalesce notifications without resetting SOS/button progress or cancelling
      // an earlier direct alert. MotionRule still enforces its normal cooldown.
      if (fall_response.onFall(now, digitalRead(PIN_USER_BTN) == LOW))
        alarm_buzzer.startSOS();
      char text[120];
      // Report the detected sequence's peaks, not the later quiet sample.
      snprintf(text, sizeof(text), "Possible fall: peak accel=%.2fg rotation=%.1fdeg/s",
               motion_rule.peakAcceleration(), motion_rule.peakRotation());
      const bool coalesced = isAlertPending(motion_alert);
      if (!coalesced) {
        alertIf(false, motion_alert, HIGH_PRI_ALERT, "");
        alertIf(true, motion_alert, HIGH_PRI_ALERT, text);
      }
      Serial.printf("%s (%s)\n", text,
                    coalesced ? "notification coalesced with pending alert" :
                    isAlertPending(motion_alert) ? "queued" : "alert queue full");
    }
#endif
  }
  MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
     : SensorMesh(board, radio, ms, rng, rtc, tables), 
       battery_data(12*24, 5*60)    // 24 hours worth of battery data, every 5 minutes
  {
  }

protected:
  /* ========================== custom logic here ========================== */
  Trigger low_batt, critical_batt;
  TimeSeriesData  battery_data;

  void onSensorDataRead() override {
#if ENV_INCLUDE_MPU6050
    if (!board.batteryReadingReady()) return;
#endif
    float batt_voltage = getVoltage(TELEM_CHANNEL_SELF);

    battery_data.recordData(getRTCClock(), batt_voltage);   // record battery
    alertIf(batt_voltage < 3.4f, critical_batt, HIGH_PRI_ALERT, "Battery is critical!");
    alertIf(batt_voltage < 3.6f, low_batt, LOW_PRI_ALERT, "Battery is low");
  }

  int querySeriesData(uint32_t start_secs_ago, uint32_t end_secs_ago, MinMaxAvg dest[], int max_num) override {
    battery_data.calcMinMaxAvg(getRTCClock(), start_secs_ago, end_secs_ago, &dest[0], TELEM_CHANNEL_SELF, LPP_VOLTAGE);
    return 1;
  }

  bool handleCustomCommand(uint32_t sender_timestamp, char* command, char* reply) override {
#if ENV_INCLUDE_MPU6050
    if (strcmp(command, "beep") == 0) {
      strcpy(reply, alarm_buzzer.beep() ? "Beep started" : "Beep unavailable or alarm active");
      return true;
    }
    if (strcmp(command, "motion") == 0) {
      const auto& v = sensors.motion.values;
      if (!sensors.motion.valid) strcpy(reply, "MPU6050 unavailable (SDA=4 SCL=6 addr=0x68)");
      else snprintf(reply, 160, "a[g]=%.3f,%.3f,%.3f gyro[dps]=%.1f,%.1f,%.1f temp[C]=%.1f",
                    v.ax, v.ay, v.az, v.gx, v.gy, v.gz, v.temperature);
      return true;
    }
#endif
    if (strcmp(command, "magic") == 0) {    // example 'custom' command handling
      strcpy(reply, "**Magic now done**");
      return true;   // handled
    }
    return false;  // not handled
  }
  /* ======================================================================= */
};

StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

void halt() {
  while (1) ;
}

static char command[160];

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();
#if ENV_INCLUDE_MPU6050
  pinMode(PIN_USER_BTN, INPUT_PULLUP); // PRG is active LOW on Heltec V4.
  if (!alarm_buzzer.begin()) Serial.println("Buzzer PWM initialization failed");
  if (HealthNodeConfig::charging_status_pin >= 0)
    pinMode(HealthNodeConfig::charging_status_pin,
            HealthNodeConfig::charging_status_active_low ? INPUT_PULLUP : INPUT_PULLDOWN);
#endif

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.begin();
#endif

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) { halt(); }

  fast_rng.begin(radio_driver.getRngSeed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Sensor ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;

  sensors.begin();
#if ENV_INCLUDE_MPU6050
  Wire1.setTimeOut(HealthNodeConfig::sensor_i2c_timeout_ms);
#endif

  the_mesh.begin(fs);

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif
}

void loop() {
  the_mesh.pollMotion(); // Give due samples first service at every loop boundary.
  the_mesh.pollFallResponse(); // Service button/tone before serial, radio and sensors.
  the_mesh.pollLowBattery();
  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
    }
    Serial.print(c);
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    command[len - 1] = 0;  // replace newline with C string null terminator
    char reply[160];
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

    command[0] = 0;  // reset command buffer
  }

  the_mesh.loop();
  the_mesh.pollFallResponse(); // Catch edges promptly after radio work.
  the_mesh.pollMotion();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();
#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.loop();
#endif
}
