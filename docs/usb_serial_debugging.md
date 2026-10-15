# USB serial debugging on Windows and Linux

The `heltec_v4_companion_radio_ble` firmware exposes local text diagnostics over
USB at **115200 baud, 8 data bits, no parity, 1 stop bit**, without flow control.
BLE companion services and health monitoring continue while the serial terminal
is open. Monitoring does not require a firmware rebuild or upload.

The procedures below use PlatformIO Core. Port names are examples and must match
the connected system. Only one application should hold
the device's serial port at a time; close flashers and other serial monitors.
The USB cable must support data transfer.

## Configuration and detailed statistics

The [USB CLI reference](usb_cli_reference.md) lists supported `get`/`set` keys,
validation and persistence behavior, the `username` alias, and detailed health
and motion fields. The same terminal setup applies on both operating systems.

After flashing, follow the [post-flash USB CLI verification
procedure](usb_cli_reference.md#post-flash-usb-cli-verification) for command and
username checks, the 60-second quiet-baseline versus repeated-query comparison,
and interpretation of timing, gap and error counters.

```text
help set
get username
set username "Medical Node 2"
get radio
health detail
health timing
health errors
health radio
motion stats
motion stats raw
```

`motion stats reset` begins a new motion/timing measurement interval.
`health stats reset` additionally resets sensor diagnostic error counters.
Neither clears an alarm, changes detector state, erases calibration, or clears
the original since-startup `max_gap_ms` and `gaps` fields. A saved calibration
change also resets the motion aggregates to avoid combining different corrections.

Send one command at a time. Replies drain in bounded chunks; a stalled USB reader
pauses replies without waiting inside the motion loop. Collection uses fixed-size
RAM aggregates and no per-sample output. Device timing still requires validation
under representative sensor, BLE and radio load.

## Windows: PowerShell

Open PowerShell in the project root containing `platformio.ini`, then run:

```powershell
.\.venv\Scripts\python.exe -m platformio device list
```

If the project does not yet have a Python environment, create one once with an
installed Python 3 interpreter:

```powershell
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install platformio
```

Run `device list` again after installation. Select the USB device rather than a
Bluetooth serial port. Windows Device Manager, under **Ports (COM & LPT)**, can
also identify the port. Disconnecting and reconnecting USB distinguishes the
board from unrelated devices. A port such as `COM6` is not a permanent assignment.

Open the monitor, replacing the example port as required:

```powershell
$serialPort = 'COM6'
.\.venv\Scripts\python.exe -m platformio device monitor --port $serialPort --baud 115200 --eol CR --echo --filter time --exit-char 3
```

The executable path avoids requiring virtual-environment activation or changes
to the PowerShell execution policy. All relative paths refer to the project root.

## Linux: Bash or a compatible shell

Open a terminal in the project root containing `platformio.ini`, then run:

```bash
.venv/bin/python -m platformio device list
```

If the project environment is absent, create it once:

```bash
python3 -m venv .venv
.venv/bin/python -m pip install platformio
```

Python 3 and its virtual-environment support must be installed through the
distribution's package manager if unavailable. A Windows `.venv` directory
cannot be reused on Linux. An existing PlatformIO Core installation can also
run the same commands with `pio` in place of `.venv/bin/python -m platformio`.

Run `device list` again and identify the USB serial device. Native ESP32 USB
commonly appears as `/dev/ttyACM0`; USB-to-serial adapters may appear as
`/dev/ttyUSB0`. The number can change after reconnection. When present,
`ls -l /dev/serial/by-id/` lists persistent device links that can also be passed
to `--port`.

```bash
serial_port=/dev/ttyACM0
.venv/bin/python -m platformio device monitor --port "$serial_port" --baud 115200 --eol CR --echo --filter time --exit-char 3
```

### Linux permission errors

Inspect the selected port's ownership and the current account's groups:

```bash
ls -l "$serial_port"
id
```

If the port grants read/write access to the `dialout` group, membership can be
added with:

```bash
sudo usermod -a -G dialout "$(id -un)"
```

Use the actual serial-device group where it differs, such as `uucp`; do not
substitute an administrative group such as `root`. Log out and back in after a
membership change. Alternatively, install the appropriate board udev rules
using the [PlatformIO Linux permissions guide](https://docs.platformio.org/en/latest/core/installation/udev-rules.html).
Run the monitor under the normal account after permissions are configured.

## Terminal operation and initial checks

After the monitor opens, commands are entered into the **serial terminal**, not
the PowerShell or Bash prompt. Press Enter after each command. `--echo` makes
typed characters visible locally; the firmware itself does not echo input.
`--eol CR` supplies the command terminator; the firmware also accepts LF and
CRLF. `--filter time` adds the host computer's reception time to each line.
These timestamps are not MPU sampling timestamps.

**Ctrl+C** closes the monitor. **Ctrl+T**, followed by **Ctrl+H**, displays its
help. Monitor options and filters are described in the
[PlatformIO serial-monitor reference](https://docs.platformio.org/en/latest/core/userguide/device/cmd_monitor.html).

Start with the following commands, one at a time:

```text
motion
health
battery
get adc.multiplier
get fall.channel
beep
```

| Command | Diagnostic purpose |
| --- | --- |
| `motion` | Latest valid acceleration X/Y/Z in g, gyroscope X/Y/Z in degrees/s, and chip temperature in Celsius. |
| `motion raw` | Scaled readings before saved gyroscope correction; acceleration and temperature remain unchanged. |
| `health` | Motion availability, completed sampling gaps, and fall/response delivery states. |
| `battery` | Cached raw ADC average, estimated battery millivolts, cache age and conversion multiplier. |
| `get adc.multiplier` | Effective battery conversion multiplier. |
| `get fall.channel` | Selected public/private fall-message destination; excludes its shared key. |
| `beep` | Requests one 150 ms tone at 2000 Hz; does not create a fall event or send a message. |

The first five commands inspect state without changing saved configuration.
`beep` activates the configured local sound output when available. Configuration
and calibration commands are documented separately in the
[local command reference](cli_commands.md#local-usb-diagnostics).
The dedicated [gyroscope calibration guide](gyro_calibration.md) covers
`calibrate`, status, coefficient inspection, saving, cancellation and reset.
It uses the same serial commands on Windows and Linux and permits any fixed
orientation. A stationary 15-second measurement follows a countdown and precedes
independent validation; detection pauses during those phases. No accelerometer
calibration is performed.
Upstream repeater/admin commands are not automatically supported by this local
BLE companion's USB command interface.

## Interpreting motion and health

A representative `motion` reply has this form; these values are illustrative:

```text
a[g]=0.010,-0.020,1.000 gyro[dps]=0.1,-0.2,0.0 temp[C]=27.5
```

At rest, the combined acceleration magnitude
`sqrt(ax*ax + ay*ay + az*az)` should be approximately 1 g because gravity is
included. All three axes should not be expected to read zero. Gentle tilting
redistributes acceleration among the axes; rotation changes the gyroscope
readings. Stationary gyroscope values should be near zero, with sensor bias
and noise. Chip temperature is not body temperature.

| `health` field | Interpretation |
| --- | --- |
| `motion=monitoring` | No prolonged fresh-data outage is latched. This does not identify the detector's armed state or establish successful fall detection. |
| `motion=FAULT` | No fresh valid motion data for five seconds; clears when a fresh valid reading arrives. |
| `motion=CALIBRATING` | Automatic fall detection is paused during calibration countdown, measurement or validation; sensor-health tracking continues. |
| `max_gap_ms` | Longest completed interval between fresh valid samples since startup. An ongoing outage contributes only after another fresh reading arrives. |
| `gaps` | Number of completed fresh-sample intervals exceeding 100 ms. Rising counts indicate interrupted sampling. |
| `fall` | Delivery state of the initial possible-fall message. |
| `help` | Delivery state of the current response-message part. |
| `part_failed` | `1` records a failed part until the next response event; a later part succeeding does not erase it. |

`max_gap_ms=0; gaps=0` does not prove healthy operation: no interval can be
measured before two fresh samples have arrived. During a persistent startup
fault, both counters can remain zero while `motion=FAULT` is reported.

Delivery states distinguish pending work, queue acceptance, local transmission,
repeater echo and terminal failure. Neither queue acceptance, local TX nor a
repeater echo establishes reception by the intended recipient. A separate
receiving companion is required for an end-to-end reception check. See the
[delivery guide](message_delivery_changes.md).

## MPU6050 messages and isolation checks

| Serial message | Meaning in the current driver |
| --- | --- |
| `MPU6050 ready at 0x68` | Identity and configuration readback passed. Fresh sample acquisition has not yet been established by this message alone. |
| `MPU6050 unavailable; retry in 5s` | Initialization failed: I2C access, identity validation or configuration write/readback did not pass. Retries occur every five seconds. |
| `MPU6050 read/data-ready failures; reinitializing` | Repeated sample failures after initialization. Causes include failed/incomplete reads or missing data-ready status after the one-second freshness timeout. The message does not distinguish these causes. |
| `Motion FAULT: no fresh valid samples; detection unavailable` | The five-second fresh-data outage threshold was reached. |
| `Motion recovered: fresh samples; detector must rearm` | Fresh data resumed; the detector still requires its normal settling/rearm conditions. |

An initialization-success message followed by reinitialization about one second
later is consistent with the data-ready timeout, but does not conclusively
exclude transaction errors. Sensor reinitialization messages alone do not
establish a complete ESP32 reboot. A full boot sequence or reset/panic output is
needed to investigate device restarts.

For persistent faults:

1. Capture the messages together with `motion`, `health` and `battery` replies.
2. Disconnect USB and battery power before checking or changing wiring.
3. Verify GY-521 VCC to 3V3, GND to common GND, SDA to GPIO4, SCL to GPIO6,
   and AD0/ADDRESS directly to GND for address `0x68`. INT remains unconnected.
4. Inspect connector seating and solder joints. With power restored, a meter
   measurement at the module's VCC/GND pins can check its approximately 3.3 V supply.
5. If accessory interference is suspected, power down, disconnect the optional
   buzzer/indicator circuit, restore power and compare logs under the same conditions.

An absent buzzer does not prevent sensor initialization. The `buzzer_enabled`
setting does not suppress sensor faults. Continued failures with verified wiring
and supply require further investigation; the aggregate messages cannot identify
the failed transaction/register or prove that the sensor is defective. Wiring
details are in the [pin reference](pin_schematic.md).

## Battery and buzzer checks

`battery` reports an estimate from cached ADC samples, not a direct meter
measurement of either the battery or the sensor's 3.3 V supply. Repeated readings
can show whether acquisition continues; a small varying `age_ms` is expected
between updates. Calibration requires an actual battery-terminal measurement:
`battery calibrate <meter_mV>` suggests a multiplier without applying it, while
`set adc.multiplier <value>` changes saved configuration. See the
[calibration procedure](cli_commands.md#battery-diagnostics-and-calibration-usb).

For `beep`, `Beep started` means that software accepted playback, not that a
buzzer was electrically detected. A disconnected buzzer therefore produces no
sound despite this reply. `Buzzer disabled by buzzer_enabled=false` identifies
the compiled disabled mode; `Beep unavailable or alarm active` indicates PWM
unavailability or an active sound sequence. The default DIYables configuration
uses GPIO47 with 50% PWM during tones and LOW during silence
(`buzzer_active_low = false`). PWM disconnects between tones. An indicator
sharing this signal requires a current-limiting resistor and a circuit matched
to the configured polarity; an indicator wired for HIGH-idle operation may show
the opposite silent/sounding behavior. See the
[wiring and polarity guide](gy521_sensor.md#buzzer-trigger-polarity).
The [DIYables module reference](buzzer_module_reference.md) documents manufacturer
specifications, the required 3.3 V DC supply, wiring and PWM behavior.

After flashing, run `beep` while no alarm or calibration is active. Confirm one
150 ms tone followed by silence, with no repeated sound. The expected ordinary
tone is 2000 Hz; an oscilloscope or logic analyzer can verify frequency, 50% duty
and LOW idle. A successful CLI response alone does not validate the waveform or
module current. Buzzer enable and polarity are compiled settings and require a
rebuild and flash; they are not USB `set` commands.

The SOS pattern is three short, three long and three short tones, followed by
five seconds of silence. The low-battery pattern is three 450 ms tones at
descending frequencies, normally repeating every ten minutes while voltage is
below 3500 mV. A rapid repeating three-tone pattern alone does not establish a
low-battery warning. Correlate sound with the serial events and voltage readings.

After a detected fall, three short PRG clicks within three seconds confirm the
fall/request assistance. Two complete clicks within one second select false
alarm only after the three-second decision window closes. These responses can
send channel messages; they are not generic mute controls. See the
[buzzer and response guide](gy521_sensor.md#fall-alarm-prg-acknowledgement-and-channel-message).

## Timestamped file capture

Close the interactive monitor before starting a logging session. The following
commands use `.pio` as the monitor working directory so generated captures stay
under an existing ignored path. They do not build or flash the firmware.

Windows PowerShell, from the checkout root with `$serialPort` set:

```powershell
New-Item -ItemType Directory -Force .pio | Out-Null
.\.venv\Scripts\python.exe -m platformio device monitor --project-dir .pio --port $serialPort --baud 115200 --eol CR --echo --filter time --filter log2file --exit-char 3
```

Linux, from the checkout root with `serial_port` set:

```bash
mkdir -p .pio
.venv/bin/python -m platformio device monitor --project-dir .pio --port "$serial_port" --baud 115200 --eol CR --echo --filter time --filter log2file --exit-char 3
```

The monitor prints the capture's absolute path. The installed `log2file` filter
uses `.pio/logs/device-monitor-<timestamp>.log`; naming can vary with PlatformIO
versions, so use the printed path. The filter records received text; local echo
and command history should not be assumed to be included. Record the commands
and physical actions associated with the capture separately. Shared captures
should omit private channel keys.

Useful capture context includes the firmware file/revision, board and sensor
connections, buzzer enabled/polarity settings, power source, commands issued,
and whether movement or accessory disconnection changes the symptom. Preserve
several consecutive cycles rather than a single error line.

## Connection troubleshooting and limitations

| Symptom | Check |
| --- | --- |
| No USB serial port listed | Data-capable cable, connector seating and port enumeration before/after reconnection. Confirm the selected port belongs to the board. |
| Port busy or Windows access denied | Close other monitors, flashing tools and applications holding the same port; enumerate again after reconnection. |
| Linux permission denied | Device ownership, serial-device group membership and applicable udev rules. |
| Typed command gives no reply | Correct USB port, running application firmware, command spelling and CR/LF termination. Typed local echo is not evidence of a device reply. |
| No unsolicited output | Send `health`; an idle device need not continuously print data. To capture startup, open the monitor before a deliberate board reset, then reconnect if the USB port changes. Reset clears volatile alarm state. |
| Corrupted or binary-looking output | Confirm 115200/8N1 and that the loaded firmware exposes this text interface. Close competing protocol clients. |
| Repeated boot or panic output | Capture the full reset sequence and distinguish whole-device restarts from the sensor-only reinitialization messages above. |

`motion`, `health` and `battery` are snapshots, not continuous stream commands.
Automatic fault/recovery and fall-delivery events are printed separately. There
is no USB command that exposes every internal 50 Hz sample or the detector's
complete internal state. If external polling is introduced, a modest initial
rate such as 2-5 motion queries per second limits diagnostic traffic; this is
a suggested observation rate, not a tested timing guarantee. Host reception
timestamps cannot establish sensor sample timing.

The Windows command syntax and monitor options can be checked locally with
`python -m platformio device monitor --help`. Linux instructions follow the
same CLI and the linked platform guidance; this guide does not establish a
Linux hardware-session result. Builds and host tests do not prove physical
sensor reliability, audible output, BLE concurrency or RF recipient reception.
