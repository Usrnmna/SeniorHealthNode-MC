# Capture collection and evidence bundles

`tools/health_capture.py` is an optional host instrument for the single
`heltec_v4_companion_radio_ble` firmware. It preserves mixed USB bytes, extracts
version 1 sessions and associates source/build provenance. It does not open or
discover serial ports, change firmware, or replace strict single-session replay.
See [firmware capture controls and format](motion_recording.md).

## Preserve inputs and connection boundaries

Use a new destination for every operation. Existing evidence directories are
rejected, including partially completed ones. From the repository root:

```powershell
.\.venv\Scripts\python.exe tools\health_capture.py collect C:\captures\run-001 --input C:\captures\connection-1.bin --input C:\captures\connection-2.bin
```

Destination directory creation is exclusive before resolving path aliases.
Existing dangling symbolic links are rejected as occupied destinations.

Each explicit input represents one connection. `--input -` accepts an already
open binary stdin stream; byte-preserving pipe behavior must be established by
the caller. PowerShell text pipelines or terminal newline conversion must not
be assumed to preserve binary bytes. Collection code is also callable with
fake/already-open binary readers; it does not enumerate or open COM devices.

`raw.log` preserves all received bytes, including ordinary text and damaged
frames. `connections.jsonl` records open/close/error offsets and host UTC and
monotonic observations; `collection.json` hashes the raw file and connection
log. Failed reads retain their prefix and return a failed collection outcome.
These clocks describe host reads, not sample acquisition. A process interruption
or host disk failure can leave a partial collection without final metadata;
retain it as incomplete evidence. No host disk power-loss guarantee is made.

## Extract and replay a bundle

Retain the matching source/configuration snapshot in an appropriate local archive
before creating a bundle. A reference is required, but the tool does not assert
that the referenced archive exists or copy its potentially secret configuration.

```powershell
.\.venv\Scripts\python.exe tools\health_capture.py bundle C:\captures\run-001\raw.log C:\captures\bundle-001 --collection C:\captures\run-001 --snapshot-reference "restricted-archive/run-001" --replay --repeat 3 --image application=.pio\build\heltec_v4_companion_radio_ble\firmware.bin --image merged=.pio\build\heltec_v4_companion_radio_ble\firmware-merged.bin
.\.venv\Scripts\python.exe tools\health_capture.py verify C:\captures\bundle-001
```

For an existing raw file without collector metadata, omit `--collection`.
Connection history is then unknown; the tool must not infer reconnects from
console text. Add explicit `--dependency NAME=DIRECTORY` inputs for retained
framework, compiler/toolchain and library packages. Their relative paths, byte
counts and hashes are recorded without copying contents. Selected image roles
are `application`, `merged`, `bootloader` and `partitions`; image files are hashed,
not installed. The target is fixed to the existing combined firmware.

`--metadata FILE.json` accepts only `device_label`, `mount_label`,
`host_clock_uncertainty_ms` and a bounded `build` object. Build fields are
`target`, `framework`, `board`, `flash_mode`, `optimization`, `cxx_standard`,
`cpu_frequency_hz`, `flash_frequency_hz` and `usb_cdc_on_boot`, with explicit
value allowlists. These are operator assertions about the effective build, not
values inferred from an ELF or confirmation of all flags. Arbitrary defines,
environment dumps, configuration contents, credentials and channel keys are not
automatically exported. Labels and snapshot references must themselves avoid
secrets. Original mixed logs may contain sensitive ordinary output and need
appropriate handling before distribution; preserving the original is intentional.

## Evidence interpretation

| Artifact | Purpose and limits |
| --- | --- |
| `raw.log`, collection sidecars | Exact copied mixed input plus validated connection offsets when available. Original input hashes are checked again after copying. |
| `segments.jsonl`, `issues.jsonl` | Half-open raw byte intervals, identities, ordinary text, incomplete/rejected segments, corruption and reconnect cuts. |
| `sessions/*.log` | Transport-complete sessions only, bounded by CRC-valid records with valid Begin identity, sequence continuity and matching End accounting. |
| `replay.jsonl`, `decoded/*.bin` | Separate outcomes from the existing strict decoder and production C++ replayer when requested. A transport-complete session can still fail semantic replay. |
| `source-manifest.json`, `provenance.json` | Maintained-input hashes, retained-source reference, host decoder/replay/tool identities, explicit dependency/image fingerprints, calibration/units references and source/tool drift checks. |
| `artifacts.jsonl`, `bundle.json` | Hash inventory and overall extraction/association outcome. Verification checks unsigned local consistency, not authenticity or hardware completeness. |

Extraction resynchronizes only at a validated Begin record. A later complete
session cannot repair a damaged earlier interval. Reconnects terminate current
frames/records/sessions. Duplicate boot/session identities, reported export loss,
corrupt CRCs and sequence gaps remain explicit failures. A damaged bundle is
retained and returns nonzero even if a later session replays successfully.
`verify` can confirm that a damaged bundle remains unchanged; its
`original_bundle_ok` remains false. Local hash consistency and deterministic
replay do not verify the binary actually installed on a node; that status stays
`unknown` in these artifacts.

Known same-boot session and packet discontinuities remain failures across
explicit reconnect boundaries. Reconnection does not erase previously observed
continuity evidence; a new boot has a separate identity.

Verification checks the exact artifact inventory, required files, source
association, interval coverage, session wire hashes against the original raw
intervals, and retained issue/segment totals. A changed session file cannot be
accepted merely by updating the outer artifact hash. Malformed metadata returns
a failed operation. These checks establish unsigned internal consistency, not
protection against coordinated replacement of the entire evidence set.

Raw parsing, record assembly, copying and metadata rows use bounded chunks.
Session and issue indexes stream to disk; storage grows with evidence duration
and the number of sessions. Bundling copies raw data and complete wire sessions,
and replay adds decoded files. Fingerprinting explicit dependencies may require
substantial disk reads. Compiler and temporary disk resources remain necessary.
There is no untethered field recorder or automatic retention/deletion policy.
Individual metadata rows and ordinary metadata files are limited to 64 KiB;
the maintained-source catalog has a separate 8 MiB verification limit.
Consistency checks add bounded host file reads and hashing. Before/after input
fingerprints detect observed drift but do not lock files against later changes.

## Software checks and bench follow-up

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tools -p "test_health*.py" -v
```

Tests use fake byte readers and generated production fixtures, including read
errors, reconnects inside frames/records, earlier damage followed by valid
sessions, sequence rollover, metadata rejection, unchanged raw bytes, existing
output collisions and repeated production replay. They do not access hardware.
Additional regressions cover reconnect continuity, altered wire/raw associations,
incomplete inventories, conflicting source references, malformed rows and
dangling destination aliases.
Use the [bench evidence template](fall_bench_template.md) to connect captures to
availability/timing snapshots, incident handling and independently observed
recipient delivery. USB throughput, physical timestamps, installed-image identity,
power-loss durability and real-world detection accuracy remain separate evidence.
