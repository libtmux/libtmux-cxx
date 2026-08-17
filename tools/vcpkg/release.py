"""Point the port at a release that now exists.

The portfile fetches a source archive by hash, and that hash cannot be known
before the tag is pushed: ``vcpkg_from_github`` downloads the archive GitHub
generates on demand for a tag, which is not byte-identical to the one
``git archive`` produces locally -- same contents, different gzip. So the port
is always updated *after* the tag it describes.

Both edits are substitutions rather than rewrites. The manifest is kept in the
exact bytes ``vcpkg format-manifest`` produced, so a release changes one value
and the diff shows one line.
"""

from __future__ import annotations

import json
import pathlib
import re
import sys

from tools.vcpkg.check import VERSION_FIELDS

SHA512 = re.compile(
    r"^(?P<lead>\s*SHA512\s+)(?P<hash>[0-9a-fA-F]{128})\s*$",
    re.MULTILINE,
)


def _replace_version(text: str, field: str, version: str) -> str | None:
    """Substitute the manifest's version value, leaving every other byte."""
    pattern = re.compile(rf'("{re.escape(field)}"\s*:\s*)"[^"]*"')
    replaced, count = pattern.subn(rf'\g<1>"{version}"', text, count=1)
    return replaced if count == 1 else None


def run(
    root: pathlib.Path,
    *,
    version: str,
    sha512: str,
    port: str = "libtmux",
) -> int:
    """Set the port's version and source hash to a release that exists."""
    if not re.fullmatch(r"[0-9a-fA-F]{128}", sha512):
        print(
            f"error: {sha512!r} is not a SHA512: expected 128 hex characters",
            file=sys.stderr,
        )
        return 2

    port_dir = root / "ports" / port
    manifest_path = port_dir / "vcpkg.json"
    portfile_path = port_dir / "portfile.cmake"
    for path in (manifest_path, portfile_path):
        if not path.is_file():
            print(f"error: {path}: missing", file=sys.stderr)
            return 2

    manifest_text = manifest_path.read_text()
    manifest = json.loads(manifest_text)
    field = next((name for name in VERSION_FIELDS if name in manifest), None)
    if field is None:
        print(f"error: {manifest_path}: declares no version field", file=sys.stderr)
        return 2

    was_version = str(manifest[field])
    updated_manifest = _replace_version(manifest_text, field, version)
    if updated_manifest is None:
        print(f"error: {manifest_path}: could not place {field}", file=sys.stderr)
        return 2

    portfile_text = portfile_path.read_text()
    found = SHA512.search(portfile_text)
    if found is None:
        print(f"error: {portfile_path}: no SHA512 line to replace", file=sys.stderr)
        return 2
    was_hash = found.group("hash")
    updated_portfile = SHA512.sub(rf"\g<lead>{sha512}", portfile_text, count=1)

    manifest_path.write_text(updated_manifest)
    portfile_path.write_text(updated_portfile)

    print(f"{manifest_path.relative_to(root)}: {field} {was_version} -> {version}")
    print(
        f"{portfile_path.relative_to(root)}: SHA512 {was_hash[:16]} -> {sha512[:16]}",
    )
    if was_version == version and was_hash == sha512:
        print("nothing changed; the port already describes this release")
    return 0
