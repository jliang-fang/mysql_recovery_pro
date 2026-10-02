#!/usr/bin/env python3
"""Controlled real-media preflight and destructive-operation approval tokens."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
from dataclasses import asdict, dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Iterable, Optional

from scripts.disaster_harness.manifest import canonical_json, sha256_bytes

ALLOWED_DEVICE = "/dev/nvme0n2"
TOKEN_VERSION = 1
PREFLIGHT_VERSION = 1
_DESTRUCTIVE_ACTIONS = frozenset({"format", "mount", "drop", "truncate", "rm-rf", "corrupt"})
_HEX_DIGEST = re.compile(r"^[0-9a-f]{64}$")


class DeviceSafetyError(RuntimeError):
    """Raised when a real-media operation is not provably isolated."""


@dataclass(frozen=True)
class DeviceIdentity:
    path: str
    major_minor: str
    size_bytes: int
    serial: str
    model: str
    wwn: str
    filesystem_uuid: str
    filesystem_type: str


@dataclass(frozen=True)
class ApprovalToken:
    token_version: int
    run_id: str
    action: str
    device: DeviceIdentity
    preflight_sha256: str
    issued_at: str
    expires_at: str
    nonce: str


def _run(argv: list[str]) -> str:
    completed = subprocess.run(argv, check=True, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    return completed.stdout


def _json_command(argv: list[str]) -> dict[str, Any]:
    try:
        return json.loads(_run(argv))
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        raise DeviceSafetyError(f"unable to inspect system state with {' '.join(argv)}: {error}") from error


def _optional_json_command(argv: list[str]) -> dict[str, Any]:
    try:
        return _json_command(argv)
    except DeviceSafetyError:
        return {}


def _device_record(device_path: str) -> dict[str, Any]:
    listing = _json_command([
        "lsblk", "--json", "--bytes", "--output",
        "PATH,TYPE,SIZE,RO,PKNAME,MAJ:MIN,MOUNTPOINTS,MODEL,SERIAL,WWN,FSTYPE,UUID",
        device_path,
    ])
    devices = listing.get("blockdevices", [])
    if len(devices) != 1 or devices[0].get("path") != device_path:
        raise DeviceSafetyError(f"unable to resolve exactly one device: {device_path}")
    record = devices[0]
    if record.get("type") != "disk" or record.get("pkname"):
        raise DeviceSafetyError(f"device is not an independent disk: {device_path}")
    if record.get("children"):
        raise DeviceSafetyError(f"device unexpectedly has child mappings: {device_path}")
    return record


def read_device_identity(device_path: str = ALLOWED_DEVICE) -> DeviceIdentity:
    if os.path.realpath(device_path) != ALLOWED_DEVICE:
        raise DeviceSafetyError(f"real-media validation only permits {ALLOWED_DEVICE}")
    record = _device_record(ALLOWED_DEVICE)
    required = {
        "major_minor": record.get("maj:min", ""),
        "size_bytes": record.get("size", 0),
        "serial": record.get("serial", ""),
        "model": record.get("model", ""),
        "wwn": record.get("wwn", ""),
    }
    if not all(required.values()):
        raise DeviceSafetyError("device identity is incomplete")
    return DeviceIdentity(
        path=ALLOWED_DEVICE,
        major_minor=str(required["major_minor"]),
        size_bytes=int(required["size_bytes"]),
        serial=str(required["serial"]),
        model=str(required["model"]),
        wwn=str(required["wwn"]),
        filesystem_uuid=str(record.get("uuid") or ""),
        filesystem_type=str(record.get("fstype") or ""),
    )


def _mounts_for(source: str) -> list[dict[str, Any]]:
    listing = _optional_json_command([
        "findmnt", "--json", "--output", "SOURCE,TARGET,FSTYPE,OPTIONS", "--source", source,
    ])
    return listing.get("filesystems", [])


def _preflight_report(identity: DeviceIdentity, evidence_root: Path,
                      target_mounts: list[dict[str, Any]], evidence_mounts: list[dict[str, Any]],
                      holders: list[str], dependencies: dict[str, Any]) -> dict[str, Any]:
    report = {
        "preflight_version": PREFLIGHT_VERSION,
        "status": "PRECHECKED",
        "checked_at": datetime.now(timezone.utc).isoformat(),
        "device": asdict(identity),
        "device_read_only": False,
        "device_mounts": target_mounts,
        "device_holders": holders,
        "evidence_root": str(evidence_root),
        "evidence_mounts": evidence_mounts,
        "storage_dependencies": dependencies,
    }
    report["preflight_sha256"] = sha256_bytes(canonical_json(report).encode("utf-8"))
    return report


def _preflight_facts(device_path: str, evidence_root: Path) -> tuple[
        DeviceIdentity, Path, list[dict[str, Any]], list[dict[str, Any]], list[str], dict[str, Any]]:
    identity = read_device_identity(device_path)
    evidence_root = Path(evidence_root).resolve()
    if not evidence_root.is_dir() or evidence_root.is_symlink():
        raise DeviceSafetyError(f"evidence root must be an existing non-symlink directory: {evidence_root}")

    target_mounts = _mounts_for(identity.path)
    evidence_mounts = _optional_json_command(
        ["findmnt", "--json", "--target", str(evidence_root)]
    ).get("filesystems", [])
    evidence_sources = {entry.get("source") for entry in evidence_mounts}
    root_device = os.path.realpath(identity.path)
    if root_device in {os.path.realpath(str(source)) for source in evidence_sources if source}:
        raise DeviceSafetyError("evidence root is located on the test device")

    holders = _holders(identity.path)
    if holders:
        raise DeviceSafetyError(f"test device has active holders: {', '.join(holders)}")

    dependencies = _storage_dependencies()
    serialized_dependencies = canonical_json(dependencies)
    if identity.path in serialized_dependencies or identity.major_minor in serialized_dependencies:
        raise DeviceSafetyError("test device is referenced by storage, swap, RAID, or Podman state")
    return identity, evidence_root, target_mounts, evidence_mounts, holders, dependencies


def _holders(device_path: str) -> list[str]:
    name = Path(device_path).name
    holders = Path("/sys/class/block") / name / "holders"
    try:
        return sorted(entry.name for entry in holders.iterdir())
    except OSError as error:
        raise DeviceSafetyError(f"unable to inspect device holders: {error}") from error


def _podman_store() -> dict[str, Any]:
    try:
        return json.loads(_run(["podman", "info", "--format", "{{json .Store}}"]))
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        raise DeviceSafetyError(f"unable to inspect Podman store: {error}") from error


def _storage_dependencies() -> dict[str, Any]:
    return {
        "pvs": _optional_json_command(["pvs", "--reportformat", "json", "--units", "b", "--nosuffix"]),
        "mdstat": Path("/proc/mdstat").read_text(encoding="utf-8"),
        "swaps": Path("/proc/swaps").read_text(encoding="utf-8"),
        "podman_store": _podman_store(),
    }


def preflight(device_path: str, evidence_root: Path) -> dict[str, Any]:
    """Collect non-mutating facts and reject unsafe real-media configurations."""
    identity, evidence_root, target_mounts, evidence_mounts, holders, dependencies = _preflight_facts(
        device_path, evidence_root
    )
    if target_mounts:
        raise DeviceSafetyError(f"test device is mounted: {identity.path}")
    return _preflight_report(identity, evidence_root, target_mounts, evidence_mounts, holders, dependencies)


def preflight_controlled_mount(device_path: str, evidence_root: Path, mount_point: Path) -> dict[str, Any]:
    """Verify the sole expected ext4/RW mount for the restricted DROP smoke workflow."""
    identity, evidence_root, target_mounts, evidence_mounts, holders, dependencies = _preflight_facts(
        device_path, evidence_root
    )
    expected_target = Path(mount_point).resolve()
    if len(target_mounts) != 1:
        raise DeviceSafetyError("controlled workflow requires exactly one target mount")
    mount = target_mounts[0]
    source = mount.get("source", "")
    target = mount.get("target", "")
    filesystem = mount.get("fstype", "")
    options = {option.strip() for option in mount.get("options", "").split(",")}
    if os.path.realpath(str(source)) != identity.path:
        raise DeviceSafetyError("controlled mount source does not match the test device")
    if Path(target).resolve() != expected_target:
        raise DeviceSafetyError("controlled mount target does not match the configured mount point")
    if filesystem != "ext4":
        raise DeviceSafetyError("controlled mount must use ext4")
    if "rw" not in options:
        raise DeviceSafetyError("controlled mount must be read-write")
    return _preflight_report(identity, evidence_root, target_mounts, evidence_mounts, holders, dependencies)


def write_preflight(evidence_root: Path, report: dict[str, Any]) -> Path:
    path = Path(evidence_root).resolve() / "preflight"
    path.mkdir(mode=0o700, exist_ok=False)
    output = path / "preflight.json"
    output.write_text(canonical_json(report) + "\n", encoding="utf-8")
    return output


def issue_approval_token(preflight_report: dict[str, Any], run_id: str, action: str,
                         expires_in: timedelta = timedelta(minutes=15), nonce: str = "") -> ApprovalToken:
    if action not in _DESTRUCTIVE_ACTIONS:
        raise DeviceSafetyError(f"unsupported destructive action: {action}")
    if preflight_report.get("status") != "PRECHECKED":
        raise DeviceSafetyError("approval requires a successful preflight")
    expected = preflight_report.get("preflight_sha256", "")
    if not _HEX_DIGEST.fullmatch(expected):
        raise DeviceSafetyError("preflight report lacks a valid hash")
    if not run_id or Path(run_id).name != run_id:
        raise DeviceSafetyError("run ID must be a single non-empty path component")
    now = datetime.now(timezone.utc)
    device = DeviceIdentity(**preflight_report["device"])
    nonce = nonce or os.urandom(16).hex()
    return ApprovalToken(
        token_version=TOKEN_VERSION,
        run_id=run_id,
        action=action,
        device=device,
        preflight_sha256=expected,
        issued_at=now.isoformat(),
        expires_at=(now + expires_in).isoformat(),
        nonce=nonce,
    )


def write_approval_token(evidence_root: Path, token: ApprovalToken) -> Path:
    tokens = Path(evidence_root).resolve() / "approval_tokens"
    tokens.mkdir(mode=0o700, exist_ok=True)
    token_id = sha256_bytes(canonical_json(asdict(token)).encode("utf-8"))
    path = tokens / f"{token.run_id}-{token.action}-{token_id[:16]}.json"
    if path.exists():
        raise DeviceSafetyError(f"refusing to overwrite approval token: {path}")
    path.write_text(canonical_json(asdict(token)) + "\n", encoding="utf-8")
    return path


def validate_approval_token(token: ApprovalToken, current_identity: DeviceIdentity,
                            run_id: str, action: str, preflight_sha256: str,
                            now: Optional[datetime] = None) -> None:
    now = now or datetime.now(timezone.utc)
    if token.token_version != TOKEN_VERSION:
        raise DeviceSafetyError("unsupported approval token version")
    if token.run_id != run_id or token.action != action:
        raise DeviceSafetyError("approval token action or run ID does not match")
    if token.preflight_sha256 != preflight_sha256:
        raise DeviceSafetyError("approval token does not bind to this preflight")
    if token.device != current_identity:
        raise DeviceSafetyError("device identity drifted after preflight")
    if now >= datetime.fromisoformat(token.expires_at):
        raise DeviceSafetyError("approval token expired")


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Read-only real-media disaster-validation preflight")
    parser.add_argument("--device", default=ALLOWED_DEVICE)
    parser.add_argument("--evidence-root", required=True)
    parser.add_argument("--write", action="store_true", help="write the read-only preflight report")
    args = parser.parse_args(argv)
    try:
        report = preflight(args.device, Path(args.evidence_root))
        if args.write:
            path = write_preflight(Path(args.evidence_root), report)
            print(path)
        else:
            print(json.dumps(report, indent=2, sort_keys=True))
    except DeviceSafetyError as error:
        print(f"preflight_rejected: {error}", file=os.sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
