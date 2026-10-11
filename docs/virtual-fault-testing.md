# Virtual fault testing

These tests run locally without uploading firmware. They exercise production
MPU6050, motion, alarm, battery, button, ADC, display-transfer, and message-encoding
classes with deterministic inputs. The sensor fault harness follows the glue in
examples/companion_radio/HealthMonitor.cpp; it does not compile or execute MyMesh itself.
Delivery tests also exercise the production group-text packet builder,
Mesh/Dispatcher, encryption, packet pool, and delivery controller with fake radio
and clock boundaries. See [message-delivery changes](message_delivery_changes.md)
for the directive-to-code map, channel contract, and adjustable limits.

## Commands

From the project root, use the existing verification entry point:

```powershell
.\verify.ps1
```

In a worktree without its own Python/compiler installation, pass existing tools.
Replace the placeholders with the Python executable and compiler directory:

```powershell
.\verify.ps1 -Python '<python-executable>' -CompilerBin '<compiler-bin-directory>'
```

The builds, dependencies, and tests still use this worktree. Only Python and the
native compiler come from the supplied paths. For the focused suite, with that
compiler available on PATH, run:

```powershell
python -m platformio test -e native -f test_real_world_faults
```

For buzzer polarity, run `python -m platformio test -e native -f test_alarm_buzzer`.
This suite calls the production `AlarmBuzzer` with both trigger polarities and
records GPIO/LEDC requests for inactive initialization, failed initialization,
beep completion, SOS gaps/stopping, battery pitch changes and cancellation, and
SOS priority. It also checks the default instance against the compiled setting.
Disabled-output cases check that playback requests make no GPIO-level or LEDC
calls, while the production fall-response state machine still escalates at its
deadline and accepts the PRG confirmation gesture. These cases do not run the
complete BLE companion loop or establish physical disconnected-module behavior.
These host fakes verify requested levels/duties, not physical PWM waveforms,
reset transients, module current draw or audible output.

## Scenario examples

For stationary gyroscope calibration, run
`python -m platformio test -e native -f test_gyro_calibration -f test_sensor_timing`.
The suite exercises arbitrary stationary orientations, independent validation,
movement/noise/temperature rejection, fresh-data gaps, timeout and clock rollover,
pending-result expiration, corrupted/interrupted storage and correction without
removing gravity or overwriting raw measurements. It does not run the complete
BLE companion or prove hardware calibration accuracy. See the
[calibration procedure](gyro_calibration.md).

| Scenario family | Inputs / example | Required result |
| --- | --- | --- |
| Truncated MPU sample (15 cases) | Return each length 0 through 14 instead of 15 bytes midway through confirmation | Invalidate sample, cancel old evidence, detect a fresh fall after recovery |
| MPU data-ready status | Clear flag, unrelated status bit, identical axes with a fresh flag, and prolonged missing data across clock wrap | Only fresh data reaches the detector; a stuck sensor invalidates and reinitializes |
| MPU configuration readback | Mismatch each of eight registers; fail a read; change reserved bits | Reject mismatched relevant bits and bus failures; ignore reserved bits; recover after retry |
| Battery calibration | Meter input, stale/zero/saturated ADC data, invalid multipliers, old and new preference files | Preserve conversion arithmetic; reject invalid suggestions; round-trip the saved multiplier; absent setting selects default |
| Cooperative loop stalls (5 cases) | Wait 101 ms, 250 ms, 1 s, 5 s, or 60 s between low acceleration and impact | Do not join evidence across missing time; recover after quiet settling |
| Disconnection at three stages | Unplug while armed, after low acceleration, or during confirmation; remain missing through reinitialization | No stale fall; reconnect, settle, then detect a new sequence |
| Device and configuration faults | Wrong WHO_AM_I; configuration write failure; alternating failed/successful samples for one minute | No usable motion until recovery; intermittent failures cannot accumulate quiet evidence |
| HAL transaction errors (3 cases) | Inject nonzero return statuses despite a full response buffer | Reject the buffer; pass the configured 5 ms timeout to HAL; recover |
| Oversized sample | Return 15 bytes | Reject without accepting old motion evidence |
| Signed sensor axes / rollover | Negative acceleration and gyro axes; clock wraps during the event | Use magnitudes correctly; detect and acknowledge once |
| Disconnect during SOS | Disconnect after an alert; reconnect before cooldown ends | Keep SOS active; preserve cooldown; later fall detection remains enabled |
| Noisy or held PRG button | Bounce press/release edges; hold at fall onset; stall the loop during a press | Require a valid post-fall gesture; neither bounce nor a held press silences SOS |
| Sensor unavailable during acknowledgment | Disconnect sensor, then perform three bouncing-but-valid clicks | Local SOS stops and assistance request becomes pending |
| Low battery versus SOS | 3300 mV while SOS is active | Defer battery playback without consuming reminder; start after acknowledgment |
| Charging versus SOS | Simulated active charging input during SOS | Clear battery policy only; preserve SOS; unplugging rearms battery warning |
| Unknown ADC value / threshold | 0, 3499, 3500, and 3501 mV in sequence | Unknown is not recovery; equality preserves state; above threshold clears |
| Allocation recovery (4 cases) | Local packet allocation succeeds on attempt 1, 2, 30, or 60, across clock wrap | Enforce retry interval, stop after success, do not restart SOS |
| Allocation exhaustion | All 60 attempts fail while motion continues | Stop retries; a later fall and acknowledgment create a fresh request |
| Reboot with request pending | Construct fresh production state after acknowledgment | Volatile alarm/request/battery state clears; require fresh motion evidence |
| Jittered one-hour replay | Fixed seed 0x6050; 20–50 ms steps, injected missing reads and 250 ms stalls; clock rollover | No alert on the bounded ordinary-motion trace; detect a later complete fall |
| OLED failures (32 cases) | Fail at each of the 32 frame chunks | Abort frame; next frame starts at byte zero and completes in 32 chunks |
| ADC scheduling under a stall | 60-second loop pause; alternating raw 0/4095 readings | One conversion per call; average 2047; divider turned off on completion |
| Environmental BUSY alongside motion | AHT remains BUSY for 1 s while MPU detects a fall | AHT terminates with failure; SOS continues; AHT can reinitialize |
| Truncated AHT measurements (6 cases) | Return each length 0–5 instead of 6 bytes | Fail measurement; reject restart until reinitialized; then recover |
| UTF-8 / buffer bounds | Oversize multibyte message; valid message with extreme coordinates | Reject oversize without changing destination; preserve surrounding guard bytes |

Parameterized scenarios expand into individual GoogleTest cases. Use the test
runner output for totals for the current checkout.

## Delivery and motion-health regressions

```powershell
python -m platformio test -e native -f test_dispatcher_delivery
python -m platformio test -e native_delivery
python -m platformio test -e native -f test_motion_health
```

These suites cover local TX-start failure and timeout recovery, bounded packet
allocation retries, queue expiry under CAD or airtime restrictions, independent
fall/response streams, packet callback ownership, repeat matching,
motion progressing during radio failure, and stale-sample health reporting.
They use the production transport path. State-only retry scenarios in the sensor
fault harness inject allocation results into `AssistanceDelivery`;
`FallResponse` retains the button/alarm/request latch without a second retry budget. Retries use fresh timestamps, so repeated channel text
can be displayed more than once; recipient-level exactly-once delivery is not
asserted. A matching repeater echo does not prove human or caregiver reception.

Reproducible combined-firmware checks are described in [build validation](build-validation.md).
The deterministic fault suite and production transport suite are both included in
`verify.ps1`. The harness does not execute the complete companion or BLE stack.

## Post-fall escalation and fall-response regressions

`python -m platformio test -e native -f test_post_fall` runs the production response,
residual-movement, formatting and outbox logic with synthetic time and inputs.
It covers the fixed 30-second deadline, final continuous quiet period, missing and
invalid samples, slow vector drift, late clicks after either warning, preserved SOS,
rollover, UTF-8 boundaries, two-part warnings, reserved confirmation capacity, stale
callbacks and exhausted retries. False-alarm tests cover 999/1000/1001 ms double
windows, the full three-second decision wait, third-release priority at the exact
deadline, held/late third presses, bounce, held-at-detection input, rollover, warning
deadline overlap, location/fallback text, backpressure and incident-scoped cancellation.
Companion-button tests check UI suppression throughout the wait and navigation
resuming afterward. Tests also enforce the absence of a leading
` | ` on standalone warning text.

The `native_delivery` suite additionally sends these payloads through production
MeshCore encryption, queues and decryption. It checks actual queued cancellation
and safe completion of already-started alerts before confirmation. False-alarm
cases cancel both queued message streams, preserve in-flight initial transmissions,
and decrypt the exact correction text and location on public/private channels. Cipher
padding and CAD backoff are handled by the test receiver/clock, not by firmware
changes. These tests use fake radios and do not establish actual reception.


## Channel settings and companion-button regressions

```powershell
python -m platformio test -e native -f test_fall_channel
python -m platformio test -e native -f test_companion_button
```

Channel tests cover public/private syntax, key formats, UTF-8 names, malformed
values, saved-record recovery, failed writes and immutable incident/outbox channel
snapshots. `native_delivery` checks hashtag key derivation and reception with a
matching private key through production crypto. Storage fakes do not prove
flash durability under physical power interruption.

Companion-button tests cover suppression while the alarm owns PRG, clearing
pending UI gestures and requiring release before ordinary UI input resumes.
They do not operate the physical switch or display.

## What this establishes

The harness calls the actual production state machines and driver decoding.
Faults and time are repeatable, with no real sleeps, serial ports, radio sends,
flash writes, or physical devices. The one-hour replay is simulated time, not an
hour-long hardware soak. The test's ordinary-motion trace is synthetic, not a
recorded human activity dataset.

## What still needs physical testing

- Real I2C timeout duration, bus contention, electrical faults, and RTOS scheduling.
  The HAL fake reports errors immediately; asserting its timeout argument does not
  establish that the real driver meets that deadline.
- Actual main-loop integration, GPIO/PWM waveforms, switch mechanics, ADC accuracy,
  stack/heap headroom, watchdog behavior, power interruption, and brownout recovery.
- Real RF transmission/reception, target resource pressure, and GPS. The original
  fault harness injects allocation results into `AssistanceDelivery`; the added delivery
  suites exercise `FallAckSender`, Mesh, and Dispatcher with a fake radio. Neither
  establishes physical delivery. Queueing, local TX completion, and repeater echo
  evidence are separate states; none proves a particular person received a message.
- ROM download recovery, flash/partition correctness, and physical wiring safety.
- Fall-detection sensitivity/specificity on representative recorded and bench data.
  Passing these tests is not a guarantee against bricking or a medical validation.

Known scheduling behavior: delayed polls stretch buzzer phases. ADC divider
power stays enabled until collection can resume and finish. Tests explicitly
record these behaviors; they do not promise hard real-time service.
