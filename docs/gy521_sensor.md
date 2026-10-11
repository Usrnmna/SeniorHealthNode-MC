# GY-521 motion alerts on Heltec V4.3 OLED

Build the `heltec_v4_companion_radio_ble` environment. This runs the polling and decision
program on the Heltec itself; a PC is only needed for building/flashing and setup.
The same image also provides the MeshCore BLE companion, display and messaging. Monitoring continues without a BLE connection.

| GY-521 | Heltec |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO4 |
| SCL | GPIO6 |
| AD0 | GND (address 0x68) |
| INT, XDA, XCL | disconnected |

The sensor uses Wire1 at 100 kHz. OLED/RTC use Wire on GPIO17/18.
This configuration targets the OLED board, not the TFT firmware environment.

The [MPU-6050/GY-521 source reference](mpu6050_gy521_reference.md) includes the
supplied register manual and module sheet, maps the current driver settings to
their registers, and explains module power and pull-up caveats. The module
sheet's Arduino wiring example does not replace the Heltec connections above.

## Stationary gyroscope calibration

The [calibration guide](gyro_calibration.md) describes USB `calibrate` and the
MeshCore local `set calibrate start` / `get calibrate` workflow. A 3-second
countdown precedes 15 seconds of stationary measurement and a separate 2-second
validation. The device can remain in any fixed orientation; exact leveling is
unnecessary. Movement restarts collection and sensor errors abort it.

Validated offsets require `calibrate save` before activation. Saved correction
applies to detector, residual-motion, telemetry and `motion` gyroscope readings;
`motion raw` exposes scaled readings before correction. Gravity remains present
in acceleration. This feature does not calibrate the accelerometer or alter fall
thresholds. Detection pauses and buzzer playback is suppressed during collection,
then monitoring resumes through normal rearm checks. Calibration requires healthy
sensor data and is unavailable during active fall/help incidents.

## Passive buzzer wiring and shared functions

The buzzer is optional. With `buzzer_enabled = true` (default), its signal is
**GPIO47**, using ESP32 LEDC channel 0 at
**2000 Hz, 50% duty** for ordinary beeps and SOS. `buzzer_active_low = false`
selects LOW during silence: initialization sets GPIO47 LOW before enabling output,
and PWM connects only during tones. The GY-521 remains on GPIO4/6
and PRG remains on GPIO0.
GPIO47 is listed on header J2 in the [Heltec V4 datasheet, pin description](https://resource.heltec.cn/download/WiFi_LoRa_32_V4/datasheet/WiFi_LoRa_32_V4.2.0.pdf)
and has no other assignment in this firmware's V4 OLED configuration.

The [DIYables passive buzzer module](https://diyables.io/products/passive-buzzer-module)
requires PWM and supports a 3.3-5 V supply. Its onboard transistor driver permits
the following connections, using a 3.3 V supply for this ESP32-S3 configuration:

| Connection | Destination |
| --- | --- |
| GPIO47 | Module I/O; 50% PWM for sound, LOW for silence with the default setting |
| 3V3 | Module VCC |
| GND | Module GND |

The [ESP32-S3 tutorial](https://newbiely.com/tutorials/esp32-s3/esp32-s3-piezo-buzzer)
uses GPIO42 as an example; this firmware retains GPIO47 because GPIO42 is assigned
to optional GPS reset. The [Arduino piezo tutorial](https://arduinogetstarted.com/tutorials/arduino-piezo-buzzer#content_about_piezo_buzzer)
also describes frequency-controlled square waves and `tone()`/`noTone()` playback.
The firmware implements equivalent tone/silence intervals through ESP32 LEDC and
the existing nonblocking state machine. No melody, blocking delay, or additional
buzzer library is required. The module needs no additional external transistor.

For a discrete low-side NPN driver, **set `buzzer_active_low = false`**, then
connect GPIO47 through a base resistor,
emitter to GND, collector to buzzer minus, and buzzer plus to its rated supply.
A 1 kÎ© base resistor and 47 kÎ© base-to-emitter pull-down are a starting point for
a small buzzer; select transistor, resistor and supply ratings for its actual
current. The pull-down keeps the driver off while the ESP32 resets. An inductive
magnetic buzzer needs the driver's specified flyback protection. Do not power a
buzzer or an 8 Î© speaker directly from the GPIO; a speaker needs a suitable
amplifier. This configuration assumes a passive transducer, not a self-oscillating
active buzzer. A three-pin module with its own compatible driver takes GPIO47
at its signal input; the module already contains the buzzer-driving circuit.
Electrical operation and acoustic output remain subject to bench validation.

Edit [`include/HealthNodeConfig.h`](../include/HealthNodeConfig.h) for the pin,
output enable, trigger polarity, PWM channel/frequency, beep length, SOS timing, button timing
and channel message. Rebuild and flash the combined image after changing these
compiled settings; `buzzer_enabled` and `buzzer_active_low` are not USB/BLE runtime commands.
Check the board pin map before changing GPIO; avoid the sensor, OLED, LoRa,
GPS, USB, flash and board power-control pins. LEDC channel 0 is reserved for
this buzzer when enabled; do not use `tone()` or another PWM user on that channel.

### Operation without a buzzer

No electrical presence detection or feedback connection exists for the buzzer.
With `buzzer_enabled = true`, the firmware generates the configured output even
when no module is connected. An open GPIO47 connection does not prevent sensor
initialization, motion monitoring, BLE operation or channel messaging.

To omit buzzer GPIO/PWM initialization and all sound playback, set the following
at the top of `HealthNodeConfig.h`, then rebuild and flash the combined image:

```cpp
constexpr bool buzzer_enabled = false;
```

Disabled mode performs no buzzer GPIO writes, PWM setup or PWM attachment.
Startup reports `Buzzer disabled; monitoring and channel alerts remain active`;
the USB `beep` command returns `Buzzer disabled by buzzer_enabled=false`.
The output pin is not actively held at the configured inactive level in this
mode. A connected driver therefore requires an appropriate external inactive
bias, or it must be disconnected. An LED sharing the buzzer signal receives no
firmware-driven indication in disabled mode.

Motion detection, the fall-response deadline, PRG confirmation/false-alarm
gestures, location handling and group-channel alerts remain active. Audible SOS
and battery reminders are omitted. PRG remains reserved for a pending fall even
without audible feedback. Battery acquisition and USB diagnostics continue.

PWM initialization failure also leaves the rest of the node running; it is not
a sensor-initialization dependency. `MPU6050 unavailable` and data-ready failures
remain sensor diagnostics and are not suppressed by disabling the buzzer.

### Buzzer trigger polarity

The setting is beside `buzzer_pin` at the top of `HealthNodeConfig.h`:

```cpp
constexpr bool buzzer_active_low = false;
```

| Setting | Matching driver | Silent GPIO47 level | Sounding output |
| --- | --- | --- | --- |
| `false` (default) | Selected DIYables PWM configuration; also suitable for the discrete NPN circuit above | LOW | 50% PWM |
| `true` | Alternative driver requiring HIGH during silence | HIGH | 50% PWM |

The linked product and tutorials specify PWM operation but do not explicitly
specify the module's inactive input polarity. LOW idle is the selected firmware
configuration; it is not a bench-verified electrical specification.

Polarity applies to startup, ordinary beep completion, SOS gaps and cancellation,
battery-tone gaps and cancellation, and the inactive interval used when changing
frequency. At tone completion, LEDC first applies inactive duty (255 for HIGH or
0 for LOW), then disconnects from GPIO47; the GPIO holds the selected idle level.
Sounding uses duty 128 at 8-bit resolution. Repeated idle polls perform no GPIO
or PWM operations. Frequency setup occurs only when the next tone needs a different
pitch; a setup failure leaves the output disconnected and inactive.
Frequencies, durations, alarm priority,
PRG gestures and channel messages are unchanged by this setting.

**An incorrect setting holds the driver active during intended silence.** Using
`false` with a LOW-trigger module leaves its input LOW; using `true` with a
HIGH-active driver leaves its input HIGH. A genuinely passive buzzer may still
play tones normally because 50% PWM is unchanged, and a constant active input
does not necessarily produce a continuous tone. Depending on the driver and
transducer, the wrong idle level can cause clicks, unnecessary current draw or
heating. Silence alone does not prove that the driver is off. This setting does
not convert a self-oscillating active buzzer into a passive one.

Firmware establishes the inactive level when `alarm_buzzer.begin()` runs; it
cannot guarantee the signal during reset or before initialization. Where the
module requires external bias, use a suitable 3.3 V-compatible pull-up for a
LOW-trigger input or pull-down for a HIGH-trigger input. Do not pull GPIO47 up to
a 5 V module supply. Check the module's actual input circuit and power ratings.

After flashing, bench-check quiet startup, `beep` followed by silence, SOS gaps
and acknowledgement, and all three battery tones. Check idle signal/current as
well as audible behavior. Host tests and a successful build do not verify these
electrical behaviors on the physical module.

[`AlarmBuzzer.h`](../src/helpers/ui/AlarmBuzzer.h) exposes the single shared
`alarm_buzzer` instance, defined in `examples/companion_radio/HealthMonitor.cpp`:

```cpp
#include <helpers/ui/AlarmBuzzer.h>

// From another combined-firmware function, after setup():
alarm_buzzer.beep();     // Default 150 ms; returns immediately.
// Or: alarm_buzzer.beep(300);  // Explicit duration in milliseconds.
```

`setup()` calls `alarm_buzzer.begin()` once. `MyMesh::pollFallResponse()` services
`alarm_buzzer.loop()` every loop; callers must not add delays or their own busy
loops. The API also supplies `startSOS()` and `stop()` for the fall-response owner.
`beep()` returns false if output is disabled, PWM initialization or frequency setup
failed, or an SOS/battery sequence is active. The
USB serial command `beep` tests just the sound without creating a
fall event or sending a channel acknowledgement.

## Low-battery buzzer reminder

The sensor checks battery voltage once per second. Below **3500 mV**, it plays
three long beeps at **2093 Hz, 1760 Hz, then 1480 Hz**. Each beep uses the SOS dash
length (`long_beep_ms = 3 * morse_dot_ms`, currently 450 ms), with 150 ms gaps.
The sequence repeats every **10 minutes**, measured from one accepted sequence
start to the next. Playback is nonblocking; late loop servicing can lengthen a
tone or gap. A fall SOS preempts it; a due battery reminder waits while SOS is
active. The ordinary `beep` command cannot interrupt either sequence.

Battery voltage **above 3500 mV** cancels the warning; exactly 3500 mV retains
the previous state. A zero ADC sample is treated as unavailable. Threshold,
poll/repeat intervals and pitches are in `HealthNodeConfig.h`.

**Charger detection requires added hardware.** The stock board's
[Heltec V4.3 schematic](https://resource.heltec.cn/download/WiFi_LoRa_32_V4/Schematic/HTIT-WB32LAF_V4.3.pdf)
connects the CN3165 CHRG output to the charge LED, not to an ESP32 GPIO.
Battery voltage alone cannot establish that an external source is actively
charging. With the default `charging_status_pin = -1`, the reminder therefore
continues until battery voltage exceeds 3500 mV, even after a charger is attached.

If a suitably conditioned **3.3 V digital active-charging signal** is added,
set `charging_status_pin` and `charging_status_active_low`. The input uses a
pull-up for active LOW, or pull-down for active HIGH. It is read every loop and
cancels only the battery warning, including a sequence in progress, without
waiting for the battery to recover. Removing the signal while the battery is
still low starts a fresh reminder. Do not connect USB/solar voltage or the raw
CHRG/LED node directly to an ESP32 GPIO; the LED circuit is fed from VIN_Charge.
Pin selection and electrical conditioning must be checked for the actual board.

The shared buzzer API provides `startLowBattery()` (false while busy/unavailable)
and `stopLowBattery()` (leaves SOS untouched). `MyMesh::pollLowBattery()` owns the
voltage policy and repeat timer. No battery-warning state is saved across reboot.
The combined firmware's `HeltecV4Board::pollBattery()` powers the divider, waits 10 ms
without blocking, then takes one ADC conversion per loop pass until eight are
averaged. `getBattMilliVolts()` returns that shared cache; telemetry and CLI reads
do not trigger extra conversions. Battery warning policy waits until the first complete
sample exists. The companion retains automatic shutdown below 3400 mV when external
power is not detected; shutdown stops monitoring and sound.

## Fall alarm, PRG acknowledgement and channel message

PRG clicks are consumed by the alarm while SOS is active, preventing simultaneous
companion navigation, mute or shutdown actions. Outside an alarm, normal companion
button actions remain available.

When `MotionRule` detects a possible fall, the sensor starts repeating
**SOS: three short, three long, three short tones**. A dot is 150 ms, a dash
450 ms, gaps inside a letter 150 ms, and gaps between letters 450 ms. After the
last tone ends there are **5000 ms of silence** before the next SOS. The timing
state machine is nonblocking. Delayed main-loop servicing can lengthen tones or
gaps; it never intentionally shortens the five-second silence.

The alarm remains latched until a valid acknowledgement or restart, even if the
sensor disconnects or the radio fails. Other beeps cannot interrupt it. The initial
possible-fall alert and the later assistance request are independent group-channel
events. Triple-press confirmation leaves the initial alert's retries active.
An accepted false-alarm double press cancels that incident's initial-alert retries
and queued automatic warnings; already transmitted messages cannot be recalled.

To confirm a fall/request assistance, press and release **PRG three separate times**:

- Each press must be shorter than `long_press_ms = 1000`, matching the existing
  Heltec `MomentaryButton` constructor. A press lasting 1000 ms or longer cancels
  the pending set. The sensor UI itself has no long-press action.
- From the first observed press to the third observed release must be at most
  `click_window_ms = 3000`. This is one total window, not three seconds per gap.
- Both edges must remain stable for `debounce_ms = 25`. The window uses edge
  timestamps, so a release exactly at 3000 ms can be confirmed 25 ms later.
- Only clicks after the fall count. A button already held when the fall is
  detected must first be released, then begin a new gesture. Alarm clicks do not
  navigate the companion UI. Very fast clicks missed by polling cannot count.

For a false alarm, press and release **PRG twice within one second**:

- `false_alarm_click_window_ms = 1000` measures first observed press-down to second
  observed release, inclusive; both presses and releases must pass the same 25 ms debounce.
- The double press stays pending until the full `click_window_ms = 3000` from
  first press-down has closed. SOS and PRG ownership continue during this wait.
  A valid third release within three seconds takes priority, including a release
  exactly at the boundary whose debounce completes afterward.
- A debounced third press cancels the double candidate. If that press is held
  for a second or released too late, neither response is accepted; SOS continues
  and a fresh gesture can be attempted. Bounce alone cannot create a third click.
- A qualifying double resolves on the first service pass after the window closes,
  once any relevant transition has settled. It stops SOS and queues
  `User Confirms False Alarm | lat=37.123456, lon=-122.654321` or
  `User Confirms False Alarm | HOME` using `false_alarm_message` and the normal
  location provider/fallback. This is the wearer's report, not sensor verification.
- False-alarm responses work before or after the automatic warning. A pending
  double does not extend the 30-second response deadline; if the warning becomes
  due during that wait, the later accepted false alarm supersedes it.
- Local cancellation works despite radio failure or outbox backpressure. Monitoring,
  the detector's cooldown/rearm, and the normal idle PRG behavior remain unchanged.

Before automatic escalation, the accepted third release silences SOS immediately
and creates an assistance event with bounded channel retries. The configured UTF-8 text is
`Fall Detected & User Has Requested Assistance`. When the assistance event is created, the sender queries the local
MeshCore `LocationProvider`, processes available GPS input, and checks that GPS
is enabled and the provider reports a valid fix. Latitude must be finite and
within -90..90 degrees; longitude must be finite and within -180..180 degrees.
Provider values are converted from millionths of a degree and printed with six
decimal places. A real, provider-validated fix at 0,0 is accepted.

The sender appends coordinates or the UTF-8 `DefaultLocation` constant from
[`HealthNodeConfig.h`](../include/HealthNodeConfig.h), initialized as `u8"HOME"`:

```text
Fall Detected & User Has Requested Assistance | lat=37.123456, lon=-122.654321
Fall Detected & User Has Requested Assistance | HOME
```

Without an accepted response by **30 seconds after detection**, the firmware
automatically sends that assistance+location text followed by
` | USER COULD NOT CONFIRM THEIR CONDITION`. If fresh samples instead establish
quiet for the **final 10 continuous seconds**, it appends
` | NO RESIDUAL MOVEMENTS DETECTED; POSSIBLE INCAPACITATION OF USER`.
The duration and independent movement thresholds are editable in `HealthNodeConfig.h`.
Invalid samples, stale data and sampling gaps cannot establish quiet.

Either warning leaves SOS running without an elapsed-time cutoff. Three accepted
presses afterward stop SOS and send the standalone `USER HAS CONFIRMED FALL`,
even if earlier delivery failed. This supersedes queued automatic-warning retries
and unsent warning parts, while an already-transmitting packet finishes normally.
The original `Possible fall` delivery remains independent for triple presses;
a false-alarm response also retires that incident's initial alert. See the
[directive table and tuning settings](message_delivery_changes.md#post-fall-response-directives).

The query uses the provider's latest reported valid fix and does not wait for a
new satellite fix, reset the GPS, or enable it automatically. GPS is disabled by
default in sensor preferences; with compatible hardware connected and detected,
enable the `gps` custom variable with value `1` through the BLE client
(`set gps 1` in a compatible local CLI; protocol payload `gps:1`).
Use `0` to disable it. The USB health command interface does not accept `gps on`. Disabled GPS, no provider, no
valid fix, or invalid coordinates selects `DefaultLocation`. This provider keeps
its last fix in memory; a disconnected receiver that has not reported an invalid
fix can still expose an older location. There is no fix-age limit.
Configured advertisement coordinates are not used as a substitute for GPS.

The location is appended once, leaving `ack_message` unchanged. The event text
and selected location are frozen for retries, preventing repeated suffixes. This
applies to assistance and false-alarm messages; the initial channel alert retains its peak-value
text format. The button input reports the wearer's assistance request or false-alarm
confirmation; it does not assess their condition.

Edit `ack_message` in `HealthNodeConfig.h` and rebuild to change the assistance
text. Select the channel through `set fall.channel public #name` or
`set fall.channel private Name <shared-key>` over USB or MeshCore One's local BLE
CLI. See the [channel command guide](cli_commands.md#fall-message-channel-usb-and-ble)
for key formats, persistence and client setup. The default is public **#falldetect**;
its key is the first 16 bytes of SHA256 of that exact name, including `#`:
`e92ddcbf922343dff181fb4948371236`.

Both message streams omit the node-name prefix and use group texts on the
selected public or private channel, never direct messages. Each incident and queued
message retains its original destination across changes. Receiving companions
need the matching key and radio settings. Companion channel slot numbers are
local to each companion and are not radio destinations.

The sender uses an unscoped flood with adjustable `ack_path_hash_size`, on the
node's existing frequency, bandwidth, spreading factor and coding rate. Channel
text is limited to **160 UTF-8 bytes including any location suffix and multipart label**.
An automatic warning that does not fit uses two labelled messages: the standalone
warning first, then assistance+location. No standalone warning begins with ` | `;
that spacer is inserted only when appending. If an individual part cannot fit,
the event fails visibly without transmitting a partial or truncated event. Save edited
headers as UTF-8. Receiver fonts determine which characters render.

Allocation failures, transmit-start failures, and TX timeouts retain the event
and retry after `ack_retry_ms = 5000`, bounded by `ack_max_attempts = 60` total
attempts. Queued packets expire after 30 seconds. After local TX, the controller
waits 15 seconds for a matching forwarded repeat and allows at most four locally
successful sends without one. A repeat proves a rebroadcast was heard, not that a
particular recipient received it. Each retry uses a fresh timestamp to pass
repeater deduplication; receiving companions may display duplicate text.
Exhaustion, missing repeats, and oversize text remain visible through serial and
`health`; these outcomes do not change SOS. It stays active after an automatic
warning and stays silenced after a user acknowledgement. `part_failed=1` retains
a failed part even if a later part succeeds. Idle double or triple clicks never send a fall-response message.
See [message-delivery directives and adjustment points](message_delivery_changes.md).

[`FallResponse.h`](../examples/companion_radio/health/FallResponse.h) owns the latch and
button gate; [`FallButtonGesture.h`](../src/helpers/ui/FallButtonGesture.h) owns gesture timing;
[`BuzzerPattern.h`](../src/helpers/ui/BuzzerPattern.h) owns sound timing;
[`FallAckSender.h`](../examples/companion_radio/health/FallAckSender.h) provides the channel
send function and [`FallAckMessage.h`](../examples/companion_radio/health/FallAckMessage.h)
encodes its bounded payload. [`FallAckLocation.h`](../examples/companion_radio/health/FallAckLocation.h)
queries the GPS provider. [`AssistanceDelivery.h`](../examples/companion_radio/health/AssistanceDelivery.h)
owns channel attempts, completion state and repeat evidence; `HealthMonitor.cpp` connects
these to `MotionRule`, the dispatcher, and GPIO.

Alarm state, partial clicks, pending channel send and motion evidence/cooldown
exist only in ordinary RAM. **Reboot clears all of them**, leaves the buzzer off,
and starts motion detection with fresh settling. After acknowledgement without a
reboot, the existing cooldown and quiet-settling requirements still apply.
Analysis continues while SOS, channel delivery, or channel allocation retries are
pending. A subsequent detection after the normal cooldown keeps an existing SOS
and partial click gesture intact. If SOS has stopped, it starts a new local alarm.
An outstanding initial channel alert is retained rather than replaced. Acknowledgments
coalesce into an already-pending assistance message without restarting its retry
budget. These policies suppress duplicate notifications, not sensor analysis.

## Build and run

For an already flashed device, the [USB serial debugging guide](usb_serial_debugging.md)
covers Windows PowerShell and Linux monitoring without another build or upload.

With PlatformIO installed, from the `SeniorHealthNode-MC` folder:

```powershell
pio run -e heltec_v4_companion_radio_ble
pio run -e heltec_v4_companion_radio_ble -t upload --upload-port COM_PORT
pio device monitor --port COM_PORT --baud 115200 --eol CR
```

Replace `COM_PORT` with the board's actual port. Create `.venv` in this project
folder as described in the [Windows setup guide](../README.md#build-and-test-on-windows),
then replace `pio` with `.\.venv\Scripts\python.exe -m platformio`.
The project configuration uses `.pio/core` for PlatformIO's cache and `.pio/p`
for toolchain packages. Run `.\verify.ps1` to build the combined firmware and run all native tests.
Configure the node's radio parameters to match your MeshCore network before use.

Serial startup reports detection at 0x68 or a five-second retry. Enter `motion`
(carriage return) to see acceleration x/y/z in g, rotation x/y/z in degrees/second,
and chip temperature in Celsius. At rest the acceleration magnitude should be
approximately 1 g. Chip temperature is not body temperature.

## Recipient setup

Add the selected public hashtag channel (initially `#falldetect`) or private
channel with the same shared key to each receiving companion, and match radio
settings. Initial alerts, automatic warnings, assistance and late confirmation
all use the incident's captured destination; no contact registration or high-priority
ACL permission is required for these fall messages. Public hashtag channel keys
are shared channel identities, not recipient-specific encryption secrets.

Pair a compatible MeshCore phone client directly with this node over BLE to
configure its name, channels and radio settings. USB serial at 115200 baud
provides `motion`, `health` and `beep` with CR, LF or CRLF line endings.

Use serial `health` to inspect both message states. `repeater echo heard` confirms
a matching forwarded packet reached this node; it does not establish that a
caregiver's companion received or displayed the message. Verify both channel texts
at the intended listening companion. The [delivery guide](message_delivery_changes.md)
explains retries, duplicate text, missing repeat evidence, and validation limits.

## Polling and rule customization

`src/helpers/sensors/MPU6050.h` polls a 15-byte burst every 20 ms when the main
loop can run (nominal 50 Hz): data-ready status followed by the 14-byte
acceleration/temperature/gyro sample. Only a complete burst with DATA_RDY_INT set
reaches the detector. It configures the device for +/-8 g, +/-500 degrees/second
and filtered 50 Hz sampling, then checks each configuration register by readback. Failed
or partial reads invalidate the sample immediately. The next read retries after
20 ms; three consecutive failures trigger reinitialization on the next short
retry. A working bus that reports no new data for one second enters the same
failure/recovery path. Only failed initialization, including a configuration
readback mismatch, uses the five-second missing-device backoff.
Successful initialization still waits 100 ms for settling; the rule independently
requires two seconds of quiet after invalid data. Retry timers are configurable
in `HealthNodeConfig.h`.
The loop is cooperative; other firmware work can delay polls, so brief events
can be missed. The INT pin and FIFO are not used. `health` reports the maximum completed
fresh-sample interval, intervals exceeding the 100 ms detector guard, and a fault
after five seconds without fresh valid motion. Recovery does not bypass quiet
settling, and the gap guard is unchanged. See [the health diagnostics](message_delivery_changes.md).

### Sampling and background work

- Motion is serviced at the start of the main loop and again after radio work.
- The combined firmware starts a 6,144-byte-stack environmental collector on ESP32
  core 0 at idle priority. Environmental initialization and conversion waits run there; the main
  loop reads fixed-size snapshots rather than invoking environmental drivers.
  Measurements refresh every 60 seconds. Empty/failed results replace the old
  entry, and entries aged 120 seconds are omitted. Channel 2 stays reserved for
  the MPU even during failures; initialized environmental channels start at 3.
- The AHT10/AHT20 driver uses reset/calibration/trigger/status/read stages with a
  one-second deadline. BME680 uses `beginReading()`, readiness checks, and
  `endReading()` only after the conversion finishes, also with a one-second wait
  deadline. Other drivers execute in the collector, away from the main loop.
- Only the collector uses `Wire1`'s shared buffers. MPU transactions use local
  buffers and the ESP32 HAL's bus mutex, with a 5 ms transaction timeout. A
  conversion wait does not hold the bus. The timeout is not a guarantee about
  total loop latency, and physical bus faults can still invalidate motion data.
- While the OLED is on, frames are queued by the companion UI and transferred in
  32-byte pieces, one per main-loop pass. Each piece readdresses its page and
  column; an I2C failure abandons that frame until the next refresh. The UI keeps
  the framebuffer unchanged while a transfer is pending.

Configuration and methods are indexed in [the timing change report](fall_timing_changes.md).
Radio CAD behavior is unchanged. Radio processing, flash operations, serial work,
and GPS processing remain cooperative work and need hardware latency measurement.

### Review and tune the algorithm

Start with [`MotionRule.h`](../examples/companion_radio/health/MotionRule.h). Its
`FallDetectionConfig` contains **all** decision settings; edit defaults there and
rebuild. The former `MOTION_ACCEL_THRESHOLD_G`, `MOTION_GYRO_THRESHOLD_DPS`, and
`MOTION_COOLDOWN_MS` build flags are replaced by this configuration. Custom code
can also pass a `FallDetectionConfig` to the `MotionRule` constructor.

[`HealthMonitor.cpp`](../examples/companion_radio/HealthMonitor.cpp), `MyMesh::pollMotion()`, supplies
fresh acceleration and rotation vector magnitudes. Neither magnitude depends on
which sensor axis points upward, but placement and attachment still matter.
**Firm torso/chest attachment** is the intended starting point. This does not make
the rule placement-independent; a loose pocket, wrist, or nearby table requires
separate evaluation. A dropped device can resemble a fall.

| Code label | Algorithm and purpose | Settings and defaults |
| --- | --- | --- |
| STAGE 0 | Discard invalid, negative, nonfinite, or interrupted evidence; ignore duplicate timestamps | `max_sample_gap_ms = 100` |
| STAGE 1 | Continuous quiet before arming at startup, recovery, expired candidates, or after an alert | `rearm_quiet_ms = 2000` |
| STAGE 2 | Low acceleration opens one fixed event window | `low_g = 0.35`, `event_window_ms = 500` |
| STAGE 3 | Require both impact and rotation; peaks may be on separate samples, in either order | `impact_g = 2.4`, `rotation_dps = 240` |
| STAGE 4 | Confirm sustained quiet after the sequence; movement restarts only the quiet timer | `confirm_quiet_ms = 1000`, `confirm_timeout_ms = 5000` |
| STAGE 5 | Emit once, then require cooldown and quiet, which can overlap | `cooldown_ms = 60000` |

Quiet means acceleration between `quiet_min_g = 0.7` and `quiet_max_g = 1.3`
(inclusive), and rotation below `quiet_max_dps = 30`. Acceleration includes gravity.
Timers use elapsed milliseconds; both event and confirmation deadlines are inclusive.
Only fresh samples count. A gap beyond 100 ms requires fresh settling; a shorter
unobserved interval is tolerated, not proof of continuous physical stillness.

**Research mapping:** stages 2-3 adapt the bounded sequence in Huynh et al. (2015),
[README study 4](../README.md#study-4), [original paper](https://doi.org/10.1155/2015/452078).
Stage 4 and all startup/cooldown/data guards are **project-specific extensions**.
The temporal/posture work in README study 2 provides context, but this code does
not reproduce its chest-and-thigh posture classifier. Voting (study 1), LSTM
(study 3), and smartphone posture/orientation rules (study 5) are not implemented.

**Tuning effects:** raising `low_g`, lowering either peak threshold, or lengthening
`event_window_ms` can admit more candidates, including false alerts. Increasing
quiet confirmation can reject continuing activity but increases delay and can
miss falls where the person keeps moving. Set `confirm_quiet_ms = 0` to compare
the sequence without quiet confirmation. Keep `confirm_timeout_ms` longer than
`confirm_quiet_ms` with room for settling. Use finite, nonnegative thresholds,
`quiet_min_g < quiet_max_g`, `low_g < quiet_min_g`, and `impact_g > quiet_max_g`;
keep timing values below 2^31 ms. Configuration is trusted source code, not a
runtime settings interface.

A soft/sliding fall without low acceleration, strong impact, or rapid rotation
will be rejected by this rule. It does not determine lying posture or injury.
Alert text preserves the event-window peaks rather than the later quiet readings.
Pending alert deliveries do not discard evidence. New falls during the detector's
60-second cooldown are still suppressed. Read failures discard evidence but
preserve an existing cooldown.

Latest valid samples also appear in environment telemetry on channel 2, before
other detected environmental sensors. Cayenne gyro encoding clips individual axes
to -327.68..327.67 degrees/second; rules, serial output, and alert text retain the
full measured range. Acceleration and gyro conversions follow the manufacturer's
[MPU-6050 register map](https://cec-code-lab.aps.edu/downloads/mpu-6050_register-map.pdf).

Rule tests: `pio test -e native -f test_motion_rule` and
`pio test -e native -f test_fall_response` and
`pio test -e native -f test_low_battery` (require a host C++ compiler).
The motion-rule and low-battery checks also have C++11 standalone entry points.
Create the output directory first if no build has been run. Use the PlatformIO
`test_fall_response` command above for response tests and their host dependencies:

```powershell
New-Item -ItemType Directory -Force .pio | Out-Null
g++ -std=c++11 -DMOTION_RULE_STANDALONE test/test_motion_rule/test_motion_rule.cpp -o .pio/motion-rule-tests.exe
.\.pio\motion-rule-tests.exe
g++ -std=c++11 -Wall -Wextra -Werror -DLOW_BATTERY_STANDALONE -I src -I include test/test_low_battery/test_low_battery.cpp -o .pio/low-battery-tests.exe
.\.pio\low-battery-tests.exe
```

The tests replay synthetic 50 Hz sequences, timing boundaries, missing features,
invalid readings, gaps, cooldown/recovery, and timer rollover. Passing them proves
software behavior only; no measured sensitivity or false-alert rate is available.
The battery tests also cover pitch order, long-tone/gap timing, the ten-minute
deadline, charging below threshold, recovery, SOS priority and timer rollover.
Charging inputs are synthetic; physical charging detection is not verified.
For real evaluation, use labeled recordings with held-out wearers and daily
activities, reporting missed falls, false alerts per wear-day, and detection delay.
Do not ask an older adult to perform falls for testing.

After flashing, verify readings at rest and during controlled bench motion, confirm a
recipient receives the alert, check cooldown/rearming, and disconnect/reconnect
the sensor to verify recovery. Hardware and over-radio delivery need a live check.
Also test `beep`, the nine SOS marks and five-second repeat silence, two clicks
versus three, a held PRG button, a late third release, a reboot during SOS, and
the acknowledgement's actual receipt on a second node with the matching channel
key. Controlled bench motion must trigger the detector before acknowledgement
can send; this firmware has no command that fabricates a fall event.
