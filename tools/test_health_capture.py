"""Fake-stream collection, lossy-session extraction and provenance regressions."""
from argparse import Namespace
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import tracemalloc
import unittest
from unittest import mock

import health_capture as capture
import health_capture_provenance as provenance
import health_identity
import health_replay


def record(kind, sequence, payload=b""):
    return struct.pack("<BHI", kind, len(payload), sequence) + payload


def session(source="a" * 64, boot=1, number=1, sample=0, first_packet=65520, dropped=0):
    # A structurally valid one-byte checkpoint intentionally cannot pass semantic
    # production replay. Structural extraction must never imply replay success.
    begin = struct.pack("<H", 1) + source.encode() + struct.pack("<IQIIIIIH", 1, boot, number, 0, sample, 0, 0, 1) + b"\0"
    end = struct.pack("<7I", 0, sample, 0, 0, dropped, 0, 2)
    return health_replay.encode_frames(record(1, 0, begin) + record(12, 1, end), first_packet)


def json_rows(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="shn-collection-test-")
        self.root = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)

    def extract(self, wire, name="extract", ranges=None, chunk_size=65536):
        output = self.root / name
        output.mkdir()
        raw = output / "raw.log"
        raw.write_bytes(wire)
        counts = capture.extract_sessions(raw, output, ranges or [(0, len(wire), None)], chunk_size)
        return output, counts

    def test_collection_preserves_binary_and_connection_clocks(self):
        first, second = b"ordinary\r\n\0\xff\x1e", b"second\n"
        output = self.root / "collected"
        summary = capture.collect_streams([("first", io.BytesIO(first)), ("reconnect", io.BytesIO(second))], output,
                                          clock=lambda: {"utc": "2026-10-29T00:00:00+00:00", "monotonic_ns": 10})
        self.assertEqual(first + second, (output / "raw.log").read_bytes())
        self.assertEqual(hashlib.sha256(first + second).hexdigest(), summary["raw"]["sha256"])
        ranges = list(capture.connection_ranges(output, summary["raw"]))
        self.assertEqual([(0, len(first)), (len(first), len(first + second))], [item[:2] for item in ranges])
        self.assertEqual(10, ranges[1][2]["open"]["host_clock"]["monotonic_ns"])

    def test_read_failure_preserves_prefix_and_reconnect_is_explicit(self):
        class FailingReader:
            calls = 0
            def read(self, limit):
                self.calls += 1
                if self.calls == 1:
                    return b"prefix"
                raise OSError("SECRET_SENTINEL must not enter exception metadata")
        output = self.root / "failed"
        result = capture.collect_streams([("one", FailingReader()), ("two", io.BytesIO(b"tail"))], output)
        self.assertEqual(b"prefixtail", (output / "raw.log").read_bytes())
        self.assertEqual(1, result["read_errors"])
        self.assertNotIn("SECRET_SENTINEL", (output / "connections.jsonl").read_text())
        self.assertEqual("read_error", list(capture.connection_ranges(output, result["raw"]))[0][2]["close"]["reason"])

    def test_collector_requests_bounded_chunks(self):
        class Reader:
            calls = 0
            def read(self, limit):
                if limit != capture.STREAM_CHUNK_SIZE:
                    raise AssertionError("unexpected unbounded request")
                self.calls += 1
                return b"x" if self.calls < 4 else b""
        result = capture.collect_streams([("bounded", Reader())], self.root / "bounded")
        self.assertEqual(3, result["raw"]["bytes"])

    def test_existing_collection_never_overwritten(self):
        output = self.root / "existing"
        output.mkdir()
        evidence = output / "raw.log"
        evidence.write_bytes(b"keep")
        with self.assertRaises(FileExistsError):
            capture.collect_streams([("unused", io.BytesIO(b"replace"))], output)
        self.assertEqual(b"keep", evidence.read_bytes())

    def test_connection_sidecar_hash_and_raw_identity_must_match(self):
        output = self.root / "sidecar"
        result = capture.collect_streams([("one", io.BytesIO(b"raw"))], output)
        with self.assertRaisesRegex(capture.CaptureError, "raw evidence"):
            list(capture.connection_ranges(output, {"bytes": 3, "sha256": "0" * 64}))
        with (output / "connections.jsonl").open("a") as destination:
            destination.write("{}\n")
        with self.assertRaisesRegex(capture.CaptureError, "hash mismatch"):
            list(capture.connection_ranges(output, result["raw"]))

    def test_connection_metadata_rejects_unbounded_rows_and_inconsistent_error_end(self):
        for name, replacement in (("oversized", " " * 4097),
                                   ("bad-close", json.dumps({"event": "close", "connection": 1, "start": 0,
                                                           "offset": 3, "reason": "read_error"}))):
            with self.subTest(name=name):
                output = self.root / name
                result = capture.collect_streams([("one", io.BytesIO(b"raw"))], output)
                events = output / "connections.jsonl"
                rows = events.read_text().splitlines()
                events.write_text(rows[0] + "\n" + replacement + "\n")
                result["connection_log"] = capture.file_hash(events)
                (output / "collection.json").write_text(json.dumps(result))
                with self.assertRaises(capture.CaptureError):
                    list(capture.connection_ranges(output, result["raw"]))

    def test_non_object_collection_metadata_is_rejected_cleanly(self):
        output = self.root / "wrong-type"
        result = capture.collect_streams([("one", io.BytesIO(b"raw"))], output)
        manifest = output / "collection.json"
        manifest.write_text("[]")
        with self.assertRaisesRegex(capture.CaptureError, "must be an object"):
            list(capture.connection_ranges(output, result["raw"]))
        events = output / "connections.jsonl"
        events.write_text("null\n")
        result["connection_log"] = capture.file_hash(events)
        manifest.write_text(json.dumps(result))
        with self.assertRaisesRegex(capture.CaptureError, "must be an object"):
            list(capture.connection_ranges(output, result["raw"]))

    def test_complete_sessions_partition_raw_and_preserve_strict_replay_boundaries(self):
        first, second = session(), session(boot=2)
        mixed = b"boot\r\n" + first + b"diagnostic\n" + second + b"tail\xff"
        output, counts = self.extract(mixed)
        self.assertEqual(2, counts["transport_complete"])
        self.assertEqual(0, counts["issues"])
        segments = json_rows(output / "segments.jsonl")
        self.assertEqual(0, segments[0]["start"])
        self.assertEqual(len(mixed), segments[-1]["end"])
        self.assertTrue(all(a["end"] == b["start"] for a, b in zip(segments, segments[1:])))
        complete = [row for row in segments if row["status"] == "transport_complete"]
        self.assertEqual(first, (output / complete[0]["wire"]["path"]).read_bytes())
        for item in complete:
            self.assertEqual("not_yet_checked", item["semantic_replay"])
            self.assertTrue(health_replay.decode_frames((output / item["wire"]["path"]).read_bytes()))
        with self.assertRaises(capture.CaptureError):
            health_replay.decode_frames(first + second)

    def test_all_frame_chunk_phases_and_packet_rollover(self):
        wire = b"text" + session(first_packet=65530) + b"tail"
        for chunk in range(1, 36):
            with self.subTest(chunk=chunk):
                _, counts = self.extract(wire, f"phase-{chunk}", chunk_size=chunk)
                self.assertEqual(1, counts["transport_complete"])
                self.assertEqual(0, counts["issues"])

    def test_corrupt_earlier_session_remains_rejected_when_later_session_is_valid(self):
        bad = bytearray(session())
        bad[25] = ord("0") if bad[25] != ord("0") else ord("1")
        wire = bytes(bad) + b"ordinary\n" + session(boot=2)
        output, counts = self.extract(wire)
        self.assertGreater(counts["issues"], 0)
        self.assertGreater(counts["rejected"], 0)
        self.assertEqual(1, counts["transport_complete"])
        self.assertEqual(wire, (output / "raw.log").read_bytes())
        self.assertIn("CRC mismatch", (output / "issues.jsonl").read_text())

    def test_arbitrary_begin_marker_does_not_create_session(self):
        wire = b"Begin session S0000 \x1eS0000" + b"0" * 24 + b"\n" + session(boot=3)
        _, counts = self.extract(wire)
        self.assertEqual(1, counts["transport_complete"])
        self.assertGreater(counts["issues"], 0)

    def test_missing_end_is_incomplete_and_later_session_does_not_repair_it(self):
        first = session()
        begin_only = first[:11 * 31]
        output, counts = self.extract(begin_only + session(boot=2))
        self.assertEqual(1, counts["incomplete"])
        self.assertEqual(1, counts["transport_complete"])
        self.assertEqual("incomplete", json_rows(output / "segments.jsonl")[0]["status"])

    def test_reconnect_mid_frame_and_record_are_never_joined(self):
        wire = session()
        for split in (5, 31, 70):
            with self.subTest(split=split):
                raw = wire + session(boot=2)
                _, counts = self.extract(raw, f"reconnect-{split}", [(0, split, None), (split, len(raw), None)])
                self.assertEqual(1, counts["transport_complete"])
                self.assertGreater(counts["issues"], 0)

    def test_duplicate_identity_is_rejected_even_with_different_source(self):
        _, counts = self.extract(session() + session(source="b" * 64))
        self.assertEqual(1, counts["transport_complete"])
        self.assertGreaterEqual(counts["rejected"], 1)

    def test_inter_session_loss_is_explicit_without_discarding_later_complete_session(self):
        first = session(first_packet=65530)
        next_packet = (65530 + len(first) // 31) & 0xFFFF
        for name, number, packet, expected_issues in (("continuous", 2, next_packet, False),
                                                       ("lost-session", 3, next_packet, True),
                                                       ("lost-packets", 2, next_packet + 1, True)):
            with self.subTest(name=name):
                _, counts = self.extract(first + session(number=number, first_packet=packet), name)
                self.assertEqual(2, counts["transport_complete"])
                self.assertEqual(expected_issues, counts["issues"] > 0)

    def test_reconnect_does_not_hide_known_same_boot_session_or_packet_loss(self):
        first = session(first_packet=10)
        expected_packet = 10 + len(first) // 31
        for name, number, packet in (("session", 3, expected_packet), ("packet", 2, expected_packet + 1)):
            second = session(number=number, first_packet=packet)
            with self.subTest(name=name):
                _, counts = self.extract(first + second, f"cross-reconnect-{name}",
                                         [(0, len(first), None), (len(first), len(first + second), None)])
                self.assertEqual(2, counts["transport_complete"])
                self.assertGreater(counts["issues"], 0)

    def test_record_packet_gaps_and_reported_export_loss_are_rejected(self):
        wire = session()
        for label, changed in (("packet-gap", wire[:31] + wire[62:]), ("end-loss", session(dropped=1))):
            with self.subTest(label=label):
                _, counts = self.extract(changed, label)
                self.assertEqual(0, counts["transport_complete"])
                self.assertGreater(counts["issues"], 0)

    def test_sample_sequence_rollover_and_mismatched_final_counters(self):
        decoded = health_replay.decode_frames(session(sample=0xFFFFFFFF))
        begin_length = struct.unpack_from("<H", decoded, 1)[0] + 7
        begin = decoded[:begin_length]
        sample = record(2, 1, b"\0" * 93)
        for final, expected in ((0, 1), (1, 0)):
            end = record(12, 2, struct.pack("<7I", 0, final, 0, 0, 0, 0, 3))
            _, counts = self.extract(health_replay.encode_frames(begin + sample + end), f"sample-{final}")
            self.assertEqual(expected, counts["transport_complete"])

    def test_invalid_begin_identity_checkpoint_and_version_are_rejected(self):
        decoded = health_replay.decode_frames(session())
        for offset, data in ((7, b"\x02\0"), (9, b"z"), (77, b"\0" * 8), (85, b"\0" * 4), (105, b"\0\0")):
            changed = bytearray(decoded)
            changed[offset:offset + len(data)] = data
            with self.subTest(offset=offset):
                _, counts = self.extract(health_replay.encode_frames(changed), f"invalid-{offset}")
                self.assertEqual(0, counts["transport_complete"])

    def test_ordinary_text_has_bounded_memory_and_is_accounted(self):
        output = self.root / "long"
        output.mkdir()
        raw = output / "raw.log"
        with raw.open("wb") as stream:
            for _ in range(32):
                stream.write(b"x" * 1024 * 1024)
            stream.write(session())
        tracemalloc.start()
        try:
            counts = capture.extract_sessions(raw, output, [(0, raw.stat().st_size, None)])
            _, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertEqual(1, counts["transport_complete"])
        self.assertLess(peak, 1024 * 1024)


class ProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="shn-bundle-test-")
        self.root = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)
        self.project = self.root / "project"
        (self.project / "include").mkdir(parents=True)
        (self.project / "tools").mkdir()
        (self.project / "include/private.h").write_text("SECRET_CONFIGURATION_SENTINEL")
        for name in ("health_replay.cpp", "health_replay_engine.h"):
            (self.project / "tools" / name).write_text("fixture tool inputs")
        self.raw = self.root / "raw.log"
        self.raw.write_bytes(session(source=health_identity.source_manifest(self.project)["source_id"]))

    def arguments(self, name="bundle", **changes):
        values = dict(recording=self.raw, output=self.root / name, project=self.project,
                      snapshot_reference="retained-private-snapshot-001", collection=None, metadata=None,
                      dependency=[], image=[], replay=False, compiler=None, repeat=2)
        values.update(changes)
        return Namespace(**values)

    def test_bundle_retains_raw_manifest_reference_and_separates_installed_identity(self):
        args = self.arguments()
        result = provenance.create_bundle(args)
        self.assertTrue(result["ok"])
        self.assertEqual(self.raw.read_bytes(), (args.output / "raw.log").read_bytes())
        saved = json.loads((args.output / "provenance.json").read_text())
        self.assertEqual("unknown", saved["installed_image_verification"])
        self.assertFalse(saved["snapshot_reference_verified"])
        self.assertEqual("retained-private-snapshot-001", saved["retained_snapshot_reference"])
        self.assertNotIn("SECRET_CONFIGURATION_SENTINEL", (args.output / "source-manifest.json").read_text())
        self.assertEqual("not_requested", saved["replay"]["status"])
        self.assertTrue(provenance.verify_bundle(args.output)["ok"])

    def test_bundle_never_overwrites_existing_output_or_original_alias(self):
        args = self.arguments(output=self.raw)
        original = self.raw.read_bytes()
        with self.assertRaises(FileExistsError):
            provenance.create_bundle(args)
        self.assertEqual(original, self.raw.read_bytes())

    def test_bundle_refuses_existing_dangling_destination_alias(self):
        target, alias = self.root / "unused-target", self.root / "existing-link"
        try:
            alias.symlink_to(target, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"directory symlinks unavailable: {error}")
        with self.assertRaises((FileExistsError, capture.CaptureError)):
            provenance.create_bundle(self.arguments(output=alias))
        self.assertFalse(target.exists())

    def test_non_allowlisted_metadata_and_secret_build_flags_are_rejected(self):
        metadata = self.root / "operator.json"
        for content in ({"environment": {"TOKEN": "secret"}}, {"build": {"flags": "-DCHANNEL_KEY=secret"}},
                        {"build": {"target": "sensor_only"}}, {"host_clock_uncertainty_ms": -1}):
            metadata.write_text(json.dumps(content))
            with self.subTest(content=content), self.assertRaises(capture.CaptureError):
                provenance.operator_metadata(metadata)

    def test_allowlisted_build_settings_labels_dependency_versions_and_image_hashes(self):
        metadata = self.root / "operator.json"
        metadata.write_text(json.dumps({"device_label": "rig-01", "mount_label": "torso-fixture",
                                       "host_clock_uncertainty_ms": 5, "build": {"target": capture.TARGET,
                                       "framework": "arduino", "cpu_frequency_hz": 240000000, "optimization": "Os"}}))
        dependency = self.root / "framework"
        dependency.mkdir()
        (dependency / "package.json").write_text(json.dumps({"name": "framework-test", "version": "2.0.17", "secret": "DO_NOT_EXPORT"}))
        (dependency / "object.bin").write_bytes(b"dependency bytes")
        image = self.root / "firmware.bin"
        image.write_bytes(b"firmware bytes")
        args = self.arguments(metadata=metadata, dependency=[f"framework={dependency}"], image=[f"application={image}"])
        result = provenance.create_bundle(args)
        self.assertTrue(result["ok"])
        saved = json.loads((args.output / "provenance.json").read_text())
        self.assertEqual("2.0.17", saved["dependencies"]["framework"]["version"])
        self.assertEqual(capture.file_hash(image)["sha256"], saved["images"]["application"]["sha256"])
        self.assertNotIn("DO_NOT_EXPORT", (args.output / "provenance.json").read_text())
        self.assertFalse((args.output / "firmware.bin").exists())

    def test_source_mismatch_keeps_evidence_but_fails_association(self):
        self.raw.write_bytes(session())
        args = self.arguments()
        result = provenance.create_bundle(args)
        self.assertFalse(result["ok"])
        saved = json.loads((args.output / "provenance.json").read_text())
        self.assertEqual(1, saved["consistency"]["source_mismatches"])
        verified = provenance.verify_bundle(args.output)
        self.assertTrue(verified["ok"])
        self.assertFalse(verified["original_bundle_ok"])

    def test_source_drift_during_bundle_fails_consistency(self):
        original = provenance.extract_sessions
        def change_source(*args, **kwargs):
            result = original(*args, **kwargs)
            (self.project / "include/private.h").write_text("changed source")
            return result
        args = self.arguments()
        with mock.patch.object(provenance, "extract_sessions", side_effect=change_source):
            self.assertFalse(provenance.create_bundle(args)["ok"])
        saved = json.loads((args.output / "provenance.json").read_text())
        self.assertFalse(saved["consistency"]["source_unchanged"])

    def test_tool_drift_during_bundle_fails_consistency(self):
        before = provenance.tool_manifest(self.project)
        after = {**before, "python_version": "changed"}
        with mock.patch.object(provenance, "tool_manifest", side_effect=[before, after]):
            result = provenance.create_bundle(self.arguments())
        self.assertFalse(result["ok"])

    def test_bundle_hash_verification_rejects_modified_raw(self):
        args = self.arguments()
        provenance.create_bundle(args)
        with (args.output / "raw.log").open("ab") as destination:
            destination.write(b"changed")
        with self.assertRaisesRegex(capture.CaptureError, "raw identity"):
            provenance.verify_bundle(args.output)

    def test_raw_mutation_during_processing_fails_consistency(self):
        original = provenance.extract_sessions
        for retained in (False, True):
            args = self.arguments(name=f"raw-drift-{retained}")
            def change_raw(*positional, **keywords):
                result = original(*positional, **keywords)
                changed = args.output / "raw.log" if retained else self.raw
                with changed.open("ab") as stream:
                    stream.write(b"later mutation")
                return result
            with self.subTest(retained=retained), mock.patch.object(provenance, "extract_sessions", side_effect=change_raw):
                result = provenance.create_bundle(args)
            self.assertFalse(result["ok"])
            saved = json.loads((args.output / "provenance.json").read_text())
            self.assertFalse(saved["consistency"]["raw_unchanged"])
            if retained:
                with self.assertRaisesRegex(capture.CaptureError, "raw identity"):
                    provenance.verify_bundle(args.output)

    def test_session_mutation_during_processing_fails_association_and_verification(self):
        original = provenance.extract_sessions
        args = self.arguments()
        def change_session(*positional, **keywords):
            result = original(*positional, **keywords)
            wire = next((args.output / "sessions").glob("*.log"))
            with wire.open("ab") as destination:
                destination.write(b"bytes outside the indexed raw interval")
            return result
        with mock.patch.object(provenance, "extract_sessions", side_effect=change_session):
            result = provenance.create_bundle(args)
        self.assertFalse(result["ok"])
        with self.assertRaises(capture.CaptureError):
            provenance.verify_bundle(args.output)

    def test_verification_rejects_an_index_that_omits_required_evidence(self):
        args = self.arguments()
        result = provenance.create_bundle(args)
        index = args.output / "artifacts.jsonl"
        index.write_text(json.dumps({"path": "raw.log", **capture.file_hash(args.output / "raw.log")}) + "\n")
        result["artifacts"] = {"files": 1, **capture.file_hash(index)}
        (args.output / "bundle.json").write_text(json.dumps(result))
        with self.assertRaises(capture.CaptureError):
            provenance.verify_bundle(args.output)

    def test_verification_rejects_conflicting_source_manifest_association(self):
        args = self.arguments()
        result = provenance.create_bundle(args)
        source_path = args.output / "source-manifest.json"
        source = json.loads(source_path.read_text())
        source["source_id"] = "0" * 64
        source_path.write_text(json.dumps(source))
        (args.output / "artifacts.jsonl").unlink()
        result["artifacts"] = provenance.artifact_index(args.output)
        (args.output / "bundle.json").write_text(json.dumps(result))
        with self.assertRaises(capture.CaptureError):
            provenance.verify_bundle(args.output)

    def test_non_object_artifact_rows_are_rejected_without_traceback(self):
        args = self.arguments()
        result = provenance.create_bundle(args)
        index = args.output / "artifacts.jsonl"
        index.write_text("null\n")
        result["artifacts"] = {"files": 1, **capture.file_hash(index)}
        (args.output / "bundle.json").write_text(json.dumps(result))
        with self.assertRaises(capture.CaptureError):
            provenance.verify_bundle(args.output)

    def test_rejected_prefix_and_reconnect_metadata_survive_bundle(self):
        collected = self.root / "collected"
        capture.collect_streams([("failed-prefix", io.BytesIO(b"\x1eSbad")), ("reconnect", io.BytesIO(self.raw.read_bytes()))], collected)
        args = self.arguments(recording=collected / "raw.log", collection=collected)
        result = provenance.create_bundle(args)
        self.assertFalse(result["ok"])
        self.assertEqual(1, result["extraction"]["transport_complete"])
        self.assertGreater(result["extraction"]["issues"], 0)
        self.assertEqual((collected / "connections.jsonl").read_bytes(), (args.output / "collection/connections.jsonl").read_bytes())

    def test_compiler_failure_preserves_complete_bundle_and_reports_failure(self):
        args = self.arguments(replay=True)
        with mock.patch.object(provenance, "compiler_identity", side_effect=capture.CaptureError("unavailable")):
            result = provenance.create_bundle(args)
        self.assertFalse(result["ok"])
        saved = json.loads((args.output / "provenance.json").read_text())
        self.assertEqual("tool_failed", saved["replay"]["status"])
        self.assertTrue(provenance.verify_bundle(args.output)["ok"])


class ProductionBundleTests(unittest.TestCase):
    def test_production_fixture_bundle_replays_and_invalid_checkpoint_is_rejected(self):
        project = Path(__file__).resolve().parents[1]
        compiler = health_replay.compiler_path(project, None)
        source_id = health_identity.source_manifest(project)["source_id"]
        runtime = os.environ.copy()
        runtime["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + runtime.get("PATH", "")
        with tempfile.TemporaryDirectory(prefix="shn-production-bundle-") as directory:
            folder = Path(directory)
            generator = folder / ("fixture.exe" if sys.platform == "win32" else "fixture")
            subprocess.run([compiler, "-std=c++17", "-O2", "-fno-fast-math", "-ffp-contract=off",
                            "-I", str(project / "include"), "-I", str(project / "src"),
                            "-I", str(project / "test/mocks"), str(project / "test/host/health_capture_fixture.cpp"),
                            "-o", str(generator)], check=True, env=runtime)
            binary, wire = folder / "records.bin", folder / "wire.log"
            subprocess.run([str(generator), str(binary), source_id], check=True, env=runtime)
            wire.write_bytes(b"diagnostic\n" + health_replay.encode_frames(binary.read_bytes(), 65530))
            args = Namespace(recording=wire, output=folder / "bundle", project=project,
                             snapshot_reference="test-current-tree", collection=None, metadata=None,
                             dependency=[], image=[], replay=True, compiler=compiler, repeat=3)
            result = provenance.create_bundle(args)
            self.assertTrue(result["ok"])
            outcomes = json_rows(args.output / "replay.jsonl")
            self.assertEqual(1, len(outcomes))
            self.assertEqual("verified", outcomes[0]["status"])
            self.assertEqual(3, len(outcomes[0]["runs"]))
            self.assertIn("falls=1", outcomes[0]["runs"][0]["stdout"])
            self.assertTrue(provenance.verify_bundle(args.output)["ok"])
            wire.write_bytes(session(source=source_id))
            args.output = folder / "invalid-checkpoint"
            result = provenance.create_bundle(args)
            self.assertEqual(1, result["extraction"]["transport_complete"])
            self.assertFalse(result["ok"])
            self.assertEqual("rejected", json_rows(args.output / "replay.jsonl")[0]["status"])


if __name__ == "__main__":
    unittest.main()
