#pragma once
#include <stdint.h>

// EDIT HERE, then rebuild heltec_v4_sensor. No alarm state is saved to flash.
namespace HealthNodeConfig {
// External passive buzzer via a 3.3 V-compatible driver; GPIO is a signal only.
constexpr int buzzer_pin = 47;          // Free header GPIO on the V4 OLED target.
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

// Sampling/recovery and background environmental collection (sensor target only).
constexpr uint32_t motion_sample_ms = 20;
constexpr uint8_t motion_fast_failures = 3; // Consecutive failed reads before reinitialization.
constexpr uint32_t motion_retry_ms = 20;
constexpr uint32_t motion_missing_retry_ms = 5000;
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

constexpr uint32_t click_window_ms = 3000; // First down -> third up, inclusive.
constexpr uint32_t long_press_ms = 1000;   // Matches heltec_v4/target.cpp; reject >=.
constexpr uint32_t debounce_ms = 25;       // Each press AND release must settle.

// Channel identity is its shared key, NOT a companion's local channel slot.
// Public hashtag channel: first 16 bytes of SHA256("#falldetect"), including '#'.
// For a private channel replace BOTH label and key with the recipient's settings.
constexpr char ack_channel_name[] = "#falldetect"; // Diagnostic label; key below selects channel.
constexpr uint8_t ack_channel_key[] = {
  0xe9, 0x2d, 0xdc, 0xbf, 0x92, 0x23, 0x43, 0xdf,
  0xf1, 0x81, 0xfb, 0x49, 0x48, 0x37, 0x12, 0x36
}; // 16 or 32 decoded key bytes; do not paste base64 text here.
constexpr char ack_message[] = u8"Fall Detected & User Has Requested Assistance";
constexpr char DefaultLocation[] = u8"HOME"; // Appended when the GPS provider has no valid location.
constexpr uint8_t ack_path_hash_size = 1; // Flood route: 1, 2, or 3 bytes per hop.
constexpr uint32_t ack_retry_ms = 5000;  // Retry LOCAL packet allocation only.
constexpr uint8_t ack_max_attempts = 60; // No automatic RF delivery retries/ACKs.

static_assert(morse_dot_ms > 0 && sos_silence_ms > 0, "Buzzer durations must be positive");
static_assert(low_battery_poll_ms > 0 && low_battery_repeat_ms > 0, "Invalid battery timing");
static_assert(battery_settle_ms > 0 && battery_adc_samples > 0, "Invalid ADC timing/count");
static_assert(motion_sample_ms > 0 && motion_fast_failures > 0 && motion_retry_ms > 0 &&
              motion_missing_retry_ms > 0, "Invalid motion retry timing");
static_assert(environment_refresh_ms > 0 && environment_stale_ms > environment_refresh_ms &&
              environment_conversion_timeout_ms > 80 && sensor_i2c_timeout_ms > 0,
              "Invalid environment timing");
static_assert(charging_status_pin < 0 || charging_status_pin != buzzer_pin, "Charging/buzzer pin conflict");
static_assert(debounce_ms > 0 && debounce_ms < long_press_ms, "Invalid debounce duration");
static_assert(click_window_ms > 0 && ack_retry_ms > 0 && ack_max_attempts > 0, "Invalid timing");
static_assert(sizeof(ack_channel_key) == 16 || sizeof(ack_channel_key) == 32, "Key must be 16 or 32 bytes");
static_assert(ack_path_hash_size >= 1 && ack_path_hash_size <= 3, "Invalid path hash size");
}
