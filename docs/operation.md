# Alarm operation, responses and battery reminders

SeniorHealthNode uses one MeshCore BLE companion image with local fall detection on the Heltec V4.3 OLED node. Monitoring and channel alerts continue when the phone is disconnected.

The sensor must move with the wearer. The [detection and research guide](fall_detection.md)
describes the experimental rule, measurement limits and methods that remain
unimplemented. Detector thresholds are compiled settings; runtime channel selection
is documented below.

## Alarm and response behavior

A detected fall also latches an external passive buzzer into an SOS loop, with five seconds of silence between sequences. While SOS is active, PRG is reserved for alarm acknowledgement. Three short PRG clicks within three seconds silence it and queue a configurable Unicode acknowledgement to a MeshCore channel, appending the GPS provider's valid coordinates or `DefaultLocation` (`HOME`). Reboot clears the alarm and detector state. The companion retains its low-voltage automatic shutdown below 3400 mV when external power is not detected; shutdown stops monitoring and sound. Buzzer GPIO47, sound/button timings, channel defaults and message are grouped in [`HealthNodeConfig.h`](../include/HealthNodeConfig.h). See the [buzzer wiring and function guide](gy521_sensor.md#passive-buzzer-wiring-and-shared-functions).

If neither response has been accepted after **30 seconds**, the node automatically sends the assistance text and location with ` | USER COULD NOT CONFIRM THEIR CONDITION` appended. If fresh motion samples establish quiet for the **final 10 continuous seconds**, it instead appends ` | NO RESIDUAL MOVEMENTS DETECTED; POSSIBLE INCAPACITATION OF USER`. The quiet duration and movement thresholds are adjustable. Missing or invalid samples cannot establish quiet. SOS keeps repeating through either warning and through delivery failures. Three presses afterward stop SOS and send the independent text `USER HAS CONFIRMED FALL`. False-alarm double presses remain available after either warning. A warning that exceeds the channel's text limit uses two labelled messages; standalone warning text has no leading ` | `. See the [response and delivery guide](message_delivery_changes.md#post-fall-response-directives).

For a false alarm, press and release PRG **twice within one second**, measured from first press-down to second release. SOS continues until the **three-second window from the first press closes**. A valid third press within that window takes priority as fall confirmation/assistance. Otherwise, the accepted double press stops SOS and queues `User Confirms False Alarm | <location>`, cancelling that incident's queued automatic warnings and initial-alert retries. A started third press that is held too long or released late cancels the double candidate and leaves SOS active. Already transmitted messages cannot be recalled.

Below 3500 mV, the node plays three 450 ms battery-warning tones at 2093, 1760, and 1480 Hz every ten minutes. Fall SOS takes priority. The warning clears above 3500 mV. Silencing as soon as charging starts requires an added charging-status signal; the stock board has no ESP32 connection for it, so sensing is disabled by default. See the [low-battery settings and hardware limitation](gy521_sensor.md#low-battery-buzzer-reminder).

USB `battery` reports cached ADC counts, voltage and sample age. `battery calibrate <meter_mV>` suggests a correction using a battery-terminal meter reading; `set adc.multiplier <value>` saves an explicit adjustment. See the [battery calibration workflow](cli_commands.md#battery-diagnostics-and-calibration-usb). The default remains 5.42 until adjusted.

## Response flow

```text
Possible fall --> channel alert + repeating SOS --> PRG response or 30 s deadline
                                                        |
                   +------------------------------------+-------------------+
                   |                                    |                   |
             3 short presses                      fast double press      no response
                   |                                    |                   |
             silence SOS                     wait for 3 s window       send warning
                   |                                    |              SOS continues
          assistance before warning            silence SOS; send            |
          fall confirmation afterward           false-alarm text      later responses
                                                cancel same-incident   remain available
                                                pending warnings and
                                                initial-alert retries
```

A valid third press takes priority over a pending double. A third press that
starts but is held too long or released late cancels the double candidate and
leaves SOS active. The [delivery guide](message_delivery_changes.md#post-fall-response-directives)
defines full gesture timing, automatic-warning formatting and cancellation rules.

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
[operation without a buzzer](gy521_sensor.md#operation-without-a-buzzer).

`buzzer_active_low = false` in [`include/HealthNodeConfig.h`](../include/HealthNodeConfig.h)
selects LOW idle for the DIYables passive module: GPIO47 uses 50% PWM for tones
and disconnects PWM during silence. Playback remains nonblocking. Set it to `true`
only for an alternative driver requiring HIGH idle, then rebuild and flash. An incorrect
setting can leave the driver energized during intended silence even when tones
sound normal, with possible extra current draw or heating depending on hardware.
See [polarity settings and wiring](gy521_sensor.md#buzzer-trigger-polarity).

| Function | Current behavior |
| --- | --- |
| Disabled buzzer | No GPIO/PWM setup or audible output; fall-response and message behavior remain active. `beep` reports the disabled setting. |
| Ordinary beep | `beep` command or `alarm_buzzer.beep()`; 150 ms at 2000 Hz. Returns unavailable while an alarm sequence is playing. |
| Fall SOS | Three short, three long, three short tones at 2000 Hz; 150 ms dots, 450 ms dashes and five seconds of silence between sequences. |
| Acknowledge fall | Three debounced PRG clicks within three seconds, each shorter than one second; silences SOS. Before automatic escalation, sends the assistance request; afterward, sends `USER HAS CONFIRMED FALL`. Idle clicks do not request help. |
| Unanswered fall | At 30 seconds, send assistance+location with the unconfirmed-condition warning, or the no-residual-movement warning if the final 10 seconds qualify as quiet. SOS has no elapsed-time cutoff. |
| False alarm | Two complete presses within one second; wait until the three-second window closes. A valid triple press wins. Stops SOS and sends `User Confirms False Alarm` with GPS or fallback location, before or after escalation. |
| Channel text | Default `#falldetect`. Assistance and false-alarm responses include location; automatic warnings append their exact text or use two labelled parts when necessary. Late confirmation is standalone. Separators are inserted only between joined text. |
| Delivery retries | Allocation/local TX failures retry after five seconds, with 60 total attempts and a 30-second queue deadline. After local TX, wait 15 seconds for a repeat; allow at most four successful local sends without one. Retries can display duplicate text. See the [delivery guide](message_delivery_changes.md). |
| Low battery | Below 3500 mV: 2093 Hz, 1760 Hz, then 1480 Hz; each tone lasts 450 ms with 150 ms gaps. Repeats every ten minutes from accepted sequence starts. |
| Battery recovery | Above 3500 mV clears the warning; exactly 3500 mV preserves its state. Battery sampling runs once per second; a zero reading is treated as unavailable. |
| Alarm priority | SOS preempts a battery sequence. A due battery reminder waits while the buzzer is busy. |
| Reboot | Clears volatile alarm, acknowledgement and reminder state; low battery is evaluated again after startup. |

> [!IMPORTANT]
> **Connecting a charger does not immediately silence the battery warning on the current hardware.** Charging sensing defaults to disabled (`charging_status_pin = -1`). With no charging-status GPIO connection, the warning clears only when measured battery voltage exceeds 3500 mV. An added, conditioned 3.3 V active-charging signal can cancel the battery warning before voltage recovers; it never cancels SOS. See the [charging input requirements](gy521_sensor.md#low-battery-buzzer-reminder).

Set the fall-message channel at runtime with `get fall.channel`, `set fall.channel public #name`, or `set fall.channel private Name <shared-key>`. For MeshCore One v1.5.1, use **Tools → CLI → local session** over BLE; the same commands work over USB serial at 115200 baud. Settings survive reboot and apply to new incidents; active incidents and queued messages retain their original channel. See the [channel command guide](cli_commands.md#fall-message-channel-usb-and-ble).

Edit [`include/HealthNodeConfig.h`](../include/HealthNodeConfig.h) for buzzer, battery, button and compiled channel defaults. Edit `FallDetectionConfig` in [`MotionRule.h`](../examples/companion_radio/health/MotionRule.h) for motion thresholds and timing. Those source changes require a firmware rebuild; channel commands do not.
