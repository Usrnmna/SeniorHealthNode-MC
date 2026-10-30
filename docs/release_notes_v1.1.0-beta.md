# Version 1.1.0 - Beta

**Release identifier: `v1.1.0-beta`. Status: local, unpublished candidate.**

SeniorHealthNode combines MeshCore BLE companion services and fall monitoring in
one `heltec_v4_companion_radio_ble` image. This candidate adds buffered motion
acquisition, experimental orientation-based detection, bounded incident recovery,
and capture/replay diagnostics. The upstream MeshCore base remains 1.17.0.

## Comparison scope

The remote `v1.0.0-beta` tag resolves to
[`b11597813f8d92c1c4476f0397ace64dbaf37bed`](https://github.com/Usrnmna/SeniorHealthNode-MC/commit/b11597813f8d92c1c4476f0397ace64dbaf37bed),
verified against `origin` on 2026-10-29. These notes compare that source with
committed changes through `f33dd01b2ec8791cd10ec64f41112e1f51a28ac6` and the current
uncommitted candidate changes. No v1.1.0-beta tag, commit or release is created by
this preparation.

The [published v1.0.0-beta description](https://github.com/Usrnmna/SeniorHealthNode-MC/releases/tag/v1.0.0-beta)
already describes some USB configuration and buzzer changes that appear after
the tagged source in Git history. Its uploaded binary has not been reconstructed
or compared here. The source tag is the comparison baseline; features mentioned
in that description are not assumed to be new to every installed v1.0.0-beta
binary. The [earlier local notes](release_notes_v1.0.0-beta.md) remain historical.

## Detection and acquisition

- MPU6050 acquisition uses a 50 Hz hardware FIFO with 14-byte records. Bounded
  two-record reads and at most four samples per poll retain chronological sample
  processing across short loop delays. Configuration remains ±8 g, ±500 degrees/s
  and `DLPF_CFG=3`; wiring and the disconnected INT pin remain unchanged.
- FIFO overflow, excessive backlog, partial reads, saturation and timing
  discontinuities are exposed rather than hidden by a replacement latest sample.
  Timestamp estimates include count-read timing and report phase corrections;
  these are not hardware interrupt timestamps.
- The strong low-g/impact/rotation sequence can declare a possible fall without
  waiting for quiet. Rotation up to 300 ms before low-g can contribute.
- Experimental impact-orientation, supported-descent and slow-descent paths use
  bounded pre-event orientation history and subsequent quiet. Context paths are
  enabled by default, but sensitivity and false-positive rates remain unmeasured.
- Fresh-data recovery requires 200 ms and at least six contiguous samples.
  The former 60-second classifier cooldown is replaced by a 1200 ms refractory
  period. Missing/nonfinite data and gaps above 100 ms break evidence; expired
  history, renewed low-g episodes and timer rollover have explicit handling.
- Sensor health and detector availability are reported separately. Decision
  traces, event summaries, coverage and timing counters expose unavailable
  intervals and rejected candidates.

Thresholds and exact evidence windows are maintained in the
[detector reference](fall_detection.md). This remains possible-fall detection,
not a diagnosis or validated emergency service.

## Incident handling and delivery

- A 30-second unanswered alarm retains its original assessment deadline. If
  fresh quiet is established later, one subsequent no-movement update can be
  queued without retrospectively strengthening the first warning.
- Repeated detections during an unresolved alarm preserve its deadline, PRG
  progress and delivery ownership. Initial notifications coalesce within bounded
  storage; pending explicit responses retain priority across later incidents.
  Omitted responses under saturation are counted. Multipart failures and message
  ownership are exposed in diagnostics.
- Two checked, generation-ordered incident records retain minimal unresolved
  state and the incident's original channel. Resolution writes a tombstone.
  Recovery never restores old samples, partial gestures or transport retries.
- With unknown power-off duration, recovery permits at most three successfully
  persisted recoveries and performs an immediate unconfirmed-condition
  assessment. The tested qualified-clock age-expiry path is not enabled on this
  target. Failed storage or resets before persistence can defeat budget progress.
- Candidate storage adapters check open/read/stat/close and write/flush/close
  outcomes, verify readback, and report ambiguous outcomes without treating an
  unreadable slot as absent. Failed transitions remain pending for later retry;
  newer transitions supersede stale unsaved ones. The existing `FIN1` encoding
  remains unchanged by these adapter refinements.

The existing double/triple PRG response gestures remain available. Radio queueing,
local transmission, repeater echoes and recipient reception remain distinct.
See [alarm operation](operation.md), [delivery behavior](message_delivery_changes.md)
and [incident storage limits](incident_storage.md).

## USB configuration, diagnostics and recording

Changes relative to the tagged source include the expanded local `get`/`set`
interface, discoverable help, strict input validation, staged preference writes
and hardware rollback reporting. SX1262 receive boost and external FEM receive
gain have separate settings and persistence. Preference copies retain their own
serializer ownership. Some of these features were already described in the
published v1.0.0-beta text.

- `health detail`, `health timing`, `health errors`, `health radio`, motion
  statistics, `motion trace` and `motion events` provide bounded snapshots.
  Storage results and synchronous persistence time are visible alongside motion
  acquisition and delivery state.
- `motion record start|status|stop` controls an optional versioned USB recorder.
  It starts disabled, snapshots detector/response state without resetting it,
  and records acquisition loss separately from export loss.
- An 8192-byte recording ring, bounded reply chunks and a USB worker queue reduce
  CDC backpressure interference. Worker readiness and write failures are visible;
  successful API writes do not prove host receipt. BLE debug output uses the
  bounded path and omits pairing PIN values.
- Strict host replay validates framing, CRCs, sequence continuity, checkpoint
  state and exact maintained-source identity before exercising production
  detector/response code. Replay does not establish physical accuracy.
- The candidate `health_capture.py` tool preserves mixed input bytes and explicit
  connection boundaries, extracts sessions, associates source/dependency/image
  hashes, and verifies evidence inventories. Damaged intervals remain failures
  even when a later session replays. It does not open serial ports, install
  firmware or authenticate evidence.
- Shared companion loop, command-service and storage-startup boundaries allow
  host tests to exercise production service ordering and parser behavior. This
  is still one firmware image with BLE and health services on the same node.

See the [USB reference](usb_cli_reference.md), [recording and replay guide](motion_recording.md)
and [capture bundle guide](capture_bundles.md). Generated fixtures must be
regenerated after source/configuration changes, including the version update.

## Buzzer and firmware identification

The passive-buzzer output now disconnects PWM during silence and explicitly holds
the configured inactive level. Tone duty is applied after attachment; pitch setup
failures leave output inactive. Repeated idle polls avoid GPIO/PWM writes.
The compiled DIYables configuration changes from HIGH idle to **LOW idle**
(`buzzer_active_low = false`); module-specific electrical behavior still requires
bench verification. Optional buzzer disablement, SOS priority and battery
reminder behavior remain supported.

The device-info response and OLED splash now identify SeniorHealthNode as
**v1.1.0-beta**, instead of the upstream v1.17.0 label. The splash preserves the
beta suffix. This is a fork release number, not a downgrade of the MeshCore base.
Clients that interpret firmware version numbers require interoperability checks.

## Documentation, build and test tooling

The README now emphasizes setup, wearer responses and limitations, with linked
architecture, operation, detector, hardware, USB and validation guides. Concept
illustrations are labelled as schematic. Documentation covers calibrated sensor
readings, FIFO timing, optional sound, bounded storage recovery, capture evidence
and a reusable bench evidence template.

PlatformIO generates a maintained-source identity for firmware/capture matching.
Host coverage expands across acquisition faults, detector paths, incident storage,
actual companion service integration, bounded USB output and production message
formatting. Capture/replay adds Python checks and generated integration fixtures.
There are no tracked GitHub Actions workflows, issue/PR templates or automated
release configuration in this checkout. Contributor and security guidance reflect
the current operations; no hosted CI run is implied.

The pre-existing uncommitted candidate work includes checked incident I/O,
companion service integration, capture collection/provenance tooling, associated
tests and guides. This release preparation adds the version/splash update and
documentation corrections. All remain local candidate changes until separately
committed and released.

## Upgrade and compatibility

1. Retain the source/configuration snapshot and record existing identity, radio,
   channel and calibration settings using an appropriate secure backup process.
2. Review buzzer idle polarity and the enabled experimental detector paths.
   Rebuild only `heltec_v4_companion_radio_ble` following the
   [build and upload guide](build-validation.md).
3. Use PlatformIO upload, or flash `firmware-merged.bin` at `0x0` with an
   appropriate flash tool. The application-only `firmware.bin` belongs at
   `0x10000` with matching bootloader and partition files. Do not erase flash as
   a routine upgrade step: erasure removes identity and saved settings.
4. Check the device version, BLE pairing/client behavior, radio/channel settings,
   `health detail`, `health timing`, fresh motion, PRG gestures, buzzer tone and
   silence, and independently observed reception. Follow the
   [post-flash procedure](usb_cli_reference.md#post-flash-usb-cli-verification).

No target split, pin reassignment or partition migration is introduced here.
Behavioral changes include quicker rearming, experimental context detection,
late quiet updates and possible alarm restoration after reboot. Existing
`SPIFFS.begin(true)` startup behavior can format storage after certain mount
failures; this release does not introduce a preservation guarantee or change
that policy. Firmware update and power-loss recovery require bench checks.

## Validation and remaining limits

Local verification on 2026-10-29:

| Check | Result |
| --- | --- |
| `heltec_v4_companion_radio_ble` build and `mergebin` | Passed; application uses 1,330,133 bytes of flash and 213,608 bytes of RAM according to PlatformIO. |
| `native` and `native_delivery` | 771 test cases passed across 26 suites. |
| Python capture/replay checks | 62 tests passed. |
| Fresh actual-companion fixture replay | 1,007 records / 944 samples; three identical runs. |
| Documentation | 510 local links/anchors across 34 Markdown files passed. |
| Configuration and diff | PlatformIO INI and board JSON parsed; `git diff --check` passed; built application contains `v1.1.0-beta`. |

Initial sandboxed host/Python runs encountered Windows access-denied errors;
the passing runs used the same checkout outside that sandbox. No hosted CI was
run. Local build/test logs are under `.pio/v1.1.0-beta-*.log`.

Merged-image SHA-256:
`324cc6f6be2fa261adeec176e6d246350b76a6000d392cf5180c5d3350e48c57`.
Maintained-source identity:
`88e2d1103dadfe35d2deece927a220795b4cee4a8dfb9e8f1d2f894887e667a7`.
These identify this local candidate only; rebuilds or further edits require fresh
evidence. The [validation guide](build-validation.md) defines commands and scope.

No physical device was flashed or tested for this release preparation. Host tests
and successful image compilation do not establish I2C timing, BLE responsiveness,
RF delivery, GPIO/PWM behavior, battery accuracy, storage power-loss durability
or fall-detection accuracy. Automatic filesystem formatting, clock-uncertain
recovery, bounded message capacity and sensitive mixed logs remain significant
operational limitations. A repeater echo does not prove human receipt.
