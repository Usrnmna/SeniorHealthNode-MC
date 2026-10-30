# Incident storage results and restart limits

The `heltec_v4_companion_radio_ble` application retains incident metadata on its
existing SPIFFS filesystem. BLE, detection, local alarms and delivery remain in
the same application. This boundary does not alter filesystem provisioning,
restart eligibility, deadlines, acknowledgements or notification ownership.

## Record and write contract

[`FallIncidentStore.h`](../examples/companion_radio/health/FallIncidentStore.h)
owns the existing two 100-byte `FIN1` records, `/fall_inc0` and `/fall_inc1`, their
checksum, wrapping generation order, selection of the older slot, verification
and repair of a damaged second slot. A resolution is a tombstone. Repair follows
successful verification of the first write. The byte encoding is unchanged.
Private destination information can be present in these records; incident files
are not part of the capture bundle allowlist.

[`IncidentWriteIO.h`](../examples/companion_radio/health/IncidentWriteIO.h) owns
the ESP32 write handle on the existing `/spiffs` VFS mount. It checks `fopen`, the
full `fwrite` byte count, `fflush` and `fclose`. Every successfully opened handle
has one close attempt, including after a failed write or flush. No operation is
retried internally. The storage policy then verifies the record through the
checked reader described below. The non-ESP32 compatibility writer retains Arduino
file operations and explicitly reports that its API results are not checked.

The pinned Arduino package is `3.20017.241212+sha.dcc1105b` (Arduino 2.0.17,
ESP-IDF 4.4.7). Its `VFSFileImpl::flush` discards `fflush` and `fsync` results and
its `close` discards `fclose` results. The matching
[SPIFFS VFS implementation](https://github.com/espressif/esp-idf/blob/v4.4.7/components/spiffs/esp_spiffs.c)
registers no `fsync` callback. The new writer therefore does not claim or invoke
an independent `fsync` barrier. `fflush` checks transfer of the C stream buffer;
`fclose` reaches `SPIFFS_close`, whose cache-flush failure propagates through the
VFS. This call path was also inspected in the installed `libspiffs.a`.

`health detail` retains the last attempted result until another attempt or reboot:

| Result | Meaning |
| --- | --- |
| `not-attempted` | No persistence attempt has completed since startup. A separate recovery fault can still exist. |
| `invalid-incident` | Input failed the existing incident/channel validation; no write started. |
| `read-existing` | A slot could not be inspected with certainty; no write started. |
| `open`, `write`, `stdio-flush`, `close` | First failed checked write operation. `errno` is captured immediately; an unexplained short write uses `EIO`. |
| `verify` | Checked readback failed or the reopened record did not match the intended record. `save_read` reports the read result separately. |
| `verified-readback` | All selected write API calls and record comparison succeeded. `checked_api=1` distinguishes the ESP32 checked writer from the compatibility path. |

`slot` identifies the attempted destination. `close_errno` retains a cleanup-close
failure even when an earlier operation already failed. An error may occur after
some or all data committed; it does not mean that the previous state is still
durable. A successful result likewise does not prove survival of a power cut.
Checksums detect damaged bytes; they do not provide atomic filesystem updates.

## Checked reads and qualified absence

[`IncidentReadIO.h`](../examples/companion_radio/health/IncidentReadIO.h) uses
read-only native `open`, `fstat`, one bounded `read` and `close` on ESP32.
Startup recovery, pre-save slot inspection and post-write verification use this
same reader and the shared record decoder. Each opened descriptor is closed
exactly once, including after stat/read failure; a failed close is not retried.
The first I/O error and any cleanup-close error remain separate. Unexplained
failures and short reads use `EIO` when no native error is available.

Only `ENOENT` observed between mounted checks qualifies as current API absence.
The pinned SPIFFS backend also maps some deleted/not-a-file conditions to this
error. Absence therefore does not prove that no historical record existed.
An unmounted filesystem, failed open/stat/read/close or mount loss after reading
is uncertain. A wrong length or invalid checksum/metadata is a corrupt record
only after successful inspection and close. No read exceeds 100 bytes or retries
internally; wrong-size files are not read. Filesystem latency remains unbounded.

An unknown peer slot cannot be bypassed to restore an older active incident,
because the peer might hold a resolution. Uncertain pre-save inspection blocks
all writes. Corrupt but readable records retain the existing repair policy.
The Arduino compatibility reader remains explicitly unchecked; its `exists`
and void close operations cannot provide the ESP32 guarantee.

`health detail` adds these bounded observations:

| Field | Meaning |
| --- | --- |
| `storage_boot` | `not-attempted`, `ok` or `failed` for the startup mount call. This historical result does not change when later saves succeed. |
| `mounted` | Current mount observation: `yes`, `no` or `unknown` for an adapter without that observation. Mounting alone does not establish readable records. |
| `format_history` | `unknown`; the Boolean mount result does not reveal whether formatting occurred. |
| `boot_read0`, `boot_read1` | Historical per-slot recovery read stage, first errno, cleanup-close error and checked-reader flag. |
| `save_read` | Latest save attempt's last inspection or verification result, with slot and error details. A write failure can leave the prior inspection as this observation. |

Read stages are `not-attempted`, `mount`, `open`, `stat`, `read`, `close`,
`missing`, `invalid-record` and `valid-record`. `recovery` remains the historical
boot decision. `persistence_fault` can clear on a later successful save without
rewriting boot observations. `checked_api` on the write result and `checked` on
read results describe different operations. No record contents or channel keys
are printed. Matching readback is not an independent uncached media read or a
power-loss durability guarantee.

These fields are formatted when the command is handled. A queued reply retains
that observation while USB output is delayed; a later command observes current
mount state again. Reading diagnostics does not open incident files or retry boot
recovery. Repeated commands can still exceed the bounded pending-reply capacity.

## Local behavior and resource bounds

Failed saves leave `persistence_dirty` set and `persistence_fault` visible.
The existing unsigned five-second retry interval is unchanged. Acknowledgement
and false-alarm dismissal still stop the local alarm even if saving the
resolution fails. A later transition supersedes an unsaved earlier transition.
Retrying a saved recovery count does not increment it again. An unresolved alarm
continues to use its original response deadline and delivery state.

Only incident transitions and restart recovery write flash. The implementation
retains fixed-size read/write results and startup flags, no queue, worker, per-sample write,
timer or additional retry budget. The serialization and two-slot readback remain
bounded in count. Each write owns one C stream and its libc buffering; memory
and runtime costs of that backend remain target measurements. A small record
does not bound filesystem latency. `health timing` continues to report total and
maximum synchronous persistence time with one-millisecond resolution.

## Existing startup behavior: policy unchanged

`main.cpp` calls the shared
[`startCompanionStorage`](../examples/companion_radio/CompanionStorageStartup.h)
boundary: `SPIFFS.begin(true)`, observation of its Boolean result, `store.begin()`
and companion startup, in that order. Failure still does not stop startup or
change formatting policy. The installed implementation initially
registers with `format_if_mount_failed=false`, then conditionally formats:

| Source-established startup condition | Existing action and consequence |
| --- | --- |
| Already mounted, or registration succeeds | Continue without formatting. |
| Blank partition produces `ESP_FAIL` on registration | Attempt format; retry registration only if format succeeds. First-use creation and recovery use the same branch. |
| Previously provisioned/corrupt filesystem produces `ESP_FAIL` | The same format branch can erase identity, preferences, channels, calibration and incident records. There is no separate preservation/provisioning decision. |
| Format fails, or registration after format fails | `begin` returns false; the caller still continues. |
| Missing partition (`ESP_ERR_NOT_FOUND`), allocation or other registration error | No automatic format in this branch; `begin` returns false and startup still continues. |

These are source-derived branches, not measurements of board failures or a
complete simulation of SPIFFS. Blank/corrupt media need not always produce the
same error. Changing automatic formatting, first-use provisioning, mount failure
behavior, filesystem type or partitions requires a separate storage-policy decision.

## Validation and remaining decisions

`test_incident_recovery` retains the earlier volatile/durable interruption model
and adds tests of the actual checked writer and shared record policy. Coverage
includes short writes, open/flush/close/readback failures, errors after commitment,
cleanup errors, unchanged encoding, repair ordering, generation rollover and
cuts around every modeled operation. Integration tests exercise the same save
and recovery calls through the checked reader. Read tests cover mount changes,
qualified absence, descriptor zero, wrong lengths, open/stat/read/close errors,
unknown peers and successful-read/failed-close cases. Startup tests invoke the
production call boundary, not copied SDK mount/format logic. Integration tests
exercise acknowledgement and false dismissal during read failures, followed by
a new event before retry, including timer rollover. Fakes are not implementations
of SPIFFS, and complete ESP32 setup/BLE/RTOS execution remains a bench requirement.

Literal public-active and private-tombstone `FIN1` fixtures independently protect
the existing 100-byte encoding and decoding contract in `native_delivery`, which
uses production `Utils` and the real cryptography library. The ordinary `native`
suite uses a deterministic SHA-256 fake. Adapter-parity tests alone cannot provide
format/checksum interoperability evidence because both adapters share the same
serializer and hash implementation.
Additional integration cases cover all boot read-error classes followed by
storage recovery, with no delayed old-incident restore, and storage diagnostics
under USB backpressure without record reads or disclosure of private keys.

The unknown-downtime policy remains unchanged: no stale samples are restored,
clock continuity is unqualified and at most three **persisted** recoveries are
allowed. Resets before a saved increment or failing storage can bypass progress
of that budget. Qualified-clock expiry remains disabled on this target.

Required bench work includes power cuts and brownouts at varied write phases,
near-full storage, failed writes/closes, repeated resets, mount failures and
concurrent BLE/RF/USB/display/sensor load. The matching
[ESP-IDF SPIFFS documentation](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/storage/spiffs.html)
describes non-real-time writes, potentially long garbage collection near capacity
and possible corruption after power loss. A checked API result cannot remove
those constraints. Use the [bench evidence template](fall_bench_template.md) and
retain storage, timing and independently observed delivery outcomes separately.
