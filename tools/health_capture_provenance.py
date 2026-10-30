"""Allowlisted provenance and verification for optional motion evidence bundles.

Source/configuration and dependency contents are never copied automatically.
Hashes associate retained inputs; they do not attest a binary installed on a node.
"""
from __future__ import annotations

import hashlib
from itertools import zip_longest
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile

from health_capture import (TARGET, CaptureError, STREAM_CHUNK_SIZE, connection_ranges,
                            copy_range, extract_sessions, file_hash, json_line, write_json)
from health_identity import source_manifest
from health_replay import build_replayer, compiler_path, decode_stream

HOST_TOOLS = ("health_capture.py", "health_capture_provenance.py", "health_replay.py", "health_identity.py")
BUILD_ENUMS = {"framework": {"arduino"}, "board": {"heltec_v4"},
               "flash_mode": {"dio", "dout", "qio", "qout"},
               "optimization": {"O0", "O1", "O2", "O3", "Os", "Og"},
               "cxx_standard": {"c++11", "gnu++11", "c++14", "gnu++14", "c++17", "gnu++17"}}
BUILD_INTS = {"cpu_frequency_hz": (1000000, 300000000),
              "flash_frequency_hz": (1000000, 120000000), "usb_cdc_on_boot": (0, 1)}
SAFE_VERSION = re.compile(r"[A-Za-z0-9_.+:-]{1,128}\Z")


def read_json(path: Path, limit=65536) -> dict:
    with path.open("rb") as source:
        content = source.read(limit + 1)
    if len(content) > limit:
        raise CaptureError(f"metadata exceeds {limit} bytes")
    result = json.loads(content)
    if not isinstance(result, dict):
        raise CaptureError("metadata must be a JSON object")
    return result


def lines(path: Path):
    """Bound individual metadata rows as well as input chunks."""
    with path.open(encoding="utf-8") as source:
        while line := source.readline(65537):
            if len(line) > 65536:
                raise CaptureError("metadata row exceeds 64 KiB")
            result = json.loads(line)
            if not isinstance(result, dict):
                raise CaptureError("metadata row must be a JSON object")
            yield result


def operator_metadata(path: Path | None) -> dict:
    result = read_json(path) if path else {}
    allowed = {"device_label", "mount_label", "host_clock_uncertainty_ms", "build"}
    if set(result) - allowed:
        raise CaptureError("operator metadata contains a non-allowlisted field")
    for key in ("device_label", "mount_label"):
        if key in result and (not isinstance(result[key], str) or not 1 <= len(result[key]) <= 128 or not result[key].isprintable()):
            raise CaptureError(f"invalid {key}")
    uncertainty = result.get("host_clock_uncertainty_ms")
    if uncertainty is not None and (type(uncertainty) is not int or not 0 <= uncertainty <= 86400000):
        raise CaptureError("host clock uncertainty must be null or 0..86400000 ms")
    build = result.get("build", {})
    if not isinstance(build, dict) or set(build) - (set(BUILD_ENUMS) | set(BUILD_INTS) | {"target"}):
        raise CaptureError("non-allowlisted build setting; arbitrary flags/defines are not exported")
    if build.get("target", TARGET) != TARGET:
        raise CaptureError("bundle target must remain heltec_v4_companion_radio_ble")
    for key, values in BUILD_ENUMS.items():
        if key in build and (not isinstance(build[key], str) or build[key] not in values):
            raise CaptureError(f"invalid build setting {key}")
    for key, (low, high) in BUILD_INTS.items():
        if key in build and (type(build[key]) is not int or not low <= build[key] <= high):
            raise CaptureError(f"invalid build setting {key}")
    return result


def named_paths(values, roles=None) -> dict:
    result = {}
    for value in values:
        name, separator, path = value.partition("=")
        if not separator or re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,63}", name) is None or name in result or (roles and name not in roles):
            raise CaptureError("named inputs require distinct allowlisted NAME=PATH entries")
        result[name] = Path(path).resolve()
    return result


def tree_files(root: Path):
    """Visit deterministic dependency paths without accumulating the whole tree."""
    if not root.is_dir() or root.is_symlink():
        raise CaptureError("dependency must be a directory without symbolic links")
    def traversal_error(error):
        raise error
    for directory, folders, files in os.walk(root, followlinks=False, onerror=traversal_error):
        folders.sort()
        for name in folders + files:
            if (Path(directory) / name).is_symlink():
                raise CaptureError("dependency symbolic links require an explicit retained snapshot")
        for name in sorted(files):
            yield Path(directory) / name


def dependency_manifest(root: Path, destination: Path | None) -> dict:
    """Fingerprint a selected package; emit only relative paths, sizes and hashes."""
    digest, count, total = hashlib.sha256(b"SHN-dependency-v1\0"), 0, 0
    stream = destination.open("x", encoding="utf-8", newline="\n") if destination else None
    try:
        for path in tree_files(root):
            item = {"path": path.relative_to(root).as_posix(), **file_hash(path)}
            encoded = json.dumps(item, sort_keys=True).encode("utf-8") + b"\n"
            digest.update(encoded)
            if stream:
                stream.write(encoded.decode("utf-8"))
            count += 1
            total += item["bytes"]
    finally:
        if stream:
            stream.close()
    result = {"sha256": digest.hexdigest(), "files": count, "bytes": total}
    package = root / "package.json"
    if package.is_file():
        description = read_json(package)
        for key in ("name", "version"):
            value = description.get(key)
            if isinstance(value, str) and SAFE_VERSION.fullmatch(value):
                result[key] = value
    return result


def tool_manifest(project: Path) -> dict:
    tools = {}
    folder = Path(__file__).resolve().parent
    for name in HOST_TOOLS:
        tools[f"host/{name}"] = file_hash(folder / name)
    for name in ("tools/health_replay.cpp", "tools/health_replay_engine.h"):
        tools[f"project/{name}"] = file_hash(project / name)
    # Replay also includes host headers outside the maintained firmware scope.
    mocks = project / "test/mocks"
    if mocks.is_dir():
        for path in tree_files(mocks):
            if path.suffix in (".h", ".hpp"):
                tools["project/" + path.relative_to(project).as_posix()] = file_hash(path)
    return {"files": tools, "python_version": platform.python_version(),
            "python_executable": file_hash(Path(sys.executable))}


def compiler_identity(project: Path, requested) -> tuple[str, dict]:
    compiler = compiler_path(project, requested)
    found = shutil.which(compiler) or compiler
    executable = Path(found).resolve()
    if not executable.is_file():
        raise CaptureError("replay compiler executable unavailable")
    version = subprocess.run([str(executable), "--version"], check=True, capture_output=True, text=True)
    return str(executable), {"file": executable.name, **file_hash(executable),
                             "version_first_line": version.stdout.splitlines()[0][:512]}


def replay_sessions(project: Path, output: Path, source_id: str, compiler: str, repeat: int) -> dict:
    """Run the existing strict decoder/production replayer; retain every outcome."""
    counts = {"verified": 0, "rejected": 0}
    (output / "decoded").mkdir()
    with tempfile.TemporaryDirectory(prefix="replayer-", dir=output) as temporary, \
            (output / "replay.jsonl").open("x", encoding="utf-8", newline="\n") as outcomes:
        executable = Path(temporary) / ("health_replay.exe" if sys.platform == "win32" else "health_replay")
        build_replayer(project, executable, compiler)
        result = {"compiler_arguments": ["-std=c++17", "-O2", "-fno-fast-math", "-ffp-contract=off"],
                  "executable": file_hash(executable), "repeat": repeat, "counts": counts}
        runtime = os.environ.copy()
        runtime["PATH"] = str(Path(compiler).parent) + os.pathsep + runtime.get("PATH", "")
        for segment in lines(output / "segments.jsonl"):
            if segment["status"] != "transport_complete":
                continue
            decoded = output / "decoded" / f"{segment['segment']:06d}.bin"
            outcome = {"segment": segment["segment"], "wire": segment["wire"]["path"],
                       "command": [str(executable), str(decoded), source_id], "runs": []}
            try:
                with (output / segment["wire"]["path"]).open("rb") as incoming, decoded.open("xb") as destination:
                    decode_stream(incoming, destination)
                outcome["decoded"] = {"path": decoded.relative_to(output).as_posix(), **file_hash(decoded)}
                for _ in range(repeat):
                    run = subprocess.run(outcome["command"], capture_output=True, text=True, env=runtime)
                    outcome["runs"].append({"returncode": run.returncode,
                                             "stdout": run.stdout.strip(), "stderr": run.stderr.strip()})
                verified = all(run["returncode"] == 0 for run in outcome["runs"]) and \
                    len({json.dumps(run, sort_keys=True) for run in outcome["runs"]}) == 1
                outcome["status"] = "verified" if verified else "rejected"
            except (CaptureError, OSError) as error:
                outcome.update(status="rejected", error=str(error))
            counts[outcome["status"]] += 1
            json_line(outcomes, outcome)
    return result


def fingerprint_fields(value) -> dict:
    """Validate an embedded file fingerprint without accepting bool counts."""
    if not isinstance(value, dict) or type(value.get("bytes")) is not int or value["bytes"] < 0 or \
            not isinstance(value.get("sha256"), str) or re.fullmatch(r"[0-9a-f]{64}", value["sha256"]) is None:
        raise CaptureError("invalid artifact fingerprint")
    return {key: value[key] for key in ("bytes", "sha256")}


def referenced_path(output: Path, name) -> Path:
    if not isinstance(name, str) or not name:
        raise CaptureError("invalid artifact path")
    relative = Path(name)
    path = output / relative
    if relative.is_absolute() or ".." in relative.parts or path.is_symlink() or output.resolve() not in path.resolve().parents:
        raise CaptureError("unsafe bundle artifact path")
    return path


def check_session_references(output: Path, raw: dict, source_id=None) -> dict:
    """Check streamed interval coverage and wire-to-raw association.

    Embedded hashes must agree with both the extracted file and its recorded
    raw interval. Hashing an already changed file into the outer inventory must
    not turn an inconsistent session into verified evidence.
    """
    totals = {"transport_complete": 0, "rejected": 0, "incomplete": 0, "ordinary_text": 0,
              "issues": 0, "source_mismatches": 0}
    cursor, number = 0, 0
    with (output / "raw.log").open("rb") as original:
        for item in lines(output / "segments.jsonl"):
            number += 1
            start, end, status = item.get("start"), item.get("end"), item.get("status")
            if type(item.get("segment")) is not int or item["segment"] != number or type(start) is not int or \
                    type(end) is not int or start != cursor or not start <= end <= raw["bytes"] or \
                    status not in ("transport_complete", "rejected", "incomplete", "ordinary_text"):
                raise CaptureError("invalid or incomplete segment interval coverage")
            cursor = end
            totals[status] += 1
            if status != "transport_complete":
                continue
            wire = item.get("wire")
            expected = fingerprint_fields(wire)
            path = referenced_path(output, wire.get("path"))
            if expected["bytes"] != end - start or file_hash(path) != expected:
                raise CaptureError("session wire identity differs from its segment reference")
            original.seek(start)
            digest, remaining = hashlib.sha256(), end - start
            while remaining:
                chunk = original.read(min(remaining, STREAM_CHUNK_SIZE))
                if not chunk:
                    raise CaptureError("raw evidence ended inside a session interval")
                digest.update(chunk)
                remaining -= len(chunk)
            if digest.hexdigest() != expected["sha256"]:
                raise CaptureError("session wire differs from its original raw interval")
            identity = item.get("identity")
            if not isinstance(identity, dict) or not isinstance(identity.get("source_id"), str) or \
                    re.fullmatch(r"[0-9a-f]{64}", identity["source_id"]) is None:
                raise CaptureError("invalid session source identity")
            if source_id is not None and identity["source_id"] != source_id:
                totals["source_mismatches"] += 1
    if cursor != raw["bytes"]:
        raise CaptureError("segment index does not cover retained raw evidence")
    for issue in lines(output / "issues.jsonl"):
        start, end = issue.get("start"), issue.get("end")
        if type(start) is not int or type(end) is not int or not 0 <= start <= end <= raw["bytes"]:
            raise CaptureError("invalid issue interval")
        totals["issues"] += 1
    return totals


def artifact_index(output: Path) -> dict:
    """Index finished local artifacts, excluding the index and unsigned root."""
    count = 0
    with (output / "artifacts.jsonl").open("x", encoding="utf-8", newline="\n") as index:
        for path in tree_files(output):
            name = path.relative_to(output).as_posix()
            if name in ("artifacts.jsonl", "bundle.json"):
                continue
            json_line(index, {"path": name, **file_hash(path)})
            count += 1
    return {"files": count, **file_hash(output / "artifacts.jsonl")}


def create_bundle(args) -> dict:
    """Preserve inputs first, then extract and check association without repair."""
    if not 1 <= args.repeat <= 10:
        raise CaptureError("--repeat must be between 1 and 10")
    if not args.snapshot_reference or len(args.snapshot_reference) > 512 or not args.snapshot_reference.isprintable():
        raise CaptureError("retained snapshot reference must be 1..512 printable characters")
    project, recording = args.project.resolve(), args.recording.resolve()
    # Resolve parents only after exclusive creation. Resolving a dangling final
    # symlink first would populate its target despite an existing destination.
    requested_output = args.output.absolute()
    output = requested_output.resolve()
    metadata = operator_metadata(args.metadata)
    dependencies = named_paths(args.dependency)
    images = named_paths(args.image, {"application", "merged", "bootloader", "partitions"})
    if not recording.is_file():
        raise CaptureError("recording must be a regular file")
    for dependency in dependencies.values():
        if output == dependency or dependency in output.parents:
            raise CaptureError("bundle output cannot be inside a fingerprinted dependency")
    requested_output.mkdir(parents=True, exist_ok=False)
    output = requested_output.resolve()
    raw_before = file_hash(recording)
    raw = copy_range(recording, output / "raw.log", 0, raw_before["bytes"])
    raw_unchanged = raw == raw_before == file_hash(recording)
    collection = None
    if args.collection:
        collection = output / "collection"
        collection.mkdir()
        for name in ("collection.json", "connections.jsonl"):
            original = args.collection / name
            before = file_hash(original)
            copied = copy_range(original, collection / name, 0, before["bytes"])
            if before != copied or before != file_hash(original):
                raise CaptureError("collection metadata changed while copying")
    before_source, before_tools = source_manifest(project), tool_manifest(project)
    write_json(output / "source-manifest.json", before_source)
    write_json(output / "operator-metadata.json", metadata)
    before_dependencies, before_images = {}, {}
    (output / "dependencies").mkdir()
    for name, path in dependencies.items():
        before_dependencies[name] = dependency_manifest(path, output / "dependencies" / (name + ".jsonl"))
    for name, path in images.items():
        before_images[name] = {"file": path.name, **file_hash(path)}
    extraction = extract_sessions(output / "raw.log", output, connection_ranges(collection, raw))
    mismatches = sum(segment["identity"]["source_id"] != before_source["source_id"]
                     for segment in lines(output / "segments.jsonl") if segment["status"] == "transport_complete")
    replay, compiler, before_compiler = {"status": "not_requested"}, None, None
    if args.replay:
        try:
            compiler, before_compiler = compiler_identity(project, args.compiler)
            replay = replay_sessions(project, output, before_source["source_id"], compiler, args.repeat)
        except (CaptureError, OSError, subprocess.CalledProcessError) as error:
            replay = {"status": "tool_failed", "error_class": type(error).__name__}
    source_unchanged = before_source == source_manifest(project)
    tools_unchanged = before_tools == tool_manifest(project)
    dependency_unchanged = all(before_dependencies[name] == dependency_manifest(path, None) for name, path in dependencies.items())
    image_unchanged = all(before_images[name] == {"file": path.name, **file_hash(path)} for name, path in images.items())
    compiler_unchanged = compiler is None or file_hash(Path(compiler)) == {key: before_compiler[key] for key in ("bytes", "sha256")}
    # A capture can still be actively appended, or its retained copy changed by
    # another process while replay runs. Neither is silently accepted as the
    # original immutable extraction input.
    raw_unchanged = raw_unchanged and raw == file_hash(recording) == file_hash(output / "raw.log")
    try:
        check_session_references(output, raw)
        sessions_unchanged = True
    except (CaptureError, OSError):
        sessions_unchanged = False
    consistency = {"raw_unchanged": raw_unchanged, "source_unchanged": source_unchanged,
                   "tools_unchanged": tools_unchanged, "dependencies_unchanged": dependency_unchanged,
                   "images_unchanged": image_unchanged, "compiler_unchanged": compiler_unchanged,
                   "sessions_unchanged": sessions_unchanged,
                   "source_mismatches": mismatches}
    provenance = {"schema": 1, "target": TARGET, "retained_snapshot_reference": args.snapshot_reference,
                  "snapshot_reference_verified": False, "source_id": before_source["source_id"],
                  "host_tools": before_tools, "dependencies": before_dependencies, "images": before_images,
                  "replay_compiler": before_compiler, "replay": replay,
                  "connection_boundaries": "validated collector sidecar" if collection else "unknown; one raw-file parse interval",
                  "dependency_scope": "explicitly named trees only; other build/runtime inputs remain unverified",
                  "build_settings_origin": "allowlisted operator assertions; effective firmware flags are not inferred",
                  "installed_image_verification": "unknown", "consistency": consistency,
                  "measurement_units": {"acceleration": "g, including gravity", "rotation": "degrees/second",
                                        "temperature": "degrees Celsius", "axes": "sensor/device axes; mounting is operator metadata"},
                  "calibration_metadata": "v1 Calibration records are retained in each session and checked by strict replay"}
    write_json(output / "provenance.json", provenance)
    ok = all(value for key, value in consistency.items() if key != "source_mismatches") and not mismatches and \
        extraction["transport_complete"] > 0 and not extraction["issues"]
    if args.replay:
        ok = ok and "counts" in replay and not replay["counts"]["rejected"]
    result = {"schema": 1, "ok": bool(ok), "raw": raw, "extraction": extraction,
              "artifacts": artifact_index(output), "installed_image_verification": "unknown",
              "integrity_scope": "unsigned local artifact consistency; not authenticity, physical completeness or installed firmware"}
    write_json(output / "bundle.json", result)
    return result


def verify_bundle(output: Path) -> dict:
    """Check saved local hashes; a consistent damaged bundle remains damaged."""
    manifest = read_json(output / "bundle.json")
    if type(manifest.get("schema")) is not int or manifest["schema"] != 1 or type(manifest.get("ok")) is not bool or \
            file_hash(output / "artifacts.jsonl") != fingerprint_fields(manifest.get("artifacts")):
        raise CaptureError("bundle artifact index hash mismatch")
    if fingerprint_fields(manifest.get("raw")) != file_hash(output / "raw.log"):
        raise CaptureError("bundle raw identity differs from retained raw evidence")
    required = ("raw.log", "segments.jsonl", "issues.jsonl", "source-manifest.json", "operator-metadata.json", "provenance.json")
    if any(not (output / name).is_file() for name in required):
        raise CaptureError("required bundle evidence is missing")
    actual_files = (path for path in tree_files(output) if path.relative_to(output).as_posix() not in ("artifacts.jsonl", "bundle.json"))
    count = 0
    for item, actual in zip_longest(lines(output / "artifacts.jsonl"), actual_files):
        if item is None or actual is None or item.get("path") != actual.relative_to(output).as_posix():
            raise CaptureError("bundle artifact inventory is incomplete, duplicated or out of order")
        path = referenced_path(output, item["path"])
        if file_hash(path) != fingerprint_fields(item):
            raise CaptureError(f"bundle artifact hash mismatch: {item['path']}")
        count += 1
    if type(manifest["artifacts"].get("files")) is not int or count != manifest["artifacts"]["files"]:
        raise CaptureError("bundle artifact count mismatch")
    # The source catalog scales with the retained project, not capture duration;
    # its separate 8 MiB bound accommodates larger maintained-input manifests.
    source = read_json(output / "source-manifest.json", 8 * 1024 * 1024)
    provenance = read_json(output / "provenance.json")
    if not isinstance(source.get("source_id"), str) or re.fullmatch(r"[0-9a-f]{64}", source["source_id"]) is None or \
            source["source_id"] != provenance.get("source_id"):
        raise CaptureError("source manifest and provenance identities disagree")
    totals = check_session_references(output, manifest["raw"], source["source_id"])
    extraction, consistency = manifest.get("extraction"), provenance.get("consistency")
    if not isinstance(extraction, dict) or not isinstance(consistency, dict):
        raise CaptureError("invalid extraction or consistency summary")
    for key, value in totals.items():
        expected = consistency.get(key) if key == "source_mismatches" else extraction.get(key)
        if type(expected) is not int or expected != value:
            raise CaptureError("bundle summary disagrees with retained session or issue evidence")
    return {"ok": True, "verified_files": count, "original_bundle_ok": manifest["ok"],
            "installed_image_verification": "unknown", "scope": manifest["integrity_scope"]}
