# Fall-detection timing implementation

The `heltec_v4_companion_radio_ble` firmware combines BLE and health monitoring.
Environmental collection runs in the background; OLED and battery acquisition
are serviced in small steps. Fall thresholds, confirmation time, two-second
quiet rearm, and 60-second cooldown are preserved.

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

## 2. MPU sample freshness and recovery

[`MPU6050.h`](../src/helpers/sensors/MPU6050.h), `poll()`:

- Read INT_STATUS and the 14 measurement bytes in one 15-byte burst. Accept
  only a complete burst with DATA_RDY_INT set; a clear flag cannot advance
  fall or residual-movement evidence. INT remains unconnected.
- Verify relevant writable bits after each initialization register write,
  including data-ready enable and its clear-on-status-read policy. A mismatch
  prevents readiness and uses the failed-initialization backoff.
- Invalidate a failed/partial reading immediately. One second without new data
  also invalidates the cache and begins counting read failures, even if I2C works.
- Retry reads after 20 ms. After three consecutive failures, attempt
  reinitialization on the next short retry.
- Use five-second backoff only after initialization fails. Successful
  reinitialization retains its 100 ms settling period.
- Use task-local read/write buffers with ESP32 `i2cWrite()` and
  `i2cWriteReadNonStop()`. These share the HAL bus mutex with the collector,
  avoiding races on `Wire1`'s shared receive buffer. Individual transactions
  use a 5 ms timeout; conversion waits do not own the bus.

A brief read fault uses the short retry interval; failed initialization uses the five-second backoff.
Invalid data still clears detector evidence and requires quiet rearming.

## 3. Analysis independent of notifications

[`HealthMonitor.cpp`](../examples/companion_radio/HealthMonitor.cpp), `MyMesh::pollMotion()`, gates
analysis on sample validity/freshness rather than alarm or message state. It
services motion at loop entry and again after radio work. An outstanding
channel alert is retained, and new detections log that their notification was
coalesced. The [message delivery guide](message_delivery_changes.md) describes
current channel retries, repeater-echo confirmation, and serial health diagnostics.

[`FallResponse.h`](../examples/companion_radio/health/FallResponse.h), `onFall()`, reports
whether a new SOS must start. An active SOS keeps its existing click progress.
`updateButton()` requests assistance before escalation or a standalone confirmation
afterward. `FallButtonGesture` also recognizes two complete presses within one
second, then waits for the three-second window to close so a valid third press
wins. False-alarm acceptance stops SOS and queues the exact text plus location;
a held/late third press leaves SOS active. `updateDeadline()` selects one automatic warning at 30 seconds using
independent residual-movement evidence. Neither duplicate detections nor partial
clicks extend that deadline. `FallNoticeOutbox` retains accepted messages and gives
fall/false-alarm confirmation priority over the same incident's automatic-warning retries.
False-alarm acceptance additionally cancels that incident's initial-alert retries.
A pending double does not postpone the response deadline; monitoring continues.
If a later detection occurs after the earlier SOS stopped, it starts a new local
SOS even with an old send pending. See the [response directives](message_delivery_changes.md#post-fall-response-directives).

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
- [`UITask.cpp`](../examples/companion_radio/ui-new/UITask.cpp), `loop()`, services one
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
reading. The companion UI and telemetry consume the same cached voltage.
Existing thresholds, warning tones and fall-SOS priority remain intact.

USB `battery` reports cached raw counts, millivolts, sample age and the effective
multiplier. `battery calibrate <meter_mV>` suggests a correction from a fresh
sample without applying it. `set adc.multiplier <value>` saves an explicit
correction in companion preferences and applies it after a successful save.
Missing settings and `0` select the existing 5.42 board default. These commands
do not initiate ADC conversions. See the [meter calibration workflow](cli_commands.md#battery-diagnostics-and-calibration-usb).

## Configuration and validation

[`HealthNodeConfig.h`](../include/HealthNodeConfig.h) exposes retry intervals,
failure count, environmental refresh/expiry/deadline, task stack, I2C timeout,
OLED chunk size, ADC settling time and sample count, fresh-data timeout and
battery-calibration limits, with configuration guards.

[`test_sensor_timing.cpp`](../test/test_sensor_timing/test_sensor_timing.cpp)
covers MPU data-ready gating, configuration readback, transient/persistent
failures, rollover, AHT timeout/short reads,
cache expiry/invalidation, byte-preserving replay and capacity, staged ADC
averaging, detection during SOS, acknowledgment retry coalescing, and OLED chunk
coverage/fault cancellation. Supporting fakes are in `test/mocks/Wire.h`,
`esp32-hal-i2c.h`, and `CayenneLPP.h`.

[`test_battery_calibration.cpp`](../test/test_battery_calibration/test_battery_calibration.cpp)
checks conversion compatibility, meter-input validation, stale/saturated sample
rejection, sample-age rollover, and preference serialization with older settings.

Reproducible combined-firmware checks are described in [build validation](build-validation.md).
The timing and fault suites run through `verify.ps1`.

Host tests use simulated transactions and time, not a
real MPU, display, radio, ADC or RTOS scheduler; they do not establish device
timing or fall accuracy.

## Remaining hardware checks

Measure sample spacing and invalid/gap events with actual environmental sensors,
OLED on/off, telemetry traffic and transient I2C faults. Verify alarm/button
behavior during pending sends, voltage accuracy, and collector stack headroom.
Motion remains cooperative with radio, serial, flash and GPS work; these changes
do not guarantee a hard 20 ms deadline. FIFO/interrupt sampling is not implemented.
Radio CAD and airtime limits can delay queued messages; queue deadlines and retries
are described in the [delivery guide](message_delivery_changes.md).
