# Version 1.1.0 - Beta

SeniorHealthNode combines MeshCore BLE companion services and fall monitoring in
one `heltec_v4_companion_radio_ble` image. This candidate adds buffered motion
acquisition, experimental orientation-based detection, bounded incident recovery,
and capture/replay diagnostics. The upstream MeshCore base remains 1.17.0.


## Detection and acquisition

- MPU6050 acquisition uses a 50 Hz hardware FIFO with 14-byte records. Bounded
  two-record reads and at most four samples per poll retain chronological sample
  processing across short loop delays. Configuration remains Ãƒâ€šÃ‚Â±8 g, Ãƒâ€šÃ‚Â±500 degrees/s
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
[detector reference](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/cbe2b9d8057533d591bd4326c6a149c6447e9515/docs/fall_detection.md). This remains possible-fall detection,
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
See [alarm operation](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/cbe2b9d8057533d591bd4326c6a149c6447e9515/docs/operation.md), [delivery behavior](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/cbe2b9d8057533d591bd4326c6a149c6447e9515/docs/message_delivery_changes.md)
and [incident storage limits](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/cbe2b9d8057533d591bd4326c6a149c6447e9515/docs/incident_storage.md).

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

See the [USB reference](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/cbe2b9d8057533d591bd4326c6a149c6447e9515/docs/usb_cli_reference.md), [recording and replay guide](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/cbe2b9d8057533d591bd4326c6a149c6447e9515/docs/motion_recording.md)
and [capture bundle guide](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/cbe2b9d8057533d591bd4326c6a149c6447e9515/docs/capture_bundles.md). Generated fixtures must be
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

Checked incident I/O, companion service integration, capture/provenance tooling,
associated tests, the version/splash update and documentation are committed in
the source revision identified above. This preparation rebuilds that firmware
and updates release assets and evidence; it introduces no firmware behavior change.

## Release assets

The planned GitHub upload inventory contains exactly these three files:

| Filename | Purpose and use |
| --- | --- |
| `SHN_v1.1.0-beta_firmware-merged.bin` | Installation download: combined ESP32-S3 firmware for `heltec_v4_companion_radio_ble`; flash at **0x0**. |
| `VERIFICATION.zip` | Build/source manifest, component hashes, image-layout checks, build/test logs and verification report. Extract to inspect the evidence. Not firmware. |
| `SHA256SUMS.txt` | SHA-256 checksums for the merged firmware and verification archive. Verify both downloaded files before use. |

These assets are prepared locally, not yet published. The existing release notes
supply the GitHub release-page body; no separate notes attachment is required.
Mark `v1.1.0-beta` as a prerelease when publishing through a separately authorized
release operation. Application-only binaries, ELF files and dependency caches
are not upload assets.

Follow the [build and installation guide](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/cbe2b9d8057533d591bd4326c6a149c6447e9515/docs/build-validation.md#uploading-the-combined-firmware)
for the combined image. The release download is named
`SHN_v1.1.0-beta_firmware-merged.bin`; its flash offset is **0x0**.
Do not flash this merged image at the application-only offset `0x10000`.

## Upgrade and compatibility

1. Retain the source/configuration snapshot and record existing identity, radio,
   channel and calibration settings using an appropriate secure backup process.
2. Review buzzer idle polarity and the enabled experimental detector paths.
   Rebuild only `heltec_v4_companion_radio_ble` following the
   [build and upload guide](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/cbe2b9d8057533d591bd4326c6a149c6447e9515/docs/build-validation.md).
3. Use PlatformIO upload, or flash `firmware-merged.bin` at `0x0` with an
   appropriate flash tool. The application-only `firmware.bin` belongs at
   `0x10000` with matching bootloader and partition files. Do not erase flash as
   a routine upgrade step: erasure removes identity and saved settings.
4. Check the device version, BLE pairing/client behavior, radio/channel settings,
   `health detail`, `health timing`, fresh motion, PRG gestures, buzzer tone and
   silence, and independently observed reception. Follow the
   [post-flash procedure](https://github.com/Usrnmna/SeniorHealthNode-MC/blob/cbe2b9d8057533d591bd4326c6a149c6447e9515/docs/usb_cli_reference.md#post-flash-usb-cli-verification).