#pragma once
#include <stdint.h>

// EDIT HERE, then rebuild heltec_v4_companion_radio_ble. No alarm state is saved to flash.
// Channel selection can override the defaults below via USB/BLE: set fall.channel.
namespace HealthNodeConfig {
// DIYables passive module: VCC=3V3, GND=GND, I/O=buzzer_pin; onboard driver.
// https://diyables.io/products/passive-buzzer-module
// GPIO supplies PWM only during tones; silent gaps disconnect PWM and set idle level.
// Optional output: false skips all buzzer GPIO/PWM setup and playback.
// Sensor monitoring, PRG responses and channel alerts remain enabled without a buzzer.
constexpr bool buzzer_enabled = true;
constexpr int buzzer_pin = 47;          // Free header GPIO on the V4 OLED target.
// true: LOW-level-trigger module (silent HIGH). false: HIGH-active driver (silent LOW).
// LOW idle is selected for the DIYables PWM configuration. The product/tutorial
// specifies PWM but not idle polarity; electrical behavior requires bench validation.
// Match the driver hardware, then rebuild and flash; wrong polarity energizes idle periods.
constexpr bool buzzer_active_low = false;
constexpr uint8_t buzzer_pwm_channel = 0; // Reserved ESP32 LEDC channel (Arduino 2.x).
constexpr uint32_t buzzer_frequency_hz = 2000;
constexpr uint32_t beep_ms = 150;
constexpr uint32_t morse_dot_ms = 150;  // Dash = 3 dots; letter gap = 3 dots.
constexpr uint32_t sos_silence_ms = 5000; // Last S ends -> next S starts.

// Low battery: three Morse-length long beeps, then repeat start-to-start.
constexpr uint16_t low_battery_mv = 3500; // Start below; clear above; equality keeps state.
constexpr uint32_t low_battery_poll_ms = 1000;
constexpr uint32_t battery_settle_ms = 10; // ADC divider power-up; loop keeps running.
constexpr uint8_t battery_adc_samples = 8; // One conversion per loop pass.
constexpr uint32_t battery_calibration_max_age_ms = 2500;
constexpr uint16_t battery_calibration_min_mv = 3300; // Meter input: one Li-ion cell.
constexpr uint16_t battery_calibration_max_mv = 4200;

// Sampling/recovery and background environmental collection (combined BLE companion).
constexpr uint32_t motion_sample_ms = 20;
constexpr uint8_t motion_fast_failures = 3; // Consecutive failed reads before reinitialization.
constexpr uint32_t motion_retry_ms = 20;
constexpr uint32_t motion_missing_retry_ms = 5000;
constexpr uint32_t motion_data_timeout_ms = 1000; // Bus works but no new sensor output.

// Stationary gyroscope calibration. Any fixed orientation is valid; gravity is retained.
// Quality limits are initial engineering values requiring actual-board validation.
constexpr uint32_t gyro_calibration_prepare_ms = 3000;
constexpr uint32_t gyro_calibration_measure_ms = 15000;
constexpr uint32_t gyro_calibration_validate_ms = 2000;
constexpr uint32_t gyro_calibration_timeout_ms = 60000;
constexpr uint32_t gyro_calibration_candidate_ms = 120000;
constexpr uint32_t gyro_calibration_max_gap_ms = 100;
constexpr uint32_t gyro_calibration_min_samples = 600; // 80% of nominal 50 Hz over 15 s.
constexpr uint32_t gyro_calibration_validation_samples = 80;
constexpr float gyro_calibration_gravity_tolerance_g = 0.15f;
constexpr float gyro_calibration_accel_span_g = 0.04f; // Per-axis span within a window.
constexpr float gyro_calibration_gyro_span_dps = 1.0f;
constexpr float gyro_calibration_noise_dps = 0.15f; // Maximum per-axis standard deviation.
constexpr float gyro_calibration_max_bias_dps = 20.0f;
constexpr float gyro_calibration_residual_dps = 0.30f;
constexpr float gyro_calibration_temperature_span_c = 2.0f;
constexpr uint32_t environment_refresh_ms = 60000;
constexpr uint32_t environment_stale_ms = 120000; // Omit expired/failed cache entries.
constexpr uint32_t environment_conversion_timeout_ms = 1000;
constexpr uint32_t environment_task_stack_bytes = 6144;
constexpr uint16_t sensor_i2c_timeout_ms = 5; // Per transaction, not whole measurement.
constexpr uint8_t oled_chunk_bytes = 32; // About 0.8 ms of data at 400 kHz.
constexpr uint32_t low_battery_repeat_ms = 10UL * 60 * 1000;
constexpr uint32_t long_beep_ms = 3 * morse_dot_ms; // Same duration as an SOS dash.
constexpr uint32_t low_battery_gap_ms = morse_dot_ms;
constexpr uint32_t low_battery_tones_hz[] = {2093, 1760, 1480};
// Stock Heltec V4 has no MCU charging-status connection. -1 disables sensing.
// Configure ONLY for a connected, conditioned 3.3 V digital charging signal.
// Active must mean battery is charging, not merely that a USB host is present.
constexpr int charging_status_pin = -1;
constexpr bool charging_status_active_low = true;

constexpr uint32_t click_window_ms = 3000; // First down -> third up, inclusive; double waits until closed.
constexpr uint32_t false_alarm_click_window_ms = 1000; // First down -> second up, inclusive.
constexpr uint32_t long_press_ms = 1000;   // Matches heltec_v4/target.cpp; reject >=.
constexpr uint32_t debounce_ms = 25;       // Each press AND release must settle.

// POST-FALL DIRECTIVES -- response deadline and final continuous quiet period.
constexpr uint32_t response_timeout_ms = 30000; // Starts when a fall is declared.
constexpr uint32_t residual_quiet_required_ms = 10000;
constexpr uint32_t residual_max_sample_gap_ms = 100; // Missing data is UNKNOWN, never quiet.
// Initial tuning values, requiring worn-device testing (including buzzer vibration).
constexpr float residual_acceleration_change_g = 0.08f; // Vector change from quiet baseline.
constexpr float residual_gravity_tolerance_g = 0.08f; // Allowed magnitude deviation from 1 g.
constexpr float residual_rotation_dps = 5.0f; // Rotation at/above this value breaks quiet.
// Store plain text only. The formatter adds " | " ONLY when appending text.
constexpr char unconfirmed_message[] = u8"USER COULD NOT CONFIRM THEIR CONDITION";
constexpr char no_movement_message[] = u8"NO RESIDUAL MOVEMENTS DETECTED; POSSIBLE INCAPACITATION OF USER";
constexpr char confirmed_message[] = u8"USER HAS CONFIRMED FALL";
constexpr char false_alarm_message[] = u8"User Confirms False Alarm"; // Formatter appends location.

// Channel identity is its shared key, NOT a companion's local channel slot.
// Public hashtag channel: first 16 bytes of SHA256("#falldetect"), including '#'.
// Runtime selection: set fall.channel public #name, or private Name <shared-key>.
// These compiled values apply only when no saved channel selection exists.
constexpr char ack_channel_name[] = "#falldetect"; // Diagnostic label; key below selects channel.
constexpr uint8_t ack_channel_key[] = {
  0xe9, 0x2d, 0xdc, 0xbf, 0x92, 0x23, 0x43, 0xdf,
  0xf1, 0x81, 0xfb, 0x49, 0x48, 0x37, 0x12, 0x36
}; // 16 or 32 decoded key bytes; do not paste base64 text here.
constexpr char ack_message[] = u8"Fall Detected & User Has Requested Assistance";
constexpr char DefaultLocation[] = u8"HOME"; // Appended when the GPS provider has no valid location.
constexpr uint8_t ack_path_hash_size = 1; // Flood route: 1, 2, or 3 bytes per hop.
constexpr uint32_t ack_retry_ms = 5000;  // Nonblocking local/channel retry spacing.
constexpr uint8_t ack_max_attempts = 60; // Total allocation/enqueue attempts per channel event.
constexpr uint32_t assistance_queue_timeout_ms = 30000; // Drop stale QUEUED packets, then retry.
constexpr uint32_t channel_repeat_wait_ms = 15000; // After TX, wait to hear a matching repeater echo.
constexpr uint8_t channel_transmit_attempts = 4; // Successful local transmissions without hearing a repeat.
constexpr uint32_t motion_fault_timeout_ms = 5000; // No fresh valid motion -> visible fault.
constexpr unsigned serial_rx_bytes_per_loop = 16; // Bound serial input/echo work per companion loop.

static_assert(morse_dot_ms > 0 && sos_silence_ms > 0, "Buzzer durations must be positive");
static_assert(low_battery_poll_ms > 0 && low_battery_repeat_ms > 0, "Invalid battery timing");
static_assert(battery_settle_ms > 0 && battery_adc_samples > 0, "Invalid ADC timing/count");
static_assert(battery_calibration_max_age_ms > 0 && battery_calibration_min_mv > 0 &&
              battery_calibration_min_mv < battery_calibration_max_mv,
              "Invalid battery calibration limits");
static_assert(motion_sample_ms > 0 && motion_fast_failures > 0 && motion_retry_ms > 0 &&
              motion_data_timeout_ms >= motion_sample_ms &&
              motion_missing_retry_ms > 0, "Invalid motion retry timing");
static_assert(gyro_calibration_prepare_ms > 0 && gyro_calibration_measure_ms > 0 &&
              gyro_calibration_validate_ms > 0 && gyro_calibration_candidate_ms > 0 &&
              gyro_calibration_timeout_ms > gyro_calibration_prepare_ms + gyro_calibration_measure_ms +
                  gyro_calibration_validate_ms && gyro_calibration_timeout_ms < 0x80000000UL &&
              gyro_calibration_candidate_ms < 0x80000000UL &&
              gyro_calibration_max_gap_ms >= motion_sample_ms &&
              gyro_calibration_max_gap_ms < gyro_calibration_validate_ms,
              "Invalid gyroscope calibration timing");
static_assert(gyro_calibration_min_samples > 1 && gyro_calibration_validation_samples > 1 &&
              gyro_calibration_min_samples <= gyro_calibration_measure_ms / motion_sample_ms + 1 &&
              gyro_calibration_validation_samples <= gyro_calibration_validate_ms / motion_sample_ms + 1 &&
              gyro_calibration_gravity_tolerance_g > 0 && gyro_calibration_accel_span_g > 0 &&
              gyro_calibration_gyro_span_dps > 0 && gyro_calibration_noise_dps > 0 &&
              gyro_calibration_max_bias_dps > 0 && gyro_calibration_residual_dps > 0 &&
              gyro_calibration_temperature_span_c > 0, "Invalid gyroscope calibration quality limits");
static_assert(environment_refresh_ms > 0 && environment_stale_ms > environment_refresh_ms &&
              environment_conversion_timeout_ms > 80 && sensor_i2c_timeout_ms > 0,
              "Invalid environment timing");
static_assert(charging_status_pin < 0 || charging_status_pin != buzzer_pin, "Charging/buzzer pin conflict");
static_assert(debounce_ms > 0 && debounce_ms < long_press_ms, "Invalid debounce duration");
static_assert(click_window_ms > 0 && ack_retry_ms > 0 && ack_max_attempts > 0, "Invalid timing");
static_assert(false_alarm_click_window_ms > 0 && false_alarm_click_window_ms < click_window_ms &&
              click_window_ms < 0x80000000UL, "Invalid false-alarm/confirmation windows");
static_assert(response_timeout_ms > 0 && response_timeout_ms < 0x80000000UL &&
              residual_quiet_required_ms > 0 && residual_quiet_required_ms <= response_timeout_ms &&
              residual_max_sample_gap_ms >= motion_sample_ms &&
              residual_max_sample_gap_ms < residual_quiet_required_ms, "Invalid post-fall timing");
static_assert(residual_acceleration_change_g > 0 && residual_gravity_tolerance_g > 0 &&
              residual_rotation_dps > 0, "Invalid residual movement thresholds");
static_assert(assistance_queue_timeout_ms > 0 && assistance_queue_timeout_ms < 0x80000000UL &&
              channel_repeat_wait_ms > 0 && channel_repeat_wait_ms < 0x80000000UL &&
              channel_transmit_attempts > 0 && channel_transmit_attempts <= ack_max_attempts &&
              ack_retry_ms < 0x80000000UL && motion_fault_timeout_ms > 100 &&
              motion_fault_timeout_ms < 0x80000000UL, "Invalid delivery/health deadlines");
static_assert(serial_rx_bytes_per_loop > 0 && serial_rx_bytes_per_loop <= 159, "Invalid serial work budget");
static_assert(sizeof(ack_channel_key) == 16 || sizeof(ack_channel_key) == 32, "Key must be 16 or 32 bytes");
static_assert(ack_path_hash_size >= 1 && ack_path_hash_size <= 3, "Invalid path hash size");
}
