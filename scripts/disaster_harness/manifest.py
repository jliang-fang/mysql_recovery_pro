#!/usr/bin/env python3
"""Evidence manifest and workspace safety for disaster-recovery experiments.

Contract (journal §3.3 design):
- Original evidence is NEVER modified; all experiments run on disposable copies.
- Workspaces must carry a safety marker and must never overlap the source tree
  or reference devices/symlinks.
- Every artifact is recorded with role, relative path, size, SHA-256.
- Manifests are canonicalized (sorted keys, no whitespace) before hashing;
  the seal digest is computed over the canonical serialization and stored
  separately from the manifest content.
"""

import hashlib
import json
import os
import stat
from pathlib import Path

WORKSPACE_MARKER = ".dbrec_disaster_workspace"
MANIFEST_VERSION = 1


class SafetyError(Exception):
    """Raised when a requested operation would violate evidence safety."""


# ---------------------------------------------------------------- workspace

def init_workspace(workspace: Path, purpose: str) -> Path:
    workspace = Path(workspace).resolve()
    if workspace.exists():
        if not workspace.is_dir():
            raise SafetyError(f"workspace path is not a directory: {workspace}")
        if any(workspace.iterdir()):
            raise SafetyError(f"refusing to use non-empty workspace: {workspace}")
    workspace.mkdir(parents=True, exist_ok=True)
    marker = workspace / WORKSPACE_MARKER
    marker.write_text(json.dumps({
        "version": MANIFEST_VERSION,
        "purpose": purpose,
    }, sort_keys=True) + "\n", encoding="utf-8")
    return workspace


def require_workspace(workspace: Path) -> Path:
    workspace = Path(workspace).resolve()
    if not workspace.is_dir():
        raise SafetyError(f"workspace does not exist: {workspace}")
    if not (workspace / WORKSPACE_MARKER).is_file():
        raise SafetyError(
            f"workspace lacks safety marker {WORKSPACE_MARKER}: {workspace}")
    return workspace


def is_dangerous_path(p: Path) -> bool:
    try:
        st = os.lstat(p)
    except OSError:
        return False
    return stat.S_ISBLK(st.st_mode) or stat.S_ISCHR(st.st_mode)


def is_within(child: Path, parent: Path) -> bool:
    try:
        child.resolve().relative_to(Path(parent).resolve())
        return True
    except ValueError:
        return False


def validate_source(source: Path) -> Path:
    """Validate a single evidence source file (never a device or symlink)."""
    source = Path(source)
    if is_dangerous_path(source):
        raise SafetyError(f"refusing raw device as evidence source: {source}")
    if source.is_symlink():
        raise SafetyError(f"refusing symlink as evidence source: {source}")
    if not source.is_file():
        raise SafetyError(f"evidence source is not a regular file: {source}")
    return source


def validate_output_dir(output_dir: Path, source: Path) -> Path:
    output_dir = Path(output_dir)
    if output_dir.is_symlink():
        raise SafetyError(f"refusing symlink as output dir: {output_dir}")
    if source and is_within(source.parent, output_dir):
        raise SafetyError(
            f"output dir would contain source dir {source.parent}: {output_dir}")
    if is_within(output_dir, source.parent):
        raise SafetyError(
            f"output dir inside source dir {source.parent}: {output_dir}")
    return output_dir


# ---------------------------------------------------------------- hashing

def sha256_file(path: Path, chunk_size: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_json(obj) -> str:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False)


def file_entry(path: Path, role: str, rel_root: Path = None) -> dict:
    path = Path(path)
    if rel_root is None:
        rel = path.name
    else:
        rel = str(path.resolve().relative_to(Path(rel_root).resolve()))
    return {
        "role": role,
        "path": rel,
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
    }


# ---------------------------------------------------------------- manifest

def build_manifest(run_dir: Path, files: list, extra: dict = None) -> dict:
    """files: list of dicts {role, path(relative or absolute), sha256, size}."""
    manifest = {
        "manifest_version": MANIFEST_VERSION,
        "files": sorted(files, key=lambda e: (e["role"], e["path"])),
    }
    if extra:
        manifest.update(extra)
    return manifest


def write_manifest(run_dir: Path, manifest: dict) -> Path:
    run_dir = Path(run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    path = run_dir / "manifest.json"
    if path.exists():
        raise SafetyError(f"refusing to overwrite existing manifest: {path}")
    with open(path, "w", encoding="utf-8") as f:
        f.write(canonical_json(manifest) + "\n")
    return path


def seal_run(run_dir: Path) -> str:
    """Compute the seal digest over the canonical manifest bytes.

    The seal is stored in a separate file so the manifest content stays
    byte-stable; verifying recomputes the digest from the manifest.
    """
    run_dir = Path(run_dir)
    manifest_path = run_dir / "manifest.json"
    if not manifest_path.is_file():
        raise SafetyError(f"no manifest to seal in {run_dir}")
    digest = sha256_file(manifest_path)
    seal = run_dir / "manifest.seal"
    if seal.exists():
        raise SafetyError(f"refusing to overwrite existing seal: {seal}")
    seal.write_text(digest + "\n", encoding="utf-8")
    return digest


def verify_seal(run_dir: Path) -> bool:
    run_dir = Path(run_dir)
    manifest_path = run_dir / "manifest.json"
    seal_path = run_dir / "manifest.seal"
    if not manifest_path.is_file() or not seal_path.is_file():
        return False
    return sha256_file(manifest_path) == seal_path.read_text(encoding="utf-8").strip()


def verify_source_immutable(source: Path, expected_sha256: str) -> bool:
    return sha256_file(Path(source)) == expected_sha256


def verify_input_clone(clone: Path, expected_sha256: str) -> bool:
    return sha256_file(Path(clone)) == expected_sha256
