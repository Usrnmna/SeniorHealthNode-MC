# Fall message delivery: implementation and adjustment guide

This document records the directive behind the delivery changes and maps it to
the code for manual review. Both essential notifications use the configured
**public channel**, initially `#falldetect`. They do not use direct messages or
require a registered caregiver contact. The initial possible-fall alert and the
post-click assistance request have independent delivery state.

## Directives and implementation

| Directive | Implemented behavior | Code to review |
| --- | --- | --- |
| 1. Retain essential requests through local transmission failures | Queue acceptance preserves the request. Packet allocation failure, transmit-start failure, transmit timeout, and queue expiry cause bounded delayed retries. | [`AssistanceDelivery.h`](../examples/simple_sensor/AssistanceDelivery.h): `begin`, `poll`, `txComplete`; [`Dispatcher.cpp`](../src/Dispatcher.cpp): `notifyTxComplete`, `releasePacket`, `expireQueuedTransmissions` |
| 2. Obtain delivery evidence within the public-channel requirement | The original recommendation to add direct-message acknowledgments was superseded by the public-channel directive. Both messages use group text. A matching received flood with a nonempty forwarding path records a repeater echo; it never claims a named recipient received or read the message. | [`FallAckSender.h`](../examples/simple_sensor/FallAckSender.h): `queueGroup`, `observeRepeat`; [`main.cpp`](../examples/simple_sensor/main.cpp): `filterRecvFloodPacket` |
| 3. Make unsuccessful delivery visible | Serial state transitions and the `health` command distinguish pending, queued, locally transmitted, repeater echo heard, invalid text, exhausted retries, and no repeat heard. Triple-click still silences SOS. No OLED/UI changes or new audible failure indication are introduced. | `main.cpp`: `reportAssistance`, `pollFallResponse`, `handleCustomCommand` |
| 4. Expose interruptions without accepting stale fall evidence | Fresh-sample interval diagnostics and a prolonged motion-data fault are independent of delivery. Invalid data and gaps over the detector limit still discard partial fall evidence. | [`MotionHealth.h`](../examples/simple_sensor/MotionHealth.h): `observe`; `main.cpp`: `pollMotion`; existing [`MotionRule.h`](../examples/simple_sensor/MotionRule.h): `update` |
| 5. Test the production delivery path under faults | Native tests exercise the delivery controller, public packet builder, Mesh/Dispatcher, packet pool, and encryption with a fake radio/clock. Separate tests exercise motion-health recovery and existing fall/buzzer rules. | [`test_assistance_delivery`](../test/test_assistance_delivery/test_assistance_delivery.cpp), [`test_dispatcher_delivery`](../test/test_dispatcher_delivery/test_dispatcher_delivery.cpp), [`test_motion_health`](../test/test_motion_health/test_motion_health.cpp) |

## Event and state contract

`MyMesh::pollMotion()` creates the initial channel text:

```text
Node name: Possible fall: peak accel=2.80g rotation=260.0deg/s
```

The values are the detected sequence's peaks. `MyMesh::pollFallResponse()`
creates a separate event after three accepted PRG clicks, using `ack_message`
and the location selected at that event's creation:

```text
Node name: Fall Detected & User Has Requested Assistance | HOME
Node name: Fall Detected & User Has Requested Assistance | lat=37.123456, lon=-122.654321
```

Each controller owns one volatile event. Another event of the same kind coalesces
while its controller is busy, preserving the existing retry budget and text.
The two controllers can progress concurrently using distinct local packet tags.
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

The node name, event text, and selected location are frozen for retries. Each
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
`heltec_v4_sensor`. These settings are compile-time constants, not CLI commands.

| Setting | Default | Adjustment and effect |
| --- | --- | --- |
| `ack_channel_name`, `ack_channel_key` | `#falldetect` and its 16-byte hashtag key | Both fall streams use this destination. The label is diagnostic; the key selects the actual channel. Change them together and configure the same public channel on receiving companions. A companion's local slot number is not a destination. |
| `ack_message` | `Fall Detected & User Has Requested Assistance` | Exact post-click UTF-8 text. The initial `Possible fall` format is in `MyMesh::pollMotion()`. |
| `DefaultLocation` | `HOME` | Assistance suffix when the existing location provider supplies no usable fix. A valid fix is captured once per event; no GPS freshness limit is added. |
| `ack_path_hash_size` | `1` byte per hop | Existing public flood route encoding; allowed values 1, 2, or 3. |
| `ack_retry_ms` | `5000` ms | Delay after an allocation or local TX failure before the next attempt. |
| `ack_max_attempts` | `60` | Total allocation/enqueue attempts per event, including successful attempts. This also sizes stored attempt fingerprints. |
| `assistance_queue_timeout_ms` | `30000` ms | Maximum tracked queue residence before failure callback and retry. Shared by both streams. |
| `channel_repeat_wait_ms` | `15000` ms | Wait after each successful local TX for a matching repeat before retrying. |
| `channel_transmit_attempts` | `4` | Maximum locally successful sends without repeat evidence. Failed local sends still consume the total attempt budget. |
| `motion_fault_timeout_ms` | `5000` ms | Time without a fresh valid sample before reporting motion unavailable. It does not change the detector's sampling-gap guard. |
| `serial_rx_bytes_per_loop` | `16` bytes | Caps serial command input and character echo per loop pass. Command execution and other I/O still need hardware timing measurements. |

Despite their historical `ack_` names, these settings do not select a DM or enable
a recipient acknowledgment. Increasing attempt limits or shortening delays raises
radio traffic and battery use. The configured local attempt limits bound work;
they are not a promise of elapsed delivery time under a stalled main loop.
Text remains limited to 160 UTF-8 bytes including node name and the location suffix;
oversize text fails visibly rather than being truncated.

`FallDetectionConfig::max_sample_gap_ms` in `MotionRule.h` remains 100 ms, with
nominal sampling every 20 ms. `MotionHealth` uses that same threshold to count
completed fresh-valid sample intervals exceeding it. `max_gap_ms` is the longest
completed interval; a still-open outage is exposed by `motion=FAULT` after its
timeout and contributes a gap when sampling resumes. No interval is invented
before the first valid fresh sample. Recovery clears the health fault but does
not bypass the detector's normal quiet-rearm rules. Raising the gap threshold
cannot recover motion that was never sampled.

## Code ownership and review points

- `AssistanceDelivery` owns frozen payloads, attempt fingerprints, deadlines,
  counters, public-delivery states, and terminal reasons. `begin()` does not
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
- `FallResponse::messageCompleted()` releases the assistance-pending latch only
  at a terminal delivery outcome. Existing SOS and triple-click policy remains.
  Other SensorMesh alert uses, such as battery contact alerts, are separate from
  these two public fall-message streams.
- `MotionHealth` observes every sensor poll without changing the driver or
  detector. `main.cpp` prints fault/recovery transitions and exposes `health`.
  No new interrupt/FIFO sampling path or general blocking-I/O rewrite is included.
- `platformio.ini` adds `native_delivery` with production Mesh and Crypto sources
  and the small `test/transport_support` host adapters. It excludes the ordinary
  suite's crypto mocks. `verify.ps1` runs both `native` and `native_delivery` so
  the transport integration suite cannot be omitted from full verification.

Use serial at 115200 baud and enter `health` followed by carriage return. It reports
`fall=...; help=...; motion=...; max_gap_ms=...; gaps=...`. `monitoring` means no
prolonged sample outage is currently latched; it is not proof that the rule is armed
or that a person has not fallen. `motion` continues to show sensor measurements.

## Validation and further changes

Verified in the primary workspace on **2026-09-30** using `verify.ps1`:

| Check | Result |
| --- | --- |
| `heltec_v4_sensor` firmware and merged image | Passed; RAM 59,276 bytes, flash 1,179,681 bytes |
| `heltec_v4_companion_radio_ble` firmware and merged image | Passed; RAM 171,456 bytes, flash 1,264,413 bytes |
| `native`: existing scenarios, dispatcher and motion-health suites | 190/190 passed |
| `native_delivery`: production Mesh/crypto/channel integration | 22/22 passed |
| Total software tests | **212/212 passed** |

The local verification log is `.pio/message-delivery-verification.log` (ignored
build output). The 45 added cases comprise 18 dispatcher, 5 motion-health, and
22 transport integration cases. Earlier allocation-only tests were migrated to
the new delivery owner while preserving their sensor/SOS fault scenarios.
The OLED/UI files have no changes. No firmware was flashed or code pushed.

Run `.\verify.ps1` from the project root to build both firmware images, merge their
flash images, and execute all native suites. Focused tests are:

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
compare serial state with received public text and duplicates. Measure sampling
gaps under sustained radio/GPS/flash activity. No hardware is flashed by these tests.

To adjust behavior, first change the documented constants, rerun the focused
suites plus `verify.ps1`, and inspect transmitted text with a companion. To change
the state contract, update its native tests and this directive map together.
For rollback, preserve a Git snapshot of the whole delivery change and restore
the controller, integration, packet metadata, dispatcher, tests, and documentation
as a coherent set. Removing only the completion hook can strand pending requests.
Reboot still clears all delivery, SOS, partial-click, and detector state; no flash
persistence, automatic retry after reboot, or new manual resend command is added.
