# Combined firmware build and validation

`heltec_v4_companion_radio_ble` is the single firmware environment. It combines
BLE companion services, GY-521/MPU6050 fall detection, SOS, PRG acknowledgement,
30-second escalation, residual-movement tracking, late confirmation, delayed
double-press false-alarm responses, persistent
public/private channel selection, delivery retries and low-battery reminders.
Buzzer polarity defaults to LOW idle (`buzzer_active_low = false`); see the
[configuration and incorrect-setting effects](gy521_sensor.md#buzzer-trigger-polarity).
Output is optional (`buzzer_enabled`); host tests cover disabled operation as
well as both enabled trigger polarities, PWM disconnection during silence,
idle polling without hardware writes and inactive output after a failed pitch change.

The [stationary gyroscope calibration](gyro_calibration.md) adds USB/BLE controls,
independent validation, explicit persistence and temporary detection suspension.
Host tests exercise its state machine, record recovery and sensor corrections.
These tests do not establish actual-board bias accuracy or client interoperability.

## Reproducible verification

Follow the [Windows setup](../README.md#build-and-test-on-windows) for PlatformIO
and a C++17-capable GCC compiler, then run:

```powershell
.\verify.ps1
```

The script anchors to its own project folder and stops on a failed command.
It builds and merges the firmware, runs `native`, then runs `native_delivery`.
Use `-Python '<python-executable>'` and
`-CompilerBin '<compiler-bin-directory>'` to select existing tools, replacing
the placeholders with the applicable executable and compiler directory.
The Python installation must contain PlatformIO. Neither suite flashes hardware.

Equivalent individual commands, from the project root:

```powershell
.\.venv\Scripts\python.exe -m platformio run -e heltec_v4_companion_radio_ble -t mergebin
.\.venv\Scripts\python.exe -m platformio test -e native
.\.venv\Scripts\python.exe -m platformio test -e native_delivery
```

For the test commands, put the C++17 compiler's `bin` directory on `PATH` first;
`verify.ps1` does this for its selected compiler automatically.

## Build outputs

PlatformIO uses `.pio/core`, `.pio/p` and `.pio/build`. Project-local board pins
are under `boards/variants/heltec_v4`. Outputs under
`.pio/build/heltec_v4_companion_radio_ble/` include:

| File | Purpose |
| --- | --- |
| `firmware.bin` | Application image; not a complete image for flashing at address zero. |
| `firmware-merged.bin` | Combined flash image produced by `mergebin`. |
| `firmware.elf` | Linked firmware and symbols for inspection/debugging. |

Use the [documented PlatformIO upload command](../README.md#getting-started) for
installation. A successful build or merge does not verify an installed device.

## What each check covers

| Check | Scope |
| --- | --- |
| Firmware build and merge | Compiles and links the combined companion/health image and packages its flash components. |
| `native` | Detector, response, double/triple gesture arbitration, residual quiet, multipart outbox, button ownership, buzzer GPIO/LEDC requests for both polarities, battery conversion/calibration and preferences, MPU freshness/readback and sensor/timing faults, channel parsing/storage and dispatcher behavior with host fakes. |
| `native_delivery` | Production packet builder, Mesh/Dispatcher, encryption/decryption and packet pool with a fake radio and clock; public/private channel matching, false-alarm text/location, and incident-scoped retry/cancellation behavior. |
| Documentation checks | Relative file links, Markdown structure, documented settings and source/command consistency. |

Use the actual command output for pass/fail results and case counts for the
checkout being tested. Stored logs, binary sizes and results from earlier source
revisions are not evidence that a later checkout passes. See
[virtual fault testing](virtual-fault-testing.md) for focused commands and scenarios.

## Physical validation boundary

Host tests do not execute the complete BLE companion on a Heltec board. They do
not establish simultaneous BLE/motion operation, physical I2C timing, GPIO/PWM,
battery accuracy, watchdog recovery, RF range, recipient reception or fall-detection
accuracy. The simulated one-hour trace is not a hardware soak test.

Physical PRG validation must include two complete presses within one second,
waiting through the three-second decision window, and third-press priority under
BLE/radio load. Host edge timestamps do not establish physical polling latency.

The residual-quiet settings (10 seconds, 0.08 g thresholds and 5 degrees/s) require
worn-device and buzzer-vibration evaluation. Queueing, local transmission and a
repeater echo are distinct evidence; none proves that a caregiver received or read
an alert. Client parser compatibility also needs a physical phone/BLE check.
