# GY-521 motion alerts on Heltec V4.3 OLED

Build the `heltec_v4_sensor` environment. This runs the polling and decision
program on the Heltec itself; a PC is only needed for building/flashing and setup.
Other firmware environments are unchanged.

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

## Build and run

With PlatformIO installed, from the `SeniorHealthNode-MC` folder:

```powershell
pio run -e heltec_v4_sensor
pio run -e heltec_v4_sensor -t upload --upload-port COM_PORT
pio device monitor --port COM_PORT --baud 115200 --echo --eol CR
```

Replace `COM_PORT` with the board's actual port. If using this repository's
`.venv`, replace `pio` with `.\.venv\Scripts\python.exe -m platformio`.
The local validation installation uses `$env:PLATFORMIO_CORE_DIR = "$PWD\.pio\core"`.
Configure the node's radio parameters to match your MeshCore network before use.

Serial startup reports detection at 0x68 or a five-second retry. Enter `motion`
(carriage return) to see acceleration x/y/z in g, rotation x/y/z in degrees/second,
and chip temperature in Celsius. At rest the acceleration magnitude should be
approximately 1 g. Chip temperature is not body temperature.

## Recipient setup

Use a MeshCore companion to discover/add the sensor, then log in to the sensor
using its admin password. Existing sensor firmware grants a newly registered
admin both low- and high-priority alert permissions. The build's default password
is `password`; change it with the existing `password <new-password>` CLI command.
Use `get acl` over serial to inspect registered contacts and permission bits.
An existing contact needs high-priority bit 128 enabled (`setperm <pubkey> <bits>`;
preserve its other permission bits). No recipient is hard-coded and alerts are
not sent to the Public channel. Without a registered recipient, there is nobody
to deliver the alert to; events are not stored for later subscribers.

Alerts are encrypted MeshCore direct messages using the existing path/flood,
acknowledgment and retry machinery. A serial `Possible fall: ... (queued)` line
means an alert task was queued, not that a recipient received it. A suffix of
`alert queue full` means the event was not queued; the motion rule does not retry it.

## Polling and rule customization

`src/helpers/sensors/MPU6050.h` reads a complete 14-byte acceleration/temperature/
gyro sample every 20 ms when the main loop can run (nominal 50 Hz). It configures
the device for +/-8 g, +/-500 degrees/second and filtered 50 Hz sampling. Failed
or partial reads invalidate the sample and cause a five-second reconnect retry.
The loop is cooperative; other firmware work can delay polls, so brief events
can be missed. INT and FIFO are not used.

### Review and tune the algorithm

Start with [`MotionRule.h`](../examples/simple_sensor/MotionRule.h). Its
`FallDetectionConfig` contains **all** decision settings; edit defaults there and
rebuild. The former `MOTION_ACCEL_THRESHOLD_G`, `MOTION_GYRO_THRESHOLD_DPS`, and
`MOTION_COOLDOWN_MS` build flags are replaced by this configuration. Custom code
can also pass a `FallDetectionConfig` to the `MotionRule` constructor.

[`main.cpp`](../examples/simple_sensor/main.cpp), `MyMesh::pollMotion()`, supplies
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
Pending alert deliveries discard partial evidence and require settling afterward;
new falls during that interval or the cooldown are not reported. Read failures
also discard evidence but preserve an existing cooldown.

Latest valid samples also appear in environment telemetry on channel 2, before
other detected environmental sensors. Cayenne gyro encoding clips individual axes
to -327.68..327.67 degrees/second; rules, serial output, and alert text retain the
full measured range. Acceleration and gyro conversions follow the manufacturer's
[MPU-6050 register map](https://cec-code-lab.aps.edu/downloads/mpu-6050_register-map.pdf).

Rule tests: `pio test -e native -f test_motion_rule` (requires a host C++ compiler).
For an older compiler without the repository's GoogleTest C++17 support, run the
same checks standalone:

```powershell
g++ -std=c++11 -DMOTION_RULE_STANDALONE test/test_motion_rule/test_motion_rule.cpp -o .pio/motion-rule-tests.exe
.\.pio\motion-rule-tests.exe
```

The tests replay synthetic 50 Hz sequences, timing boundaries, missing features,
invalid readings, gaps, cooldown/recovery, and timer rollover. Passing them proves
software behavior only; no measured sensitivity or false-alert rate is available.
For real evaluation, use labeled recordings with held-out wearers and daily
activities, reporting missed falls, false alerts per wear-day, and detection delay.
Do not ask an older adult to perform falls for testing.

After flashing, verify readings at rest and during controlled bench motion, confirm a
recipient receives the alert, check cooldown/rearming, and disconnect/reconnect
the sensor to verify recovery. Hardware and over-radio delivery need a live check.
