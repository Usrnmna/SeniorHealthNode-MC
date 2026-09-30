# Virtual fault testing

These tests run locally without uploading firmware. They exercise production
MPU6050, motion, alarm, battery, button, ADC, display-transfer, and message-encoding
classes with deterministic inputs. The small harness follows the glue in
examples/simple_sensor/main.cpp; it does not compile or execute MyMesh itself.

## Commands

From the project root, use the existing verification entry point:

```powershell
.\verify.ps1
```

In a worktree without its own Python/compiler installation, pass existing tools:

```powershell
.\verify.ps1 -Python C:\Users\usrnm\SeniorHealthNode-MC\.venv\Scripts\python.exe -CompilerBin C:\Users\usrnm\SeniorHealthNode-MC\.pio\tools\mingw64\bin
```

The builds, dependencies, and tests still use this worktree. Only Python and the
native compiler come from the supplied paths. For the focused suite, with that
compiler available on PATH, run:

```powershell
python -m platformio test -e native -f test_real_world_faults
```

## Scenario examples

| Scenario family | Inputs / example | Required result |
| --- | --- | --- |
| Truncated MPU sample (14 cases) | Return each length 0 through 13 instead of 14 bytes midway through confirmation | Invalidate sample, cancel old evidence, detect a fresh fall after recovery |
| Cooperative loop stalls (5 cases) | Wait 101 ms, 250 ms, 1 s, 5 s, or 60 s between low acceleration and impact | Do not join evidence across missing time; recover after quiet settling |
| Disconnection at three stages | Unplug while armed, after low acceleration, or during confirmation; remain missing through reinitialization | No stale fall; reconnect, settle, then detect a new sequence |
| Device and configuration faults | Wrong WHO_AM_I; configuration write failure; alternating failed/successful samples for one minute | No usable motion until recovery; intermittent failures cannot accumulate quiet evidence |
| HAL transaction errors (3 cases) | Inject nonzero return statuses despite a full response buffer | Reject the buffer; pass the configured 5 ms timeout to HAL; recover |
| Oversized sample | Return 15 bytes | Reject without accepting old motion evidence |
| Signed sensor axes / rollover | Negative acceleration and gyro axes; clock wraps during the event | Use magnitudes correctly; detect and acknowledge once |
| Disconnect during SOS | Disconnect after an alert; reconnect before cooldown ends | Keep SOS active; preserve cooldown; later fall detection remains enabled |
| Noisy or held PRG button | Bounce press/release edges; hold at fall onset; stall the loop during a press | Require three new valid clicks; neither bounce nor a held press silences SOS |
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

Parameterized values count as individual GoogleTest cases: 19 standalone scenarios
plus 67 parameterized cases = **86 added cases**, across 26 scenario definitions.
The original 81 cases remain included: **167 total native cases**.

## Verified local results — 2026-09-29

Verification ran in `C:\Users\usrnm\.codex\worktrees\cad7\SeniorHealthNode-MC`.
The baseline completed before the new suite or mock changes were installed.

| Run | Observed result | Local log |
| --- | --- | --- |
| Original `verify.ps1` | Both firmware builds and merged images; 81/81 native cases passed | `.pio/baseline-verification.log` |
| Focused new suite | 86/86 fault cases passed | `.pio/fault-tests.log` |
| Expanded `verify.ps1` | Both firmware builds and merged images; 167/167 cases across nine suites passed | `.pio/expanded-verification.log` |

The worktree lacked a local Python environment, so these runs used the explicit
Python/compiler paths above. Initial sandbox package installation failed with
Windows access denied; verification completed with execution permission. This
was a tooling issue, not a failed firmware assertion. Logs and build products
remain local under ignored `.pio`. Production firmware sources were not changed,
and no firmware was uploaded.

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
- Actual radio packet allocation, queueing, RF transmission, reception, and GPS.
  Retry tests inject the allocation outcome into FallResponse; they do not execute
  FallAckSender or the MeshCore network. Queued is not confirmed delivery.
- ROM download recovery, flash/partition correctness, and physical wiring safety.
- Fall-detection sensitivity/specificity on representative recorded and bench data.
  Passing these tests is not a guarantee against bricking or a medical validation.

Known scheduling behavior: delayed polls stretch buzzer phases. ADC divider
power stays enabled until collection can resume and finish. Tests explicitly
record these behaviors; they do not promise hard real-time service.
