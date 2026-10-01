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

## Passive buzzer wiring and shared functions

The default external buzzer signal is **GPIO47**, using ESP32 LEDC channel 0 at
**2000 Hz, 50% duty** while sounding. Initialization sets the output LOW before
attaching PWM. The GY-521 remains on GPIO4/6 and PRG remains on GPIO0.
GPIO47 is listed on header J2 in the [Heltec V4 datasheet, pin description](https://resource.heltec.cn/download/WiFi_LoRa_32_V4/datasheet/WiFi_LoRa_32_V4.2.0.pdf)
and has no other assignment in this firmware's V4 OLED configuration.

Use an **external passive buzzer with a 3.3 V logic-compatible transistor driver**:

| Connection | Destination |
| --- | --- |
| GPIO47 | Driver signal input; HIGH/PWM enables the buzzer |
| GND | Driver and buzzer supply common ground |
| Driver/buzzer supply | Supply rated for the selected buzzer and driver |
| Driver output | Passive buzzer, following the driver's polarity/wiring |

For a discrete low-side NPN driver, connect GPIO47 through a base resistor,
emitter to GND, collector to buzzer minus, and buzzer plus to its rated supply.
A 1 kÎ© base resistor and 47 kÎ© base-to-emitter pull-down are a starting point for
a small buzzer; select transistor, resistor and supply ratings for its actual
current. The pull-down keeps the driver off while the ESP32 resets. An inductive
magnetic buzzer needs the driver's specified flyback protection. Do not power a
buzzer or an 8 Î© speaker directly from the GPIO; a speaker needs a suitable
amplifier. This configuration assumes a passive transducer, not a self-oscillating
active buzzer or an active-low driver. No specific buzzer model has been tested.

Edit [`include/HealthNodeConfig.h`](../include/HealthNodeConfig.h) for the pin,
PWM channel/frequency, beep length, SOS timing, button timing and channel message.
Check the board pin map before changing GPIO; avoid the sensor, OLED, LoRa,
GPS, USB, flash and board power-control pins. LEDC channel 0 is reserved for
this buzzer; do not use `tone()` or another PWM user on that channel.

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
`beep()` returns false if PWM initialization failed or an SOS/battery sequence is active. The
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
possible-fall alert and the later assistance request are independent public-channel
events. Acknowledging does not recall the initial alert or cancel its retries.

To acknowledge, press and release **PRG three separate times**:

- Each press must be shorter than `long_press_ms = 1000`, matching the existing
  Heltec `MomentaryButton` constructor. A press lasting 1000 ms or longer cancels
  the pending set. The sensor UI itself has no long-press action.
- From the first observed press to the third observed release must be at most
  `click_window_ms = 3000`. This is one total window, not three seconds per gap.
- Both edges must remain stable for `debounce_ms = 25`. The window uses edge
  timestamps, so a release exactly at 3000 ms can be confirmed 25 ms later.
- Only clicks after the fall count. A button already held when the fall is
  detected must first be released, then clicked three times. Ordinary display
  wake behavior remains available. Very fast clicks missed by polling cannot count.

The accepted third release silences SOS immediately and creates **one assistance event per
acknowledged fall**, with bounded public-channel retries. The configured UTF-8 text is
`Fall Detected & User Has Requested Assistance`. When the assistance event is created, the sender queries the local
MeshCore `LocationProvider`, processes available GPS input, and checks that GPS
is enabled and the provider reports a valid fix. Latitude must be finite and
within -90..90 degrees; longitude must be finite and within -180..180 degrees.
Provider values are converted from millionths of a degree and printed with six
decimal places. A real, provider-validated fix at 0,0 is accepted.

The sender appends coordinates or the UTF-8 `DefaultLocation` constant from
[`HealthNodeConfig.h`](../include/HealthNodeConfig.h), initialized as `u8"HOME"`:

```text
Node name: Fall Detected & User Has Requested Assistance | lat=37.123456, lon=-122.654321
Node name: Fall Detected & User Has Requested Assistance | HOME
```

The query uses the provider's latest reported valid fix and does not wait for a
new satellite fix, reset the GPS, or enable it automatically. GPS is disabled by
default in sensor preferences; with compatible hardware connected and detected,
use the existing `gps on` command to enable it. Disabled GPS, no provider, no
valid fix, or invalid coordinates selects `DefaultLocation`. This provider keeps
its last fix in memory; a disconnected receiver that has not reported an invalid
fix can still expose an older location. This change does not add a fix-age limit.
Configured advertisement coordinates are not used as a substitute for GPS.

The location is appended once, leaving `ack_message` unchanged. The event text
and selected location are frozen for retries, preventing repeated suffixes. This
applies to the assistance message; the initial public alert retains its peak-value
text format. The button input reports a request for assistance, not a
medical assessment of the wearer.

In `HealthNodeConfig.h`, edit `ack_message`, `ack_channel_name`, and
`ack_channel_key`. The default destination is the public hashtag channel **#falldetect**. Its key
is the first 16 bytes of SHA256 of the exact name, including `#`:
`e92ddcbf922343dff181fb4948371236`. Add `#falldetect` as a public hashtag channel
on each receiving companion. Both fall-message streams use this public channel.
To select another public channel, set its exact decoded key bytes and label;
renaming the label alone does not change the destination. Companion channel slot numbers are local
to each companion and are not radio destinations. The configured key is hashed
and used with the existing MeshCore group encryption and plain-text payload format.

The sender uses an unscoped flood with adjustable `ack_path_hash_size`, on the
node's existing frequency, bandwidth, spreading factor and coding rate. Channel
text is limited to **160 UTF-8 bytes including `node name: ` and the location suffix**. Oversize text is
rejected and logged, never truncated through a Unicode character. Save edited
headers as UTF-8. Receiver fonts determine which characters render.

Allocation failures, transmit-start failures, and TX timeouts retain the event
and retry after `ack_retry_ms = 5000`, bounded by `ack_max_attempts = 60` total
attempts. Queued packets expire after 30 seconds. After local TX, the controller
waits 15 seconds for a matching forwarded repeat and allows at most four locally
successful sends without one. A repeat proves a rebroadcast was heard, not that a
particular recipient received it. Each retry uses a fresh timestamp to pass
repeater deduplication; receiving companions may display duplicate text.
Exhaustion, missing repeats, and oversize text remain visible through serial and
`health`; SOS stays silenced. Idle triple clicks never send this message.
See [message-delivery directives and adjustment points](message_delivery_changes.md).

[`FallResponse.h`](../examples/companion_radio/health/FallResponse.h) owns the latch and
button gate; [`TripleClick.h`](../src/helpers/ui/TripleClick.h) owns gesture timing;
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
Analysis continues while SOS, public delivery, or channel allocation retries are
pending. A subsequent detection after the normal cooldown keeps an existing SOS
and partial click gesture intact. If SOS has stopped, it starts a new local alarm.
An outstanding initial public alert is retained rather than replaced. Acknowledgments
coalesce into an already-pending assistance message without restarting its retry
budget. These policies suppress duplicate notifications, not sensor analysis.

## Build and run

With PlatformIO installed, from the `SeniorHealthNode-MC` folder:

```powershell
pio run -e heltec_v4_companion_radio_ble
pio run -e heltec_v4_companion_radio_ble -t upload --upload-port COM_PORT
pio device monitor --port COM_PORT --baud 115200 --echo --eol CR
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

Add the configured public hashtag channel, initially `#falldetect`, to each
receiving companion and use compatible radio settings. Both initial possible-fall
and post-click assistance messages use it; no contact registration or high-priority
ACL permission is required for these fall messages. Public hashtag channel keys
are shared channel identities, not recipient-specific encryption secrets.

Pair a compatible MeshCore phone client directly with this node over BLE to
configure its name, channels and radio settings. USB serial at 115200 baud
provides `motion`, `health` and `beep` with carriage-return line endings.

Use serial `health` to inspect both message states. `repeater echo heard` confirms
a matching forwarded packet reached this node; it does not establish that a
caregiver's companion received or displayed the message. Verify both public texts
at the intended listening companion. The [delivery guide](message_delivery_changes.md)
explains retries, duplicate text, missing repeat evidence, and validation limits.

## Polling and rule customization

`src/helpers/sensors/MPU6050.h` reads a complete 14-byte acceleration/temperature/
gyro sample every 20 ms when the main loop can run (nominal 50 Hz). It configures
the device for +/-8 g, +/-500 degrees/second and filtered 50 Hz sampling. Failed
or partial reads invalidate the sample immediately. The next read retries after
20 ms; three consecutive failures trigger reinitialization on the next short
retry. Only failed initialization uses the five-second missing-device backoff.
Successful initialization still waits 100 ms for settling; the rule independently
requires two seconds of quiet after invalid data. Retry timers are configurable
in `HealthNodeConfig.h`.
The loop is cooperative; other firmware work can delay polls, so brief events
can be missed. INT and FIFO are not used. `health` reports the maximum completed
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
For an older compiler without the repository's GoogleTest C++17 support, run the
same checks standalone:

```powershell
g++ -std=c++11 -DMOTION_RULE_STANDALONE test/test_motion_rule/test_motion_rule.cpp -o .pio/motion-rule-tests.exe
.\.pio\motion-rule-tests.exe
g++ -std=c++11 -Wall -Wextra -Werror -DFALL_RESPONSE_STANDALONE -I src -I include test/test_fall_response/test_fall_response.cpp -o .pio/fall-response-tests.exe
.\.pio\fall-response-tests.exe
g++ -std=c++11 -Wall -Wextra -Werror -DLOW_BATTERY_STANDALONE -I src -I include test/test_low_battery/test_low_battery.cpp -o .pio/low-battery-tests.exe
.\.pio\low-battery-tests.exe
```

The tests replay synthetic 50 Hz sequences, timing boundaries, missing features,
invalid readings, gaps, cooldown/recovery, and timer rollover. Passing them proves
software behavior only; no measured sensitivity or false-alert rate is available.
The six battery tests also cover pitch order, long-tone/gap timing, the ten-minute
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
