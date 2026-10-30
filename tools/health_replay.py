"""Validate a mixed USB log and replay one complete SHN motion session.

The optional utility compiles the production header-based detector/response on
the host. It is never needed by the running companion firmware.
"""
from __future__ import annotations

import argparse
import binascii
import hashlib
import io
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from health_identity import source_manifest

FRAME = re.compile(rb"\x1e([SM])([0-9a-fA-F]{4})([0-9a-fA-F]{20})([0-9a-fA-F]{4})\n")
MAX_PAYLOAD = 2208
STREAM_CHUNK_SIZE = 64 * 1024


class CaptureError(ValueError):
    pass


def file_identity(path: Path) -> tuple[int, str]:
    """Fingerprint a bounded stream without retaining capture contents."""
    digest, size = hashlib.sha256(), 0
    with path.open("rb") as source:
        while chunk := source.read(STREAM_CHUNK_SIZE):
            digest.update(chunk)
            size += len(chunk)
    return size, digest.hexdigest()


def replay_inputs(project: Path) -> dict:
    """Identify maintained firmware and host replay inputs around an execution.

    Host headers and replay sources are outside the firmware source-ID scope.
    Compiler packages and installed firmware remain separate evidence.
    """
    paths = {Path(__file__).resolve(), Path(__file__).with_name("health_identity.py").resolve(),
             project / "tools/health_replay.cpp", project / "tools/health_replay_engine.h"}
    mocks = project / "test/mocks"
    if mocks.is_dir():
        paths.update(path for path in mocks.rglob("*") if path.is_file() and path.suffix in (".h", ".hpp"))
    return {"source": source_manifest(project),
            "tools": {str(path): file_identity(path) if path.is_file() else None for path in paths}}


def decode_stream(source, destination, chunk_size: int = STREAM_CHUNK_SIZE) -> None:
    """Validate frames incrementally and write decoded records to a binary sink.

    Every RS byte belongs to a frame. CRC, packet sequence, record boundaries,
    padding and record sequence are checked; corrupt/lost input is not repaired.
    Input chunks are at most 64 KiB. Only an unfinished 31-byte frame and one
    bounded record survive a chunk boundary; ordinary text is never retained.
    The destination may contain a valid prefix if later input is rejected, so
    callers must keep it temporary until complete semantic replay succeeds.
    """
    if not isinstance(chunk_size, int) or not 1 <= chunk_size <= STREAM_CHUNK_SIZE:
        raise ValueError("chunk_size must be between 1 and 65536 bytes")
    record = bytearray()
    expected_packet = None
    expected_record = 0
    target = None
    frames = 0
    carry = b""
    carry_at = 0
    bytes_read = 0
    while True:
        chunk = source.read(chunk_size)
        if not chunk:
            break
        data = carry + chunk
        base = bytes_read - len(carry)
        bytes_read += len(chunk)
        carry = b""
        offset = 0
        while True:
            start = data.find(b"\x1e", offset)
            if start < 0:
                break
            if len(data) - start < 31:
                carry, carry_at = data[start:], base + start
                break
            match = FRAME.match(data, start)
            if match is None:
                raise CaptureError(f"malformed/truncated frame at byte {base + start}")
            offset = match.end()
            marker, sequence_hex, payload_hex, checksum_hex = match.groups()
            sequence = int(sequence_hex, 16)
            if binascii.crc_hqx(data[start + 1:start + 26], 0xFFFF) != int(checksum_hex, 16):
                raise CaptureError(f"frame CRC mismatch at packet {sequence}")
            if expected_packet is not None and sequence != expected_packet:
                raise CaptureError(f"packet sequence gap: expected {expected_packet}, received {sequence} (export loss)")
            expected_packet = (sequence + 1) & 0xFFFF
            frames += 1
            if marker == b"S":
                if record:
                    raise CaptureError("new record before previous record completed (export loss)")
                target = None
            elif not record:
                raise CaptureError("continuation without record start; incomplete session")
            record.extend(bytes.fromhex(payload_hex.decode("ascii")))
            if target is None and len(record) >= 7:
                kind, length, sequence_record = struct.unpack_from("<BHI", record)
                if not 1 <= kind <= 12 or length > MAX_PAYLOAD:
                    raise CaptureError("invalid record kind/length")
                if sequence_record != expected_record:
                    raise CaptureError(f"record sequence gap: expected {expected_record}, received {sequence_record} (recording loss)")
                if expected_record == 0 and kind != 1:
                    raise CaptureError("session must start with Begin")
                target = length + 7
            if target is not None and len(record) >= target:
                if any(record[target:]):
                    raise CaptureError("nonzero record padding")
                if destination.write(record[:target]) != target:
                    raise CaptureError("short write to decoded-record destination")
                record.clear()
                target = None
                expected_record += 1
    if carry:
        raise CaptureError(f"malformed/truncated frame at byte {carry_at}")
    if record:
        raise CaptureError("truncated final record")
    if not frames:
        raise CaptureError("no motion capture frames found")


def decode_frames(data: bytes) -> bytes:
    """In-memory convenience wrapper for small fixtures; the CLI streams files."""
    output = io.BytesIO()
    decode_stream(io.BytesIO(data), output)
    return output.getvalue()


def encode_frames(records: bytes, first_packet: int = 0) -> bytes:
    """Deterministic fixture encoder using the same documented transport format."""
    output = bytearray()
    offset = 0
    packet = first_packet
    while offset < len(records):
        if len(records) - offset < 7:
            raise CaptureError("truncated fixture record")
        length = struct.unpack_from("<H", records, offset + 1)[0] + 7
        record = records[offset:offset + length]
        if len(record) != length:
            raise CaptureError("truncated fixture payload")
        for part in range(0, length, 10):
            fragment = record[part:part + 10].ljust(10, b"\0")
            body = (b"S" if part == 0 else b"M") + f"{packet:04x}".encode() + fragment.hex().encode()
            crc = binascii.crc_hqx(body, 0xFFFF)
            output.extend(b"\x1e" + body + f"{crc:04x}".encode() + b"\n")
            packet = (packet + 1) & 0xFFFF
        offset += length
    return bytes(output)


def compiler_path(project: Path, requested: str | None) -> str:
    if requested:
        return requested
    bundled = project / ".pio/tools/mingw64/bin/g++.exe"
    if bundled.is_file():
        return str(bundled)
    compiler = shutil.which("g++")
    if not compiler:
        raise CaptureError("g++ unavailable; supply --compiler")
    return compiler


def build_replayer(project: Path, destination: Path, compiler: str) -> None:
    command = [compiler, "-std=c++17", "-O2", "-fno-fast-math", "-ffp-contract=off",
               "-I", str(project / "include"), "-I", str(project / "src"),
               "-I", str(project / "test/mocks"), str(project / "tools/health_replay.cpp"), "-o", str(destination)]
    subprocess.run(command, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path, help="binary-preserved mixed USB log")
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--compiler", help="C++ compiler path; defaults to repository MinGW or g++")
    parser.add_argument("--decoded", type=Path, help="optional validated raw record output")
    parser.add_argument("--repeat", type=int, default=2, help="determinism repetitions (1..10; default 2)")
    args = parser.parse_args()
    try:
        if not 1 <= args.repeat <= 10:
            raise CaptureError("--repeat must be between 1 and 10")
        # A decoded export is a different format. Preserve the original evidence
        # even when the output uses a relative alias, symlink or hard link.
        if args.decoded and (args.decoded.resolve() == args.recording.resolve() or
                             (args.decoded.exists() and args.decoded.samefile(args.recording))):
            raise CaptureError("--decoded must not replace the original recording")
        if args.decoded and args.decoded.exists():
            raise CaptureError("--decoded destination already exists; existing evidence is retained")
        project = args.project.resolve()
        if not args.recording.is_file():
            raise CaptureError("recording must be a regular file")
        original_capture = file_identity(args.recording)
        original_inputs = replay_inputs(project)
        source = original_inputs["source"]["source_id"]
        with tempfile.TemporaryDirectory(prefix="shn-replay-") as directory:
            folder = Path(directory)
            decoded = folder / "decoded.bin"
            with args.recording.open("rb") as captured, decoded.open("wb") as destination:
                decode_stream(captured, destination)
            executable = folder / ("health_replay.exe" if sys.platform == "win32" else "health_replay")
            compiler = compiler_path(project, args.compiler)
            build_replayer(project, executable, compiler)
            runtime = os.environ.copy()
            runtime["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + runtime.get("PATH", "")
            outputs = []
            for _ in range(args.repeat):
                result = subprocess.run([str(executable), str(decoded), source], check=True,
                                        capture_output=True, text=True, env=runtime)
                outputs.append(result.stdout)
            if len(set(outputs)) != 1:
                raise CaptureError("repeat runs produced different outcomes")
            if original_capture != file_identity(args.recording):
                raise CaptureError("recording changed during replay; decoded output was not published")
            if original_inputs != replay_inputs(project):
                raise CaptureError("source or host replay inputs changed during replay; decoded output was not published")
            if args.decoded:
                # Exclusive creation also protects evidence created after the
                # initial check while compilation/replay was in progress.
                with decoded.open("rb") as validated, args.decoded.open("xb") as destination:
                    shutil.copyfileobj(validated, destination, STREAM_CHUNK_SIZE)
            print(outputs[0].strip())
            print(f"Determinism: {args.repeat} identical run(s). Source identity: {source}")
        return 0
    except subprocess.CalledProcessError as error:
        print((error.stderr or str(error)).strip(), file=sys.stderr)
        return 1
    except (CaptureError, OSError) as error:
        print(f"Capture rejected: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
