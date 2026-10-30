"""Preserve mixed USB evidence and extract explicitly bounded v1 sessions.

The optional host utility reads regular files or an already-open binary stream.
It does not discover or open serial devices. Transport-complete extraction is
separate from the existing strict production-rule replay.
"""
from __future__ import annotations

import argparse
import binascii
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import time

from health_replay import CaptureError, FRAME, MAX_PAYLOAD, STREAM_CHUNK_SIZE

TARGET = "heltec_v4_companion_radio_ble"
PAYLOAD_LENGTHS = {2: 93, 3: 27, 4: 23, 5: 23, 6: 22, 7: 31,
                   8: 26, 9: 46, 10: 5, 11: 20, 12: 28}


def json_line(destination, value: dict) -> None:
    destination.write(json.dumps(value, sort_keys=True, allow_nan=False) + "\n")


def write_json(path: Path, value: dict) -> None:
    """Create one metadata artifact exclusively; existing evidence is immutable."""
    with path.open("x", encoding="utf-8", newline="\n") as destination:
        json.dump(value, destination, indent=2, sort_keys=True, allow_nan=False)
        destination.write("\n")


def file_hash(path: Path) -> dict:
    digest, size = hashlib.sha256(), 0
    with path.open("rb") as source:
        while block := source.read(STREAM_CHUNK_SIZE):
            digest.update(block)
            size += len(block)
    return {"bytes": size, "sha256": digest.hexdigest()}


def copy_range(source: Path, destination: Path, start: int, end: int) -> dict:
    """Copy an exact half-open raw interval without line or newline conversion."""
    digest, remaining = hashlib.sha256(), end - start
    with source.open("rb") as incoming, destination.open("xb") as outgoing:
        incoming.seek(start)
        while remaining:
            block = incoming.read(min(remaining, STREAM_CHUNK_SIZE))
            if not block:
                raise CaptureError("raw evidence changed or ended during copying")
            if outgoing.write(block) != len(block):
                raise CaptureError("short write preserving raw evidence")
            digest.update(block)
            remaining -= len(block)
    return {"bytes": end - start, "sha256": digest.hexdigest()}


def collect_streams(connections, output: Path, clock=None) -> dict:
    """Collect (label, binary-reader) connections; failed reads retain their prefix.

    A read returning b'' closes that connection. A reconnect is a new reader and
    an explicit boundary, even if no bytes were lost. Exceptions are recorded by
    class rather than by potentially secret-bearing device/OS error text.
    Readers are caller-owned. Memory and individual read requests are bounded.
    """
    output.mkdir(parents=True, exist_ok=False)
    clock = clock or (lambda: {"utc": datetime.now(timezone.utc).isoformat(),
                              "monotonic_ns": time.monotonic_ns()})
    digest, offset, count, errors = hashlib.sha256(), 0, 0, 0
    with (output / "raw.log").open("xb") as raw, \
            (output / "connections.jsonl").open("x", encoding="utf-8", newline="\n") as events:
        for count, (label, reader) in enumerate(connections, 1):
            if not isinstance(label, str) or not label or len(label) > 128 or not label.isprintable():
                raise CaptureError("connection labels must be 1..128 printable characters")
            start, reason = offset, "eof"
            json_line(events, {"event": "open", "connection": count, "label": label,
                               "offset": offset, "host_clock": clock()})
            while True:
                try:
                    block = reader.read(STREAM_CHUNK_SIZE)
                    if not isinstance(block, bytes) or len(block) > STREAM_CHUNK_SIZE:
                        raise ValueError("reader must return bounded binary chunks")
                except Exception as error:
                    reason = "read_error"
                    errors += 1
                    json_line(events, {"event": "read_error", "connection": count,
                                       "offset": offset, "error_class": type(error).__name__,
                                       "host_clock": clock()})
                    break
                if not block:
                    break
                if raw.write(block) != len(block):
                    raise CaptureError("short write collecting raw evidence")
                digest.update(block)
                offset += len(block)
            json_line(events, {"event": "close", "connection": count, "offset": offset,
                               "start": start, "reason": reason, "host_clock": clock()})
    summary = {"schema": 1, "raw": {"bytes": offset, "sha256": digest.hexdigest()},
               "connections": count, "read_errors": errors,
               "connection_log": file_hash(output / "connections.jsonl"),
               "clock_scope": "host read/open/close observations; not sensor acquisition time"}
    write_json(output / "collection.json", summary)
    return summary


def connection_ranges(collection: Path | None, raw: dict):
    """Validate collector identity and stream its contiguous connection ranges.

    A missing/unfinished sidecar is rejected rather than silently joining a
    disconnect. No arbitrary marker strings in console text define boundaries.
    """
    if collection is None:
        yield 0, raw["bytes"], None
        return
    with (collection / "collection.json").open("rb") as source:
        encoded = source.read(65537)
    if len(encoded) > 65536:
        raise CaptureError("oversized collection manifest")
    manifest = json.loads(encoded)
    if not isinstance(manifest, dict):
        raise CaptureError("collection manifest must be an object")
    if manifest.get("schema") != 1 or manifest.get("raw") != raw:
        raise CaptureError("collection metadata does not match the raw evidence")
    events_path = collection / "connections.jsonl"
    if file_hash(events_path) != manifest.get("connection_log"):
        raise CaptureError("collection connection log hash mismatch")
    offset, number, opened, read_errors, failed_at = 0, 0, None, 0, None
    with events_path.open(encoding="utf-8") as events:
        while line := events.readline(4097):
            if len(line) > 4096:
                raise CaptureError("oversized collection metadata record")
            item = json.loads(line)
            if not isinstance(item, dict):
                raise CaptureError("connection event must be an object")
            event = item.get("event")
            if event == "open":
                number += 1
                if opened is not None or item.get("offset") != offset or item.get("connection") != number:
                    raise CaptureError("invalid connection-open boundary")
                opened = item
            elif event == "read_error":
                if opened is None or failed_at is not None or item.get("connection") != number or \
                        type(item.get("offset")) is not int or not offset <= item["offset"] <= raw["bytes"]:
                    raise CaptureError("invalid connection-error boundary")
                failed_at = item["offset"]
                read_errors += 1
            elif event == "close":
                end = item.get("offset")
                if opened is None or item.get("connection") != number or item.get("start") != offset or \
                        type(end) is not int or not offset <= end <= raw["bytes"] or item.get("reason") not in ("eof", "read_error") or \
                        (item["reason"] == "read_error") != (failed_at is not None) or (failed_at is not None and failed_at != end):
                    raise CaptureError("invalid connection-close boundary")
                yield offset, end, {"connection": number, "open": opened, "close": item}
                offset, opened, failed_at = end, None, None
            else:
                raise CaptureError("unknown connection event")
    if opened is not None or offset != raw["bytes"] or number != manifest.get("connections") or read_errors != manifest.get("read_errors"):
        raise CaptureError("incomplete collection metadata")


def frames(source, start: int, end: int, chunk_size=STREAM_CHUNK_SIZE):
    """Yield valid frames or explicit corrupt spans, with absolute raw offsets."""
    if not 1 <= chunk_size <= STREAM_CHUNK_SIZE:
        raise ValueError("invalid frame chunk size")
    source.seek(start)
    at, carry = start, b""
    while at < end:
        block = source.read(min(chunk_size, end - at))
        if not block:
            raise CaptureError("raw input shortened during extraction")
        base = at - len(carry)
        data, at, carry = carry + block, at + len(block), b""
        scan = 0
        while True:
            begin = data.find(b"\x1e", scan)
            if begin < 0:
                break
            if len(data) - begin < 31:
                carry = data[begin:]
                break
            match = FRAME.match(data, begin)
            if match is None:
                yield base + begin, base + begin + 1, None, "malformed frame"
                scan = begin + 1
                continue
            scan = match.end()
            marker, packet_hex, payload, crc = match.groups()
            if binascii.crc_hqx(data[begin + 1:begin + 26], 0xFFFF) != int(crc, 16):
                yield base + begin, base + scan, None, "frame CRC mismatch"
            else:
                yield base + begin, base + scan, (marker, int(packet_hex, 16), bytes.fromhex(payload.decode())), None
    if carry:
        yield end - len(carry), end, None, "truncated frame at connection end"


def records(source, start: int, end: int, chunk_size=STREAM_CHUNK_SIZE):
    """Assemble bounded records; recovery requires a complete valid new S record."""
    pending, target, begin, first, last = bytearray(), 0, 0, 0, 0
    for at, stop, frame, error in frames(source, start, end, chunk_size):
        if error:
            pending.clear()
            yield at, stop, None, error
            continue
        marker, packet, fragment = frame
        if marker == b"S":
            if pending:
                yield begin, at, None, "new record before previous record completed"
            pending, begin, first, target = bytearray(), at, packet, 0
        elif not pending:
            yield at, stop, None, "continuation without record start"
            continue
        elif packet != (last + 1) & 0xFFFF:
            pending.clear()
            yield at, stop, None, "packet sequence gap within record"
            continue
        last = packet
        pending.extend(fragment)
        if not target:
            kind, length, sequence = struct.unpack_from("<BHI", pending)
            if not 1 <= kind <= 12 or length > MAX_PAYLOAD:
                pending.clear()
                yield at, stop, None, "invalid record kind or length"
                continue
            target = length + 7
        if len(pending) >= target:
            if any(pending[target:]):
                yield begin, stop, None, "nonzero record padding"
            else:
                yield begin, stop, (first, last, bytes(pending[:target])), None
            pending.clear()
    if pending:
        yield begin, end, None, "truncated record at connection end"


def begin_identity(record: bytes) -> dict:
    payload = record[7:]
    if len(payload) < 100 or struct.unpack_from("<H", payload)[0] != 1:
        raise CaptureError("unsupported or truncated Begin")
    source = payload[2:66]
    config, boot, session, wall, sample, generation, discarded, length = struct.unpack_from("<IQIIIIIH", payload, 66)
    if re.fullmatch(rb"[0-9a-f]{64}", source) is None or not boot or not session or not 0 < length <= 2048 or length != len(payload) - 100:
        raise CaptureError("invalid Begin identity or checkpoint boundary")
    return {"source_id": source.decode(), "configuration_id": f"{config:08x}",
            "boot_id": f"{boot:016x}", "session_id": session, "start_ms": wall,
            "sample_sequence": sample, "acquisition_generation": generation,
            "discarded_lower_bound": discarded}


def extract_sessions(raw_path: Path, output: Path, ranges, chunk_size=STREAM_CHUNK_SIZE) -> dict:
    """Partition all raw intervals; later complete sessions never repair old loss.

    Only a validated Begin can resynchronize after damage. Reconnection ends the
    current segment, including a partial frame/record. Index/issue streams and
    on-disk identity claims keep memory independent of recording duration.
    """
    (output / "sessions").mkdir()
    (output / "identities").mkdir()
    counts = {"transport_complete": 0, "rejected": 0, "incomplete": 0, "ordinary_text": 0,
              "issues": 0, "connections": 0}
    segment_number = 0
    # A reconnect ends sample/record continuity but does not erase a known
    # same-boot session/packet gap in the next complete session's identity.
    previous_complete = None
    with raw_path.open("rb") as source, \
            (output / "segments.jsonl").open("x", encoding="utf-8", newline="\n") as index, \
            (output / "issues.jsonl").open("x", encoding="utf-8", newline="\n") as issues:
        for start, end, connection in ranges:
            counts["connections"] += 1
            region, identity, damaged, issue_count = start, None, False, 0
            next_record, next_packet, sample, generation, discarded = 0, None, 0, 0, 0
            calibration_count = 0

            def issue(at, stop, reason):
                nonlocal damaged, issue_count
                json_line(issues, {"start": at, "end": stop, "reason": reason,
                                   "connection": counts["connections"]})
                counts["issues"] += 1
                issue_count += 1
                damaged = True

            def finish(stop, status):
                nonlocal region, identity, damaged, issue_count, segment_number, previous_complete
                if stop == region and identity is None and not issue_count:
                    return
                segment_number += 1
                item = {"segment": segment_number, "start": region, "end": stop,
                        "status": status, "issue_count": issue_count,
                        "connection": counts["connections"], "identity": identity}
                if status == "transport_complete":
                    name = f"sessions/{segment_number:06d}.log"
                    item["wire"] = {"path": name, **copy_range(raw_path, output / name, region, stop)}
                    item["records"] = next_record
                    item["calibration_records"] = calibration_count
                    item["semantic_replay"] = "not_yet_checked"
                    previous_complete = (identity["boot_id"], identity["session_id"], next_packet)
                json_line(index, item)
                counts[status] += 1
                region, identity, damaged, issue_count = stop, None, False, 0

            for at, stop, item, error in records(source, start, end, chunk_size):
                if error:
                    issue(at, stop, error)
                    continue
                first, last, record = item
                kind, length, sequence = struct.unpack_from("<BHI", record)
                if kind == 1 and sequence == 0:
                    try:
                        candidate = begin_identity(record)
                    except CaptureError as error:
                        issue(at, stop, str(error))
                        continue
                    if previous_complete is not None and candidate["boot_id"] == previous_complete[0]:
                        if candidate["session_id"] != previous_complete[1] + 1 or first != previous_complete[2]:
                            issue(at, at, "packet/session gap between complete sessions; intervening evidence is unknown")
                    if identity is not None and not damaged:
                        issue(at, at, "new Begin before End")
                        finish(at, "incomplete")
                    else:
                        finish(at, "rejected" if damaged else "ordinary_text")
                    region, identity = at, candidate
                    next_record, next_packet = 1, (last + 1) & 0xFFFF
                    sample, generation, discarded = (candidate[key] for key in
                        ("sample_sequence", "acquisition_generation", "discarded_lower_bound"))
                    calibration_count = 0
                    claim = output / "identities" / f"{candidate['boot_id']}-{candidate['session_id']:08x}"
                    try:
                        with claim.open("x", encoding="ascii") as unique:
                            unique.write(candidate["source_id"] + "\n")
                    except FileExistsError:
                        issue(at, stop, "duplicate boot/session identity")
                    continue
                if identity is None:
                    issue(at, stop, "record outside a validated Begin/End session")
                    continue
                if damaged:
                    continue
                if sequence != next_record or first != next_packet:
                    issue(at, stop, "packet or record sequence gap")
                    continue
                if length != PAYLOAD_LENGTHS.get(kind):
                    issue(at, stop, "invalid v1 operation payload length")
                    continue
                next_record, next_packet = next_record + 1, (last + 1) & 0xFFFF
                if kind == 2:
                    observed = struct.unpack_from("<I", record, 7)[0]
                    if observed != (sample + 1) & 0xFFFFFFFF:
                        issue(at, stop, "sample sequence gap")
                    sample = observed
                elif kind == 9:
                    calibration_count += 1
                elif kind == 11:
                    generation, discarded = struct.unpack_from("<II", record, 11)
                elif kind == 12:
                    _, final_sample, final_generation, final_discarded, dropped, short, issued = struct.unpack_from("<7I", record, 7)
                    if dropped or short or (final_sample, final_generation, final_discarded, issued) != (sample, generation, discarded, next_record):
                        issue(at, stop, "End reports recording loss or inconsistent counters")
                        finish(stop, "rejected")
                    else:
                        finish(stop, "transport_complete")
            if identity is not None and not damaged:
                issue(end, end, "session ended without End at connection boundary")
                finish(end, "incomplete")
            else:
                finish(end, "rejected" if damaged else "ordinary_text")
            if connection is not None and connection["close"]["reason"] == "read_error":
                issue(end, end, "collector read error; unknown bytes may be missing")
    return counts


def main() -> int:
    from health_capture_provenance import create_bundle, verify_bundle
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    collect = sub.add_parser("collect", help="preserve regular-file or stdin connections; never opens serial devices")
    collect.add_argument("output", type=Path, help="new evidence directory; must not exist")
    collect.add_argument("--input", action="append", required=True, help="regular file, or '-' for binary stdin; repeat for reconnects")
    bundle = sub.add_parser("bundle", help="copy raw evidence, extract sessions and associate allowlisted provenance")
    bundle.add_argument("recording", type=Path)
    bundle.add_argument("output", type=Path, help="new bundle directory; must not exist")
    bundle.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[1])
    bundle.add_argument("--snapshot-reference", required=True, help="explicit retained matching source/configuration snapshot reference")
    bundle.add_argument("--collection", type=Path, help="collector directory supplying validated connection boundaries")
    bundle.add_argument("--metadata", type=Path, help="allowlisted operator/build JSON; no arbitrary flags or configuration exports")
    bundle.add_argument("--dependency", action="append", default=[], metavar="NAME=DIRECTORY", help="explicit dependency tree to hash without copying contents")
    bundle.add_argument("--image", action="append", default=[], metavar="ROLE=FILE", help="application, merged, bootloader or partitions image to hash")
    bundle.add_argument("--replay", action="store_true", help="run existing strict production replayer on each transport-complete session")
    bundle.add_argument("--compiler", help="C++17 replay compiler; defaults to repository MinGW or g++")
    bundle.add_argument("--repeat", type=int, default=2, help="replay determinism repetitions (1..10)")
    verify = sub.add_parser("verify", help="check local bundle artifact hashes; does not verify installed firmware")
    verify.add_argument("bundle", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "collect":
            if args.input.count("-") > 1:
                raise CaptureError("binary stdin can be consumed only once")
            for value in args.input:
                if value != "-" and not Path(value).is_file():
                    raise CaptureError("collection inputs must be regular files or binary stdin")
            def inputs():
                for number, value in enumerate(args.input, 1):
                    if value == "-":
                        yield f"stdin-{number}", sys.stdin.buffer
                    else:
                        with Path(value).open("rb") as stream:
                            yield f"input-{number}", stream
            result = collect_streams(inputs(), args.output)
            print(json.dumps(result, sort_keys=True))
            return int(bool(result["read_errors"]))
        if args.command == "verify":
            result = verify_bundle(args.bundle)
        else:
            result = create_bundle(args)
        print(json.dumps(result, sort_keys=True))
        return int(not result["ok"])
    except (CaptureError, OSError, ValueError, KeyError) as error:
        print(f"Capture operation failed; any existing evidence is retained: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
