"""Point the port at a release that now exists.

The portfile fetches a source archive by hash, and that hash cannot be known
before the tag is pushed: ``vcpkg_from_github`` downloads the archive GitHub
generates on demand for a tag, which is not byte-identical to the one
``git archive`` produces locally -- same contents, different gzip. So the port
is always updated *after* the tag it describes.

Edits are narrow substitutions rather than rewrites, preserving the bytes
``vcpkg format-manifest`` produced around each changed field. A new upstream
version also resets ``port-version``; Windows support changes only by opt-in.
"""

from __future__ import annotations

import contextlib
import hashlib
import json
import os
import pathlib
import re
import stat
import sys
import tempfile
import typing as t

from tools.vcpkg.check import VERSION_FIELDS

LEGACY_SUPPORTS: t.Final = "!windows & !mingw"
WINDOWS_SUPPORTS: t.Final = "(linux | osx) | (windows & x64 & !uwp & !xbox & !mingw)"
PORT_NAME: t.Final = "libtmux"

SHA512 = re.compile(
    r"^(?P<lead>[ \t]*SHA512[ \t]+)(?P<hash>[0-9a-fA-F]{128})"
    r"(?P<tail>[ \t]*)(?P<eol>\r\n|\n|\r|$)",
    re.MULTILINE,
)
PORT_VERSION = re.compile(
    r'^[ \t]*"port-version"[ \t]*:[ \t]*[0-9]+[ \t]*,[ \t]*(?:\r\n|\n|\r)',
    re.MULTILINE,
)
SEMVER = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$",
)


def _replace_version(text: str, field: str, version: str) -> str | None:
    """Substitute the manifest's version value, leaving every other byte."""
    pattern = re.compile(rf'("{re.escape(field)}"\s*:\s*)"[^"]*"')
    replaced, count = pattern.subn(
        lambda match: match.group(1) + json.dumps(version),
        text,
        count=1,
    )
    return replaced if count == 1 else None


def _replace_supports(text: str) -> str | None:
    """Enable the audited Windows target without reformatting the manifest."""
    pattern = re.compile(r'("supports"\s*:\s*)"!windows & !mingw"')
    replaced, count = pattern.subn(
        lambda match: match.group(1) + json.dumps(WINDOWS_SUPPORTS),
        text,
        count=1,
    )
    return replaced if count == 1 else None


def _compare_semver(left: str, right: str) -> int | None:
    """Compare SemVer precedence, or return None when either value is invalid."""
    parsed = (SEMVER.fullmatch(left), SEMVER.fullmatch(right))
    if parsed[0] is None or parsed[1] is None:
        return None
    left_match, right_match = t.cast(tuple[re.Match[str], re.Match[str]], parsed)
    left_pre = left_match.group(4)
    right_pre = right_match.group(4)
    for prerelease in (left_pre, right_pre):
        if prerelease is not None and any(
            len(part) > 1 and part.startswith("0") and part.isdigit()
            for part in prerelease.split(".")
        ):
            return None

    def compare_numeric(left_numeric: str, right_numeric: str) -> int:
        if len(left_numeric) != len(right_numeric):
            return 1 if len(left_numeric) > len(right_numeric) else -1
        if left_numeric == right_numeric:
            return 0
        return 1 if left_numeric > right_numeric else -1

    for index in range(1, 4):
        core_order = compare_numeric(
            left_match.group(index),
            right_match.group(index),
        )
        if core_order != 0:
            return core_order

    if left_pre is None or right_pre is None:
        if left_pre == right_pre:
            return 0
        return 1 if left_pre is None else -1
    for left_part, right_part in zip(
        left_pre.split("."),
        right_pre.split("."),
        strict=False,
    ):
        if left_part == right_part:
            continue
        left_numeric = left_part.isdigit()
        right_numeric = right_part.isdigit()
        if left_numeric and right_numeric:
            return compare_numeric(left_part, right_part)
        if left_numeric != right_numeric:
            return -1 if left_numeric else 1
        return 1 if left_part > right_part else -1
    left_count = len(left_pre.split("."))
    right_count = len(right_pre.split("."))
    if left_count == right_count:
        return 0
    return 1 if left_count > right_count else -1


def _read_utf8(path: pathlib.Path) -> str | None:
    try:
        return path.read_bytes().decode("utf-8")
    except UnicodeDecodeError as error:
        print(f"error: {path}: is not UTF-8: {error}", file=sys.stderr)
        return None


def _stage_bytes(path: pathlib.Path, contents: bytes) -> pathlib.Path:
    """Write a replacement beside its target without exposing partial bytes."""
    descriptor, temporary = tempfile.mkstemp(
        prefix=f".{path.name}.release-",
        dir=path.parent,
    )
    staged = pathlib.Path(temporary)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(contents)
            stream.flush()
            staged.chmod(stat.S_IMODE(path.stat().st_mode))
            os.fsync(stream.fileno())
    except BaseException:
        staged.unlink(missing_ok=True)
        raise
    return staged


def _sync_directory(directory: pathlib.Path) -> None:
    """Make replacement ordering durable on filesystems that expose directory fsync."""
    if os.name == "nt":
        return
    descriptor = os.open(
        directory,
        os.O_RDONLY | getattr(os, "O_DIRECTORY", 0),
    )
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _commit_outputs(
    outputs: list[tuple[pathlib.Path, bytes, bytes]],
) -> None:
    """Replace every changed file and restore earlier replacements on failure."""
    staged: list[tuple[pathlib.Path, pathlib.Path, bytes]] = []
    replaced: list[tuple[pathlib.Path, bytes]] = []
    try:
        for path, updated, original in outputs:
            staged.append((path, _stage_bytes(path, updated), original))
        for path, temporary, original in staged:
            os.replace(temporary, path)  # noqa: PTH105
            replaced.append((path, original))
            _sync_directory(path.parent)
    except OSError as error:
        rollback_errors: list[str] = []
        for path, original in reversed(replaced):
            rollback: pathlib.Path | None = None
            try:
                rollback = _stage_bytes(path, original)
                os.replace(rollback, path)  # noqa: PTH105
                _sync_directory(path.parent)
            except OSError as rollback_error:
                rollback_errors.append(f"{path}: {rollback_error}")
            finally:
                if rollback is not None:
                    rollback.unlink(missing_ok=True)
        if rollback_errors:
            details = "; ".join(rollback_errors)
            message = f"{error}; rollback failed: {details}"
            raise OSError(message) from error
        raise
    finally:
        for _, temporary, _ in staged:
            temporary.unlink(missing_ok=True)


def _remove_stale_outputs(paths: tuple[pathlib.Path, ...]) -> None:
    """Remove inert staging files left by an interrupted earlier release."""
    for path in paths:
        for stale in path.parent.glob(f".{path.name}.release-*"):
            stale.unlink()


@contextlib.contextmanager
def _release_lock(root: pathlib.Path) -> t.Iterator[None]:
    """Hold one process-wide release writer lock for this repository."""
    identity = hashlib.sha256(os.fsencode(root)).hexdigest()[:24]
    lock_path = pathlib.Path(tempfile.gettempdir()) / f"libtmux-release-{identity}.lock"
    stream = lock_path.open("a+b")
    acquired = False
    try:
        if os.name == "nt":
            import msvcrt

            stream.seek(0)
            if stream.read(1) == b"":
                stream.write(b"\0")
                stream.flush()
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl

            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        acquired = True
        yield
    finally:
        if acquired:
            if os.name == "nt":
                import msvcrt

                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl

                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)
        stream.close()


def _run_without_lock(
    root: pathlib.Path,
    *,
    version: str,
    sha512: str,
    enable_windows: bool = False,
) -> int:
    """Set the port's version and source hash to a release that exists."""
    if not re.fullmatch(r"[0-9a-fA-F]{128}", sha512):
        print(
            f"error: {sha512!r} is not a SHA512: expected 128 hex characters",
            file=sys.stderr,
        )
        return 2

    try:
        resolved_root = root.resolve(strict=True)
    except OSError as error:
        print(
            f"error: {root}: cannot resolve repository root: {error}", file=sys.stderr
        )
        return 2

    port_dir = resolved_root / "ports" / PORT_NAME
    if port_dir.is_symlink():
        print(
            f"error: {port_dir}: symbolic-link port directories are refused",
            file=sys.stderr,
        )
        return 2
    manifest_path = port_dir / "vcpkg.json"
    portfile_path = port_dir / "portfile.cmake"
    for path in (manifest_path, portfile_path):
        if not path.is_file():
            print(f"error: {path}: missing", file=sys.stderr)
            return 2
        if path.is_symlink():
            print(
                f"error: {path}: symbolic-link release inputs are refused",
                file=sys.stderr,
            )
            return 2
        try:
            path.resolve(strict=True).relative_to(resolved_root)
        except (OSError, ValueError) as error:
            print(f"error: {path}: escapes repository root: {error}", file=sys.stderr)
            return 2

    try:
        _remove_stale_outputs((manifest_path, portfile_path))
    except OSError as error:
        print(
            f"error: stale release files could not be removed: {error}", file=sys.stderr
        )
        return 2

    manifest_text = _read_utf8(manifest_path)
    portfile_text = _read_utf8(portfile_path)
    if manifest_text is None or portfile_text is None:
        return 2
    try:
        manifest = json.loads(manifest_text)
    except json.JSONDecodeError as error:
        print(f"error: {manifest_path}: invalid JSON: {error}", file=sys.stderr)
        return 2
    if not isinstance(manifest, dict):
        print(f"error: {manifest_path}: manifest is not an object", file=sys.stderr)
        return 2
    if manifest.get("name") != PORT_NAME:
        print(
            f"error: {manifest_path}: expected port name {PORT_NAME!r}",
            file=sys.stderr,
        )
        return 2

    fields = [name for name in VERSION_FIELDS if name in manifest]
    if fields != ["version-semver"]:
        declared = ", ".join(fields) if fields else "none"
        print(
            f"error: {manifest_path}: declares {declared}; libtmux releases require "
            "exactly version-semver",
            file=sys.stderr,
        )
        return 2
    field = fields[0]

    was_version = str(manifest[field])
    hashes = list(SHA512.finditer(portfile_text))
    if len(hashes) != 1:
        print(
            f"error: {portfile_path}: expected exactly one SHA512 line; "
            f"found {len(hashes)}",
            file=sys.stderr,
        )
        return 2
    found = hashes[0]
    was_hash = found.group("hash")
    version_changed = was_version != version
    hash_changed = was_hash.lower() != sha512.lower()

    comparison = _compare_semver(version, was_version)
    if comparison is None:
        print(
            f"error: {manifest_path}: release versions must be valid SemVer",
            file=sys.stderr,
        )
        return 2
    if version_changed and comparison <= 0:
        print(
            f"error: {manifest_path}: refusing non-increasing release "
            f"{was_version} -> {version}",
            file=sys.stderr,
        )
        return 2

    if not version_changed and hash_changed:
        print(
            f"error: {portfile_path}: refusing to change the source hash of "
            f"published version {version}",
            file=sys.stderr,
        )
        return 2

    supports = manifest.get("supports")
    transition_supports = False
    if version_changed and supports not in {LEGACY_SUPPORTS, WINDOWS_SUPPORTS}:
        print(
            f"error: {manifest_path}: refusing unrecognized supports policy "
            f"{supports!r} on a new release",
            file=sys.stderr,
        )
        return 2
    if version_changed and supports == WINDOWS_SUPPORTS and not enable_windows:
        print(
            f"error: {manifest_path}: each new Windows-enabled release requires "
            "--enable-windows",
            file=sys.stderr,
        )
        return 2
    if enable_windows:
        if supports == LEGACY_SUPPORTS:
            if not version_changed:
                print(
                    f"error: {manifest_path}: Windows support requires a new "
                    "upstream version",
                    file=sys.stderr,
                )
                return 2
            transition_supports = True
        elif supports != WINDOWS_SUPPORTS:
            print(
                f"error: {manifest_path}: --enable-windows requires supports "
                f"to be {LEGACY_SUPPORTS!r} or {WINDOWS_SUPPORTS!r}",
                file=sys.stderr,
            )
            return 2

    updated_manifest = manifest_text
    expected_manifest = dict(manifest)
    if version_changed:
        replaced = _replace_version(updated_manifest, field, version)
        if replaced is None:
            print(f"error: {manifest_path}: could not place {field}", file=sys.stderr)
            return 2
        updated_manifest = replaced
        expected_manifest[field] = version
        if "port-version" in manifest:
            updated_manifest, count = PORT_VERSION.subn("", updated_manifest, count=1)
            if count != 1:
                print(
                    f"error: {manifest_path}: could not remove port-version",
                    file=sys.stderr,
                )
                return 2
            expected_manifest.pop("port-version")

    if transition_supports:
        replaced = _replace_supports(updated_manifest)
        if replaced is None:
            print(f"error: {manifest_path}: could not place supports", file=sys.stderr)
            return 2
        updated_manifest = replaced
        expected_manifest["supports"] = WINDOWS_SUPPORTS

    try:
        decoded_manifest = json.loads(updated_manifest)
    except json.JSONDecodeError as error:
        print(
            f"error: {manifest_path}: update would produce invalid JSON: {error}",
            file=sys.stderr,
        )
        return 2
    if decoded_manifest != expected_manifest:
        print(
            f"error: {manifest_path}: update would change unrelated fields",
            file=sys.stderr,
        )
        return 2

    updated_portfile = portfile_text
    if version_changed:
        updated_portfile = SHA512.sub(
            lambda match: (
                match.group("lead") + sha512 + match.group("tail") + match.group("eol")
            ),
            portfile_text,
            count=1,
        )
        replaced_hash = SHA512.search(updated_portfile)
        if replaced_hash is None or replaced_hash.group("hash") != sha512:
            print(
                f"error: {portfile_path}: could not validate the updated SHA512",
                file=sys.stderr,
            )
            return 2

    # Replacing the hash first makes an interrupted update safe to rerun: the
    # old manifest still declares a new-version transition on the next attempt.
    outputs = [
        (
            portfile_path,
            updated_portfile.encode("utf-8"),
            portfile_text.encode("utf-8"),
        ),
        (
            manifest_path,
            updated_manifest.encode("utf-8"),
            manifest_text.encode("utf-8"),
        ),
    ]
    changed_outputs = [
        (path, updated, original)
        for path, updated, original in outputs
        if updated != original
    ]
    try:
        _commit_outputs(changed_outputs)
    except OSError as error:
        print(f"error: release files could not be updated: {error}", file=sys.stderr)
        return 2

    if not changed_outputs:
        print("nothing changed; the port already describes this release")
        return 0

    print(
        f"{manifest_path.relative_to(resolved_root)}: "
        f"{field} {was_version} -> {version}"
    )
    print(
        f"{portfile_path.relative_to(resolved_root)}: "
        f"SHA512 {was_hash[:16]} -> {sha512[:16]}",
    )
    return 0


def run(
    root: pathlib.Path,
    *,
    version: str,
    sha512: str,
    enable_windows: bool = False,
) -> int:
    """Update one release while excluding concurrent writers and cleanup."""
    try:
        resolved_root = root.resolve(strict=True)
        with _release_lock(resolved_root):
            return _run_without_lock(
                resolved_root,
                version=version,
                sha512=sha512,
                enable_windows=enable_windows,
            )
    except OSError as error:
        print(
            f"error: release update is locked or unavailable: {error}", file=sys.stderr
        )
        return 2
