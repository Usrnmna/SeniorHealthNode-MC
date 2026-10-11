# Stationary gyroscope calibration

The `heltec_v4_companion_radio_ble` firmware provides a nonblocking, stationary
MPU6050 gyroscope calibration. It estimates the zero-rotation offset of each axis
from **15 continuous seconds** of fresh readings, checks the result against a
separate **2-second validation window**, and saves it only on an explicit command.
A **3-second preparation countdown** precedes collection. An uninterrupted attempt
therefore takes approximately **20 seconds**, subject to cooperative sampling.

Calibration supports **any fixed orientation**: horizontal, vertical or tilted.
The GY-521 does not need precise leveling. The device must remain physically
stationary on a stable surface throughout measurement and validation.

This feature corrects gyroscope offsets only. Acceleration retains gravity,
approximately 1 g in magnitude at rest. Accelerometer offsets, accelerometer
scale, gyroscope scale and fall-detection thresholds are unchanged. Six-position
accelerometer calibration is not implemented.

## Preparation

1. Install firmware containing the calibration commands. Editing source files
   alone does not update a device or an existing firmware binary.
2. Verify stable MPU6050 readings with `motion` and `health`. Repeated
   `MPU6050 unavailable` or read/data-ready failures require correction before
   calibration; offsets cannot repair I2C, power or data-ready faults.
3. Place the assembled device on a rigid, vibration-free surface. Arrange the
   USB cable to prevent movement. Hand-held or worn calibration is unsuitable.
4. Allow the sensor temperature to stabilize under representative operating
   conditions. Chip temperature is available through `motion`; it is not body
   temperature. A fixed warm-up time is not imposed by the firmware.
5. Ensure no fall alarm or fall/help delivery remains active. Calibration starts,
   saves and resets are rejected while an incident or its queued messages remain
   active; normal acknowledgement and delivery behavior must complete first.

Automatic fall detection is **paused during preparation, measurement and
validation**. `health` reports `motion=CALIBRATING`. BLE, USB, radio services,
battery acquisition and sensor-health tracking continue. Buzzer playback is
stopped at entry, the `beep` command is rejected, and battery tones are suppressed
until collection ends. Calibration also works without a connected buzzer.

Detection resumes automatically after success, cancellation, timeout or failure,
using the previous saved correction until a new one is saved. Partial fall
evidence is discarded; the usual quiet rearm conditions and any existing
cooldown remain applicable. The pending-result review period does not pause
monitoring.

## USB procedure on Windows or Linux

Open the USB serial port at **115200 baud**, using the
[Windows PowerShell and Linux serial guide](usb_serial_debugging.md). CR, LF or
CRLF terminates a command. The following commands are identical on both systems.

```text
health
motion raw
calibrate
```

Keep the device stationary. Serial progress appears at approximately one-second
intervals and on phase transitions. `calibrate status` also requests progress.
An illustrative transition is:

```text
Calibration: measuring 7s n=351 retry=0 keep-still; detection=paused
Calibration: validating 0s n=0 retry=0 checking-residual; detection=paused
Calibration: ready-unsaved 0s n=101 retry=0 validated-unsaved; detection=resuming-with-normal-rearm
```

Here, `n` is the current phase's accepted sample count, `retry` counts restarted
attempts, and seconds represent elapsed time within the current phase. These
values are examples rather than required exact counts. `ready-unsaved` means
validation passed, but the live correction has not changed.

Review the pending result and the existing saved result, then save:

```text
calibrate show
calibrate show saved
calibrate save
motion
motion raw
```

`calibrate show` selects the pending result when available; otherwise it shows
the active saved/default correction. `calibrate show saved` always shows the
active correction. The reply identifies `UNSAVED`, `SAVED` or `DEFAULT` and reports:

- `bias[dps]`: X/Y/Z offsets subtracted from subsequent gyroscope readings.
- `noise`: X/Y/Z sample standard deviations in degrees/s during measurement.
- `temp`: mean measurement temperature in Celsius.
- `span`: measurement temperature range in Celsius.
- `n`: measurement sample count, excluding countdown and validation samples.

After saving, `motion` displays corrected angular rates. `motion raw` displays
the scaled readings before correction, rather than integer register counts.
Both retain the same acceleration and temperature. Correction also applies to
fall detection, residual-movement assessment and gyroscope telemetry; telemetry
retains its existing encoding limits.

## Commands

| USB command | Behavior |
| --- | --- |
| `calibrate` or `calibrate start` | Start countdown, measurement and validation. |
| `calibrate status` | Report phase, elapsed phase time, current sample count, restarts and reason. |
| `calibrate show` | Inspect pending coefficients, or saved/default coefficients if none are pending. |
| `calibrate show saved` | Inspect the currently applied correction even when a new result is pending. |
| `calibrate save` | Persist and activate a validated pending result; invalidate detector evidence for rearm. |
| `calibrate cancel` | End collection or discard a pending result; retain saved correction. |
| `calibrate reset` | Persist zero correction; reject while collection or a fall/help incident is active. |
| `get calibrate` | Alias for `calibrate status`. |
| `set calibrate start`, `set calibrate save`, `set calibrate cancel`, `set calibrate reset` | Aliases matching companion custom-variable control. |
| `motion raw` | Inspect uncorrected, scaled sensor readings. |

A new start is rejected while calibration is active or a validated result is
pending. Cancel or save that result first. Unsaved results expire **120 seconds
after validation**, and are discarded on reboot. A failed save retains the live
correction and pending result, allowing a retry before expiration.

To remove an existing correction:

```text
calibrate cancel
calibrate reset
calibrate show saved
```

`DEFAULT`, zero offsets and `n=0` identify an uncalibrated correction. Default
temperature/noise fields are placeholders, not measured evidence.

## MeshCore local CLI over BLE

Use a client supporting the companion custom-variable `get`/`set` interface,
such as the local CLI described in the [fall-channel guide](cli_commands.md#fall-message-channel-usb-and-ble).
Select the **local companion session**, not a remote repeater session:

```text
session local
set calibrate start
get calibrate
```

Repeat `get calibrate` until `ready-unsaved`, then apply the validated result:

```text
set calibrate save
get calibrate
```

Cancellation and reset use `set calibrate cancel` and `set calibrate reset`.
A bare `calibrate` is a USB command; it must not be assumed to exist in a client
application's own command parser.

The firmware uses existing companion opcodes **40/41**. Enumeration includes
`calibrate:<status>`, and control writes use `calibrate:start`, `calibrate:save`,
`calibrate:cancel` or `calibrate:reset`. Reads and writes share the same firmware
state as USB. A BLE write receives a standard OK/error frame; it does not stream
progress or return the USB coefficient report. Detailed coefficients and rejected
command explanations are available through USB. A successful start response
means collection started, not that validation or saving has completed.

The protocol supports this workflow without changing firmware architecture.
Physical client/BLE interoperability remains a separate bench check.

## Quality checks and failure recovery

Only complete, data-ready MPU6050 readings enter calibration. Identical stationary
values may still be fresh. Cached values are never counted again merely because
the main loop runs. Measurement averages use readings before any existing
correction, preventing repeated calibration from accumulating offsets.

| Status/reason | Meaning and recovery |
| --- | --- |
| `preparing` | Preparation countdown; continue holding the device stationary on its support. |
| `measuring` | Collecting the continuous stationary window. |
| `validating` | Checking new independent samples against the proposed offsets. |
| `ready-unsaved` | Validation passed; inspect and save within 120 seconds. |
| `movement`, `rotation-too-large`, `acceleration-not-stationary` | Countdown and measurement restart after motion or unacceptable rotation/gravity values. Check support, vibration and sensor health. |
| `gyro-noise`, `temperature-drift`, `validation-residual` | Quality checks restarted the attempt. Stabilize the environment and temperature; investigate persistent failures. |
| `sensor-error`, `sample-gap` | Attempt aborted on invalid sensor state or a fresh-data gap exceeding 100 ms. Correct the sampling fault, then start again. |
| `invalid-sample`, `temperature-range` | Attempt aborted on nonfinite readings or temperature outside -40 to 85 Celsius. Investigate sensor data. |
| `too-few-samples` | Measurement or validation did not collect enough fresh samples within its window. Investigate sampling delays. |
| `timeout` | The 60-second total attempt budget expired. Repeated restarts cannot extend this deadline. |
| `candidate-expired` | An unsaved validated result expired; calibration must be repeated. |
| Save error | Live correction is unchanged. Investigate filesystem access; retry a pending save before expiration. |

Calibration cannot distinguish every constant rotation from sensor bias. A
physically stationary support remains necessary even when the quality checks
pass. A quiet recording also cannot establish detector sensitivity, specificity,
or suitability of fall thresholds.

## Settings and persistence

All calibration limits are grouped under `gyro_calibration_*` in
[`HealthNodeConfig.h`](../include/HealthNodeConfig.h). Changes to these compiled
limits require rebuilding and flashing the combined firmware. The initial
engineering limits require actual-board validation:

| Setting | Default |
| --- | --- |
| Preparation / measurement / validation | 3000 / 15000 / 2000 ms |
| Overall attempt timeout / pending-result lifetime | 60000 / 120000 ms |
| Maximum fresh-sample gap | 100 ms |
| Minimum measurement / validation samples | 600 / 80 |
| Acceleration magnitude tolerance around 1 g | 0.15 g |
| Per-axis acceleration span | 0.04 g |
| Per-axis gyroscope span / standard deviation | 1.0 / 0.15 degrees/s |
| Maximum absolute per-axis offset | 20 degrees/s |
| Maximum absolute per-axis validation mean after correction | 0.30 degrees/s |
| Temperature span / difference between phase means | 2.0 Celsius |

Persistence uses two alternating checked records, `/gyro_cal0` and `/gyro_cal1`,
on the node's existing filesystem. These are device storage names. Records
contain a format version, generation, sensor/configuration signature, offsets,
measurement quality metadata and a truncated SHA-256 integrity check. The
configuration signature covers WHO_AM_I, scale ranges, filter and sample divider;
it is not a unique physical-module identifier. Replacing the module requires
recalibration even when its configuration is identical.

The previous valid slot is retained while the other slot is written and checked.
The live correction changes only after readback succeeds. Reboot selects the
newest valid record; missing records use zero correction. If neither existing
record is valid, startup reports invalid calibration and uses zero correction.
Recalibration or reset can replace readable corrupt records; unreadable storage
must be repaired before saving. Power interruption and filesystem behavior still
require hardware verification, including ambiguous readback failures.

Saving or resetting performs a deliberate flash write. Normal sampling and
progress reporting do not write calibration records. Saved values survive reboot
and firmware updates that preserve the filesystem; filesystem erasure removes
them. Automatic calibration at every boot is not performed.

## Verification

`motion stats` and `motion stats raw` provide corrected and uncorrected
fresh-sample aggregates. Applying saved coefficients or resetting to zero
correction clears motion/timing aggregates with `reset=calibration-changed`;
sensor error counters and since-startup health gap counters remain intact.
Use `motion stats reset` before a comparison interval. See the
[statistics reference](usb_cli_reference.md#health-and-motion-inspection) for
units, sample counts and reset behavior.

Host tests in
[`test_gyro_calibration.cpp`](../test/test_gyro_calibration/test_gyro_calibration.cpp)
exercise arbitrary orientations, independent validation, movement/noise/temperature
rejection, freshness and timeouts, clock rollover, expiration, storage faults and
correction of sensor readings while preserving gravity and raw values.

```text
pio test -e native -f test_gyro_calibration -f test_sensor_timing
```

On hardware, compare `motion` with `motion raw` at rest, verify saved coefficients
after reboot, check cancellation and movement rejection, and verify normal
monitoring after every exit. Repeat at representative temperatures and with
BLE/radio activity. These checks are distinct from synthetic host tests. Successful
stationary calibration does not establish fall-detection accuracy or medical
performance.
