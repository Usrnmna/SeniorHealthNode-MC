# USB companion configuration and statistics

The `heltec_v4_companion_radio_ble` image provides a local text CLI alongside the
BLE companion protocol and fall detection. The [Windows and Linux serial
guide](usb_serial_debugging.md) describes connection at 115200 baud. Commands are
case-sensitive and terminated by CR, LF, or CRLF. Send one command and wait for
its complete response before sending another. The maximum command length is 158
bytes, excluding the terminator; an oversized or NUL-containing line is rejected
in full. Input is not echoed by the firmware.

```text
help
help get
help set
help set freq
help health
help motion
get username
set username "Medical Node 2"
get radio
health detail
health timing
motion stats
```

## Local sound check

`beep` requests one nonblocking 150 ms, 2000 Hz tone without creating a fall event
or channel message. The DIYables passive module connects I/O to GPIO47, VCC to
3V3 and GND to GND. The compiled default `buzzer_active_low = false` selects LOW
idle; sounding uses 50% PWM, disconnected again at tone completion. No buzzer
presence detection exists, so `Beep started` confirms software acceptance only.
Playback is unavailable during calibration or an active alarm sequence.
`buzzer_enabled = false` disables sound without disabling monitoring.

After flashing, verify one tone followed by silence before the sampling baseline
below. Keep sound activity consistent between baseline and repeated-query runs:
physical buzzer vibration can affect motion readings. See the
[sound diagnostics](usb_serial_debugging.md#battery-and-buzzer-checks) and
[compiled settings and wiring](gy521_sensor.md#buzzer-trigger-polarity).

## Configuration

Use `get <key>` to inspect a setting and `set <key> <value>` to change it. Successful
changes save automatically. Numeric input must be a complete finite decimal
number; integer fields reject fractional values. Invalid input is rejected
without clamping or shortening. Boolean settings accept `on`, `off`, `1`, or `0`.

| Key | Values and meaning |
| --- | --- |
| `username`, `name` | Aliases for the same advertised companion name; 1–31 UTF-8 bytes. Spaces are accepted, with optional enclosing double quotes. Control characters, invalid UTF-8 and empty/space-only names are rejected. |
| `radio` | A complete `frequency,bandwidth,SF,CR` tuple, with MHz and kHz units, such as `915.125,125,7,5`. The tuple is validated as one operation. |
| `freq` | Frequency in MHz, 150–960 within SX1262 software limits. Actual board frequency coverage remains hardware dependent. |
| `bw` | Bandwidth in kHz: `7.8`, `10.4`, `15.6`, `20.8`, `31.25`, `41.7`, `62.5`, `125`, `250`, `500`. |
| `sf`, `cr` | Integer spreading factor 5–12; coding-rate denominator 5–8, representing 4/5 through 4/8. |
| `tx` | Transmit power request in dBm, −9 through the compiled `MAX_LORA_TX_POWER` (22 for this target). This is not a measured antenna power value. |
| `radio.rxgain` | SX1262 boosted receiver gain, boolean. |
| `radio.fem.rxgain` | External front-end LNA, boolean; available only when the board exposes LNA control. Independent of `radio.rxgain`. |
| `repeat` | Companion client-repeat mode, boolean. Enabling it or changing frequency while it is enabled must satisfy the existing client-repeat frequency ranges. This does not change the firmware role. |
| `af` | Airtime budget factor, 0–99. |
| `dutycycle` | Equivalent airtime-budget percentage, 1–100; `af = 100 / dutycycle - 1`. This controls firmware scheduling, not a measured RF duty cycle. |
| `rxdelay` | Receive-delay base, 0–20; a dimensionless input to the existing scheduling formula. |
| `multi.acks` | Additional acknowledgement transmissions, integer 0–3. |
| `lat`, `lon` | Stored latitude −90–90 and longitude −180–180, in decimal degrees. A change updates the companion's current location; subsequent valid GPS fixes can replace it. |
| `gps` | Stored GPS enable preference, boolean. A changed setting requires an available GPS provider. |
| `gps_interval` | GPS update interval in seconds, integer 1–86400. |
| `advert.loc` | Location in advertisements: 0 omitted, 1 shared. |
| `telemetry.base`, `telemetry.loc`, `telemetry.env` | Telemetry permissions: 0 deny, 1 use contact permissions, 2 allow all. |
| `manual.add.contacts` | Boolean; 1 requires manual contact addition. |
| `autoadd.config` | Existing companion auto-add bitmask, 0–255. Defined bits: 1 replace the oldest non-favourite contact when full, 2 chat nodes, 4 repeaters, 8 room servers, 16 sensor nodes. Upper bits are reserved. |
| `autoadd.max.hops` | 0 unlimited, 1 direct, N allows up to N−1 hops; maximum 64. |
| `path.hash.mode` | 0, 1 or 2 for the companion's existing path-hash modes. |
| `default.scope` | `none` or `Name <32 hexadecimal digits>` for the companion's default flood scope and 16-byte transport key. Names may contain spaces and occupy at most 30 UTF-8 bytes. Reads show the name and key-presence status, never the key. This is separate from `fall.channel`. |
| `pin` | `0` selects the board's default pairing policy; otherwise a six-digit integer 100000–999999. Saved immediately, effective after reboot. |
| `adc.multiplier` | Existing battery conversion multiplier, 0–10; 0 selects the board default. Reads report the effective multiplier. See [battery calibration](cli_commands.md#battery-diagnostics-and-calibration-usb). |
| `fall.channel` | Existing persisted public/private fall-message destination. See [channel configuration](cli_commands.md#fall-message-channel-usb-and-ble). |
| `calibrate` | Existing calibration status/control alias. See [stationary calibration](gyro_calibration.md). |
| `public.key`, `role` | Read-only public identity and firmware role. |

Individual radio-field setters retain the other radio parameters. Frequency,
bandwidth, SF and CR must match the intended network configuration. A successful
hardware API result confirms acceptance by the driver, not communication with
another node.

### Username behavior

```text
set username "Medical Node 2"
get username
get name
```

Both reads return the same stored name. Future ordinary channel messages and
self-advertisements use that name. No new private identity or public key is
created. Existing queued messages retain their original content. Peers may retain
cached names until a subsequent advertisement or contact refresh. The BLE device
discovery label may require reboot/reconnection to refresh.

Fall alerts and post-fall responses retain their existing nameless group-text
format. Changing `username` does not insert a name into those messages or change
their channel destination.

### Shared USB and BLE settings

The USB configuration path and corresponding BLE setters share candidate
validation, peripheral application and checked preference persistence. This
covers name, radio parameters/power, tuning, location, telemetry/other parameters,
path-hash mode, PIN, GPS custom variables, auto-add configuration and default flood
scope. Binary protocol command identifiers and reply formats remain unchanged;
malformed values that were previously clipped or clamped now return an error.

Configuration writes are refused during active fall/help incidents, active
calibration, queued transmissions, packet reception or transmission completion.
Retry after the operation becomes idle. Read-only diagnostics remain available.
The existing `fall.channel` and `calibrate` controls retain their separately
documented rules; a channel change affects new incidents only.

Preferences are written to a temporary file, read back byte-for-byte, and promoted
with a backup of the previous committed file. Startup uses that backup if the
active file is absent following an interrupted rename. Apply or save failures
attempt to restore previous live peripheral settings. A failed rollback reports
uncertain hardware state and requires recovery/reboot; it does not report success.
Physical power-loss and filesystem-fault behavior still require device testing.

Explicit setters can perform flash writes and peripheral reconfiguration, which
may delay sampling. They are maintenance operations, not a way to continuously
adjust the device during monitoring. Statistics collection and read-only
statistics commands perform no flash writes.

### Upstream command compatibility

The upstream remote-node CLI and the local companion CLI have different firmware
roles. Applicable get/set settings are listed above. Other upstream keys are
recognized but return an explicit unsupported reply:

| Keys | Reason |
| --- | --- |
| `int.thresh`, `cad`, `agc.reset.interval`, `txdelay`, `direct.txdelay` | These controls are not exposed as mutable companion preferences. |
| `advert.interval`, `flood.advert.interval`, `flood.max`, `flood.max.advert`, `flood.max.unscoped`, `loop.detect` | Repeater/server forwarding and advertising controls are not implemented as companion settings. |
| `allow.read.only`, `guest.password`, `owner.info` | Remote administration/server settings are absent from this firmware role. |
| `bridge.type`, `bridge.enabled`, `bridge.delay`, `bridge.source`, `bridge.baud`, `bridge.channel`, `bridge.secret` | No bridge is configured in this firmware target. |
| `extra.sf`, `bootloader.ver` | Hardware-specific upstream facilities are unavailable in this target's text CLI. |
| `pwrmgt.support`, `pwrmgt.source`, `pwrmgt.bootreason`, `pwrmgt.bootmv` | The upstream power-management command family is not exposed through this companion text interface. |
| `prv.key` | Private identity import/export is not exposed through USB text commands. |

Unknown keys return `Error: unknown setting`. Unsupported commands never report
successful changes. The upstream remote-node reference in
[cli_commands.md](cli_commands.md) does not expand the local firmware role.

## Health and motion inspection

| Command | Output |
| --- | --- |
| `health` | Existing concise status, including since-startup `max_gap_ms` and `gaps`. |
| `health detail` | Uptime, sensor initialization/validity, current sample age or `n/a`, motion fault, detector phase, cooldown, calibration and alarm/delivery states. |
| `health timing` | Fresh sample count, interval count, covered duration, latest/minimum/maximum/mean spacing in milliseconds, observed sample rate, gaps and rejected nonfinite samples. |
| `health errors` | Bus failures, short reads, polls without data-ready, data-ready timeout attempts, initialization attempts/failures, reinitializations, successful recoveries, configuration and identity mismatches. |
| `health radio` | Current radio settings, outbound queue size, radio busy state, packet/airtime counters, last packet RSSI/SNR when available, noise-floor estimate, flood/direct counters. Local radio counters do not establish recipient delivery. |
| `health stats reset` | Reset motion/timing aggregates and sensor diagnostic counters. Preserve detector state, faults, alarms, calibration, radio counters and since-startup gap counters. |
| `motion` | Existing latest corrected sample: acceleration axes, gyro axes, chip temperature. |
| `motion raw` | Existing latest uncorrected, scaled sample; not integer register values. |
| `motion stats` | Corrected axis and magnitude statistics plus chip temperature. |
| `motion stats raw` | Uncorrected gyro axis/magnitude statistics; acceleration and temperature are shared with corrected statistics. |
| `motion stats reset` | Reset motion/timing aggregates only. Sensor error counters, detector state, faults, alarms and calibration remain unchanged. |

Each motion statistics response identifies its sample count, reset reason,
first/last sample times and covered duration. Each row reports minimum, maximum,
arithmetic mean and **sample standard deviation** (denominator N−1). Standard
deviation is `n/a` for one sample. With no samples, the entire summary is
unavailable. Gyro units are degrees/second, acceleration is g including gravity,
and temperature is MPU6050 chip temperature in Celsius.

Statistics include fresh valid samples only, including samples collected during
calibration. Invalid readings and repeated cache reads do not contribute zeros
or increase the sample count. The ordinary detector runs before statistics are
updated. A sensor outage leaves previous aggregates available, identified as
historical by `valid_now=0`; it cannot produce a reassuring zero-motion sample.

Covered duration is the sum of intervals between accepted samples, including
outages. Observed rate is `(N−1) × 1000 / coverage_ms`, not an instantaneous sensor
rate. Resettable timing values cover the current statistics interval. The
original `health` fields `max_gap_ms` and `gaps` continue to cover startup onward
and are not cleared by either statistics reset command. No-data-ready polls can
occur before the next conversion and are not themselves bus failures; timeout
and reinitialization counters identify persistent problems. A recovery is the
first fresh sample after a failure, not merely a successful identity read.
An incomplete HAL read that also returns a bus error increments both counters;
the diagnostic categories are not mutually exclusive.

Applying or resetting saved gyro calibration automatically starts a new motion
statistics interval with `reset=calibration-changed`. Aggregates therefore never
combine different active correction coefficients. Sensor error counters remain.
Reboot clears all RAM statistics. Single-precision online arithmetic is
approximate; regular explicit resets define comparable measurement intervals.

## Sampling overhead and verification

Collection uses fixed-size running moments and counters, without heap allocation,
historical sample storage or per-sample output. A shared reciprocal is calculated
once per sample for axis updates. Standard-deviation square roots and text
formatting occur only for an explicit query. A command captures one consistent
statistics snapshot, then formats at most one motion-statistics row per companion
loop. USB output drains at most 32 bytes per loop and never writes more than the
reported immediate transmit capacity. Backpressure pauses replies rather than
waiting for the host. USB input also retains its existing 16-byte-per-loop budget.

No logging, streaming or trailing-window commands are added. Statistics are
aggregates since startup, explicit reset or calibration application.

Native tests exercise parsing, UTF-8 bounds, candidate isolation, failed saves,
sample moments, invalid/fresh data handling, rollover, serial backpressure and
unchanged detector decisions under collection/reset. These checks do not compile
or validate the complete ESP32 image, physical serial/BLE operation or timing.

The following post-flash procedure compares sampling with diagnostics idle and
with repeated queries. Measured hardware timing is required before claiming
negligible sampling impact.

## Post-flash USB CLI verification

Connect at **115200 baud, 8N1, without flow control**, using the
[Windows or Linux terminal procedure](usb_serial_debugging.md). Send each command
separately and wait for its complete response. The command blocks below list
individual commands; they are not shell scripts or instructions to paste a burst
of commands into the device.

### 1. Check command availability and sensor readiness

```text
help
help set
help health
help motion
get username
get name
get radio
health detail
health errors
motion
motion raw
```

The help commands should list the new configuration and statistics functions.
`get username` and `get name` should return the same value. Record that original
name and the radio tuple before testing setters.

After several seconds stationary, `health detail` should show `initialized=1`,
`valid=1`, `fault=0` and `calibration_active=0`. The detector should reach
`detector=armed` after its quiet rearm period, provided no cooldown remains.
Resolve sensor faults before comparing timing. A valid sensor cache alone does
not establish that the detector is armed or receiving fresh samples.

### 2. Check the username setter and rejection of invalid input

Run while incident handling, calibration and radio activity are idle:

```text
set username "CLI Test Node"
get username
get name
set sf 99
get radio
```

Both name reads should return `CLI Test Node`. `set sf 99` should return an error;
the subsequent radio tuple should match the recorded original tuple. A busy
response to a valid setter requires retrying when the relevant activity is idle.

Restore the original name with `set username "<original name>"`, replacing the
placeholder with the recorded value, then verify it with `get username`. This
test changes the persisted advertised name temporarily. Existing fall messages
retain their nameless group-text format.

### 3. Measure a quiet baseline

Keep the device stationary and maintain a consistent BLE/radio workload for
both comparison runs. Avoid calibration and configuration writes during timing
measurements: calibration deliberately pauses detection, and setters may perform
flash writes or peripheral reconfiguration.

```text
health stats reset
```

After the reset response, wait **60 seconds without issuing commands**, then run:

```text
health timing
health errors
health detail
```

Record all three responses as the baseline. The reset clears motion/timing
aggregates and sensor diagnostic counters; it does not clear an existing fault,
alarm, calibration, detector state or since-startup gap counter.

### 4. Measure repeated-query load

Begin a new measurement interval:

```text
health stats reset
```

For **60 seconds** after the reset response, repeatedly cycle through the commands
below. Wait for each full reply before sending the next command, including all
nine measurement rows in a motion-statistics reply. Do not reset statistics
within this interval.

```text
motion stats
motion stats raw
health detail
health radio
```

Finish with the same measurement commands used for the baseline:

```text
health timing
health errors
health detail
```

The manual runs need not contain exactly the same sample count. Compare
`coverage_ms`, interval statistics and `observed_hz`; account for any difference
in actual duration when comparing counts. Repeat the paired baseline/query runs
with representative BLE and radio traffic, using comparable traffic in each run.

### 5. Interpret the comparison

The compiled nominal sample interval is 20 ms (50 Hz), and the detector's maximum
permitted sample gap is 100 ms. `health timing` reports these as `nominal_ms` and
`gap_limit_ms`; use the reported values if configuration changes.

| Result | Expected interpretation |
| --- | --- |
| `interval_ms mean` near 20 ms | Average fresh-sample spacing is close to the nominal interval. A repeatable increase during queries indicates additional delay requiring investigation. |
| `observed_hz` near 50 Hz | Fresh-sample throughput is close to nominal. A sustained decrease under queries is more significant than minor differences between manual runs. |
| Resettable `gaps=0` | No completed interval between accepted samples exceeded 100 ms during the measurement. A gap above this threshold interrupts partial detector evidence and requires rearming. |
| Interval `max` without substantial query-related spikes | Large delays are not appearing specifically during query load. A good mean alone cannot exclude occasional disruptive gaps. |
| No new bus errors, data timeouts or reinitializations | No corresponding sensor-failure evidence accumulated after reset. Failures present in both runs must be investigated before attributing them to statistics. |
| `valid=1`, `fault=0`, detector armed when stationary outside cooldown/calibration | Sensor data and detector state remain available under the tested conditions. These fields do not establish detection accuracy. |
| `no_data_ready` increases without timeouts or other failures | Polls can precede a new conversion; this counter alone does not establish a bus fault. Interpret it alongside sample rate, gaps and timeout counters. |

The original `health` fields `max_gap_ms` and `gaps`, also labelled
`startup_max_gap_ms` and `startup_gaps` in detailed output, are since-startup
values. Neither statistics-reset command clears them. Use the resettable
`interval_ms max` and `gaps` in `health timing` to compare the two measurement
intervals; retain the startup values as broader context.

Gap statistics describe intervals completed by a subsequent fresh sample. An
ongoing outage may therefore not yet appear as a large interval or counted gap.
Inspect sensor validity, sample age and fault state as well. The current fault
timeout is five seconds without fresh valid motion data; absence of that fault
does not imply that every interval stayed below 100 ms.

No fixed percentage change is established as an acceptance threshold. Repeated
query-associated increases in interval maxima, gap counts, errors or sustained
mean spacing warrant investigation. Preserve the baseline and loaded responses
with the BLE/radio conditions and approximate run durations for comparison.

### Verification limits and additional bench checks

Statistics collection remains enabled during both runs. This comparison measures
the additional effect of requesting, formatting and transmitting replies; it
does **not** isolate collection overhead against statistics-disabled firmware.
That requires a separate instrumented or disabled-collection comparison.

A controlled bench motion sequence can additionally compare detector behavior
with and without repeated queries. Begin each trial with the detector armed and
account for the 60-second post-event cooldown and normal quiet rearm period.
Such trials may activate SOS and transmit actual test alerts on the configured
fall channel. Matching responses in a few trials do not establish fall-detection
accuracy or medical performance.

An optional backpressure check pauses serial reads while a reply is pending,
then resumes them and inspects timing, errors and sample counts. Maintain device
power and confirm that uptime did not restart. Closing a terminal or disconnecting
USB may reset or depower the board and is not automatically a valid backpressure
test. These checks provide evidence for the tested workload, not a guarantee for
every operating condition.
