"""Transport validation and complete synthetic capture-to-production replay checks."""
from pathlib import Path
import io
import os
import struct
import subprocess
import sys
import tempfile
import tracemalloc
import unittest
from unittest import mock

import health_identity
import health_replay


def record(kind, sequence, payload=b""):
    return struct.pack("<BHI", kind, len(payload), sequence) + payload


class FrameTests(unittest.TestCase):
    def setUp(self):
        self.records = record(1, 0, bytes(range(80))) + record(2, 1, bytes(range(100))) + record(12, 2, b"done")
        self.frames = health_replay.encode_frames(self.records, 65530)

    def test_round_trip_mixed_logs_and_packet_rollover(self):
        mixed = b"ordinary startup log\n" + self.frames.replace(b"\n", b"\nordinary diagnostic\n") + b"stopped\n"
        self.assertEqual(self.records, health_replay.decode_frames(mixed))

    def test_stream_boundaries_match_in_memory_decoder(self):
        mixed = b"ordinary\n" + self.frames.replace(b"\n", b"\ntext\n") + b"tail"
        expected = health_replay.decode_frames(mixed)
        # Sizes 1..35 split the separator, hex fields, CRC and LF at all offsets.
        for chunk_size in range(1, 36):
            with self.subTest(chunk_size=chunk_size):
                output = io.BytesIO()
                health_replay.decode_stream(io.BytesIO(mixed), output, chunk_size)
                self.assertEqual(expected, output.getvalue())

    def test_stream_rejects_crc_split_across_chunks(self):
        changed = bytearray(self.frames)
        changed[27] = ord("f") if changed[27] != ord("f") else ord("a")
        for chunk_size in (1, 27, 28, 29):
            with self.subTest(chunk_size=chunk_size):
                with self.assertRaisesRegex(health_replay.CaptureError, "CRC"):
                    health_replay.decode_stream(io.BytesIO(changed), io.BytesIO(), chunk_size)

    def test_stream_rejects_partial_frame_at_eof(self):
        for suffix in (b"\x1e", self.frames[:15], self.frames[:-1]):
            with self.subTest(suffix_length=len(suffix)):
                with self.assertRaisesRegex(health_replay.CaptureError, "malformed/truncated"):
                    health_replay.decode_stream(io.BytesIO(b"log\n" + suffix), io.BytesIO(), 7)

    def test_stream_does_not_accumulate_long_ordinary_text(self):
        class LongLog:
            def __init__(self, frames):
                self.remaining = 32 * 1024 * 1024
                self.tail = io.BytesIO(frames)
                self.largest_read = 0

            def read(self, size):
                self.largest_read = max(self.largest_read, size)
                if not 0 < size <= health_replay.STREAM_CHUNK_SIZE:
                    raise AssertionError("unbounded input read")
                if self.remaining:
                    count = min(size, self.remaining)
                    self.remaining -= count
                    return b"x" * count  # No newline in the entire 32 MiB prefix.
                return self.tail.read(size)

        source, output = LongLog(self.frames), io.BytesIO()
        tracemalloc.start()
        try:
            health_replay.decode_stream(source, output)
            _, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertLess(peak, 1024 * 1024)
        self.assertLessEqual(source.largest_read, health_replay.STREAM_CHUNK_SIZE)
        self.assertEqual(self.records, output.getvalue())

    def test_stream_rejects_short_destination_write(self):
        class ShortSink:
            def write(self, block):
                return len(block) - 1

        with self.assertRaisesRegex(health_replay.CaptureError, "short write"):
            health_replay.decode_stream(io.BytesIO(self.frames), ShortSink())

    def test_bad_crc_rejected(self):
        changed = bytearray(self.frames)
        changed[12] = ord("f") if changed[12] != ord("f") else ord("a")
        with self.assertRaisesRegex(health_replay.CaptureError, "CRC"):
            health_replay.decode_frames(changed)

    def test_missing_fragment_rejected(self):
        with self.assertRaisesRegex(health_replay.CaptureError, "sequence gap"):
            health_replay.decode_frames(self.frames[:31] + self.frames[62:])

    def test_missing_record_rejected(self):
        wire = health_replay.encode_frames(record(1, 0) + record(12, 2))
        with self.assertRaisesRegex(health_replay.CaptureError, "record sequence gap"):
            health_replay.decode_frames(wire)

    def test_nonzero_padding_rejected(self):
        body = b"S0000" + (record(1, 0) + b"\0\0\1").hex().encode()
        crc = health_replay.binascii.crc_hqx(body, 0xFFFF)
        with self.assertRaisesRegex(health_replay.CaptureError, "padding"):
            health_replay.decode_frames(b"\x1e" + body + f"{crc:04x}".encode() + b"\n")

    def test_truncated_short_write_rejected(self):
        with self.assertRaisesRegex(health_replay.CaptureError, "malformed/truncated"):
            health_replay.decode_frames(self.frames[:-3])

    def test_continuation_without_begin_rejected(self):
        with self.assertRaisesRegex(health_replay.CaptureError, "continuation"):
            health_replay.decode_frames(self.frames[31:])

    def test_oversized_record_rejected(self):
        wire = health_replay.encode_frames(record(1, 0, b"x" * 2300))
        with self.assertRaisesRegex(health_replay.CaptureError, "kind/length"):
            health_replay.decode_frames(wire)

    def test_no_recording_rejected(self):
        with self.assertRaisesRegex(health_replay.CaptureError, "no motion"):
            health_replay.decode_frames(b"ordinary logs only\n")


class IdentityTests(unittest.TestCase):
    def test_selected_board_and_pin_changes_change_identity(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            pins = root / "boards/variants/heltec_v4/pins_arduino.h"
            pins.parent.mkdir(parents=True)
            pins.write_text("pin 1")
            board = root / "boards/heltec_v4.json"
            board.write_text('{"usb_mode":1}')
            first = health_identity.source_manifest(root)["source_id"]
            board.write_text('{"usb_mode":0}')
            second = health_identity.source_manifest(root)["source_id"]
            pins.write_text("pin 2")
            third = health_identity.source_manifest(root)["source_id"]
            self.assertEqual(3, len({first, second, third}))

    def test_untracked_and_modified_inputs_change_identity(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "include").mkdir()
            setting = root / "include/HealthNodeConfig.h"
            setting.write_text("setting 1")
            first = health_identity.source_manifest(root)["source_id"]
            setting.write_text("setting 2")
            second = health_identity.source_manifest(root)["source_id"]
            (root / "include/new_untracked.h").write_text("new source")
            third = health_identity.source_manifest(root)["source_id"]
            self.assertEqual(3, len({first, second, third}))

    def test_manifest_contains_hashes_without_source_contents(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "include").mkdir()
            (root / "include/private.h").write_text("PRIVATE_VALUE_SENTINEL")
            manifest = health_identity.source_manifest(root)
            self.assertNotIn("PRIVATE_VALUE_SENTINEL", str(manifest))


class CommandTests(unittest.TestCase):
    def test_decoded_export_cannot_replace_original_capture(self):
        with tempfile.TemporaryDirectory() as folder:
            capture = Path(folder) / "capture.bin"
            original = b"original mixed USB recording"
            capture.write_bytes(original)
            error = io.StringIO()
            with mock.patch.object(sys, "argv", ["health_replay.py", str(capture), "--decoded", str(capture)]), \
                    mock.patch.object(health_replay, "decode_stream") as decode, \
                    mock.patch("sys.stderr", error):
                self.assertEqual(1, health_replay.main())
            decode.assert_not_called()
            self.assertEqual(original, capture.read_bytes())
            self.assertIn("original recording", error.getvalue())

    def test_decoded_export_cannot_replace_capture_through_hard_link(self):
        with tempfile.TemporaryDirectory() as folder:
            capture = Path(folder) / "capture.bin"
            alias = Path(folder) / "decoded.bin"
            original = b"original mixed USB recording"
            capture.write_bytes(original)
            alias.hardlink_to(capture)
            error = io.StringIO()
            with mock.patch.object(sys, "argv", ["health_replay.py", str(capture), "--decoded", str(alias)]), \
                    mock.patch.object(health_replay, "decode_stream") as decode, \
                    mock.patch("sys.stderr", error):
                self.assertEqual(1, health_replay.main())
            decode.assert_not_called()
            self.assertEqual(original, capture.read_bytes())
            self.assertIn("original recording", error.getvalue())


class ProductionRoundTripTests(unittest.TestCase):
    def test_capture_parse_replay_repeats_identically_and_rejects_source_drift(self):
        project = Path(__file__).resolve().parents[1]
        compiler = health_replay.compiler_path(project, None)
        source = health_identity.source_manifest(project)["source_id"]
        runtime = os.environ.copy()
        runtime["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + runtime.get("PATH", "")
        with tempfile.TemporaryDirectory(prefix="shn-capture-test-") as folder:
            folder = Path(folder)
            suffix = ".exe" if sys.platform == "win32" else ""
            generator, executable = folder / ("fixture" + suffix), folder / ("replay" + suffix)
            subprocess.run([compiler, "-std=c++17", "-O2", "-fno-fast-math", "-ffp-contract=off",
                            "-I", str(project / "include"), "-I", str(project / "src"),
                            "-I", str(project / "test/mocks"), str(project / "test/host/health_capture_fixture.cpp"),
                            "-o", str(generator)], check=True, env=runtime)
            health_replay.build_replayer(project, executable, compiler)
            raw = folder / "capture.bin"
            subprocess.run([str(generator), str(raw), source], check=True, env=runtime)
            wire = health_replay.encode_frames(raw.read_bytes(), 65520)
            decoded = health_replay.decode_frames(b"diagnostic\n" + wire + b"stopped\n")
            self.assertEqual(raw.read_bytes(), decoded)
            raw.write_bytes(decoded)
            results = [subprocess.run([str(executable), str(raw), source], check=True, env=runtime,
                                      capture_output=True, text=True).stdout for _ in range(3)]
            self.assertEqual(1, len(set(results)))
            self.assertIn("falls=1", results[0])
            self.assertIn("candidate_expirations=1", results[0])
            rejected = subprocess.run([str(executable), str(raw), "0" * 64], env=runtime,
                                      capture_output=True, text=True)
            self.assertNotEqual(0, rejected.returncode)
            self.assertIn("identity mismatch", rejected.stderr)


if __name__ == "__main__":
    unittest.main()
