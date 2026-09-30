# Fall-detection timing implementation

Scope: the five requested fixes in `heltec_v4_sensor`. CAD and radio code are
unchanged. The BLE companion retains its existing synchronous environmental,
display, and battery behavior. Fall thresholds, confirmation time, two-second
quiet rearm, and 60-second cooldown are unchanged.

## 1. Environmental conversions and telemetry requests

- [`EnvironmentSensorManager.cpp`](../src/helpers/sensors/EnvironmentSensorManager.cpp):
  `begin()` starts `collectEnvironment()` at idle priority on ESP32 core 0. The collector owns
  `initializeEnvironment()` and all environmental driver calls. `querySensors()`
  reads cached snapshots; it does not start conversions or wait for readiness.
- [`EnvironmentSensorManager.h`](../src/helpers/sensors/EnvironmentSensorManager.h):
  sixteen fixed-size cache slots and a short cross-core snapshot lock. No sensor
  I/O or conversion wait holds that lock. Task allocation failure leaves motion
  enabled and environmental readings unavailable, with a serial diagnostic.
- [`BoundedAHT.h`](../src/helpers/sensors/BoundedAHT.h): `begin()`, `start()` and
  `poll()` implement reset, calibration, measurement, status and data stages.
  Initialization and measurement each have a one-second deadline. Missing bytes
  and stuck BUSY status fail rather than entering an unlimited library wait.
- `query_bme680()` starts conversion, yields while checking readiness, and calls
  `endReading()` only when ready. Its one-second deadline rejects late data; an
  expired transaction is discarded before the next fresh measurement.
- [`TelemetrySnapshot.h`](../src/helpers/sensors/TelemetrySnapshot.h) and
  [`EnvironmentTelemetry.h`](../src/helpers/sensors/EnvironmentTelemetry.h)
  publish, age-check and replay encoded readings without changing their bytes.
  Empty or overflowed results invalidate the preceding cache entry. Refresh is
  every 60 seconds; entries aged 120 seconds are omitted. Device libraries that
  do not report a read error retain their own validity limitations. Other vendor
  drivers retain their internal waits in the collector; a stuck collector can
  stop environmental updates, whose cached values then expire.

Channel 2 remains reserved for motion even during an MPU fault. Environmental
channels start at 3 in initialization order. Location/environment permission
masks are still applied at query time. Each cache slot holds up to 64 payload
bytes; the collector has a configured 6,144-byte task stack, plus RTOS overhead.

## 2. Transient MPU failures

[`MPU6050.h`](../src/helpers/sensors/MPU6050.h), `poll()`:

- Invalidate a failed/partial reading immediately.
- Retry reads after 20 ms. After three consecutive failures, attempt
  reinitialization on the next short retry.
- Use five-second backoff only after initialization fails. Successful
  reinitialization retains its 100 ms settling period.
- Use task-local read/write buffers with ESP32 `i2cWrite()` and
  `i2cWriteReadNonStop()`. These share the HAL bus mutex with the collector,
  avoiding races on `Wire1`'s shared receive buffer. Individual transactions
  use a 5 ms timeout; conversion waits do not own the bus.

A brief fault no longer automatically pauses hardware reads for five seconds.
Invalid data still clears detector evidence and requires quiet rearming.

## 3. Analysis independent of notifications

[`main.cpp`](../examples/simple_sensor/main.cpp), `MyMesh::pollMotion()`, gates
analysis on sample validity/freshness rather than alarm or message state. It
services motion at loop entry and again after radio work. An outstanding direct
alert is retained, and new detections log that their notification was coalesced.

[`FallResponse.h`](../examples/simple_sensor/FallResponse.h), `onFall()`, reports
whether a new SOS must start. An active SOS keeps its existing click progress.
`updateButton()` coalesces an acknowledgment into an already-pending assistance
message without extending its retry budget. If a later detection occurs after
the earlier SOS stopped, it starts a new local SOS even with an old send pending.

## 4. OLED transfers

- [`SSD1306Display.cpp/.h`](../src/helpers/ui/SSD1306Display.cpp): `endFrame()`
  queues a frame, `service()` sends one 32-byte piece, and `framePending()` exposes
  progress. Each piece has explicit column/page addressing. Bus errors abandon
  the frame; the next refresh retries with a new frame. Bus clock and timeout
  settings are restored after each piece.
- [`OledFrameTransfer.h`](../src/helpers/ui/OledFrameTransfer.h) tracks offsets,
  completion and cancellation without allocating another framebuffer.
- [`DisplayDriver.h`](../src/helpers/ui/DisplayDriver.h) supplies optional
  `service()`/`framePending()` hooks with no-op defaults.
- [`UITask.cpp`](../examples/simple_sensor/UITask.cpp), `loop()`, services one
  piece per pass and does not redraw over a frame still being sent.

A healthy data piece takes about 0.8 ms of bus time at 400 kHz, plus addressing
and software overhead. This is a calculated estimate, not a device measurement.

## 5. Shared, staged battery acquisition

[`HeltecV4Board.cpp/.h`](../variants/heltec_v4/HeltecV4Board.cpp), `pollBattery()`,
uses [`StagedBatteryRead.h`](../src/helpers/StagedBatteryRead.h): enable the ADC
divider, return during its 10 ms settling interval, then take one conversion per
loop call until eight samples are averaged. Start the next acquisition one second
after completion. `getBattMilliVolts()` returns the cache without hardware I/O.

`MyMesh::pollLowBattery()` advances acquisition and waits for its first valid
reading. [`SensorMesh.cpp`](../examples/simple_sensor/SensorMesh.cpp),
`handleRequest()` and `loop()`, omit startup voltage until it exists;
`onSensorDataRead()` also avoids treating startup as a depleted battery.
Existing thresholds, warning tones and fall-SOS priority remain intact.

## Configuration and validation

[`HealthNodeConfig.h`](../include/HealthNodeConfig.h) exposes retry intervals,
failure count, environmental refresh/expiry/deadline, task stack, I2C timeout,
OLED chunk size, ADC settling time and sample count, with configuration guards.

[`test_sensor_timing.cpp`](../test/test_sensor_timing/test_sensor_timing.cpp)
covers transient/persistent MPU failures, rollover, AHT timeout/short reads,
cache expiry/invalidation, byte-preserving replay and capacity, staged ADC
averaging, detection during SOS, acknowledgment retry coalescing, and OLED chunk
coverage/fault cancellation. Supporting fakes are in `test/mocks/Wire.h`,
`esp32-hal-i2c.h`, and `CayenneLPP.h`.

Final verification: `verify.ps1` passed both `heltec_v4_sensor` and
`heltec_v4_companion_radio_ble` builds, generated both merged firmware images,
and passed all **81 native test cases**, including **11 new timing/fault tests**.
The sandbox initially blocked native executable launch with Windows `WinError 5`;
the complete verification then passed with execution permission. The log is
`.pio/fall-timing-verify.log`. CAD/radio sources and `MotionRule.h` were also
compared with the pre-edit snapshot and are unchanged.

No firmware was flashed. Host tests use simulated transactions and time, not a
real MPU, display, radio, ADC or RTOS scheduler; they do not establish device
timing or fall accuracy.

## Remaining hardware checks

Measure sample spacing and invalid/gap events with actual environmental sensors,
OLED on/off, telemetry traffic and transient I2C faults. Verify alarm/button
behavior during pending sends, voltage accuracy, and collector stack headroom.
Motion remains cooperative with radio, serial, flash and GPS work; these changes
do not guarantee a hard 20 ms deadline. FIFO/interrupt sampling is not implemented.
CAD behavior remains exactly as before this change.
