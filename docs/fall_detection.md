# Possible-fall detection and research

Fall detection and MeshCore BLE companion services run together in the single
`heltec_v4_companion_radio_ble` firmware on the same node. Decisions run locally;
a phone connection is not required. The detector reports **possible falls**.
A candidate, an automatic observation, and a button-confirmed wearer response
are distinct kinds of evidence.

This is the maintained reference for detector behavior, acquisition, diagnostics,
and recovery. [Alarm operation](operation.md) describes wearer responses and
[the sensor guide](gy521_sensor.md) covers assembly. Thresholds are engineering
settings for firm torso/chest attachment. No measured sensitivity, specificity,
false-alert rate or clinical performance is established for this firmware.

[Motion recording and production replay](motion_recording.md) describes optional
session capture, source/configuration identity, acquisition versus export loss,
wall-time availability causes and the limits of deterministic host replay.

## Detector flow

```text
Fresh contiguous data --> Armed --> Candidate --> Possible fall --> Short refractory
       ^                   ^           |                               |
       |                   +-- expiry -+-------------------------------+
       |
Invalid data / FIFO loss / excessive sample gap
(clear associated evidence; never interpret missing data as quiet)

Possible fall --> incident + SOS + bounded notification delivery
                     |
        original 30 s deadline + continued motion assessment + PRG response
```

[`MotionRule.h`](../examples/companion_radio/health/MotionRule.h) uses fixed-size
history and no allocation, waits or I/O. Startup, sensor recovery and calibration
exit require **200 ms and at least six fresh contiguous samples**; movement does
not prevent recovery. Invalid/nonfinite readings or a sample gap above **100 ms**
break event association. Duplicate timestamps do not add evidence. Timer
comparisons use unsigned elapsed milliseconds, including timer rollover.
Expired pre-trigger rotation and reference-history entries are retired during
sample processing so they cannot become recent again after a full timer cycle.

Ordinary candidate expiration permits new evidence without another quiet interval.
A new low-g episode following a rise above the low-g threshold opens its own
500 ms strong-sequence window. A previously matched low-g/impact pair can still
complete with rotation through its original deadline; a rebound cannot erase
that evidence or extend its lifetime. Context references, features and confirmation
deadlines retain their own original bounds across renewed low-g episodes.
Continuous low-g does not extend a window. An emitted event has a **1200 ms refractory period**
to avoid treating one impact train as several falls. There is no 60-second
classifier cooldown. Notification coalescing is separate from classification.

## Algorithm stages and default settings

Acceleration is the three-axis magnitude in **g, including gravity**. Rotation is
the magnitude of the bias-corrected gyroscope vector in **degrees/second**.
Relative orientation compares gravity-like acceleration directions before and
after a transition; it is not a body-posture or injury classifier.

| Evidence path | Required evidence | Decision |
| --- | --- | --- |
| `strong-sequence` | Low acceleration at or below 0.35 g; impact at or above 2.4 g after that low reading within 500 ms; rotation at or above 240 degrees/s in the same sequence or up to 300 ms before the low reading | Immediate possible fall when the sequence is complete. Continuing movement cannot veto it. |
| `impact-orientation` — experimental | Impact at or above 2.4 g; relative orientation change of at least 55 degrees | Requires 1000 ms of subsequent qualifying quiet. No low-g or rapid-rotation requirement. |
| `supported-descent` — experimental | Relative change of at least 55 degrees; acceleration trough at or below 0.85 g; peak at or above 1.6 g; rotation peak at or above 80 degrees/s | Requires 1000 ms of subsequent qualifying quiet. |
| `slow-descent` — experimental | Relative change of at least 65 degrees; trough at or below 0.85 g; peak at or above 1.15 g | Requires 2000 ms of subsequent qualifying quiet. No rapid-rotation or free-fall requirement. |

The original sequence remains available without an orientation reference. The
additional paths are enabled by `context_paths = true` and require acceleration
vectors; magnitude-only callers cannot establish orientation. Context candidates
start on low acceleration, an impact of at least 1.6 g, or unloading to at most
0.85 g while rotating at least 10 degrees/s. Therefore orientation change or lying
still alone does not trigger an alarm. A completely smooth descent that provides
none of these triggers can still be missed.

A bounded 64-entry history supplies a gravity-like reference from **200–1000 ms
before candidate start**. Reference samples have magnitude 0.85–1.15 g and rotation
below 30 degrees/s. The relevant peak/trough/rotation features must be no more
than 2000 ms old when orientation support is first established. Context is
associated for at most 6000 ms; supporting evidence has a 5000 ms confirmation
deadline. Every quiet-confirmation sample must still supply a usable, sufficiently
changed orientation. Returning toward the prior orientation breaks confirmation. Old peaks cannot be associated indefinitely.

Quiet for the experimental paths means magnitude 0.7–1.3 g, inclusive, and
rotation below 30 degrees/s. It is supporting evidence for these weaker paths,
not a requirement for the strong sequence. This detector quiet definition is
separate from the stricter post-fall assessment below.

## Runtime behavior

### Acquisition and sample timing

[`MPU6050.h`](../src/helpers/sensors/MPU6050.h) verifies identity and configuration,
then collects all acceleration/gyro axes and temperature through the MPU6050
hardware FIFO. Output remains **50 Hz**, **±8 g**, **±500 degrees/s**, with
`DLPF_CFG=3`. The 100 ms hardware settling period runs with FIFO capture disabled;
FIFO reset/enable follows settling so startup readings are not reused.

FIFO records are 14 bytes. A bus burst reads at most **two records (28 bytes)**;
[`HealthMonitor.cpp`](../examples/companion_radio/HealthMonitor.cpp) processes at
most **four samples per motion poll**, before and after mesh work. The INT pin
remains disconnected. Capture is buffered; host processing remains cooperative
and bounded. Environmental collection, staged battery sampling and incremental
OLED transfers continue in the same firmware.

Samples are processed chronologically. The FIFO provides no sample timestamps,
so times are estimated from its count and the 20 ms period. The count observation
includes elapsed I2C work instead of using poll-entry time. The newest record is
dated one period before that observation, with **20 ms nominal phase uncertainty**
under the assumed sensor rate, plus count-read latency and clock uncertainty. These are estimated acquisition
times, not measured interrupt timestamps. They avoid assigning a loop's same
processing time to an entire burst, but cannot guarantee sub-period event timing.
When count-read timing produces an overlap smaller than one sample period,
FIFO order permits a forward correction of at most 20 ms. Corrected timestamps
remain chronological and no later than the completed count read. Adjustments
are counted and flagged; larger overlaps and gaps still invalidate continuity.

FIFO overflow, partial/failed reads, contradictory timing and a backlog beyond
1000 ms explicitly break continuity and discard buffered evidence. A partial
trailing record is left for completion. The code does not substitute a latest
register reading to hide lost history. Discard counts are **lower bounds** when
the exact lost count cannot be read. Failed reads retry after 20 ms; three
consecutive failures trigger reinitialization. No FIFO output for one second
enters recovery, and failed initialization backs off for five seconds.

The 5 ms I2C limit applies per transaction, not to the whole main loop. FIFO does
not remove sensor bandwidth, sampling-phase, storage-stall or overload limits.
Saturated acceleration/gyro axes are flagged; a clipped peak is not an accurate
measurement of the true peak. See [register evidence and filter limits](mpu6050_gy521_reference.md).

### Availability and event evidence

Sensor health and detector availability are reported separately. `health` includes
`sensor` and `detector`; `health detail` identifies calibration, stale data,
recovery, armed/candidate state, refractory suppression and configuration faults.
A healthy sensor does not imply an armed detector. Unavailable duration accounts
for a stalled loop after its last sample's freshness expires; replaying buffered
samples does not retrospectively remove that wall-time interval. A prolonged five-second
sensor fault is distinct from the detector's 100 ms gap guard.

[`MotionTrace.h`](../examples/companion_radio/health/MotionTrace.h) retains bounded
RAM samples and an event ring. `motion trace` retrieves a capture of up to 96
samples around the latest accepted/rejected decision, including up to 16 trailing
samples; `motion events` retrieves up to 32 decision/transition summaries.
`motion evidence` retains up to five candidate checkpoints (start, acceleration
minimum/maximum, gyro maximum including pre-trigger rotation, and an earlier
classifier-selected reference sample) so a multi-second event does not lose its initial features
when the recent ring wraps. Data loss freezes a rejected candidate without
inventing a zero-motion reading. Rows include estimated time, scaled raw
axes, corrected gyro axes, chip temperature, state, reason, quality flags and the
commanded buzzer state where applicable. New decisions overwrite older captures;
these commands do not provide a permanent incident archive.
Pre-trigger checkpoints never cross a sensor or calibration interruption.
Startup faults without a candidate leave ordinary samples rolling; later faults
without a new candidate preserve the previous accepted/rejected capture.

`health timing` reports sample counts, interval coverage, pending/backlog samples,
discarded-record lower bounds and timestamp uncertainty. `health errors` adds FIFO
loss/reset, timing-discontinuity, saturation and calibration-temperature diagnostics.
`health detail` includes unavailable time, persistence status and coalesced initial
notifications and omitted confirmations under delivery saturation. USB replies
take a consistent snapshot and drain incrementally,
without per-sample printing. Exact command fields and flag meanings are in the
[USB reference](usb_cli_reference.md#health-and-motion-inspection).

### Detection and notification ownership

Each accepted detector event remains a possible fall. The initial text names its
evidence path and preserves event peaks. Additional detections while an incident
is unresolved retain its original response deadline, button progress and alarm.
They do not restart an initial alert's retry budget, including after that initial
alert has completed. After acknowledgement or false-alarm dismissal, a new event
can open a new local alarm after the short refractory interval.

The initial-alert controller owns one active immutable message and one waiting
latest incident. If new incidents outpace delivery, the waiting slot retains the
newest and increments a coalesced counter. Response messages have their separate
bounded outbox. Early assistance requests, later fall confirmations and false-alarm
responses share the reserved explicit-response capacity and precedence over
automatic warnings. If all confirmation capacity remains occupied while a newer
incident is resolved, local acknowledgement still stops SOS; the earlier
confirmation retains ownership and the newer transmission can be omitted.
`health detail` reports `dropped_confirmations` for this delivery-saturation gap.
False-alarm dismissal cancels pending initial alerts and automatic
warnings belonging to that incident; already transmitted text cannot be recalled.
BLE, mesh and the existing delivery controllers retain their roles. Queueing,
local TX and repeater evidence do not establish recipient reception.

## Post-fall assessment and restart recovery

The first automatic assessment stays at **30 seconds after declaration**. Fresh
motion must establish quiet over the final **10 continuous seconds** to select
the no-residual-movement warning. Otherwise it reports an unconfirmed condition.
After that general warning is accepted by the outbox, subsequent fresh quiet can
produce **one** no-residual-movement update while the alarm remains unresolved.
There is no repeated upgrade loop, automatic injury diagnosis or deadline reset.

The post-fall settings in [`HealthNodeConfig.h`](../include/HealthNodeConfig.h)
use a 0.08 g vector-change limit, 0.08 g magnitude deviation from 1 g, rotation
below 5 degrees/s and a maximum sample gap of 100 ms. Missing data invalidates
quiet rather than contributing zero movement. Later samples do not retroactively
strengthen the original deadline assessment. Samples captured before incident
declaration are excluded from its post-fall assessment.

Automatic warnings begin with `Possible fall; condition unconfirmed`, with
location and the applicable warning, or use two labelled parts if necessary.
They do not assert that the wearer requested assistance. Explicit PRG responses
retain the assistance, fall-confirmation and false-alarm texts described in
[operation](operation.md). SOS continues through automatic warnings and delivery
failures until an accepted response or loss of power.

Buzzer-on samples remain available to detection and assessment. Trace rows report
the commanded sound state at processing; buffered samples flag its timing
uncertainty. No motion is presumed absent merely because sound is active.
Mechanical coupling, mounting and tone-related false movement require bench
measurement before any interference-specific correction is justified.

[`FallIncidentStore.h`](../examples/companion_radio/health/FallIncidentStore.h)
saves minimal unresolved-incident metadata in two checked, versioned records on
the existing filesystem. Open, resolution and restart-recovery transitions write
records; samples, traces, partial gestures and delivery retry progress are never
written at sample rate. A resolution record acts as a tombstone so an older
unresolved record does not normally reactivate an acknowledged incident.

| Restart evidence | Behavior |
| --- | --- |
| Qualified continuous clock, if explicitly enabled in a future target configuration | Storage supports retaining elapsed response time and expiring records aged 24 hours or more. This path is tested but is not enabled for the current target. |
| Current target: unqualified restart clock | Restore SOS and the original destination, report clock uncertainty, restore at most three times within the saved recovery budget, and treat the initial response deadline as already due. |
| Resolved, expired or recovery-limit record | Do not restore the alarm. Expiry/limit writes a resolved record. |
| Any existing corrupt/unreadable incident record | Report storage uncertainty and do not restore an older active record that might precede a lost resolution. |

Recovery restores no motion or quiet evidence and no partially entered button
gesture. A restored unresolved incident can issue a fresh automatic warning;
original radio retry progress is not resumed. The ESP32 software clock resets at power-on and can be advanced by saved contact
timestamps. A plausible epoch therefore does not establish elapsed downtime.
The current target records an unknown origin time and uses the bounded
unknown-clock policy; reliable wall-clock age and 24-hour expiry remain disabled
until a continuous clock is explicitly qualified. The optional expiry policy
concerns restart eligibility, not a cutoff for a continuously powered alarm.
The three-recovery budget is
enforceable only after successful writes; rapid resets before writing or failing
storage cannot prove that a recovery count advanced.

Pending writes and storage faults remain visible in `health detail`; failed
writes retry at five-second intervals. Abrupt power loss before a transition is
saved can lose a new incident or retain an older unresolved one. Physical power-cut
and filesystem tests remain necessary. Normal flash operations may stall the
loop; FIFO covers short stalls, while longer interruptions invalidate evidence.
The ESP32 writer checks open, full write, C-stream flush and backend close before
record verification. The diagnostic result identifies the failed operation;
API success and readback do not prove physical durability. See
[incident storage and mount-policy limits](incident_storage.md), including the
unchanged `SPIFFS.begin(true)` automatic-format risk and unknown-downtime policy.

## Tuning and software checks

All detector defaults are grouped in `FallDetectionConfig` in
[`MotionRule.h`](../examples/companion_radio/health/MotionRule.h). They are compiled
settings, not runtime USB/BLE setters. `context_paths = false` selects the strong
sequence alone for controlled comparisons.

| Configuration | Defaults and role |
| --- | --- |
| `low_g`, `impact_g`, `rotation_dps` | 0.35 g, 2.4 g, 240 degrees/s: strong sequence thresholds. |
| `event_window_ms`, `pretrigger_ms` | 500 ms strong window; 300 ms preceding gyro association. |
| `recovery_ms`, `recovery_samples` | At least 200 ms and six contiguous fresh samples after invalid evidence. |
| `refractory_ms`, `max_sample_gap_ms` | 1200 ms event suppression; 100 ms maximum evidence gap. |
| `quiet_min_g`, `quiet_max_g`, `quiet_max_dps` | 0.7 g, 1.3 g, 30 degrees/s: experimental-path quiet limits. |
| `context_paths` | `true`: enable experimental vector/context paths. |
| `context_angle_deg`, `slow_angle_deg` | 55 and 65 degrees relative orientation change. |
| `support_impact_g`, `support_rotation_dps` | 1.6 g and 80 degrees/s for supported-descent evidence; 1.6 g also opens an impact candidate. |
| `unload_g`, `slow_peak_g` | 0.85 g trough and 1.15 g peak for the slow path. |
| `context_feature_ms`, `context_window_ms` | 2000 ms feature age when orientation support begins; 6000 ms total context candidate window. |
| `confirm_quiet_ms`, `slow_quiet_ms`, `confirm_timeout_ms` | 1000 ms context quiet, 2000 ms slow quiet, 5000 ms confirmation deadline. |

The reference-vector magnitude gate (0.85–1.15 g), 200–1000 ms reference age,
64-entry history and 10 degrees/s unloading-trigger gate are explicit constants
in `MotionRule.h`. They require review alongside any major threshold change.
Configuration validation rejects
nonfinite/nonpositive thresholds, unsafe timer values, incompatible duration or
threshold relationships and unsupported history/recovery bounds. Invalid
configuration reports `config-fault`; it does not silently appear armed.

Lower thresholds or longer association windows can admit falls and ordinary
activity alike. More quiet can reject activity but delay or miss soft falls with
continued movement. The strong path can now alert despite continued motion;
that removes a false-negative mechanism but also removes the prior quiet filter
on device drops. Ordinary sitting, bending and lying down often lack combined
trigger/transition evidence, yet rapid lying down, a loose attachment, or a dropped
unworn device can match these paths. No wear/contact sensor distinguishes all
those cases. Relative orientation avoids a universal lying-posture requirement
but cannot cover every fall ending in a seated or unchanged orientation.

The existing stationary gyro correction is applied. Temperature delta from the
saved calibration and raw-versus-corrected readings are available for evaluation;
no temperature compensation model or accelerometer calibration is applied.
The current 50 Hz output and DLPF settings are retained pending recorded-data and
bench comparison. Their bandwidths do not guarantee rejection of aliased vibration
or capture of narrow impacts. Sampling phase, clipping and attachment are part of
the validation, not solved by lowering thresholds.

Relevant host suites exercise the production rule, acquisition driver, response
and persistence classes and selected companion integration paths:

```powershell
pio test -e native
pio test -e native_delivery
pio run -e heltec_v4_companion_radio_ble
```

See [build and validation](build-validation.md) and [virtual fault coverage](virtual-fault-testing.md)
for setup, focused suites and harness boundaries. Synthetic regressions cover
continued movement, missing features, supported/slow transitions, pre-trigger
rotation, expiration/recovery, repeated incidents, acknowledgement, data loss,
FIFO phase/jitter/overflow, daily-activity negatives, late quiet, restart and
rollover. These are software behavior checks, not measured fall performance.

Remaining evaluation needs include labelled recordings with held-out wearers and
routine activities; true fall variants and device drops; sensor rate/phase and
I2C load; BLE/RF traffic; buzzer coupling; temperature and calibration drift;
power interruptions; actual recipient delivery and repeat behavior. Older adults
must not be asked to perform falls for testing. Published results below do not
supply validation for these firmware-specific paths.

## Fall-detection research

The bibliography identifies **five distinct studies** and records the reference filenames used for the research review. Those filenames are provenance labels, not bundled project files. The strong sequence adapts study [4](#study-4). Relative-orientation paths, recovery, buffering and incident assessment are separately identified project extensions. The other methods remain research context; none of the reported performance results has been reproduced or validated in SeniorHealthNode.

### Research context

**Smartphone orientation and voting across multiple rules.** Astriani et al. [1](#study-1) combined five threshold-based methods (AGVeSR, Alim, alpha-angle, GyroReDi, and AGPeak), declaring a fall when at least three agreed. They collected 252 activity records (84 falls and 168 non-falls) in six phone orientations from adults aged 25-35, with simulated falls onto a mattress. Table V reports 76.67% accuracy for acceleration alone, 91.67% for AGVeSR, and 95.82% for the five-method vote. Jumping and sitting/standing transitions remained sources of confusion. The paper describes both a 180/72 training/testing split and leave-one-out validation, so its evaluation protocol should not be assumed to be an independently replicated benchmark. For this project, the useful direction is to evaluate orientation changes and agreement between several motion features rather than assume one mounting orientation will work for everyone. See PDF pp. 4-6.

**Posture plus the preceding motion transition.** Li et al. [2](#study-2) used two sensor nodes, on the chest and thigh, sampled at 120 Hz. Their algorithm identifies a static posture and examines the preceding five seconds of acceleration and angular velocity when the posture is classified as lying. In experiments involving three men in their twenties, they reported 91% sensitivity from 70 fall records and 92% specificity from 72 non-fall records. Falling against a wall into a seated position could be missed; quickly lying down could trigger false alarms. This supports investigating temporal context and posture, but the two-body-location method cannot be reproduced directly by SeniorHealthNode's single MPU6050. See PDF sections 3-4, pp. 2-6.

**Learning from sequences does not guarantee better detection.** Wisesa and Mahardika [3](#study-3) trained a long short-term memory (LSTM) recurrent neural network on waist-sensor data from UMAFall. They retained 617 valid recordings, randomly dividing them into 494 training and 123 validation samples. Their X-axis accelerometer model's prediction table identifies all 38 falls and misclassifies 2 of 85 daily activities as falls. In contrast, their combined accelerometer/gyroscope model misses 31 of 38 falls and falsely flags 10 daily activities (Tables 3-4). The reported peak validation accuracy of 100% for the single-axis model is distinct from those saved-model prediction results. For SeniorHealthNode, an LSTM is an experimental alternative requiring suitable data, evaluation on unseen wearers, and resource measurements; simply adding sensor channels or a neural network is not evidence of improvement. See PDF pp. 5-9 (article pp. 4-8).

**A timed low-acceleration, impact, and rotation sequence.** Huynh et al. [4](#study-4) tested a chest-mounted accelerometer/gyroscope system on 36 adults aged 18-28 and 38-56, collecting 702 laboratory movements and using half the dataset for development and half for assessment. Their rule first detects low acceleration, then requires both an acceleration peak and an angular-velocity peak within a 0.5-second window. The optimized study thresholds were 0.30-0.35 g for low acceleration, 2.4 g for impact, and 240 degrees/s for rotation. Table 3 reports 96.3% sensitivity and 96.2% specificity; the optimized acceleration-only comparison achieved 97.36% and 82.72%, respectively. The main improvement was fewer false alarms, with a small sensitivity reduction. These are chest-mounted, simulated-fall results, not thresholds established for this hardware or its intended users. See PDF pp. 3-7.

**A smartphone prototype with limited testing.** Rakhman et al. [5](#study-5) combined acceleration, rotation, posture, and orientation checks using a phone at the left chest. One male participant performed 120 simulated falls and 210 daily-activity trials. Table III records 112 detected falls out of 120 (93.33%); backward falls were detected in 26 of 30 trials. Daily activities including running, lying on a bed, and descending stairs caused false alarms. The paper's claimed 98% daily-activity accuracy is not consistent with Table IV: its alarm column lists seven false alarms, its running row totals 31 despite listing 30 trials, and its downstairs percentage does not match its counts. That aggregate claim should therefore be treated cautiously. The conclusion describes the work as limited to detection and leaves SMS/GPS extensions for future work, despite broader alert language in the abstract. See PDF pp. 4-5 (proceedings pp. 102-103).

### Implications for SeniorHealthNode

The following separates the implemented rule from further research and validation:

- **Implemented: sequences and experimental temporal context.** The strong path uses a bounded low-acceleration / impact / rotation window [4](#study-4) without a quiet veto. Additional paths combine a relative gravity-vector change with impact or descent evidence and subsequent quiet. These project-specific paths do not reproduce a published classifier. Study [2](#study-2) motivates temporal context, but its chest-and-thigh posture method is not implemented; a relative orientation change does not classify lying or establish injury.
- **Define how the node is worn.** Chest, thigh, waist, and phone-orientation results are not interchangeable [1](#study-1)-[5](#study-5). A nearby, unworn sensor measures its own movement and cannot directly infer the person's body motion. Placement, fastening, units, sensor ranges, and sampling timing need to be specified before adapting published thresholds.
- **Compare methods on the same held-out recordings.** Evaluate a simple temporal rule before adding voting or an LSTM, and keep test wearers separate from threshold tuning or model training. Study [3](#study-3) demonstrates why performance must be measured rather than inferred from model complexity.
- **Measure missed falls and false alerts separately.** Sensitivity is the fraction of falls detected; specificity is the fraction of non-fall activities correctly rejected. Overall accuracy alone can hide missed falls. A future evaluation should also report false alerts per wear-day, detection delay, and performance across routine activities and intended mounting positions.
- **Test detection and delivery separately.** Published classifier results do not establish MeshCore delivery reliability. Record whether an event was detected, whether its alert was queued, and whether the intended recipient received it, alongside battery and radio conditions.

The studies use different participants, hardware, placements, datasets, and evaluation procedures, so their percentages are not a ranking or a performance estimate for this project. None validates the current SeniorHealthNode firmware in everyday use by older adults. Voting, LSTM inference, and body-posture classification are not implemented.

### Bibliography

The entries identify the five studies reviewed for this detector. Page locators above refer to the reviewed PDFs; published pagination is included below where available.

<a id="study-1"></a>

1. Astriani, Maria Seraphina; Heryadi, Yaya; Kusuma, Gede Putra; and Abdurachman, Edi. (2019). **Human Fall Detection using Accelerometer and Gyroscope Sensors in Unconstrained Smartphone Positions.** *International Journal of Recent Technology and Engineering*, 8(3), 69-75. [DOI: 10.35940/ijrte.C3877.098319](https://doi.org/10.35940/ijrte.C3877.098319). Reference filenames: `fall_detect_paper1.pdf` and identical `fall_detect_paper7.pdf`.

<a id="study-2"></a>

2. Li, Qiang; Stankovic, John A.; Hanson, Mark; Barth, Adam; Lach, John; and Zhou, Gang. (2009). **Accurate, Fast Fall Detection Using Gyroscopes and Accelerometer-Derived Posture Information.** *Body Sensor Networks (BSN 2009)*, 138-143. [DOI: 10.1109/BSN.2009.46](https://doi.org/10.1109/BSN.2009.46). Reference filenames: `fall_detect_paper2.pdf` and identical `fall_detect_paper3.pdf`.

<a id="study-3"></a>

3. Wisesa, I Wayan Wiprayoga, and Mahardika, Genggam. (2019). **Fall detection algorithm based on accelerometer and gyroscope sensor data using Recurrent Neural Networks.** *IOP Conference Series: Earth and Environmental Science*, 258, 012035. [DOI: 10.1088/1755-1315/258/1/012035](https://doi.org/10.1088/1755-1315/258/1/012035). Reference filename: `fall_detect_paper4.pdf`.

<a id="study-4"></a>

4. Huynh, Quoc T.; Nguyen, Uyen D.; Irazabal, Lucia B.; Ghassemian, Nazanin; and Tran, Binh Q. (2015). **Optimization of an Accelerometer and Gyroscope-Based Fall Detection Algorithm.** *Journal of Sensors*, 2015, Article ID 452078, 8 pages. [DOI: 10.1155/2015/452078](https://doi.org/10.1155/2015/452078). Reference filename: `fall_detect_paper5.pdf`.

<a id="study-5"></a>

5. Rakhman, Arkham Zahri; Nugroho, Lukito Edi; Widyawan; and Kurnianingsih. (2014). **Fall Detection System Using Accelerometer and Gyroscope Based on Smartphone.** *2014 1st International Conference on Information Technology, Computer and Electrical Engineering (ICITACEE)*, 99-104. [IEEE publication record](https://ieeexplore.ieee.org/document/7065722/). Reference filename: `fall_detect_paper6.pdf`.
