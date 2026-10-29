# Motion recording and production replay

The BLE companion includes an optional USB development recorder. Detection,
local alarms, acknowledgement, mesh operation and persistence do not require a
host recorder or replay utility. The firmware target remains
`heltec_v4_companion_radio_ble`.

Recording is disabled after startup. It snapshots current detector and response
state when enabled; starting or stopping a recording does not reset evidence,
rearm the detector, acknowledge an incident or change a response deadline.

## Capture controls

The existing USB command interface accepts:

```text
motion record start
motion record status
motion record stop
```

Start emits a versioned session header, checkpoint and current calibration,
acquisition and buzzer metadata. Status reports whether collection is active or
draining, the session and sample sequences, issued/dropped records, short
writes, exported packets, buffered bytes, high-water usage and capacity. Stop
ends collection immediately and schedules an `End` record. Capture is complete
only after that record has been received and validated. Status also reports USB
worker readiness, queued/in-flight packets, rejected/failed writes, completed
API calls and minimum free worker stack bytes. API completion does not prove
host receipt. `draining` describes recorder frames still awaiting queue admission;
`usb_pending_packets` includes all queued or in-flight output, including ordinary
diagnostics. A new session cannot start while the previous session is active or
has unqueued frames, or if worker initialization failed. Once the previous
`End` is queued, FIFO ordering keeps it before the next `Begin` even when its USB
write is still in progress. Unrelated diagnostic traffic does not prevent start.
Separate complete sessions must still be extracted for replay.

The serial log must preserve bytes, including the ASCII record-separator byte
`0x1e`. Text editors, terminal line wrapping and newline conversion can damage
the transport. Ordinary text may appear between complete frames. The replay
utility ignores that text and strictly validates capture frames. A mixed
console log can also contain ordinary diagnostic output; the capture format
itself contains no channel names, keys, location, radio identity or credentials.

The recorder uses an 8192-byte ring, a fixed serialization staging buffer and
a small deferred-End slot. Records are admitted whole or dropped whole. Each
drain handles at most one 31-byte frame. USB transmission is isolated behind a
bounded worker queue because the platform CDC driver can wait internally even
after an apparent capacity check. A blocked worker causes bounded queue/ring
backpressure and visible recording loss; it does not make acquisition wait for
USB. The worker uses eight 33-byte queue slots, a fixed 3072-byte stack and static
RTOS control structures. The producer performs zero-timeout queue admission;
the worker alone may wait on the CDC driver. Health replies and enabled BLE debug
messages use this path; BLE pairing PIN values are omitted. Debug text is limited
to 95 bytes and three queue attempts. Existing firmware-library logs and rescue
operations are not a general nonblocking logging guarantee. A zero-byte queue
rejection retains the frame for a later attempt; a positive partial write counts
as export loss and is not retried. Frame sequence and CRC checks expose worker
write failures in the received capture.

Command replies receive queue admission before recording frames, and USB input
continues to be parsed when replies cannot drain. A stop command therefore does
not depend on completion of its start reply. Queue capacity, worker scheduling
and physical USB behavior still require target measurements. Samples are never
written to flash. The staging buffer occupies 4096 bytes; other recorder metadata,
availability/service counters and RTOS controls add fixed overhead.

Each normal sample occupies 100 binary record bytes and ten 31-byte transport
frames: 15,500 transport bytes/s at the configured 50 Hz, before transitions and
metadata. This is a format calculation, not measured USB throughput. A consumer
or service cadence below that rate eventually loses records. Initial checkpoint
traffic and ordinary diagnostic output add load. `status` and the final report
provide the build-specific RAM totals and validation evidence.

## Replay

From the repository directory:

```powershell
.\.venv\Scripts\python.exe tools\health_replay.py C:\captures\motion-session.bin
```

The command validates framing, source/configuration identity, session boundaries
and payloads, compiles the host replay executable, and runs the same capture
twice. `--repeat 3` changes the repetition count; `--compiler <path>` selects
another C++17 compiler. `--decoded <path>` optionally writes the validated
binary records after successful replay; this path cannot alias the original
capture, including through a hard link. Calibration metadata must satisfy the
same validity rules as the sensor's applied correction. The repository MinGW
compiler is used when available; otherwise `g++` must be available on `PATH`.

The CLI reads the USB log in chunks of at most 64 KiB and writes validated
records to a temporary file. It retains at most one incomplete 31-byte frame
and one bounded record between chunks; long ordinary log lines are discarded
without accumulating them. Optional decoded output is also copied in bounded
chunks after successful replay. Host memory therefore does not grow with
session duration, apart from fixed compiler/runtime resources. Sufficient
temporary disk space for the decoded session remains necessary. The
`decode_frames(bytes)` convenience function used by small tests intentionally
returns an in-memory result and is not used by the CLI.

Replay calls the production `MotionRule` and `FallResponse` methods. Recorded
expected outputs are compared after those calls. They do not become classifier
inputs. A checkpoint preserves an in-progress candidate, post-fall movement
window, original deadline and partial button gesture without fabricating a
warmup period. Response channel data is deliberately omitted; replay uses
placeholder channels and does not claim to verify routing or delivery.

One complete session is accepted per input. Unsupported versions, malformed
records, nonfinite measurements, unknown calibration, incompatible settings,
source drift, packet/record/sample sequence gaps, a missing `Begin` or `End`,
extra sessions and reported export loss are rejected. Lossy recordings remain
diagnostic evidence but cannot establish complete deterministic replay. Missing
samples are never replaced with quiet, interpolated or joined across an explicit
acquisition discontinuity.

## Source and configuration identity

The build pre-script `tools/health_identity.py` hashes exact working-tree file
bytes, relative paths and lengths. Tracked modifications and relevant untracked
source files are included; a commit ID is not used as a substitute. The scope is
maintained `src`, `include`, companion sources, Heltec V4 source/pin variants,
local libraries, the selected board JSON, project configuration, merge script
and identity script. The generated header and per-file hash manifest are stored
under `.pio/build/<environment>/health_capture/`.

Only hashes, paths and lengths appear in the manifest. Source/configuration
contents and secret values do not appear there. The recorded 64-character
SHA-256 identifies this maintained-input scope; it is not an application-binary
hash or a hash of downloaded compiler/framework/library packages. Those package
versions, effective build options and the built image hash remain separate
verification evidence. Unmanaged overrides or modified external dependencies
must be documented independently.

A separate runtime fingerprint covers effective detector settings and response
timing/movement constants. The checkpoint also contains explicit settings and
button timing values, which replay checks. Old recordings require the matching
retained source/configuration snapshot. Source mismatch is rejected rather than
silently replaying a newer algorithm against old expected results.

## Version 1 wire format

Integers use little-endian encoding. Booleans are exactly `0` or `1`; enums occupy
one byte. Measurements use IEEE-754 binary32 bits, preserving all recorded float
precision rather than rounded display values. No compiler object padding or
pointers are serialized.

Each transport frame is 31 bytes:

```text
RS | S-or-M | packet sequence:4 hex | data:20 hex | CRC16:4 hex | LF
```

`S` starts a binary record and `M` continues it. Ten data bytes are carried per
frame. The final fragment is zero-padded. Packet sequence is unsigned 16-bit and
wraps; it continues across sessions during the current boot. CRC-16/CCITT uses
initial value `0xffff` over the 25 ASCII bytes after `RS`, excluding CRC and LF.
Complete frames allow ordinary text between them. Short writes are not retried
as partial frames; they are counted as export loss.

Each decoded record begins with `kind:u8`, `payload_length:u16` and
`record_sequence:u32`. Sequence starts at zero for `Begin` and advances for every
issued record, including records dropped before export. The payload is:

| Kind | Payload, in order |
| --- | --- |
| 1 `Begin` | Version:u16; source SHA-256:64 ASCII bytes; configuration fingerprint:u32; boot token:u64; session:u32; wall time:u32; current sample sequence:u32; acquisition generation:u32; discarded-sample lower bound:u32; checkpoint length:u16; explicit checkpoint fields. |
| 2 `Sample` | Sample sequence:u32; estimated capture time:u32; processing parameter time:u32; raw[7]:f32; corrected[7]:f32; quality:u16; calibration fingerprint:u32; maintenance:bool; expected outputs. |
| 3 `Invalidate` | Detector parameter time:u32; processing time:u32; mask:u8 (`1` detector, `2` response motion, `3` both); expected outputs. |
| 4 `Fall` | Response parameter time:u32; button pressed:bool; expected outputs after `onFall`. |
| 5 `Service` | Response parameter time:u32; button pressed:bool; expected outputs after `updateButton`, then `updateDeadline`. |
| 6 `Accept` | Parameter time:u32; expected outputs after outbox acceptance clears the pending response. |
| 7 `Restore` | Parameter time:u32; elapsed incident time:u32; incident ID:u32; pressed:bool; expected outputs. |
| 8 `Seed` | Parameter time:u32; incident ID:u32; expected outputs. |
| 9 `Calibration` | Observation time:u32; correction fingerprint:u32; calibration state:u8; active:bool; bias[3]:f32; noise[3]:f32; temperature:f32; temperature span:f32; calibration sample count:u32. |
| 10 `Buzzer` | Observation time:u32; commanded sounding:bool. |
| 11 `Acquisition` | Observation time:u32; generation:u32; discarded-sample lower bound:u32; FIFO overflows:u32; timing discontinuities:u32. |
| 12 `End` | Stop time:u32; final sample sequence:u32; generation:u32; discarded lower bound:u32; recorder dropped records:u32; short writes:u32; total issued records including `End`:u32. |

Expected outputs occupy 18 bytes: detector state:u8, reason:u8, event kind:u8,
operation result:bool, alarm active:bool, pending notice:u8, incident:u32,
pending incident:u32 and omitted confirmation count:u32. `result` is the return
value of the relevant production method, or false for a void operation.
Metadata records do not mutate replayed detector/response state.

`MotionCaptureFormat.h` contains the authoritative explicit checkpoint field
lists. The checkpoint capacity is 2048 bytes. Adding or changing checkpoint
fields requires a format-version change. Records 7 and 8 support the production
response API; ordinary USB capture begins after startup recovery and describes
that recovered state in its checkpoint.

## Units, timing and loss

Both measurement arrays use `ax, ay, az, gx, gy, gz, temperature`. Acceleration
is in g and includes gravity; angular velocity is in degrees/s; temperature is
in degrees Celsius. Axes follow the device driver register axes. No conversion
to anatomical/wearer axes is implied. Current correction subtracts saved gyro
bias; accelerometer and temperature values are unchanged. Calibration identity
is the fingerprint of its explicitly serialized correction metadata.

All timestamps are unsigned 32-bit milliseconds and may wrap. Capture times
are FIFO-derived estimates, not sensor hardware timestamps. Processing times
are the actual time parameters used by orchestration, usually the saved poll
entry or bus-completion observation. They are not a measure of completion of
every operation in that poll. Metadata timestamps are observed when emitted and
can therefore be later than a following operation's saved parameter time.
Record sequence defines replay order; sorting by timestamp would be incorrect.
Replay checks unambiguous forward operation timing and metadata against the
latest operation clock. A gap of half the timer range or more is ambiguous.

Sample quality bits preserve the acquisition definitions: estimated time,
accelerometer saturation, gyro saturation and bounded timestamp adjustment.
`0x200` marks calibration maintenance; `0x400` marks current buzzer-state timing
uncertainty for buffered samples. Commanded buzzer observations indicate software
intent, not measured acoustic or mechanical output.

Acquisition loss concerns measurements unavailable to the detector: FIFO
overflow, reset, failed transfer, timing discontinuity or stale coverage.
Discarded-sample counts are lower bounds where the number lost is unknown.
An explicit invalidation prevents evidence across such interruptions. Recording
loss concerns observations not exported even though firmware may have processed
them. Packet, record and sample sequences, queue statistics and `End` distinguish
these cases. Buffered replay cannot establish past wall-time availability.

## Availability and service measurements

`health detail` reports an exclusive wall-time breakdown beginning at the first
motion poll. Each interval uses the last observed cause, with precedence:
configuration fault, calibration, startup until the first accepted sample or
latched prolonged sensor fault, sensor unavailable, stale sample, recovery,
refractory, then available
(armed or candidate). All causes sum to `monitoring_ms`; unavailable time is
that sum minus available time. Read-only queries do not accumulate time again.
The existing 5-second motion outage fault ends startup even if no sample has
ever arrived. Subsequent time is sensor unavailable until fresh data begins
detector recovery; successful bus initialization alone does not clear the fault.

Available, recovery and refractory intervals become stale when the previously
observed sample reaches its freshness horizon. Higher-priority causes remain
assigned until a subsequent observation changes them. A batch drained after a
stall cannot rewrite the preceding stale wall time. Cause changes inside a batch
are observed at service boundaries rather than independently timestamped; totals
must not be interpreted as interrupt-level precision or physical uptime.

`health timing` reports motion-poll count, accumulated/max poll duration, maximum
entry-to-entry service gap, accumulated/max capture-to-processing delay, and
accumulated/max synchronous persistence duration. Arithmetic uses unsigned
elapsed milliseconds and 64-bit totals. The measurement resolution is one
millisecond; sub-millisecond overhead is not resolved. These service counters
and cause totals reset on reboot, independently of resettable sample statistics.

## Verification boundary

Native codec tests cover production replay, checkpoints, slow events, later
stillness, rejection, calibration, discontinuity, rollover, partial gestures,
secret exclusion and malformed/lost input. `tools/test_health_replay.py` tests
mixed-output framing, CRC, sequence loss, source identity and repeated complete
capture-to-production replay. Actual `HealthMonitor.cpp`/`HealthRecording.cpp`
integration fixtures provide a separate orchestration check; they still use
host clocks, sensor/storage/transport boundaries and simulated USB capacity.

Host execution uses recorded binary32 inputs with the host compiler and math
library. A near-threshold floating-point difference across architectures is a
replay divergence requiring investigation, not an automatically accepted match.
Passing tests do not establish physical sensor timing, RTOS scheduling, USB
throughput, BLE/RF load, buzzer coupling, flash durability, wearer sensitivity,
false alerts per wear-hour or recipient delivery. Bench recordings with the
actual enclosure and simultaneous services are the next validation step.

Related references: [fall detection](fall_detection.md),
[USB commands](usb_cli_reference.md), [build verification](build-validation.md)
and [fault-test scope](virtual-fault-testing.md).
