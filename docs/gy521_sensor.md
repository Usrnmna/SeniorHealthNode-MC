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

With PlatformIO installed, from the repository root:

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
acknowledgment and retry machinery. A serial `Motion alert` line means the event
was queued, not that a recipient received it.

## Polling and rule customization

`src/helpers/sensors/MPU6050.h` reads a complete 14-byte acceleration/temperature/
gyro sample every 20 ms when the main loop can run (nominal 50 Hz). It configures
the device for +/-8 g, +/-500 degrees/second and filtered 50 Hz sampling. Failed
or partial reads invalidate the sample and cause a five-second reconnect retry.
The loop is cooperative; other firmware work can delay polls, so brief events
can be missed. INT and FIFO are not used.

`examples/simple_sensor/main.cpp`, `MyMesh::pollMotion()`, passes the sample's
acceleration and rotation magnitudes into `MotionRule::update()`. Change that
function or the rule to use `ax`, `ay`, `az`, `gx`, `gy`, `gz`, or `temperature`
for your desired condition. The default rule triggers on either:

- acceleration magnitude >= 2.5 g (includes gravity), or
- rotation magnitude >= 250 degrees/second.

Override `MOTION_ACCEL_THRESHOLD_G`, `MOTION_GYRO_THRESHOLD_DPS`, and
`MOTION_COOLDOWN_MS` in the environment's build flags, or edit `MotionRule.h`.
Defaults require 60 seconds since the previous event plus two continuous seconds
at 0.7–1.3 g and below 30 degrees/second before rearming. Pending alert deliveries
are allowed to finish before evaluating another event. Invalid readings interrupt
the quiet period. These are demonstration motion thresholds, not a validated fall
detection algorithm.

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

After flashing, verify readings at rest and during controlled motion, confirm a
recipient receives the alert, check cooldown/rearming, and disconnect/reconnect
the sensor to verify recovery. Hardware and over-radio delivery need a live check.
