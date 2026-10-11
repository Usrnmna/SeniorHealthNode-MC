# SeniorHealthNode

**MeshCore-based motion monitoring and mesh alerts for people who may be unable to reach a phone.**

SeniorHealthNode is designed for older adults, people with limited mobility, and anyone who lives or works alone. It offers an extra layer of connection for those who may need help when a phone is out of reach or difficult to use. It monitors movement and sends alerts to others when it detects a possible fall or the wearer requests help.

The aim is to support independence while giving family members, caregivers, or others a way to notice when someone may need assistance. The device is worn firmly on the torso or chest so that it moves with the person. In the current version, the wearer can request assistance after a possible-fall alarm; this is not a standalone help button available at any time.

Alerts travel over MeshCore, a network of small radios that can pass messages between devices without relying on cellular service or an internet connection. Other people need compatible equipment set up to receive those messages, and radio coverage determines how far they can travel. Movement monitoring continues on the worn device even when its phone connection is disconnected.

The sections below explain how this prototype works, what equipment and setup it needs, and its current limitations. Start with [getting started](#getting-started), then use the [documentation guide](#documentation-guide) for configuration and troubleshooting.

This repository is based on **MeshCore 1.17.0**. It retains the normal MeshCore radio, identity, telemetry, and contact-based messaging foundations. The **single firmware image combines the MeshCore BLE companion and local fall detection on the same Heltec V4.3 OLED node**. A phone connects to this node over BLE; motion monitoring and channel alerts run locally even when the phone is disconnected.

The GY-521/MPU6050 motion sensor runs a **possible-fall sequence detector**: low acceleration, followed by impact and rapid rotation, followed by a short quiet period. It queues a channel alert, initially to public `#falldetect`, with bounded local retries and repeater-echo tracking. All fall notifications are group texts on the selected public or private channel, never DMs. They omit the node-name prefix while retaining status wording, readings, and any location suffix. This is an experimental rule, not a determination that a fall occurred; routine activity can trigger it and falls can go undetected. The sensor must move with the wearer; a nearby, unworn node cannot measure the person's body motion.

Detection thresholds and timers are grouped in `FallDetectionConfig` at the top of [MotionRule.h](examples/companion_radio/health/MotionRule.h), with numbered `STAGE` comments showing where each algorithm is used. Post-fall response and residual-movement settings are in [HealthNodeConfig.h](include/HealthNodeConfig.h). See the [tuning guide](docs/gy521_sensor.md#polling-and-rule-customization). Posture classification, trained models, general inactivity alerts outside a fall incident, an independent manual help button, and caregiver event histories are not implemented.

A detected fall also latches an external passive buzzer into an SOS loop, with five seconds of silence between sequences. While SOS is active, PRG is reserved for alarm acknowledgement. Three short PRG clicks within three seconds silence it and queue a configurable Unicode acknowledgement to a MeshCore channel, appending the GPS provider's valid coordinates or `DefaultLocation` (`HOME`). Reboot clears the alarm and detector state. The companion retains its low-voltage automatic shutdown below 3400 mV when external power is not detected; shutdown stops monitoring and sound. Buzzer GPIO47, sound/button timings, channel defaults and message are grouped in [`HealthNodeConfig.h`](include/HealthNodeConfig.h). See the [buzzer wiring and function guide](docs/gy521_sensor.md#passive-buzzer-wiring-and-shared-functions).

If neither response has been accepted after **30 seconds**, the node automatically sends the assistance text and location with ` | USER COULD NOT CONFIRM THEIR CONDITION` appended. If fresh motion samples establish quiet for the **final 10 continuous seconds**, it instead appends ` | NO RESIDUAL MOVEMENTS DETECTED; POSSIBLE INCAPACITATION OF USER`. The quiet duration and movement thresholds are adjustable. Missing or invalid samples cannot establish quiet. SOS keeps repeating through either warning and through delivery failures. Three presses afterward stop SOS and send the independent text `USER HAS CONFIRMED FALL`. False-alarm double presses remain available after either warning. A warning that exceeds the channel's text limit uses two labelled messages; standalone warning text has no leading ` | `. See the [response and delivery guide](docs/message_delivery_changes.md#post-fall-response-directives).

For a false alarm, press and release PRG **twice within one second**, measured from first press-down to second release. SOS continues until the **three-second window from the first press closes**. A valid third press within that window takes priority as fall confirmation/assistance. Otherwise, the accepted double press stops SOS and queues `User Confirms False Alarm | <location>`, cancelling that incident's queued automatic warnings and initial-alert retries. A started third press that is held too long or released late cancels the double candidate and leaves SOS active. Already transmitted messages cannot be recalled.

Below 3500 mV, the sensor plays three 450 ms battery-warning tones at 2093, 1760, and 1480 Hz every ten minutes. Fall SOS takes priority. The warning clears above 3500 mV. Silencing as soon as charging starts requires an added charging-status signal; the stock board has no ESP32 connection for it, so sensing is disabled by default. See the [low-battery settings and hardware limitation](docs/gy521_sensor.md#low-battery-buzzer-reminder).

USB `battery` reports cached ADC counts, voltage and sample age. `battery calibrate <meter_mV>` suggests a correction using your meter reading; `set adc.multiplier <value>` saves an explicit adjustment. See the [battery calibration workflow](docs/cli_commands.md#battery-diagnostics-and-calibration-usb). The default remains 5.42 until adjusted.

## Combined firmware

`heltec_v4_companion_radio_ble` is the only firmware target. It includes MeshCore BLE, contacts, messaging, display, GPS and environmental telemetry, plus GY-521/MPU6050 fall detection, SOS, PRG acknowledgement and low-battery reminders.

Build it with `pio run` or `pio run -e heltec_v4_companion_radio_ble`. The `native` and `native_delivery` environments are host-side software tests, not firmware images. One upload installs the companion and health-monitoring features together.

### Build and test on Windows

This folder is the project root: it contains `platformio.ini`, `src`, `test`, and
`variants`. No source files from a parent or sibling `SeniorHealthNode` folder are
required. PlatformIO keeps its core cache in `.pio/core`, toolchain packages in
the shorter `.pio/p` path, and build output in `.pio/build`.

From this folder, create the local build environment:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install platformio==6.2.0
```

The native tests require a C++17-capable GCC compiler. The old PlatformIO MinGW
5.1 compiler cannot build GoogleTest 1.17. A standalone Windows compiler is
available from [WinLibs](https://winlibs.com/). Put its `bin` directory on `PATH`,
pass it through the `verify.ps1 -CompilerBin` parameter, or extract it into
`.pio/tools` so the compiler is `.pio/tools/mingw64/bin/g++.exe`.

```powershell
.\verify.ps1
```

The script anchors every command to its own folder, builds the combined firmware
and its merged flash image, runs all native test suites, and stops on
failure. It can also be invoked by absolute path from another directory. The
individual commands are `pio run -e heltec_v4_companion_radio_ble -t mergebin`, `pio test -e native`,
and `pio test -e native_delivery`;
with the local environment, replace `pio` with
`.\.venv\Scripts\python.exe -m platformio`. These commands do not flash hardware.

The native tests include deterministic sensor-fault scenarios and production-path
message-delivery fault tests. See [virtual fault testing](docs/virtual-fault-testing.md)
for scenario examples and [message-delivery changes](docs/message_delivery_changes.md)
for directives, state meanings, tunables, and validation status.

The board driver's external FEM LNA starts disabled. This is separate from the SX126x radio-chip receive boost. Configure the companion's radio and BLE settings through a compatible MeshCore client.

> [!IMPORTANT]
> This is a prototype, not a validated fall detector, medical device, or guaranteed emergency service. An alert can be missed because of sensor placement, radio coverage, power, configuration, or an unavailable recipient. Use an established emergency plan for situations where a missed alert could cause harm.

## How it fits into MeshCore

| Part | Role |
| --- | --- |
| `heltec_v4_companion_radio_ble` | One Heltec V4.3 OLED node runs BLE companion services, motion monitoring, telemetry and fall alerts together. |
| Connected phone | A compatible MeshCore client connects directly to this node over BLE for ordinary messaging and configuration. |
| Receiving nodes and mesh | Other compatible nodes listen on the configured channel; repeaters can extend the radio path, subject to coverage and radio settings. |

The initial possible-fall alert, PRG assistance request, fall confirmation and false-alarm response are group-channel messages, initially on public `#falldetect`. The destination can be changed to another public hashtag or a private shared-key channel through USB or BLE commands. Listening companions need the matching channel key and radio settings; registered contacts are not required for these fall messages. Local TX completion and a heard repeater echo are tracked separately; neither identifies a recipient who received the text. Idle button clicks send no acknowledgement.

## Buzzer and acknowledgement behavior

The buzzer is optional. `buzzer_enabled = true` preserves sound output when a
module is connected and permits operation with no module connected. Setting
`buzzer_enabled = false` in `HealthNodeConfig.h` skips buzzer GPIO/PWM setup and
playback after rebuilding and flashing. Motion monitoring, BLE, PRG responses
and group-channel messages remain active. See
[operation without a buzzer](docs/gy521_sensor.md#operation-without-a-buzzer).

`buzzer_active_low = true` in [`include/HealthNodeConfig.h`](include/HealthNodeConfig.h)
defaults to LOW-level-trigger passive buzzer modules: GPIO47 stays HIGH when
silent and uses 50% PWM for tones. Set it to `false` for a HIGH-active driver
(including the discrete NPN alternative), then rebuild and flash. An incorrect
setting can leave the driver energized during intended silence even when tones
sound normal, with possible extra current draw or heating depending on hardware.
See [polarity settings and wiring](docs/gy521_sensor.md#buzzer-trigger-polarity).

| Function | Current behavior |
| --- | --- |
| Disabled buzzer | No GPIO/PWM setup or audible output; fall-response and message behavior remain active. `beep` reports the disabled setting. |
| Ordinary beep | `beep` command or `alarm_buzzer.beep()`; 150 ms at 2000 Hz. Returns unavailable while an alarm sequence is playing. |
| Fall SOS | Three short, three long, three short tones at 2000 Hz; 150 ms dots, 450 ms dashes and five seconds of silence between sequences. |
| Acknowledge fall | Three debounced PRG clicks within three seconds, each shorter than one second; silences SOS. Before automatic escalation, sends the assistance request; afterward, sends `USER HAS CONFIRMED FALL`. Idle clicks do not request help. |
| Unanswered fall | At 30 seconds, send assistance+location with the unconfirmed-condition warning, or the no-residual-movement warning if the final 10 seconds qualify as quiet. SOS has no elapsed-time cutoff. |
| False alarm | Two complete presses within one second; wait until the three-second window closes. A valid triple press wins. Stops SOS and sends `User Confirms False Alarm` with GPS or fallback location, before or after escalation. |
| Channel text | Default `#falldetect`. Assistance and false-alarm responses include location; automatic warnings append their exact text or use two labelled parts when necessary. Late confirmation is standalone. Separators are inserted only between joined text. |
| Delivery retries | Allocation/local TX failures retry after five seconds, with 60 total attempts and a 30-second queue deadline. After local TX, wait 15 seconds for a repeat; allow at most four successful local sends without one. Retries can display duplicate text. See the [delivery guide](docs/message_delivery_changes.md). |
| Low battery | Below 3500 mV: 2093 Hz, 1760 Hz, then 1480 Hz; each tone lasts 450 ms with 150 ms gaps. Repeats every ten minutes from accepted sequence starts. |
| Battery recovery | Above 3500 mV clears the warning; exactly 3500 mV preserves its state. Battery sampling runs once per second; a zero reading is treated as unavailable. |
| Alarm priority | SOS preempts a battery sequence. A due battery reminder waits while the buzzer is busy. |
| Reboot | Clears volatile alarm, acknowledgement and reminder state; low battery is evaluated again after startup. |

> [!IMPORTANT]
> **Connecting a charger does not immediately silence the battery warning on the current hardware.** Charging sensing defaults to disabled (`charging_status_pin = -1`). With no charging-status GPIO connection, the warning clears only when measured battery voltage exceeds 3500 mV. An added, conditioned 3.3 V active-charging signal can cancel the battery warning before voltage recovers; it never cancels SOS. See the [charging input requirements](docs/gy521_sensor.md#low-battery-buzzer-reminder).

Set the fall-message channel at runtime with `get fall.channel`, `set fall.channel public #name`, or `set fall.channel private Name <shared-key>`. In MeshCore One v1.5.1, use **Tools → CLI → local session** over BLE; the same commands work over USB serial at 115200 baud. Settings survive reboot and apply to new incidents; active incidents and queued messages retain their original channel. See the [channel command guide](docs/cli_commands.md#fall-message-channel-usb-and-ble).

Edit [`include/HealthNodeConfig.h`](include/HealthNodeConfig.h) for buzzer, battery, button and compiled channel defaults. Edit `FallDetectionConfig` in [`MotionRule.h`](examples/companion_radio/health/MotionRule.h) for motion thresholds and timing. Those source changes require a firmware rebuild; channel commands do not.

## Source layout

Health monitoring runs inside the **BLE companion**. `main.cpp` services motion, alarms, radio, BLE and the display in the same firmware loop.

| File | Responsibility |
| --- | --- |
| [`src/helpers/sensors/MPU6050.h`](src/helpers/sensors/MPU6050.h) | Reads a GY-521/MPU6050 on `Wire1` at I2C address `0x68`. It verifies the device ID and configuration, accepts data-ready samples, and recovers after failed reads or stopped updates. |
| [`src/helpers/sensors/EnvironmentSensorManager.cpp`](src/helpers/sensors/EnvironmentSensorManager.cpp) and [`.h`](src/helpers/sensors/EnvironmentSensorManager.h) | Make the motion sensor available to the combined firmware and add its latest valid readings to environmental telemetry. |
| [`examples/companion_radio/health/MotionRule.h`](examples/companion_radio/health/MotionRule.h) | Implements the low-acceleration / impact / rotation sequence, optional quiet confirmation, sampling-gap rejection, and cooldown. All settings are grouped at the top. |
| [`HealthMonitor.cpp`](examples/companion_radio/HealthMonitor.cpp) | Services motion detection, fall acknowledgement, buzzer playback and battery polling; handles the `motion`, `health`, and `beep` commands. |
| [`examples/companion_radio/health/FallChannel.h`](examples/companion_radio/health/FallChannel.h) and [`FallChannelStore.h`](examples/companion_radio/health/FallChannelStore.h) | Parse public/private destinations and persist checked channel records separately from the ordinary chat channel list. |
| [`include/HealthNodeConfig.h`](include/HealthNodeConfig.h), [`AlarmBuzzer.h`](src/helpers/ui/AlarmBuzzer.h), and [`BuzzerPattern.h`](src/helpers/ui/BuzzerPattern.h) | Configure and initialize the passive buzzer; expose `alarm_buzzer.beep()`, nonblocking SOS and three-tone battery playback. |
| [`LowBatteryAlert.h`](examples/companion_radio/health/LowBatteryAlert.h) | Apply the battery threshold, charging override and ten-minute reminder interval. |
| [`FallResponse.h`](examples/companion_radio/health/FallResponse.h), [`FallButtonGesture.h`](src/helpers/ui/FallButtonGesture.h), and [`FallAckSender.h`](examples/companion_radio/health/FallAckSender.h) | Recognize post-fall triple confirmation and delayed fast-double false-alarm responses, silence SOS, and queue configured UTF-8 channel text. |
| [`ResidualMovement.h`](examples/companion_radio/health/ResidualMovement.h), [`FallNotice.h`](examples/companion_radio/health/FallNotice.h), [`FallNoticeMessage.h`](examples/companion_radio/health/FallNoticeMessage.h), and [`FallNoticeOutbox.h`](examples/companion_radio/health/FallNoticeOutbox.h) | Track continuous post-fall quiet, name each response directive, format full warning texts, and retain messages with reserved capacity for late confirmation. |
| [`AssistanceDelivery.h`](examples/companion_radio/health/AssistanceDelivery.h) and [`MotionHealth.h`](examples/companion_radio/health/MotionHealth.h) | Track the two fall-message streams, bounded retries and repeater echoes; report prolonged sampling outages and completed sampling gaps. |
| [`boards/heltec_v4.json`](boards/heltec_v4.json) and [`pins_arduino.h`](boards/variants/heltec_v4/pins_arduino.h) | Select the project-local Arduino pin variant. Application board drivers remain in `variants/heltec_v4`. |
| [`variants/heltec_v4/platformio.ini`](variants/heltec_v4/platformio.ini) | Enables the MPU6050 for `heltec_v4_companion_radio_ble` and assigns its I2C pins to GPIO4 and GPIO6. The OLED/RTC bus remains separate. |
| [`docs/gy521_sensor.md`](docs/gy521_sensor.md) and [`test/test_motion_rule/test_motion_rule.cpp`](test/test_motion_rule/test_motion_rule.cpp) | Document wiring, configuration, and checks for the motion rule. |

## Possible-fall detection

The staged detector runs locally in `heltec_v4_companion_radio_ble`. A PC is needed for building, flashing and setup; motion decisions run on the Heltec.

### Algorithm stages and default settings

The `STAGE 0` through `STAGE 5` comments in [`MotionRule.h`](examples/companion_radio/health/MotionRule.h) identify each part of the implementation. Internally, the rule moves through `Settling`, `Armed`, `Peaks`, and `Confirming` states.

| Code stage | Behavior | Configuration defaults |
| --- | --- | --- |
| 0: validate samples | Reject invalid, negative, or nonfinite magnitudes; discard partial evidence across a long sample gap. Ignore duplicate timestamps. | `max_sample_gap_ms = 100` |
| 1: settle and arm | Require continuous quiet at startup, after interrupted or expired evidence, and before rearming after an alert. | `rearm_quiet_ms = 2000` |
| 2: start an event | Acceleration at or below the low threshold opens a fixed window. Repeated low readings cannot extend it. | `low_g = 0.35`, `event_window_ms = 500` |
| 3: collect both peaks | Require acceleration at or above the impact threshold **and** rotation at or above its threshold within the event window. Peaks may occur on different samples and in either order. | `impact_g = 2.4`, `rotation_dps = 240` |
| 4: confirm quiet | Require one continuous quiet interval, completed before the deadline measured from when both peaks have been observed. Movement restarts the quiet timer, not the deadline. | `confirm_quiet_ms = 1000`, `confirm_timeout_ms = 5000` |
| 5: emit and suppress repeats | Emit one possible-fall event, then require cooldown and quiet before rearming. These periods can overlap. | `cooldown_ms = 60000` |

Quiet means acceleration between `quiet_min_g = 0.7` and `quiet_max_g = 1.3`, inclusive, with rotation below `quiet_max_dps = 30`. Acceleration is the three-axis vector magnitude in **g, including gravity**; rotation is the three-axis angular-velocity magnitude in **degrees/second**. A sample gap exactly equal to 100 ms is tolerated; a longer gap resets evidence. Timing uses unsigned elapsed milliseconds to handle timer rollover.

Stages 2-3 adapt the timed sequence from Huynh et al. [4](#study-4). Quiet confirmation, startup settling, data guards, and cooldown are **project-specific extensions**. This is an adaptation for evaluation, not a reproduction of the paper's validated experimental setup or its reported performance. Quiet magnitudes do not establish lying posture or injury. Soft/sliding falls without the required low acceleration, impact, or rotation will be missed; quiet confirmation can also miss falls followed by continued movement. A dropped device can resemble a fall.

### Runtime behavior

- [`MPU6050.h`](src/helpers/sensors/MPU6050.h) configures filtered 50 Hz sampling with ranges of +/-8 g and +/-500 degrees/second, then verifies the configuration by readback. Each burst contains data-ready status and measurements; only a fresh sample reaches the detector. Failed/partial reads invalidate the sample and retry after 20 ms; three consecutive failures trigger reinitialization. One second without new data enters the same recovery path. Failed initialization uses five-second backoff. Polling is cooperative, so other firmware work can delay it; host interrupt and FIFO capture are not used.
- [`MyMesh::pollMotion()` in HealthMonitor.cpp](examples/companion_radio/HealthMonitor.cpp) passes fresh vector magnitudes to `MotionRule::update()` independently of pending alarms or delivery. Notifications coalesce while an earlier alert is pending. The detector's 60-second cooldown and quiet-rearm requirements remain in force.
- The combined firmware collects environmental readings in a background task and serves telemetry from a bounded-age cache. OLED frames transfer in small pieces; battery readings use staged ADC acquisition and a shared cache. Configuration, methods, files, and validation limits are listed in [the timing implementation report](docs/fall_timing_changes.md).
- [`MotionRule.h`](examples/companion_radio/health/MotionRule.h) holds the configuration and decision logic without Arduino dependencies, dynamic allocation, or blocking waits. `peakAcceleration()` and `peakRotation()` preserve event-window peaks for the alert, rather than reporting the later quiet readings.
- The initial channel alert uses `Possible fall: peak accel=%.2fg rotation=%.1fdeg/s`. The independent delivery controller retains it through local failures, then waits for repeater evidence. `health` reports both message states and motion outages/gaps. The [delivery implementation guide](docs/message_delivery_changes.md) maps directives to code and adjustment points.

Chip temperature remains available in telemetry and the `motion` command; it is not body temperature. Gyroscope telemetry clips individual axes to the Cayenne encoding range of -327.68 to 327.67 degrees/second, while the detector, serial readings, and alert text retain the full measured range.

### Tuning and software checks

Edit `FallDetectionConfig` at the top of [`MotionRule.h`](examples/companion_radio/health/MotionRule.h) and rebuild to change the active defaults. Custom code can instead pass a configuration to the `MotionRule` constructor. These settings are compile-time configuration; there is no runtime command to change them.

Set `confirm_quiet_ms = 0` to compare the low-acceleration/impact/rotation sequence without post-event quiet confirmation. Startup settling, sample validation, and cooldown still apply. Use the [tuning guide](docs/gy521_sensor.md#review-and-tune-the-algorithm) for configuration constraints and tradeoffs. Firm torso/chest attachment is the intended starting point; different placements require separate evaluation.

The [motion-rule tests](test/test_motion_rule/test_motion_rule.cpp) replay synthetic 50 Hz recordings. They cover settling, ordinary motion and isolated peaks, missing sequence features, peak order, timing boundaries, repeated low readings, interrupted quiet, invalid data, sample gaps, duplicate timestamps, cooldown/reconnection, timer rollover, and custom settings including sequence-only mode. From this folder, run:

```powershell
pio test -e native -f test_motion_rule
pio test -e native -f test_fall_response
pio test -e native -f test_low_battery
```

This requires a host C++ compiler; the [sensor guide](docs/gy521_sensor.md#polling-and-rule-customization) also documents the standalone C++11 harness. Passing these checks establishes software behavior, not sensitivity, specificity, false alerts per wear-day, or successful radio delivery. Voting, LSTM inference, and body-posture classification remain unimplemented; the studies below explain those possible future directions.

## Fall-detection research

The bibliography identifies **five distinct studies** and records the reference filenames used for the research review. Those filenames are provenance labels, not bundled project files. The current rule adapts the timed sequence from study [4](#study-4), with separately labeled project extensions. The other methods remain research context; none of the reported performance results has been reproduced or validated in SeniorHealthNode.

### Research context

**Smartphone orientation and voting across multiple rules.** Astriani et al. [1](#study-1) combined five threshold-based methods (AGVeSR, Alim, alpha-angle, GyroReDi, and AGPeak), declaring a fall when at least three agreed. They collected 252 activity records (84 falls and 168 non-falls) in six phone orientations from adults aged 25-35, with simulated falls onto a mattress. Table V reports 76.67% accuracy for acceleration alone, 91.67% for AGVeSR, and 95.82% for the five-method vote. Jumping and sitting/standing transitions remained sources of confusion. The paper describes both a 180/72 training/testing split and leave-one-out validation, so its evaluation protocol should not be assumed to be an independently replicated benchmark. For this project, the useful direction is to evaluate orientation changes and agreement between several motion features rather than assume one mounting orientation will work for everyone. See PDF pp. 4-6.

**Posture plus the preceding motion transition.** Li et al. [2](#study-2) used two sensor nodes, on the chest and thigh, sampled at 120 Hz. Their algorithm identifies a static posture and examines the preceding five seconds of acceleration and angular velocity when the posture is classified as lying. In experiments involving three men in their twenties, they reported 91% sensitivity from 70 fall records and 92% specificity from 72 non-fall records. Falling against a wall into a seated position could be missed; quickly lying down could trigger false alarms. This supports investigating temporal context and posture, but the two-body-location method cannot be reproduced directly by SeniorHealthNode's single MPU6050. See PDF sections 3-4, pp. 2-6.

**Learning from sequences does not guarantee better detection.** Wisesa and Mahardika [3](#study-3) trained a long short-term memory (LSTM) recurrent neural network on waist-sensor data from UMAFall. They retained 617 valid recordings, randomly dividing them into 494 training and 123 validation samples. Their X-axis accelerometer model's prediction table identifies all 38 falls and misclassifies 2 of 85 daily activities as falls. In contrast, their combined accelerometer/gyroscope model misses 31 of 38 falls and falsely flags 10 daily activities (Tables 3-4). The reported peak validation accuracy of 100% for the single-axis model is distinct from those saved-model prediction results. For SeniorHealthNode, an LSTM is an experimental alternative requiring suitable data, evaluation on unseen wearers, and resource measurements; simply adding sensor channels or a neural network is not evidence of improvement. See PDF pp. 5-9 (article pp. 4-8).

**A timed low-acceleration, impact, and rotation sequence.** Huynh et al. [4](#study-4) tested a chest-mounted accelerometer/gyroscope system on 36 adults aged 18-28 and 38-56, collecting 702 laboratory movements and using half the dataset for development and half for assessment. Their rule first detects low acceleration, then requires both an acceleration peak and an angular-velocity peak within a 0.5-second window. The optimized study thresholds were 0.30-0.35 g for low acceleration, 2.4 g for impact, and 240 degrees/s for rotation. Table 3 reports 96.3% sensitivity and 96.2% specificity; the optimized acceleration-only comparison achieved 97.36% and 82.72%, respectively. The main improvement was fewer false alarms, with a small sensitivity reduction. These are chest-mounted, simulated-fall results, not thresholds established for this hardware or its intended users. See PDF pp. 3-7.

**A smartphone prototype with limited testing.** Rakhman et al. [5](#study-5) combined acceleration, rotation, posture, and orientation checks using a phone at the left chest. One male participant performed 120 simulated falls and 210 daily-activity trials. Table III records 112 detected falls out of 120 (93.33%); backward falls were detected in 26 of 30 trials. Daily activities including running, lying on a bed, and descending stairs caused false alarms. The paper's claimed 98% daily-activity accuracy is not consistent with Table IV: its alarm column lists seven false alarms, its running row totals 31 despite listing 30 trials, and its downstairs percentage does not match its counts. That aggregate claim should therefore be treated cautiously. The conclusion describes the work as limited to detection and leaves SMS/GPS extensions for future work, despite broader alert language in the abstract. See PDF pp. 4-5 (proceedings pp. 102-103).

### Implications for SeniorHealthNode

The following separates the implemented rule from further research and validation:

- **Implemented: sequences as well as peaks.** The detector uses a bounded low-acceleration / impact / rotation window [4](#study-4) and an optional post-event quiet check. Study [2](#study-2) motivates exploring temporal context and posture, but quiet magnitudes do not classify lying and its two-node posture method is not implemented.
- **Define how the node is worn.** Chest, thigh, waist, and phone-orientation results are not interchangeable [1](#study-1)-[5](#study-5). A nearby, unworn sensor measures its own movement and cannot directly infer the person's body motion. Placement, fastening, units, sensor ranges, and sampling timing need to be specified before adapting published thresholds.
- **Compare methods on the same held-out recordings.** Evaluate a simple temporal rule before adding voting or an LSTM, and keep test wearers separate from threshold tuning or model training. Study [3](#study-3) demonstrates why performance must be measured rather than inferred from model complexity.
- **Measure missed falls and false alerts separately.** Sensitivity is the fraction of falls detected; specificity is the fraction of non-fall activities correctly rejected. Overall accuracy alone can hide missed falls. A future evaluation should also report false alerts per wear-day, detection delay, and performance across routine activities and intended mounting positions.
- **Test detection and delivery separately.** Published classifier results do not establish MeshCore delivery reliability. Record whether an event was detected, whether its alert was queued, and whether the intended recipient received it, alongside battery and radio conditions.

The studies use different participants, hardware, placements, datasets, and evaluation procedures, so their percentages are not a ranking or a performance estimate for this project. None validates the current SeniorHealthNode firmware in everyday use by older adults. Voting, LSTM inference, and body-posture classification are not implemented.

### Bibliography

The entries cite the five supplied works, rather than reproducing the reference lists inside them. Page locators above refer to the supplied PDFs; published pagination is included below where available.

<a id="study-1"></a>

1. Astriani, Maria Seraphina; Heryadi, Yaya; Kusuma, Gede Putra; and Abdurachman, Edi. (2019). **Human Fall Detection using Accelerometer and Gyroscope Sensors in Unconstrained Smartphone Positions.** *International Journal of Recent Technology and Engineering*, 8(3), 69-75. [DOI: 10.35940/ijrte.C3877.098319](https://doi.org/10.35940/ijrte.C3877.098319). Supplied files: `fall_detect_paper1.pdf` and identical `fall_detect_paper7.pdf`.

<a id="study-2"></a>

2. Li, Qiang; Stankovic, John A.; Hanson, Mark; Barth, Adam; Lach, John; and Zhou, Gang. (2009). **Accurate, Fast Fall Detection Using Gyroscopes and Accelerometer-Derived Posture Information.** *Body Sensor Networks (BSN 2009)*, 138-143. [DOI: 10.1109/BSN.2009.46](https://doi.org/10.1109/BSN.2009.46). Supplied files: `fall_detect_paper2.pdf` and identical `fall_detect_paper3.pdf`.

<a id="study-3"></a>

3. Wisesa, I Wayan Wiprayoga, and Mahardika, Genggam. (2019). **Fall detection algorithm based on accelerometer and gyroscope sensor data using Recurrent Neural Networks.** *IOP Conference Series: Earth and Environmental Science*, 258, 012035. [DOI: 10.1088/1755-1315/258/1/012035](https://doi.org/10.1088/1755-1315/258/1/012035). Supplied file: `fall_detect_paper4.pdf`.

<a id="study-4"></a>

4. Huynh, Quoc T.; Nguyen, Uyen D.; Irazabal, Lucia B.; Ghassemian, Nazanin; and Tran, Binh Q. (2015). **Optimization of an Accelerometer and Gyroscope-Based Fall Detection Algorithm.** *Journal of Sensors*, 2015, Article ID 452078, 8 pages. [DOI: 10.1155/2015/452078](https://doi.org/10.1155/2015/452078). Supplied file: `fall_detect_paper5.pdf`.

<a id="study-5"></a>

5. Rakhman, Arkham Zahri; Nugroho, Lukito Edi; Widyawan; and Kurnianingsih. (2014). **Fall Detection System Using Accelerometer and Gyroscope Based on Smartphone.** *2014 1st International Conference on Information Technology, Computer and Electrical Engineering (ICITACEE)*, 99-104. [IEEE publication record](https://ieeexplore.ieee.org/document/7065722/). Supplied file: `fall_detect_paper6.pdf`.

## Getting started

Run the following commands from `SeniorHealthNode-MC`, with PlatformIO Core installed.

1. Use a **Heltec V4.3 OLED** board and connect a GY-521: `VCC -> 3V3`, `GND -> GND`, `SDA -> GPIO4`, `SCL -> GPIO6`, and `AD0 -> GND` for address `0x68`.
2. Set radio parameters to match your MeshCore network. Build and upload the combined firmware with PlatformIO:

   ```powershell
   pio run -e heltec_v4_companion_radio_ble
   pio run -e heltec_v4_companion_radio_ble -t upload --upload-port COM_PORT
   ```

3. Connect a passive buzzer through a suitable driver on GPIO47, following the [wiring guide](docs/gy521_sensor.md#passive-buzzer-wiring-and-shared-functions). Configure the message and fallback location in [`HealthNodeConfig.h`](include/HealthNodeConfig.h), then rebuild after changing those settings. Select the channel with the USB/BLE commands above.
4. Add the selected public hashtag channel (initially `#falldetect`) or matching private channel and shared key to each receiving companion and match radio settings. Fall messages do not require contact registration. Pair your phone with this node over BLE using the default PIN `12345` (`012345` in a six-digit prompt), then configure its name, channels and radio settings through the MeshCore client. A saved custom PIN takes precedence over the default.
5. Use a serial monitor at **115200 baud** and enter `motion` to inspect the latest sensor reading. Test alert delivery with the intended recipient and at the intended location before relying on the setup.

Replace `COM_PORT` with your board's port. The [GY-521 sensor guide](docs/gy521_sensor.md) has the full wiring table, recipient setup, commands, and validation steps. Use `health` to distinguish queueing, local TX, repeater evidence, and exhaustion. Confirm that the intended companion actually receives the channel messages; a heard repeat is not recipient acknowledgment.

## Validation and limitations

The native suites exercise synthetic motion sequences, SOS and button timing,
battery policy, sensor faults, sampling-health diagnostics, and production channel
message delivery with injected radio failures. The `native_delivery` environment
uses production Mesh/crypto code without the ordinary suite's crypto mocks.
See [build validation](docs/build-validation.md) for reproducible commands and the
limits of each check. Software checks concern program behavior, not detection
accuracy on people. Physical buzzer behavior, live sensor readings, concurrent
BLE operation, radio coverage and recipient delivery require hardware testing.
There is currently no dedicated
panic button, inactivity detector, caregiver dashboard, or automatic call to
emergency services in this repository.

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

Default limits in [`HealthNodeConfig.h`](include/HealthNodeConfig.h) are:

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
guide](docs/message_delivery_changes.md) for state transitions, late echoes,
failure handling, and configuration details.

### Serial health and sampling diagnostics

The [stationary gyroscope calibration guide](docs/gyro_calibration.md) describes
`calibrate`, progress, save/cancel/reset, raw readings and MeshCore local CLI use.
Calibration accepts any fixed orientation, collects 15 seconds of stationary
data after a countdown, and validates a new result before explicit saving.
Automatic fall detection pauses during collection and resumes with normal rearm.
Acceleration retains gravity; accelerometer calibration is not included.

The [USB serial debugging guide](docs/usb_serial_debugging.md) provides Windows
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
reference](docs/cli_commands.md) and [`MotionHealth.h`](examples/companion_radio/health/MotionHealth.h).

### Timing, wiring, and regression coverage

- **Sampling and peripheral scheduling:** bounded MPU6050 recovery, background
  environmental collection with cache expiry, 32-byte OLED transfer pieces, and
  staged battery ADC acquisition reduce interference with motion polling. The
  [timing report](docs/fall_timing_changes.md) maps these changes to their source
  files and tunables. Sampling remains cooperative; interrupt/FIFO capture is not
  implemented.
- **Pin reference:** the [simple pin schematic](docs/pin_schematic.md) collects
  external MPU6050/buzzer wiring, onboard display/radio/control signals, and optional
  GPS, RTC, and sensor connections. It uses ESP32 GPIO numbers and is derived from
  firmware; it is not a complete PCB netlist or physically verified wiring plan.
- **Virtual faults:** the [fault-testing guide](docs/virtual-fault-testing.md)
  covers sensor disconnections, truncated I2C reads, loop stalls, button bounce,
  battery/SOS priority, OLED failures, ADC scheduling, and a deterministic replay
  of one simulated hour.
- **Production transport tests:** `native_delivery` exercises the actual group-text
  packet builder, Mesh/Dispatcher, encryption, and packet pool with a fake radio
  and clock. Coverage includes queue expiry, TX failures, repeat matching,
  independent message streams, and sampling-health recovery. `verify.ps1` runs
  both `native` and `native_delivery` after building the combined firmware and merged image.

## Documentation guide

| Guide | Contents |
| --- | --- |
| [Sensor and buzzer](docs/gy521_sensor.md) | Wiring, button gestures, battery reminders, detector tuning and recipient setup. |
| [MPU-6050 and GY-521 reference](docs/mpu6050_gy521_reference.md) | Supplied PDFs, module schematic caveats, driver registers, measurement units and scale factors. |
| [Pin reference](docs/pin_schematic.md) | Firmware-derived GPIO connections and optional peripherals. |
| [Heltec V4 specification](docs/heltec_v4_spec_reference.md) | V4.3.1 specifications, schematic-verified battery/FEM corrections, ADC scaling and V4.2 comparisons. |
| [Local commands](docs/cli_commands.md) | USB diagnostics, persisted public/private fall channels and BLE client usage. |
| [USB serial debugging](docs/usb_serial_debugging.md) | Windows PowerShell and Linux monitoring, timestamped logs, sensor/fault interpretation and connection troubleshooting. |
| [Gyroscope calibration](docs/gyro_calibration.md) | Stationary calibration in any orientation, USB/BLE controls, persistence, quality checks and verification. |
| [Response and delivery](docs/message_delivery_changes.md) | Automatic escalation, fall/false-alarm responses, multipart text, retry states and limits. |
| [Timing](docs/fall_timing_changes.md) | Motion polling, background telemetry, OLED transfers and battery acquisition. |
| [Build and validation](docs/build-validation.md) | Reproducible firmware and host-test commands, outputs and hardware boundaries. |
| [Fault testing](docs/virtual-fault-testing.md) | Synthetic sensor, button, battery, channel-storage and transport scenarios. |
| [Companion protocol](docs/companion_protocol.md) | BLE frames, including the `fall.channel` custom variable. |
| [FAQ](docs/faq.md) | SeniorHealthNode setup answers followed by upstream MeshCore background. |

## MeshCore resources and license

SeniorHealthNode builds on [MeshCore](https://github.com/ripplebiz/MeshCore) and its [documentation](https://docs.meshcore.io). For the base companion, repeater, and client features, refer to the upstream project. The repository retains the [MIT license](license.txt).
