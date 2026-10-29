"""Fingerprint maintained firmware inputs, including unstaged and untracked files.

Usable as a PlatformIO pre-script or a plain host module. Only hashes and paths
are emitted; configuration values, keys and credentials are never exported.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct

SOURCE_ROOTS = ("src", "include", "examples/companion_radio", "variants/heltec_v4",
                "boards/variants/heltec_v4", "lib")
SOURCE_FILES = ("platformio.ini", "platformio.local.ini", "boards/heltec_v4.json",
                "merge-bin.py", "tools/health_identity.py")
SOURCE_EXTENSIONS = {".h", ".hpp", ".c", ".cpp", ".ino", ".ini", ".py", ".json", ".csv", ".ld"}


def source_manifest(project: Path) -> dict:
    """Hash exact file bytes, paths and lengths; Git status does not define input."""
    files = set()
    for name in SOURCE_ROOTS:
        root = project / name
        if root.is_dir():
            files.update(p for p in root.rglob("*") if p.is_file() and p.suffix.lower() in SOURCE_EXTENSIONS)
    files.update(project / name for name in SOURCE_FILES if (project / name).is_file())
    digest = hashlib.sha256(b"SeniorHealthNode-maintained-firmware-inputs-v1\0")
    entries = []
    for path in sorted(files, key=lambda p: p.relative_to(project).as_posix()):
        name = path.relative_to(project).as_posix()
        encoded_name = name.encode("utf-8")
        content = path.read_bytes()
        digest.update(struct.pack("<I", len(encoded_name)))
        digest.update(encoded_name)
        digest.update(struct.pack("<Q", len(content)))
        digest.update(content)
        entries.append({"path": name, "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()})
    return {"schema": 1, "scope": "maintained firmware source and project configuration; exact working-tree bytes",
            "source_id": digest.hexdigest(), "files": entries}


def generate(project: Path, output: Path) -> dict:
    manifest = source_manifest(project)
    output.mkdir(parents=True, exist_ok=True)
    header = '#pragma once\n#define HEALTH_SOURCE_ID "' + manifest["source_id"] + '"\n'
    header_path = output / "health_source_identity.h"
    if not header_path.exists() or header_path.read_text(encoding="utf-8") != header:
        header_path.write_text(header, encoding="utf-8", newline="\n")
    (output / "health_source_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    return manifest


# PlatformIO executes extra scripts with Import injected by SCons. Importing this
# module in the host utility has no filesystem or build side effects.
if "Import" in globals():
    Import("env")  # type: ignore[name-defined]  # noqa: F821
    project = Path(env.subst("$PROJECT_DIR"))  # type: ignore[name-defined]  # noqa: F821
    generated = Path(env.subst("$BUILD_DIR")) / "health_capture"  # type: ignore[name-defined]  # noqa: F821
    generate(project, generated)
    env.Append(CPPPATH=[str(generated)])  # type: ignore[name-defined]  # noqa: F821
elif __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    manifest = generate(args.project.resolve(), args.output) if args.output else source_manifest(args.project.resolve())
    print(manifest["source_id"])
