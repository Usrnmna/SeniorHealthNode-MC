# Firmware architecture and source guide

The [USB CLI reference](usb_cli_reference.md) documents companion `get`/`set`
configuration, `set username`, and detailed health/motion statistics. Statistics
use bounded RAM aggregates after fall processing; replies drain incrementally.
No logging or streaming capability is added. Hardware timing remains subject to
bench verification.

`heltec_v4_companion_radio_ble` is the only firmware target. It includes MeshCore BLE, contacts, messaging, display, GPS and environmental telemetry, plus GY-521/MPU6050 fall detection, SOS, PRG acknowledgement and low-battery reminders.

Build commands, Windows tool installation and flashing are documented in the [build guide](build-validation.md). The `native` and `native_delivery` environments are host-side software tests, not firmware images. One upload installs the companion and health-monitoring features together.

## Execution and data flow

```text
GY-521 --> fresh valid sample --> MotionRule --> possible-fall incident
                  |                                  |
                  +--> motion statistics             +--> initial-alert delivery
                  +--> residual-movement tracking    +--> FallResponse + SOS

PRG gesture + deadline + residual state --> FallResponse --> response outbox
                                                |                 |
                                                v                 v
                                          alarm control     response delivery

initial-alert delivery + response delivery --> selected channel --> mesh
```

The branches share one BLE companion image. Motion polling, response handling,
radio/BLE services and display servicing use the cooperative main loop.
Environmental acquisition runs in a background task; cached telemetry avoids
conversion waits in the motion path. An accepted response silences SOS; automatic
warnings leave it active. Queueing, local transmission and repeater evidence remain
separate from recipient reception. The [response guide](message_delivery_changes.md)
defines delivery states and cancellation behavior.

The board driver's external FEM LNA starts disabled. This is separate from the
SX126x radio-chip receive boost. Radio and BLE settings are configured through a
compatible MeshCore client; the [USB settings reference](usb_cli_reference.md)
also documents the separate receive-gain controls.

## Source layout

Health monitoring runs inside the **BLE companion**. `main.cpp` services motion, alarms, radio, BLE and the display in the same firmware loop.

| File | Responsibility |
| --- | --- |
| [`src/helpers/sensors/MPU6050.h`](../src/helpers/sensors/MPU6050.h) | Reads a GY-521/MPU6050 on `Wire1` at I2C address `0x68`. It verifies the device ID and configuration, accepts data-ready samples, and recovers after failed reads or stopped updates. |
| [`src/helpers/sensors/EnvironmentSensorManager.cpp`](../src/helpers/sensors/EnvironmentSensorManager.cpp) and [`.h`](../src/helpers/sensors/EnvironmentSensorManager.h) | Make the motion sensor available to the combined firmware and add its latest valid readings to environmental telemetry. |
| [`examples/companion_radio/health/MotionRule.h`](../examples/companion_radio/health/MotionRule.h) | Implements the low-acceleration / impact / rotation sequence, optional quiet confirmation, sampling-gap rejection, and cooldown. All settings are grouped at the top. |
| [`HealthMonitor.cpp`](../examples/companion_radio/HealthMonitor.cpp) | Services motion detection, fall acknowledgement, buzzer playback and battery polling; handles the `motion`, `health`, and `beep` commands. |
| [`examples/companion_radio/health/FallChannel.h`](../examples/companion_radio/health/FallChannel.h) and [`FallChannelStore.h`](../examples/companion_radio/health/FallChannelStore.h) | Parse public/private destinations and persist checked channel records separately from the ordinary chat channel list. |
| [`include/HealthNodeConfig.h`](../include/HealthNodeConfig.h), [`AlarmBuzzer.h`](../src/helpers/ui/AlarmBuzzer.h), and [`BuzzerPattern.h`](../src/helpers/ui/BuzzerPattern.h) | Configure and initialize the passive buzzer; expose `alarm_buzzer.beep()`, nonblocking SOS and three-tone battery playback. |
| [`LowBatteryAlert.h`](../examples/companion_radio/health/LowBatteryAlert.h) | Apply the battery threshold, charging override and ten-minute reminder interval. |
| [`FallResponse.h`](../examples/companion_radio/health/FallResponse.h), [`FallButtonGesture.h`](../src/helpers/ui/FallButtonGesture.h), and [`FallAckSender.h`](../examples/companion_radio/health/FallAckSender.h) | Recognize post-fall triple confirmation and delayed fast-double false-alarm responses, silence SOS, and queue configured UTF-8 channel text. |
| [`ResidualMovement.h`](../examples/companion_radio/health/ResidualMovement.h), [`FallNotice.h`](../examples/companion_radio/health/FallNotice.h), [`FallNoticeMessage.h`](../examples/companion_radio/health/FallNoticeMessage.h), and [`FallNoticeOutbox.h`](../examples/companion_radio/health/FallNoticeOutbox.h) | Track continuous post-fall quiet, name each response directive, format full warning texts, and retain messages with reserved capacity for late confirmation. |
| [`AssistanceDelivery.h`](../examples/companion_radio/health/AssistanceDelivery.h) and [`MotionHealth.h`](../examples/companion_radio/health/MotionHealth.h) | Track the two fall-message streams, bounded retries and repeater echoes; report prolonged sampling outages and completed sampling gaps. |
| [`boards/heltec_v4.json`](../boards/heltec_v4.json) and [`pins_arduino.h`](../boards/variants/heltec_v4/pins_arduino.h) | Select the project-local Arduino pin variant. Application board drivers remain in `variants/heltec_v4`. |
| [`variants/heltec_v4/platformio.ini`](../variants/heltec_v4/platformio.ini) | Enables the MPU6050 for `heltec_v4_companion_radio_ble` and assigns its I2C pins to GPIO4 and GPIO6. The OLED/RTC bus remains separate. |
| [`docs/gy521_sensor.md`](gy521_sensor.md) and [`test/test_motion_rule/test_motion_rule.cpp`](../test/test_motion_rule/test_motion_rule.cpp) | Document wiring, configuration, and checks for the motion rule. |

## Delivery and diagnostics

Channel delivery and motion-health diagnostics run alongside BLE companion services,
motion analysis and local alarms in the same firmware.

### Fall and response messages

The initial possible-fall alert and post-fall response messages
have independent delivery controllers on the configured channel, initially
`#falldetect`. Each retains its event text through packet-allocation failures,
TX-start failures, TX timeouts, and queue expiry. Motion analysis, button handling,
and buzzer servicing continue while delivery is pending, subject to the existing
cooperative main loop.

Default limits in [`HealthNodeConfig.h`](../include/HealthNodeConfig.h) are:

| Control | Default behavior |
| --- | --- |
| Local failure retry | Wait 5 seconds before retrying; at most 60 allocation/enqueue attempts per event. |
| Queue residence | Expire a tracked packet after 30 seconds if it has not started transmitting. |
| Repeat evidence | Wait 15 seconds after local TX for a matching forwarded packet; allow at most four successful local sends without one. |
| Retry text | Keep the original channel, message, and selected location; use a fresh timestamp for each attempt. Fall messages omit the node-name prefix. |

Receivers may display duplicate text. A matching repeater echo is rebroadcast
evidence, not confirmation that a caregiver received or read the message. A direct
listener may receive text even when no repeat is heard. Pending events are held
in RAM and do not survive reboot. See the [delivery implementation and adjustment
guide](message_delivery_changes.md) for state transitions, late echoes,
failure handling, and configuration details.

### Serial health and sampling diagnostics

The [stationary gyroscope calibration guide](gyro_calibration.md) describes
`calibrate`, progress, save/cancel/reset, raw readings and MeshCore local CLI use.
Calibration accepts any fixed orientation, collects 15 seconds of stationary
data after a countdown, and validates a new result before explicit saving.
Automatic fall detection pauses during collection and resumes with normal rearm.
Acceleration retains gravity; accelerometer calibration is not included.

The [USB serial debugging guide](usb_serial_debugging.md) provides Windows
PowerShell and Linux setup, port discovery, timestamped logging, command examples
and troubleshooting for MPU6050, battery and buzzer behavior.

At 115200 baud, enter `health` followed by carriage return to inspect:

| Field | Meaning |
| --- | --- |
| `fall` / `help` | Separate states for the initial fall alert and the current response-message part: idle, pending, queued, locally transmitted, repeater echo heard, or a terminal failure/unconfirmed result. |
| `part_failed` | `1` if a part of the current or most recently completed response event exhausted retries or failed formatting. A later part's successful delivery does not erase it; the next event resets it. |
| `motion` | `CALIBRATING` during the explicit calibration pause; otherwise `FAULT` after 5 seconds without fresh valid motion data. A fresh valid reading clears the fault. `monitoring` does not mean the detector is armed. |
| `max_gap_ms` / `gaps` | Longest completed interval between fresh valid samples, and count of intervals exceeding the detector's 100 ms gap limit. An ongoing outage contributes an interval when sampling resumes. |

These diagnostics do not relax the detector's data guards or quiet-rearm rules.
Serial input is capped at 16 bytes per loop pass, with no firmware character echo; command
execution and other blocking work can still delay sampling. See the [CLI
reference](cli_commands.md) and [`MotionHealth.h`](../examples/companion_radio/health/MotionHealth.h).

### Timing, wiring, and regression coverage

- **Sampling and peripheral scheduling:** bounded MPU6050 recovery, background
  environmental collection with cache expiry, 32-byte OLED transfer pieces, and
  staged battery ADC acquisition reduce interference with motion polling. The
  [timing report](fall_timing_changes.md) maps these changes to their source
  files and tunables. Sampling remains cooperative; interrupt/FIFO capture is not
  implemented.
- **Pin reference:** the [simple pin schematic](pin_schematic.md) collects
  external MPU6050/buzzer wiring, onboard display/radio/control signals, and optional
  GPS, RTC, and sensor connections. It uses ESP32 GPIO numbers and is derived from
  firmware; it is not a complete PCB netlist or physically verified wiring plan.
- **Virtual faults:** the [fault-testing guide](virtual-fault-testing.md)
  covers sensor disconnections, truncated I2C reads, loop stalls, button bounce,
  battery/SOS priority, OLED failures, ADC scheduling, and a deterministic replay
  of one simulated hour.
- **Production transport tests:** `native_delivery` exercises the actual group-text
  packet builder, Mesh/Dispatcher, encryption, and packet pool with a fake radio
  and clock. Coverage includes queue expiry, TX failures, repeat matching,
  independent message streams, and sampling-health recovery. `verify.ps1` runs
  both `native` and `native_delivery` after building the combined firmware and merged image.
