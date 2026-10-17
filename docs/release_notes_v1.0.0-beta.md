# Version 1.0.0 - Beta

SeniorHealthNode adds stationary gyroscope calibration, broader USB configuration,
detailed sensor statistics, and a dedicated false-alarm response to the combined
MeshCore BLE companion and fall-monitoring firmware. Sensor freshness checks,
configuration persistence, and passive-buzzer playback also receive improvements.

These notes describe the current compiled workspace relative to
[v0.1.0-beta](https://github.com/Usrnmna/SeniorHealthNode-MC/releases/tag/v0.1.0-beta),
whose source tag resolves to `886e31505e04e6cf9368633891e124e2f21c10b8`.
The intended GitHub release tag is **`v1.0.0-beta`**.

## New features

### Stationary gyroscope calibration over USB and BLE

The MPU6050 can now estimate and correct its zero-rotation offset without changing
the device's mounting orientation. Horizontal, vertical, and tilted placement are
supported; the assembled device must remain stationary on a stable surface.

- A **3-second preparation countdown** precedes **15 continuous seconds of
  stationary measurement** and a separate **2-second validation window**.
- Movement or excessive variation restarts measurement. Sensor faults abort the
  attempt; sample-gap, noise, temperature, bias, and residual-motion checks reject
  unsuitable results.
- Validated coefficients remain pending until `calibrate save` explicitly saves
  and activates them. Unsaved results expire after 120 seconds and clear on reboot.
- Saved offsets apply to fall detection, residual-movement assessment, gyroscope
  telemetry, and corrected USB readings. `motion raw` exposes uncorrected scaled
  readings for comparison.
- Fall detection pauses during preparation, measurement, and validation, then
  resumes through normal rearm checks. BLE, USB, radio services, battery acquisition,
  and sensor-health tracking continue. Sound playback is suppressed during collection.
- Calibration is unavailable during an active fall/help incident or its pending
  delivery. Cancellation retains the previously saved correction.

USB workflow:

```text
health
motion raw
calibrate
calibrate status
calibrate show
calibrate save
motion
```

Additional controls include `calibrate show saved`, `calibrate cancel`, and
`calibrate reset`. Compatible BLE clients can use `get calibrate` and
`set calibrate start|save|cancel|reset` through local companion custom variables.

This is gyroscope offset calibration. Gravity remains in acceleration readings;
six-position accelerometer calibration, scale calibration, and automatic changes
to fall thresholds are not included.

### Expanded USB companion configuration

The USB text CLI now provides applicable MeshCore companion `get`/`set` settings,
with discoverable help and shared validation and persistence across corresponding
USB and BLE operations.

| Configuration area | Available keys |
| --- | --- |
| Identity | `username`, `name`; read-only `public.key` and `role` |
| Radio | `radio`, `freq`, `bw`, `sf`, `cr`, `tx`, `radio.rxgain`, `radio.fem.rxgain` |
| Scheduling and acknowledgements | `repeat`, `af`, `dutycycle`, `rxdelay`, `multi.acks` |
| Location and GPS | `lat`, `lon`, `gps`, `gps_interval`, `advert.loc` |
| Telemetry permissions | `telemetry.base`, `telemetry.loc`, `telemetry.env` |
| Contacts and routing | `manual.add.contacts`, `autoadd.config`, `autoadd.max.hops`, `path.hash.mode`, `default.scope` |
| Pairing and health settings | `pin`, `adc.multiplier`, existing `fall.channel`, and calibration control through `calibrate` |

For example:

```text
help
help get
help set
help set freq
get freq
get radio
get username
set username "Medical Node 2"
get name
```

`username` and `name` refer to the same saved companion name. Ordinary channel
messages and advertisements use that name; fall notifications remain nameless
group texts. Names accept spaces and valid UTF-8 within the 31-byte limit.

Numeric values must be finite and within supported limits; integer fields reject
fractions. Invalid values are rejected without silently clamping or truncating.
Boolean settings accept `on`, `off`, `1`, or `0`. The `radio` tuple is validated
as one operation. Internal SX1262 boosted receive gain and external front-end LNA
gain now have separate controls and persistence.

Preference writes stage and verify serialized bytes before replacing the active
file, retaining a backup for interrupted-rename recovery. Failed application or
save operations attempt to restore previous live settings; failed restoration
reports uncertain hardware state. Explicit setters can write flash or reconfigure
peripherals and should be treated as maintenance operations.

The companion exposes settings appropriate to its firmware role. Repeater,
server, bridge, and other unavailable upstream controls return explicit unsupported
responses. Private identity import/export is not exposed through the USB text CLI.
The [USB CLI reference](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/main/docs/usb_cli_reference.md) lists all supported keys and limits.

### Detailed health and motion statistics

USB diagnostics now expose detector state, sample timing, sensor failures, radio
activity, and aggregate motion measurements:

| Command | Information |
| --- | --- |
| `health detail` | Uptime, sensor initialization and validity, sample age, motion fault, detector phase, cooldown, calibration, alarm, and delivery states |
| `health timing` | Sample and interval counts, covered duration, latest/minimum/maximum/mean spacing in milliseconds, observed sample rate, gaps, and nonfinite-sample rejection |
| `health errors` | Bus errors, short reads, data-ready polls/timeouts, initialization failures, reinitializations, recoveries, and register/identity mismatches |
| `health radio` | Radio settings, outbound queue, busy state, packet/airtime counters, available RSSI/SNR, noise estimate, and flood/direct counters |
| `motion stats` | Corrected axis and magnitude minimum, maximum, mean, and sample standard deviation; chip-temperature statistics |
| `motion stats raw` | Uncorrected gyroscope statistics, with the same acceleration and temperature measurements |
| `motion stats reset` | Reset motion and timing aggregates |
| `health stats reset` | Reset motion/timing aggregates and sensor diagnostic counters |

Statistics include fresh, valid samples only. Invalid reads and repeated cache
queries do not add samples or synthetic zeros. Calibration changes begin a new
motion-statistics interval. Resets preserve detector state, alarms, faults, saved
calibration, radio counters, and the original since-startup gap counters.

The existing `health` field `max_gap_ms` is the largest completed interval between
fresh valid samples since startup, in milliseconds. It includes a sensor outage
once fresh data resumes. Resettable statistics provide separate measurement windows.

Collection uses fixed-size RAM aggregates after detector processing, without
sample-history storage or per-sample output. Replies are formatted incrementally;
USB output drains at most 32 bytes per loop and pauses under backpressure. USB
input retains its 16-byte-per-loop budget. No logging or streaming feature is added.
Physical timing still requires measurement on the assembled device.

### False-alarm confirmation with PRG

A detected fall now supports a distinct false-alarm gesture:

1. Press and release **PRG twice within one second**, from the first press-down
   to the second release.
2. Wait for the **three-second window from the first press** to close.
3. If no qualifying third press completes, SOS stops and the node queues
   `User Confirms False Alarm | <location>`.

A valid triple press within the three-second window takes priority and confirms
the fall/requests assistance. A started third press that is held too long or
released late cancels the double candidate and leaves SOS active. Debouncing and
held-button rejection apply to both gestures.

False-alarm acceptance cancels queued automatic warnings and initial-alert retries
for the same incident. It preserves unrelated incidents and cannot recall messages
already transmitted. The gesture remains available after automatic escalation.
A pending double press does not extend the existing 30-second response deadline.

## Improvements and fixes

### Fall-message formatting

All fall notifications omit the node-name prefix and its separator. Alert wording,
measured peaks, GPS or fallback location, warning suffixes, and multipart labels
are preserved. Public and private destinations continue to use MeshCore group texts.

```text
Possible fall: peak accel=2.80g rotation=260.0deg/s
Fall Detected & User Has Requested Assistance | HOME
User Confirms False Alarm | HOME
USER HAS CONFIRMED FALL
```

These readings and locations illustrate formatting, rather than measured results.
Retry payloads retain the event text and captured location.

### MPU6050 freshness and fault recovery

- Initialization now reads back relevant writable register bits before reporting
  readiness, including data-ready configuration.
- A single 15-byte transaction reads `INT_STATUS` and the complete measurement
  block. Only complete bursts with data-ready set advance motion evidence.
- A working I2C bus returning no fresh data for one second triggers read/data-ready
  failure handling. Three consecutive failures initiate reinitialization; failed
  initialization retains the five-second retry backoff.
- Stale or invalid data cannot establish fall confirmation or residual quiet.
  Diagnostics distinguish initialization, read, freshness, and recovery events.

The existing GY-521 wiring remains VCC to 3V3, GND and AD0 to GND, SDA to GPIO4,
and SCL to GPIO6. INT remains unconnected; address `0x68` is used.

### Battery measurement and calibration

USB `battery` now reports cached raw ADC counts, estimated millivolts, sample age,
and the effective conversion multiplier. `battery calibrate <meter_mV>` uses a
fresh sample and an actual battery-terminal meter reading to suggest a multiplier.
`set adc.multiplier <value>` explicitly saves and applies it; `0` restores the
existing board default of `5.42`.

Invalid meter input and stale, zero, or saturated ADC samples are rejected.
Older preference records without the new setting retain the board default.
Diagnostics consume the shared staged battery cache without starting additional
ADC conversions.

### Passive-buzzer playback and optional operation

The DIYables passive module configuration uses **VCC → 3V3**, **GND → GND**, and
**I/O → GPIO47**, with its onboard transistor driver. Tones use hardware-generated
50% PWM. Silent intervals disconnect PWM and hold the configured inactive GPIO
level; repeated idle polls perform no GPIO or PWM writes.

- `buzzer_active_low = false` selects **LOW idle** by default. `true` supports an
  alternative driver requiring HIGH idle. Product references specify PWM but do
  not explicitly establish idle polarity; electrical operation requires bench validation.
- `buzzer_enabled = false` skips buzzer GPIO/PWM setup and playback while retaining
  monitoring, PRG responses, and channel messaging. A physically absent buzzer also
  does not prevent operation when output remains enabled.
- Frequency changes occur when required by the next tone. A failed frequency
  setup leaves the output disconnected and inactive.
- The existing 150 ms ordinary beep, SOS cadence, battery pitches, and alarm
  priority remain nonblocking.

Both settings are compiled configuration values in `HealthNodeConfig.h`; changes
require rebuilding and flashing. An incorrect idle polarity can energize the
driver during intended silence even when PWM tones sound normal.

### Pairing and reference documentation

The compiled default BLE pairing PIN changes from `123456` in the baseline to
**`12345`**. A saved custom PIN takes precedence. The USB `pin` setting supports
`0` for the board default or a custom six-digit integer from `100000` to `999999`;
saved changes become effective after reboot.

Documentation now includes Windows PowerShell and Linux USB setup, calibration
procedures, a complete local CLI reference, and post-flash baseline-versus-query
comparisons. Hardware references document Heltec revision-specific RF wiring,
battery-divider discrepancies, and MPU6050 register/module specifications.
Documentation and revised code comments use an impersonal technical style.

## Upgrade and verification

The firmware target remains **`heltec_v4_companion_radio_ble`** for the Heltec
V4.3 OLED configuration. One image provides BLE companion services and local fall
monitoring together. The MeshCore base remains **1.17.0**; its existing firmware
version string is separate from the GitHub release tag.

Use **`firmware-merged.bin` at flash offset `0x0`** for the combined flash layout.
`firmware.bin` contains the application only and belongs at `0x10000` with matching
bootloader and partition files. Erasing the entire flash also removes stored
identity, preferences, channel configuration, and saved calibration.

After flashing, inspect `health`, `motion`, `get username`, `get radio`, and the
new help/statistics commands. Check `beep` followed by silence, then perform
stationary calibration if appropriate. Follow the
[post-flash verification procedure](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/main/docs/usb_cli_reference.md#post-flash-usb-cli-verification)
to compare a quiet baseline with repeated diagnostic queries. Compare sample rate,
mean/max spacing, gaps, and sensor failures over comparable intervals; isolated
maxima require context rather than an automatic pass/fail conclusion.

The recorded beta build completed successfully. Application and bootloader checksums
and validation hashes passed, and the merged image matched its component bytes at
the expected offsets. The recorded targeted host regression run passed **179 of 179
cases**, covering buzzer behavior, fall responses, low battery, post-fall handling,
and simulated hardware faults. This is a targeted result, not a claim that every
test suite was rerun for this build.

Recorded beta merged-image identification:

```text
File: firmware-merged.bin
Size: 1,373,104 bytes
SHA-256: afb4f08c497ed29b0c30c420018d6e92580edc7a9141c25154654a3267198de0
```

## Beta limitations

Fall detection remains experimental: ordinary activity can trigger alerts and
falls can go undetected. Calibration does not establish detection accuracy or
repair power, wiring, I2C, or sensor-freshness faults. The device must move with
the wearer for body-motion monitoring.

Local tests and image verification do not establish physical BLE/USB operation,
RF delivery, sensor accuracy, long-duration timing, battery accuracy, or buzzer
electrical behavior. A repeater echo indicates rebroadcast evidence, rather than
receipt or acknowledgement by a particular person. Pending incident state and
RAM statistics clear on reboot. Physical validation remains necessary before
reliance on the system.

## Documentation

- [USB configuration and statistics](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/main/docs/usb_cli_reference.md)
- [Windows and Linux USB serial debugging](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/main/docs/usb_serial_debugging.md)
- [Stationary gyroscope calibration](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/main/docs/gyro_calibration.md)
- [Sensor, buzzer, and response behavior](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/main/docs/gy521_sensor.md)
- [Post-fall messages and delivery](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/main/docs/message_delivery_changes.md)
- [Pin schematic](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/main/docs/pin_schematic.md)
- [Build and validation](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/main/docs/build-validation.md)

