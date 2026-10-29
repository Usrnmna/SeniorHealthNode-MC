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
Regression coverage also checks that stale duty readback during pin attachment
cannot replace the sounding duty. Product data and physical checks are in the
[buzzer module reference](buzzer_module_reference.md).

The [stationary gyroscope calibration](gyro_calibration.md) adds USB/BLE controls,
independent validation, explicit persistence and temporary detection suspension.
Host tests exercise its state machine, record recovery and sensor corrections.
These tests do not establish actual-board bias accuracy or client interoperability.

## Build and test on Windows

The repository root contains `platformio.ini`, `src`, `test`, and
`variants`. No source files from a parent or sibling `SeniorHealthNode` folder are
required. PlatformIO keeps its core cache in `.pio/core`, toolchain packages in
the shorter `.pio/p` path, and build output in `.pio/build`.

From the repository root, create the local build environment:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install platformio==6.2.0
```

The native tests require a C++17-capable GCC compiler. The old PlatformIO MinGW
5.1 compiler cannot build GoogleTest 1.17. A standalone Windows compiler is
available from [WinLibs](https://winlibs.com/). Put its `bin` directory on `PATH`,
pass it through the `verify.ps1 -CompilerBin` parameter, or extract it into
`.pio/tools` so the compiler is `.pio/tools/mingw64/bin/g++.exe`.

## Reproducible verification

Follow the [Windows setup](#build-and-test-on-windows) for PlatformIO
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

Use the [PlatformIO upload procedure](#uploading-the-combined-firmware) for
installation. A successful build or merge does not verify an installed device.

## Uploading the combined firmware

After completing [assembly and setup](setup.md), build and upload from the project
root, replacing `COM_PORT` with the board's USB serial port:

```powershell
pio run -e heltec_v4_companion_radio_ble
pio run -e heltec_v4_companion_radio_ble -t upload --upload-port COM_PORT
```

With the project virtual environment, replace `pio` with
`.\.venv\Scripts\python.exe -m platformio`. One upload installs BLE companion and
health monitoring together. PlatformIO selects the application layout for upload.
For a separate flash tool, `firmware-merged.bin` contains the combined layout at
`0x0`; `firmware.bin` is the application at `0x10000` and requires matching bootloader
and partition files. A full flash erase removes identity, preferences, saved
channel selection, calibration and unresolved-incident metadata. See the [post-flash checks](usb_cli_reference.md#post-flash-usb-cli-verification).

## What each check covers

| Check | Scope |
| --- | --- |
| Firmware build and merge | Compiles and links the combined companion/health image and packages its flash components. |
| `native` | Strong/context detector paths, history/recovery/refractory behavior, FIFO phase/jitter/overflow and loss, bounded traces/evidence, late quiet, checked incident storage/recovery, double/triple gestures, multipart outbox, button ownership, buzzer GPIO/LEDC requests, battery/calibration/preferences, channel storage and dispatcher behavior with host fakes. |
| `native_delivery` | Production packet builder, Mesh/Dispatcher, encryption/decryption and packet pool with a fake radio and clock; public/private channel matching, false-alarm text/location, and incident-scoped retry/cancellation behavior. |
| Documentation checks | Relative file links, Markdown structure, documented settings and source/command consistency. |
| Capture/replay checks | Versioned checkpoint/operation codecs, strict mixed-USB parsing, exact working-tree identity, actual health-method capture fixtures and repeated production detector/response replay. |

Use the actual command output for pass/fail results and case counts for the
checkout being tested. Stored logs, binary sizes and results from earlier source
revisions are not evidence that a later checkout passes. See
[virtual fault testing](virtual-fault-testing.md) for focused commands and scenarios.

The recording pipeline has an additional host check:

```powershell
.\.venv\Scripts\python.exe tools\test_health_replay.py -v
```

Setting `HEALTH_CAPTURE_FIXTURE` to an output path during `native` exports the
actual health-integration fixture. Replay that file using
`tools/health_replay.py <path> --repeat 2` after the run. Regenerate it after
changing source because the embedded content identity must match. See
[capture and replay usage](motion_recording.md).

## Physical validation boundary

Host tests do not execute the complete BLE companion on a Heltec board. They do
not establish simultaneous BLE/motion operation, physical I2C timing, GPIO/PWM,
battery accuracy, watchdog recovery, RF range, recipient reception or fall-detection
accuracy. The simulated one-hour trace is not a hardware soak test.

Physical PRG validation must include two complete presses within one second,
waiting through the three-second decision window, and third-press priority under
BLE/radio load. Host edge timestamps do not establish physical polling latency.

Experimental detector context thresholds and sampling/filter settings require
labelled recordings and held-out wearer/activity evaluation. Software recovery
tests cannot establish reliable power-off age: the current target uses bounded
clock-uncertain recovery, and flash writes/power interruptions need bench tests.

The residual-quiet settings (10 seconds, 0.08 g thresholds and 5 degrees/s) require
worn-device and buzzer-vibration evaluation. Queueing, local transmission and a
repeater echo are distinct evidence; none proves that a caregiver received or read
an alert. Client parser compatibility also needs a physical phone/BLE check.
