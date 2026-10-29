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
SOS priority. It also checks the default instance against the compiled setting,
PWM attachment only during tones, disconnection during silent gaps, idle polling
without GPIO/LEDC writes, restoration of the ordinary pitch after battery playback,
and inactive disconnected output after a failed frequency setup. LOW idle is the
compiled default (`buzzer_active_low = false`).
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
| Truncated MPU sample (14 cases) | Return each length 0 through 13 instead of a 14-byte FIFO record during an incomplete candidate | Invalidate sample, cancel old evidence, detect a fresh fall after recovery |
| MPU FIFO freshness | Empty FIFO, identical complete records and prolonged missing output across clock wrap | Only complete FIFO records advance evidence; a stuck sensor invalidates and reinitializes |
| MPU configuration readback | Mismatch each of ten registers; fail a read; change reserved bits | Reject mismatched relevant bits and bus failures; ignore reserved bits; recover after retry |
| Battery calibration | Meter input, stale/zero/saturated ADC data, invalid multipliers, old and new preference files | Preserve conversion arithmetic; reject invalid suggestions; round-trip the saved multiplier; absent setting selects default |
| Cooperative loop stalls (5 cases) | Wait 101 ms, 250 ms, 1 s, 5 s, or 60 s between low acceleration and impact | Do not join evidence across missing time; recover after contiguous fresh samples |
| Disconnection at three stages | Unplug while armed, after low acceleration, or after impact in an incomplete sequence; remain missing through reinitialization | No stale fall; reconnect, recover with fresh samples, then detect a new sequence |
| Device and configuration faults | Wrong WHO_AM_I; configuration write failure; alternating failed/successful samples for one minute | No usable motion until recovery; intermittent failures cannot accumulate quiet evidence |
| HAL transaction errors (3 cases) | Inject nonzero return statuses despite a full response buffer | Reject the buffer; pass the configured 5 ms timeout to HAL; recover |
| Oversized sample | Return 16 bytes for a 14-byte request | Reject without accepting old motion evidence |
| Signed sensor axes / rollover | Negative acceleration and gyro axes; clock wraps during the event | Use magnitudes correctly; detect and acknowledge once |
| Disconnect during SOS | Disconnect after an alert, then reconnect | Keep SOS active; preserve the short event refractory deadline; fresh-data recovery permits later detection |
| Noisy or held PRG button | Bounce press/release edges; hold at fall onset; stall the loop during a press | Require a valid post-fall gesture; neither bounce nor a held press silences SOS |
| Sensor unavailable during acknowledgment | Disconnect sensor, then perform three bouncing-but-valid clicks | Local SOS stops and assistance request becomes pending |
| Low battery versus SOS | 3300 mV while SOS is active | Defer battery playback without consuming reminder; start after acknowledgment |
| Charging versus SOS | Simulated active charging input during SOS | Clear battery policy only; preserve SOS; unplugging rearms battery warning |
| Unknown ADC value / threshold | 0, 3499, 3500, and 3501 mV in sequence | Unknown is not recovery; equality preserves state; above threshold clears |
| Allocation recovery (4 cases) | Local packet allocation succeeds on attempt 1, 2, 30, or 60, across clock wrap | Enforce retry interval, stop after success, do not restart SOS |
| Allocation exhaustion | All 60 attempts fail while motion continues | Stop retries; a later fall and acknowledgment create a fresh request |
| Fresh construction after acknowledgement | Construct a new in-memory harness after resolution, without loading incident storage | Volatile request/battery state clears; require fresh motion evidence. This case does not test firmware restart restoration. |
| Jittered one-hour replay | Fixed seed 0x6050; 20–50 ms steps, injected missing reads and 250 ms stalls; clock rollover | No alert on the bounded ordinary-motion trace; detect a later complete fall |
| OLED failures (32 cases) | Fail at each of the 32 frame chunks | Abort frame; next frame starts at byte zero and completes in 32 chunks |
| ADC scheduling under a stall | 60-second loop pause; alternating raw 0/4095 readings | One conversion per call; average 2047; divider turned off on completion |
| Environmental BUSY alongside motion | AHT remains BUSY for 1 s while MPU detects a fall | AHT terminates with failure; SOS continues; AHT can reinitialize |
| Truncated AHT measurements (6 cases) | Return each length 0–5 instead of 6 bytes | Fail measurement; reject restart until reinitialized; then recover |
| UTF-8 / buffer bounds | Oversize multibyte message; valid message with extreme coordinates | Reject oversize without changing destination; preserve surrounding guard bytes |

Parameterized scenarios expand into individual GoogleTest cases. Use the test
runner output for totals for the current checkout.

## Detector evidence, FIFO and restart regressions

```powershell
python -m platformio test -e native -f test_motion_rule -f test_motion_fifo
python -m platformio test -e native -f test_incident_recovery -f test_post_fall
```

The detector suite exercises strong sequences despite continuing movement,
missing low-g or gyro peaks through contextual paths, supported/slow descents,
pre-trigger rotation, expiration followed by a new candidate, brief faults during
motion, repeated events within a minute, short refractory behavior across faults,
invalid configuration and timer rollover. Synthetic sitting, bending, gradual
lying and isolated bumps test specific negatives; a device drop matching the
strong sequence deliberately remains an explicit ambiguity. These inputs are not
a labelled human-activity dataset.

The FIFO suite uses the production `MPU6050Sensor` with a register/FIFO fake.
It covers two-record batching, sample phase/jitter, retained peaks during bounded
host stalls, overflow before and during a burst, partial reads, trailing partial
records, stale backlog, software-queued expiry, discontinuity recovery and axis
saturation. Driver-to-rule tests check that loss cannot connect an old low-g
fragment to a later peak and that complete buffered strong evidence can be
recognized before a subsequent loss. It does not measure real oscillator timing,
I2C transactions or ISR latency.

Trace tests exercise bounded rings, raw/corrected axes, acceptance/rejection,
retained peak/trough checkpoints during long candidates and frozen loss evidence.
Post-fall tests include the original deadline and one later fresh-quiet update,
without retroactively strengthening an earlier assessment or restoring old quiet.

Incident tests use the production store with a filesystem fake and production
response state. They cover alternating records, resolution tombstones, torn writes,
corruption, unreadable records, repair/readback failure, generation rollover,
qualified-clock expiry and clock-uncertain recovery limits. The target currently
uses the clock-uncertain path because its software clock cannot prove elapsed
downtime; plausible saved timestamps do not qualify that clock. Recovery-count
advancement depends on a successful write. Response restoration tests start with
fresh motion/gesture evidence, retain the original destination and deadline where
age is known, and permit a new quiet assessment after an overdue general warning.
They do not establish flash durability or execute a physical power cycle.
Backpressure regressions also expose the dropped-confirmation counter when an
earlier confirmation retains the last pending ownership and a newer incident is
resolved locally; this remains a bounded delivery-capacity limit.

The [maintained detector reference](fall_detection.md) separates implemented
behavior from threshold assumptions and remaining validation needs.

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
It covers the fixed 30-second deadline, final continuous quiet period, one later
quiet upgrade, no invented assistance request in automatic text, missing and
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

## Actual companion health methods and recording

`test_health_integration` compiles production `HealthMonitor.cpp` and
`HealthRecording.cpp` against a narrow host `MyMesh` shell. The methods own
orchestration; the shell supplies clock/GPIO, the production MPU6050 driver with
a FIFO bus fake, storage latency/failure and transport enqueue boundaries.
Coverage includes both simulated motion-service positions, I2C time advancement,
calibration, persistence delays, original deadlines, gesture priority, repeated
incidents, false-alarm cancellation, delivery callbacks and saturation, late
stillness, recording backpressure and availability accounting.

This differs from `test_fall_pipeline`, which remains a component pipeline.
Neither executes the complete `main.cpp`/`MyMesh.cpp` scheduler, BLE stack or RTOS.
The USB queue tests exercise production enqueue/delivery code with a manual
worker; physical task scheduling, internal CDC locks and throughput remain
unverified. [Recording checks](motion_recording.md) add portable checkpoints,
strict transport validation and repeated production replay of a generated stream.

## Persistence interruption model

`DurableIncidentFS` separates volatile file contents from durable state and
models before/after operation cuts, partial writes, failed or torn flushes,
resolution and recovery-count updates, repair and generation rollover.
Durably completed resolutions do not revive older alarms in this model.
Resets before a successful durable recovery-count update cannot consume a
persisted recovery budget; unknown-clock policy is unchanged.

The bundled Arduino filesystem exposes void `flush`/`close` and discards the
underlying synchronization result. Tests explicitly demonstrate successful
cached readback followed by rollback on reboot when flush fails silently.
Firmware success therefore means **verified readback**, not verified physical
durability. A lower-level failure-reporting storage boundary and controlled
power-cut measurements remain necessary before claiming durable completion.

## What still needs physical testing

- Real FIFO capture/overflow and timestamp phase, I2C timeout duration, bus
  contention, electrical faults, and RTOS scheduling.
  The HAL fake reports errors immediately; asserting its timeout argument does not
  establish that the real driver meets that deadline.
- Actual main-loop integration, GPIO/PWM waveforms, switch mechanics, ADC accuracy,
  stack/heap headroom, watchdog behavior, interrupted incident writes, repeated
  resets, clock continuity, power interruption and brownout recovery.
- Real RF transmission/reception, target resource pressure, and GPS. The original
  fault harness injects allocation results into `AssistanceDelivery`; the added delivery
  suites exercise `FallAckSender`, Mesh, and Dispatcher with a fake radio. Neither
  establishes physical delivery. Queueing, local TX completion, and repeater echo
  evidence are separate states; none proves a particular person received a message.
- ROM download recovery, flash/partition correctness, and physical wiring safety.
- Fall-detection sensitivity/specificity on representative recorded and bench data,
  with soft/slow/supported falls, device drops, loose attachment and daily activities;
  threshold/filter, gyro temperature and buzzer-coupling evaluation.
  Passing these tests is not a guarantee against bricking or a medical validation.

Known scheduling behavior: delayed polls stretch buzzer phases. ADC divider
power stays enabled until collection can resume and finish. Tests explicitly
record these behaviors; they do not promise hard real-time service.
