# Fall-detection bench evidence template

This template records future measurements. Empty fields mean unmeasured, not a
pass. Host tests and synthetic fixtures do not fill hardware-result fields.
Hardware access, flashing and destructive storage/power tests require their own
authorized run. Detector settings remain fixed during each comparison.

## Run and provenance

| Field | Value |
| --- | --- |
| Run/session ID, UTC date, operator role | |
| Source ID and retained source/configuration reference | |
| Capture bundle, raw SHA-256, session byte offsets, complete/incomplete classification | |
| Replay tool/engine hashes, repeated replay result and divergence | |
| Framework, compiler and library fingerprints; explicit effective build options | |
| Application and merged image SHA-256 | |
| Installed-image evidence, method and uncertainty | Unknown unless independently established |
| Board/sensor revision, enclosure, mounting location/orientation/firmness | |
| Calibration ID/results, temperature/warmup, battery/supply, filesystem occupancy | |
| Sampling/filter settings and buzzer configuration | |
| Independent clock/video/reference alignment and timing uncertainty | |

Source/build identity, self-reported firmware identity and verification of the
installed binary are different evidence. Retain restricted source/configuration
outside distributable bundles when it contains secrets. Ordinary mixed USB logs
may contain sensitive text even when recorder frames exclude it.

## Service-load matrix

Run baseline and reproducible combinations; retain commands, durations and
actual offered/achieved load. Pair buzzer sounding/silent runs and document
enclosure coupling. Avoid changing thresholds to make a failing load trial pass.

| Trial | BLE/client traffic | Mesh/RF and recipient setup | USB consumer/diagnostics | OLED/environmental sensing | Storage occupancy/write/reset stress | Duration/outcome |
| --- | --- | --- | --- | --- | --- | --- |
| Baseline | | | | | | |
| Combined ordinary load | | | | | | |
| Stalled/disconnected USB | | | | | | |
| Delivery saturation | | | | | | |
| Storage/power experiment | | | | | | |

## Snapshots and loss accounting

Capture `health detail`, `health timing`, `health errors`, `motion record status`
and relevant radio status before/after each interval. Retain original output with
host timestamps. Commands are observations and can themselves add load.

| Host time / node uptime / boot ID | Snapshot reference | Monitoring and exclusive unavailable causes | Maximum service gap / sample delay / persistence duration | FIFO discard lower bound / overflow / acquisition faults | Recorder drops / packet loss / USB rejection or failure | Reset or discontinuity |
| --- | --- | --- | --- | --- | --- | --- |
| | | | | | | |

Availability/service totals start at first motion poll and reset on reboot;
resettable motion/sensor counters have separate reset commands. Use the reset
reason and timestamps in each snapshot. Millisecond operation time uses unsigned
rollover arithmetic; do not subtract unrelated boots or an ambiguous half-range
interval. Treat wrapped 32-bit counters and reconnects explicitly. Cause totals
are observed service-boundary accounting, not physical uptime. Discard lower
bounds are not exact lost-sample counts. Export loss and acquisition loss are
different; preserve incomplete recordings and all reconnect offsets.

## Incident and delivery ledger

| Boot + incident ID | Label / confidence / independent evidence | Candidate / detection time and path | Original deadline / later stillness / unavailable motion | Local alarm and button response | Persistence attempt/result | Initial/response coalescing or omission | Delivery attempt/token/channel alias | Queue / local TX / repeater echo | Independently observed recipient text/time | Final outcome |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| | | | | | | | | | | |

Use a non-secret channel alias and retain private routing evidence separately.
Track retries and duplicate recipient messages independently. A queue admission,
local TX callback or repeater echo is not recipient delivery. A local dismissal
is not a verified non-fall label. Record overflow/coalescing/dropped-confirmation
counters even when a later incident succeeds. Match callbacks to their incident
and token; record stale callbacks without reassigning them to a new incident.

## Recorded-data and acceptance worksheet

Use a predefined event taxonomy with onset, descent/impact when present, end,
continued movement, later stillness and label uncertainty. Include seated,
unchanged-orientation, soft/slow/supported and no-free-fall cases, plus ordinary
sitting, lying down, bending, walking, device-only drops and loose attachment.
Do not ask older or vulnerable participants to perform falls. Controlled surrogate
or trained healthy-adult trials need an appropriate approved safety protocol;
they do not reproduce all real falls.

Keep participants and sessions held out from tuning, stratify placement and
mounting, and retain wear duration including ordinary activity. Report event
sensitivity with counts/uncertainty, false alerts per observed wear-hour,
detection-latency distributions, unavailable time by cause, capture completeness
and delivery outcomes. Do not discard failed exports or unavailable intervals
from denominators without explaining their effect. Identify simulated versus
observed falls and keep their results separate.

| Gate | Target and status | Evidence / regression check | Proceed, investigate or fallback |
| --- | --- | --- | --- |
| Software behavior and repeatable replay | Exact required invariants; no unresolved failures | Native/native_delivery/Python results, matching-source replay | |
| Timing and bounded memory under measured load | Provisional product target to be set before trial | Sample/service gaps, stack/heap, watchdog and loss ledger | |
| Storage durability and mount policy | Product decision plus controlled fault protocol | Power-cut outcomes, recovery budget and failed write visibility | |
| Sensitivity, false alerts and latency | Provisional targets to be agreed before held-out evaluation | Per-path/placement metrics with counts and uncertainty | |
| Recipient delivery and duplicates | Provisional product targets | Independent receiver observations against attempt ledger | |

An experiment determines whether a later implementation is justified. Increasing
sample rate, lowering thresholds, enlarging queues or adding sensors is not an
automatic remedy. Fallbacks include keeping current defaults, disabling an
experimental path for a controlled comparison, or rejecting an incomplete
recording from deterministic replay while retaining it as diagnostic evidence.
Untethered field recording, mount policy and unknown-downtime policy remain
explicit decisions. USB tethered runs do not establish ordinary long-term wear.

Related references: [recording and replay](motion_recording.md),
[incident storage](incident_storage.md), [fall detection](fall_detection.md),
[software verification boundaries](virtual-fault-testing.md).
