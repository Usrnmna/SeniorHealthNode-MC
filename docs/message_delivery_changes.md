# Fall message delivery: implementation and adjustment guide

This document records the directive for channel delivery and maps it to
the code for manual review. Both notification streams use the configured
**group channel**, initially public `#falldetect`, with runtime public/private selection. They do not use direct messages or
require a registered caregiver contact. The initial possible-fall alert and the
response messages have independent delivery state.

All fall messages contain the message text without a node-name prefix or its
separator. Locations, measured peaks, warning wording, and multipart labels are
retained. This applies to both public and private group channels.

## Directives and implementation

| Directive | Implemented behavior | Code to review |
| --- | --- | --- |
| 1. Retain essential requests through local transmission failures | Queue acceptance preserves the request. Packet allocation failure, transmit-start failure, transmit timeout, and queue expiry cause bounded delayed retries. | [`AssistanceDelivery.h`](../examples/companion_radio/health/AssistanceDelivery.h): `begin`, `poll`, `txComplete`; [`Dispatcher.cpp`](../src/Dispatcher.cpp): `notifyTxComplete`, `releasePacket`, `expireQueuedTransmissions` |
| 2. Obtain delivery evidence for group-channel messages | Both streams use group text on the selected public or private channel. A matching received flood with a nonempty forwarding path records a repeater echo; it never claims a named recipient received or read the message. | [`FallAckSender.h`](../examples/companion_radio/health/FallAckSender.h): `queueGroup`, `observeRepeat`; [`MyMesh.cpp`](../examples/companion_radio/MyMesh.cpp): `filterRecvFloodPacket` |
| 3. Make unsuccessful delivery visible | Serial state transitions and the `health` command distinguish pending, queued, locally transmitted, repeater echo heard, invalid text, exhausted retries, and no repeat heard. Accepted triple-click or delayed false-alarm double-click still silences SOS. Delivery status is reported over serial; there is no additional audible failure indication. | `HealthMonitor.cpp`: `reportAssistance`, `pollFallResponse`, `handleHealthCommand` |
| 4. Expose interruptions without accepting stale fall evidence | Fresh-sample interval diagnostics and a prolonged motion-data fault are independent of delivery. Invalid data and gaps over the detector limit still discard partial fall evidence. | [`MotionHealth.h`](../examples/companion_radio/health/MotionHealth.h): `observe`; `HealthMonitor.cpp`: `pollMotion`; existing [`MotionRule.h`](../examples/companion_radio/health/MotionRule.h): `update` |
| 5. Test the production delivery path under faults | Native tests exercise the delivery controller, group-text packet builder, Mesh/Dispatcher, packet pool, and encryption with a fake radio/clock. Separate tests exercise motion-health recovery and existing fall/buzzer rules. | [`test_assistance_delivery`](../test/test_assistance_delivery/test_assistance_delivery.cpp), [`test_dispatcher_delivery`](../test/test_dispatcher_delivery/test_dispatcher_delivery.cpp), [`test_motion_health`](../test/test_motion_health/test_motion_health.cpp) |

## Event and state contract

`MyMesh::pollMotion()` creates the initial channel text:

```text
Possible fall (strong-sequence): peak accel=2.80g rotation=260.0deg/s
```

The path identifies the supporting detector evidence; values preserve the event peaks.
The [detector reference](fall_detection.md) defines the experimental paths and limits. `MyMesh::pollFallResponse()`
creates a separate event after three accepted PRG clicks, using `ack_message`
and the location selected at that event's creation:

```text
Fall Detected & User Has Requested Assistance | HOME
Fall Detected & User Has Requested Assistance | lat=37.123456, lon=-122.654321
```

Initial-alert transport owns one immutable event; `FallInitialAlert` retains one
waiting latest incident. Repeated observations of the same unresolved incident
do not restart its retry budget, even after its initial delivery finishes. Several
new incidents while transport is busy replace the waiting slot with the newest
and increment the visible coalesced count. Response delivery uses a bounded outbox: one active event, one waiting
ordinary event, and a reserved slot for all explicit PRG responses, including
early assistance requests. A full slot leaves the request
pending in `FallResponse`; repeated unscheduled ordinary events may coalesce, but
a pending explicit response is protected. If confirmation capacity stays saturated
while a newer incident is resolved, local SOS still stops, but the newer
confirmation can be omitted rather than replacing the earlier owner.
`health detail` exposes `dropped_confirmations`; bounded queues cannot guarantee
transmission of every response during prolonged delivery failure. Each accepted event freezes its text and
location. False-alarm cancellation is scoped to the incident ID captured by the
initial-alert controller; a coalesced newer detection cannot change that owner.
Initial-alert and response delivery progress concurrently using
distinct local packet tags. Response parts share one controller and run sequentially.
Neither waiting for radio work nor waiting for a repeat pauses motion analysis,
button processing, or buzzer servicing. The main loop remains cooperative: a
blocking operation elsewhere can still delay all these services.

The state progression is `pending -> queued -> transmitted -> repeater echo heard`.
An allocation or local transmission failure returns to `pending` unless the
attempt budget is exhausted. A queued packet has a residence deadline even when
CAD, airtime budget, or another transmission prevents it from starting. Queue
expiry does not abort a packet already handed to the radio. A local completion
callback reports success/failure once, clears packet tracking metadata, and
defers any retry until the next controller poll.

After successful local transmission, the controller waits for a matching repeat.
If none arrives before the repeat deadline, it schedules another transmission,
subject to both limits below. It retains hashes of previous attempts until the
next event, so a late repeat can resolve an exhausted event. A received repeat
cancels a still-queued retry; a transmission already in progress cannot be recalled.
An unrelated packet, a different payload, or a zero-hop copy does not confirm it.

The event text and selected location are frozen for retries. Each
attempt receives a fresh standard MeshCore timestamp and therefore a fresh
packet identity so repeater deduplication does not suppress recovery attempts.
**Receivers may display duplicate text. Exactly-once receipt is not guaranteed.**

Queue acceptance proves only local ownership; local TX completion proves only
that the driver finished sending. A repeater echo is evidence of a matching
rebroadcast. It does not identify a human recipient or establish receipt at a
caregiver's companion. MeshCore's [Quick Start Guide, page 5](https://files.liamcottle.net/MeshCore/Documentation/MeshCore_Quick_Start_Guide.pdf)
likewise distinguishes a heard repeat from knowing who received channel text.
Missing repeat evidence also does not prove nobody received the message: direct
listeners can receive it with no repeater present, and the return radio path can fail.
The packet fingerprint does not authenticate a repeater identity. An exact
recorded frame with a forwarding path can also satisfy this evidence check.

## Manual settings

Edit [`include/HealthNodeConfig.h`](../include/HealthNodeConfig.h), then rebuild
`heltec_v4_companion_radio_ble`. The table below describes compiled defaults. Select the channel at runtime with the [USB/BLE commands](cli_commands.md#fall-message-channel-usb-and-ble); the other settings require rebuilding.

| Setting | Default | Adjustment and effect |
| --- | --- | --- |
| `ack_channel_name`, `ack_channel_key` | `#falldetect` and its 16-byte hashtag key | Fallback when no saved channel selection exists. Use `set fall.channel public #name` or `set fall.channel private Name <shared-key>` for runtime changes. Each incident retains its original key; new incidents use the saved selection. A local slot number is not a destination. |
| `ack_message` | `Fall Detected & User Has Requested Assistance` | Exact post-click UTF-8 text. The initial `Possible fall` format is in `MyMesh::pollMotion()`. |
| `click_window_ms` | `3000` ms | First down to third release, inclusive. A qualifying double waits until this entire window closes. |
| `false_alarm_click_window_ms` | `1000` ms | First down to second release, inclusive. Must be positive and shorter than `click_window_ms`. |
| `debounce_ms`, `long_press_ms` | `25`, `1000` ms | Both edges must settle; a press lasting one second or longer cancels the gesture. |
| `false_alarm_message` | `User Confirms False Alarm` | Exact text with GPS or fallback location appended by the formatter. |
| `response_timeout_ms` | `30000` ms | Fixed deadline from the declared fall/SOS start. Partial clicks and duplicate detections do not extend it. |
| `residual_quiet_required_ms` | `10000` ms | Continuous quiet ending at the response deadline; must be positive and no longer than the response timeout. |
| `residual_max_sample_gap_ms` | `100` ms | Maximum fresh-sample gap and tolerated age at the deadline. Invalid data also resets quiet evidence. |
| `residual_acceleration_change_g` | `0.08` g | Maximum acceleration-vector change from the quiet baseline; accumulated drift can break quiet. |
| `residual_gravity_tolerance_g` | `0.08` g | Maximum acceleration-magnitude deviation from 1 g during quiet. |
| `residual_rotation_dps` | `5.0` degrees/s | Rotation at or above this value breaks quiet. |
| `unconfirmed_message`, `no_movement_message`, `confirmed_message` | Exact user-response texts below | Plain UTF-8 text without leading separators. Rebuild after editing. |
| `DefaultLocation` | `HOME` | Assistance/false-alarm suffix when the existing location provider supplies no usable fix. A valid fix is captured once per event; no GPS freshness limit is added. |
| `ack_path_hash_size` | `1` byte per hop | Group-message flood route encoding; allowed values 1, 2, or 3. |
| `ack_retry_ms` | `5000` ms | Delay after an allocation or local TX failure before the next attempt. |
| `ack_max_attempts` | `60` | Total allocation/enqueue attempts per event, including successful attempts. This also sizes stored attempt fingerprints. |
| `assistance_queue_timeout_ms` | `30000` ms | Maximum tracked queue residence before failure callback and retry. Shared by both streams. |
| `channel_repeat_wait_ms` | `15000` ms | Wait after each successful local TX for a matching repeat before retrying. |
| `channel_transmit_attempts` | `4` | Maximum locally successful sends without repeat evidence. Failed local sends still consume the total attempt budget. |
| `motion_fault_timeout_ms` | `5000` ms | Time without a fresh valid sample before reporting motion unavailable. It does not change the detector's sampling-gap guard. |
| `serial_rx_bytes_per_loop` | `16` bytes | Caps serial command input per loop pass; firmware character echo is disabled. Command execution and other I/O still need hardware timing measurements. |

Despite their historical `ack_` names, these settings do not select a DM or enable
a recipient acknowledgment. Increasing attempt limits or shortening delays raises
radio traffic and battery use. The configured local attempt limits bound work;
they are not a promise of elapsed delivery time under a stalled main loop.
Each channel message remains limited to 160 UTF-8 bytes including any location
suffix and multipart label. Automatic warnings split as described below if the combined text does
not fit. If an individual part still cannot fit, the entire event fails visibly;
no partial event or truncated UTF-8 is sent. Each part has its own bounded retry
budget. Even after a first-part failure, the second part is attempted;
`part_failed=1` preserves the failed outcome until the next response event.

## Post-fall response directives

Detector paths and the initial text are defined in the
[maintained detector reference](fall_detection.md). Automatic warnings use
`Possible fall; condition unconfirmed` with location; only a button-requested
assistance event uses `ack_message`.
`FallResponse::updateButton()` runs before `updateDeadline()`, so a gesture already
confirmed by debounce wins a deadline reached in the same loop. A third release
whose debounce finishes after the deadline produces a late confirmation.

| Condition | Message | SOS |
| --- | --- | --- |
| Two complete presses in one second; three-second decision window closes without a debounced third press | `User Confirms False Alarm` with GPS or fallback location, before or after escalation | Stop when accepted after the window |
| Three accepted presses before escalation | Existing `ack_message` with GPS or fallback location | Stop immediately |
| No accepted response at 30 seconds; moving or unknown | Possible-fall context and location, then ` | USER COULD NOT CONFIRM THEIR CONDITION` | Keep repeating |
| No accepted response at 30 seconds; final continuous quiet window satisfied | Possible-fall context and location, then ` | NO RESIDUAL MOVEMENTS DETECTED; POSSIBLE INCAPACITATION OF USER` | Keep repeating |
| Later fresh 10-second quiet after a general warning is accepted by the outbox | One no-residual-movement update with possible-fall context and location | Keep repeating |
| Three accepted presses after either automatic warning | Independent `USER HAS CONFIRMED FALL` (no node-name prefix or location suffix) | Stop immediately |

One warning is selected at the original deadline. If it is the general warning,
one later qualifying quiet assessment can produce an upgrade. Motion resuming
afterward does not send a retraction or stop SOS. Sending, hearing a repeat, and exhausting
retries do not close the incident. Double and triple presses remain available without
an incident-age limit while the powered firmware is running. Restart clears
motion/gesture/retry state but can restore a checked unresolved alarm through
[bounded incident recovery](fall_detection.md#post-fall-assessment-and-restart-recovery). The existing SOS pattern, including its five-second pause, remains intact.

The first press-down starts both gesture timers. The second release must occur
within 1000 ms; the false-alarm decision waits until elapsed time exceeds 3000 ms.
A third release at exactly 3000 ms is still valid after debounce and wins over the
double candidate. A debounced third down cancels the candidate, so a held/late
third press cannot accidentally declare a false alarm. SOS remains active until
a gesture is accepted. A pending double does not extend the automatic-warning
deadline. Idle gestures and a held button from before the incident do not count.
The gesture handler uses unsigned elapsed time and never waits or blocks the loop.

Quiet requires fresh finite samples near 1 g, low gyro magnitude, and limited
acceleration-vector change from a fixed quiet baseline. Movement, invalid readings,
or a gap exceeding the configured maximum restart evidence collection. Cached
readings do not extend evidence. Samples after the fixed deadline cannot strengthen
the warning retroactively; a long service stall selects the general warning.
These are initial engineering thresholds, not physically validated measures of
incapacitation. Test worn-device motion and buzzer vibration before relying on them.

The default warning texts fit in one message with GPS coordinates or `HOME`.
Long customized text or fallback locations may exceed 160 bytes. For example,
with `DefaultLocation` set to the longer location below, the formatter preserves
all text using two ordinary channel messages:

```text
[1/2] NO RESIDUAL MOVEMENTS DETECTED; POSSIBLE INCAPACITATION OF USER
[2/2] Possible fall; condition unconfirmed | Main residence, upstairs bedroom at the far end of the eastern hallway
```

The warning is first so location delivery does not precede the essential warning.
Parts are independently retried; receiving one does not prove the other arrived.
Standalone text never starts with ` | `; `FallNoticeMessage::join()` adds that
separator only between two nonempty strings.

A late confirmation supersedes its own automatic warning's queued packet, unsent
parts and remaining retries. A packet already handed to the radio finishes normally.
Its late callbacks and echoes cannot complete or fail the replacement confirmation.
Triple confirmation leaves the separate initial `Possible fall` alert active.
False-alarm confirmation additionally cancels that incident's queued initial alert
and all further initial-alert retries, including while waiting for a repeater echo.
It retires automatic warnings immediately even if the response outbox is full.
Other incidents' events are unaffected. An in-flight packet may finish, and remote
repeaters may still forward an already-sent alert; cancellation cannot retract it.
The correction uses the incident's captured group channel and freezes its location
when the outbox accepts it. It has the same bounded delivery attempts as other
responses; stopping SOS does not depend on successful transmission.

### Adding another directive

1. Add its plain text and any tunables to `HealthNodeConfig.h`, with units and guards.
2. Add a named `FallNotice` and its text mapping in `FallNotice.h`; classify it in
   `isAutomaticWarning()` if it follows the automatic-warning formatting/lifecycle,
   or `isExplicitResponse()` for reserved response capacity and warning supersession.
3. Add its condition in the focused response methods in `FallResponse.h`. Keep
   user confirmation first, then specific warnings, then the general fallback.
4. Add observable-behavior tests to `test_post_fall`; update this condition table.
   Run `verify.ps1`. New kinds of delivery or extra message parts require an explicit
   formatter/outbox change, rather than hidden side effects in a condition.

`FallDetectionConfig::max_sample_gap_ms` in `MotionRule.h` remains 100 ms, with
nominal sampling every 20 ms. `MotionHealth` uses that same threshold to count
completed fresh-valid sample intervals exceeding it. `max_gap_ms` is the longest
completed interval; a still-open outage is exposed by `sensor=FAULT` after its
timeout and contributes a gap when sampling resumes. No interval is invented
before the first valid fresh sample. Recovery clears the health fault but does
not bypass the detector's bounded fresh-data recovery or short event refractory period. Raising the gap threshold
cannot recover motion that was never sampled.

## Code ownership and review points

- `AssistanceDelivery` owns frozen payloads, attempt fingerprints, deadlines,
  counters, channel-delivery states, and terminal reasons. `begin()` does not
  reset a busy event; `poll()` performs at most one enqueue attempt per call.
- `FallAckSender::queueGroup()` uses the existing channel encryption and group
  text wire format. It sets local tracking metadata and asks Mesh to flood.
  `observeRepeat()` checks forwarded group packets against retained fingerprints.
- `Packet::tx_tag` and `tx_deadline` are local metadata; they are not serialized
  or included in packet hashes. Review `Packet.h`/`Packet.cpp` alongside
  `Dispatcher.h`/`Dispatcher.cpp` when changing packet lifecycle behavior.
- Dispatcher clears metadata on allocation and completion, notifies tagged
  owners on success or release, expires queued packets, and supports queued
  cancellation. Untagged MeshCore traffic retains its existing semantics.
- `MyMesh::onPacketTxComplete()` routes completion to both controllers; only the
  matching tag acts. `filterRecvFloodPacket()` observes echoes before normal
  Mesh deduplication and returns control to existing validation/forwarding.
- `FallResponse::messageAccepted()` releases a request only when the outbox owns
  it. An older delivery completion cannot erase a new confirmation. `FallNoticeOutbox`
  owns bounded pending capacity and multipart progression; `AssistanceDelivery::supersede()`
  retires old tags/hashes before queued cancellation can invoke a callback.
  Normal companion messaging continues alongside the two fall-message streams.
- `MotionHealth` observes current sensor validity/freshness. `HealthMonitor.cpp`
  feeds bounded chronological FIFO batches to detection, post-fall assessment and
  traces, and reports availability independently of prolonged sensor health.
  [Acquisition and evidence](fall_detection.md#runtime-behavior) describe the
  current buffering, loss policy and diagnostic output.
- `platformio.ini` defines `native_delivery` with production Mesh and Crypto sources
  and the small `test/transport_support` host adapters. It excludes the ordinary
  suite's crypto mocks. `verify.ps1` runs both `native` and `native_delivery` so
  the transport integration suite cannot be omitted from full verification.

Use serial at 115200 baud and enter `health` to inspect
`fall=...; help=...; sensor=...; detector=...; age_ms=...; gaps=...`.
`help` describes the current response part; `health detail` adds `part_failed`,
which retains a prior multipart failure until the next event. Sensor validity
and a lack of prolonged faults do not establish detector readiness or a wearer's
condition. The [USB reference](usb_cli_reference.md#health-and-motion-inspection)
defines availability, trace, FIFO and persistence diagnostics.

## Validation and further changes

Run `.\verify.ps1` to build the combined BLE companion and fall-detection image,
merge its flash image, and run both native suites. Verification commands and boundaries are described
in [build validation](build-validation.md). The transport tests cover production
Mesh/crypto with fake radio and clock boundaries; they do not execute BLE hardware.

Focused tests are:

```powershell
python -m platformio test -e native -f test_dispatcher_delivery
python -m platformio test -e native_delivery
python -m platformio test -e native -f test_motion_health
```

The regression scope includes TX-start failures, TX timeout, allocation exhaustion,
CAD/airtime queue expiry, callback ownership, rollover, independent message streams,
repeat matching, and motion progressing during radio faults. The existing virtual
fault suite retains sensor, button, buzzer, ADC, and environmental failure scenarios.
See [virtual fault testing](virtual-fault-testing.md) for its boundaries.

Synthetic tests and firmware builds do not establish RF range, actual reception,
human acknowledgment, sensor sensitivity, watchdog behavior, or physical timing.
Bench verification requires the sensor, a repeater, and a receiving companion:
interrupt the radio path, restore it within and after the retry budgets, and
compare serial state with received channel text and duplicates. Measure sampling
gaps under sustained radio/GPS/flash activity. No hardware is flashed by these tests.

To adjust behavior, first change the documented constants, rerun the focused
suites plus `verify.ps1`, and inspect transmitted text with a companion. To change
the state contract, update its native tests and this directive map together.
For rollback, preserve a Git snapshot of the whole delivery change and restore
the controller, integration, packet metadata, dispatcher, tests, and documentation
as a coherent set. Removing only the completion hook can strand pending requests.
Delivery payloads/retry progress, partial clicks and motion evidence remain
volatile. `FallIncidentStore` persists minimal unresolved-incident metadata, so
a checked record can restore SOS and issue a new assessment after reboot under
the bounded clock-uncertain policy. It does not resume the former radio retry
queue or provide a manual resend command. `FallChannelStore` persists the
selection for future incidents separately. See
[recovery limitations](fall_detection.md#post-fall-assessment-and-restart-recovery).
