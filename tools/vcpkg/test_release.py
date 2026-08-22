"""Regression tests for safe vcpkg release transitions."""

from __future__ import annotations

import contextlib
import io
import json
import pathlib
import tempfile
import unittest
from unittest import mock

from tools.vcpkg import __main__ as vcpkg_main
from tools.vcpkg import release
from tools.vcpkg.check import VERSION_FIELDS

OLD_HASH = "1" * 128
NEW_HASH = "2" * 128
OLD_VERSION = "1.2.3"
NEW_VERSION = "1.2.4"


class ReleaseTest(unittest.TestCase):
    """Keep release updates explicit, immutable, and byte-preserving."""

    def setUp(self) -> None:
        """Create one isolated port fixture."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.port_dir = self.root / "ports" / "libtmux"
        self.port_dir.mkdir(parents=True)
        self.manifest_path = self.port_dir / "vcpkg.json"
        self.portfile_path = self.port_dir / "portfile.cmake"

    def write_port(
        self,
        *,
        field: str = "version-semver",
        version: str = OLD_VERSION,
        port_version: int | None = 7,
        supports: str | None = release.LEGACY_SUPPORTS,
        newline: str = "\n",
        hash_tail: str = "",
        after_hash: str = "",
    ) -> None:
        """Write a formatted port while retaining exact fixture bytes."""
        lines = ["{", '  "name": "libtmux",', f'  "{field}": "{version}",']
        if port_version is not None:
            lines.append(f'  "port-version": {port_version},')
        lines.append('  "description": "fixture",')
        if supports is not None:
            lines.append(f'  "supports": {json.dumps(supports)},')
        lines.extend(['  "license": "MIT"', "}", ""])
        self.manifest_path.write_bytes(newline.join(lines).encode("utf-8"))
        self.portfile_path.write_bytes(
            (
                f"vcpkg_from_github({newline}"
                f"    OUT_SOURCE_PATH SOURCE_PATH{newline}"
                f"    SHA512 {OLD_HASH}{hash_tail}{newline}"
                f"{after_hash}"
                f"){newline}"
            ).encode()
        )

    def snapshot(self) -> tuple[bytes, bytes]:
        """Return both release inputs exactly as stored."""
        return self.manifest_path.read_bytes(), self.portfile_path.read_bytes()

    def run_release(
        self,
        *,
        version: str = NEW_VERSION,
        sha512: str = NEW_HASH,
        enable_windows: bool = False,
    ) -> int:
        """Run the release tool without writing its report to the test log."""
        with (
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            return release.run(
                self.root,
                version=version,
                sha512=sha512,
                enable_windows=enable_windows,
            )

    def assert_failed_without_writes(self, **kwargs: object) -> None:
        """Assert a rejected transition leaves both inputs byte-for-byte intact."""
        before = self.snapshot()
        with mock.patch.object(release, "_commit_outputs") as commit_outputs:
            self.assertEqual(self.run_release(**kwargs), 2)
        commit_outputs.assert_not_called()
        self.assertEqual(self.snapshot(), before)

    def test_new_version_resets_port_revision_and_preserves_supports(self) -> None:
        """Reset packaging revisions without implicitly enabling Windows."""
        self.write_port(newline="\r\n")

        self.assertEqual(self.run_release(), 0)

        manifest = self.manifest_path.read_bytes()
        self.assertIn(f'"version-semver": "{NEW_VERSION}"'.encode(), manifest)
        self.assertNotIn(b'"port-version"', manifest)
        self.assertIn(
            f'"supports": {json.dumps(release.LEGACY_SUPPORTS)}'.encode(),
            manifest,
        )
        self.assertIn(f"SHA512 {NEW_HASH}".encode(), self.portfile_path.read_bytes())
        self.assertIn(b"\r\n", manifest)
        self.assertNotIn(b"\n", manifest.replace(b"\r\n", b""))

    def test_hash_update_preserves_line_endings_spaces_and_blank_lines(self) -> None:
        """Change only the digest bytes in a formatted CRLF portfile."""
        self.write_port(
            newline="\r\n",
            hash_tail="  ",
            after_hash="\r\n",
        )
        before = self.portfile_path.read_bytes()

        self.assertEqual(self.run_release(), 0)

        after = self.portfile_path.read_bytes()
        self.assertEqual(after.replace(NEW_HASH.encode(), OLD_HASH.encode()), before)

    def test_enable_windows_transitions_only_the_exact_legacy_value(self) -> None:
        """Lift the old exclusion only on an explicitly opted-in new release."""
        self.write_port()

        self.assertEqual(self.run_release(enable_windows=True), 0)

        manifest = json.loads(self.manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(
            manifest["supports"],
            "(linux | osx) | (windows & x64 & !uwp & !xbox & !mingw)",
        )
        self.assertNotIn("port-version", manifest)

    def test_non_semver_version_fields_are_rejected(self) -> None:
        """Keep the repo-specific release command on libtmux's version scheme."""
        for field in VERSION_FIELDS:
            if field == "version-semver":
                continue
            with self.subTest(field=field):
                self.write_port(field=field)
                self.assert_failed_without_writes()

    def test_versions_must_advance_by_semver_precedence(self) -> None:
        """Reject stale tags, malformed versions, and build-only changes."""
        cases = (
            ("1.2.2", "1.2.3"),
            ("1.2.3-alpha.2", "1.2.3-alpha.3"),
            ("1.2.4-alpha.02", "1.2.3"),
            ("1.2.3+other", "1.2.3+build"),
            ("01.2.4", "1.2.3"),
        )
        for requested, current in cases:
            with self.subTest(requested=requested, current=current):
                self.write_port(version=current)
                self.assert_failed_without_writes(version=requested)

        self.write_port(version="1.2.3-alpha.2")
        self.assertEqual(self.run_release(version="1.2.3-alpha.3"), 0)

    def test_semver_comparison_handles_unbounded_numeric_identifiers(self) -> None:
        """Compare valid SemVer numerics without Python integer conversion limits."""
        enormous = "9" * 5000
        self.assertEqual(release._compare_semver(f"{enormous}.0.0", "8.0.0"), 1)
        self.assertEqual(
            release._compare_semver(f"1.0.0-{enormous}", "1.0.0-8"),
            1,
        )

    def test_explicit_zero_port_revision_is_removed(self) -> None:
        """Keep a new upstream version at implicit port revision zero."""
        self.write_port(port_version=0)

        self.assertEqual(self.run_release(), 0)

        manifest = json.loads(self.manifest_path.read_text(encoding="utf-8"))
        self.assertNotIn("port-version", manifest)

    def test_transitioned_supports_survive_the_next_release(self) -> None:
        """Keep an already-enabled policy stable across later releases."""
        self.write_port(supports=release.WINDOWS_SUPPORTS, port_version=None)

        self.assertEqual(self.run_release(enable_windows=True), 0)

        manifest = json.loads(self.manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(manifest["supports"], release.WINDOWS_SUPPORTS)

    def test_each_new_windows_release_requires_the_opt_in(self) -> None:
        """Require fresh tagged-source evidence for every enabled release."""
        self.write_port(supports=release.WINDOWS_SUPPORTS, port_version=None)
        self.assert_failed_without_writes()

    def test_same_version_cannot_change_the_published_hash(self) -> None:
        """Reject a mutable source archive for an already-published version."""
        self.write_port()
        self.assert_failed_without_writes(version=OLD_VERSION, sha512=NEW_HASH)

    def test_invalid_hash_fails_before_either_write(self) -> None:
        """Reject values that are not complete hexadecimal SHA-512 digests."""
        self.write_port()

        for sha512 in ("", "0" * 127, "g" * 128):
            with self.subTest(sha512=sha512):
                self.assert_failed_without_writes(sha512=sha512)

    def test_same_version_cannot_enable_legacy_windows_support(self) -> None:
        """Require a new upstream version for the Windows support transition."""
        self.write_port()
        self.assert_failed_without_writes(
            version=OLD_VERSION,
            sha512=OLD_HASH,
            enable_windows=True,
        )

    def test_same_transitioned_release_is_a_byte_for_byte_noop(self) -> None:
        """Make an opted-in release rerun exactly idempotent."""
        self.write_port(supports=release.WINDOWS_SUPPORTS)
        before = self.snapshot()

        with mock.patch.object(release, "_commit_outputs") as commit_outputs:
            self.assertEqual(
                self.run_release(
                    version=OLD_VERSION,
                    sha512=OLD_HASH.upper(),
                    enable_windows=True,
                ),
                0,
            )
        commit_outputs.assert_called_once_with([])
        self.assertEqual(self.snapshot(), before)

    def test_enable_windows_rejects_unknown_or_missing_supports(self) -> None:
        """Fail closed when the manifest is not in a recognized support state."""
        for supports in (None, "!windows", "windows"):
            with self.subTest(supports=supports):
                self.write_port(supports=supports)
                self.assert_failed_without_writes(enable_windows=True)

    def test_new_release_rejects_unknown_or_missing_supports(self) -> None:
        """Never carry an unowned platform policy into a new release."""
        for supports in (None, "custom-policy"):
            with self.subTest(supports=supports):
                self.write_port(supports=supports)
                self.assert_failed_without_writes()

    def test_same_unknown_release_is_an_exact_noop(self) -> None:
        """Permit inspection-only reruns that make no policy or byte change."""
        for supports in (None, "custom-policy"):
            with self.subTest(supports=supports):
                self.write_port(supports=supports)
                before = self.snapshot()
                self.assertEqual(
                    self.run_release(version=OLD_VERSION, sha512=OLD_HASH),
                    0,
                )
                self.assertEqual(self.snapshot(), before)

    def test_invalid_output_shape_fails_before_either_write(self) -> None:
        """Reject an unplaceable revision without partially updating the hash."""
        self.write_port(port_version=None)
        manifest = json.loads(self.manifest_path.read_text(encoding="utf-8"))
        manifest["port-version"] = 7
        self.manifest_path.write_text(
            json.dumps(manifest, indent=2) + "\n",
            encoding="utf-8",
        )
        self.assert_failed_without_writes()

    def test_malformed_or_ambiguous_inputs_fail_before_either_write(self) -> None:
        """Reject inputs that cannot identify one manifest version and hash."""
        cases = ("invalid-json", "two-versions", "no-hash", "two-hashes")
        for case in cases:
            with self.subTest(case=case):
                self.write_port()
                if case == "invalid-json":
                    self.manifest_path.write_bytes(b"{not-json}\n")
                elif case == "two-versions":
                    text = self.manifest_path.read_text(encoding="utf-8")
                    self.manifest_path.write_text(
                        text.replace(
                            '  "version-semver": "1.2.3",\n',
                            '  "version-semver": "1.2.3",\n'
                            '  "version-string": "1.2.3",\n',
                        ),
                        encoding="utf-8",
                    )
                elif case == "no-hash":
                    self.portfile_path.write_text(
                        "vcpkg_from_github(OUT_SOURCE_PATH SOURCE_PATH)\n",
                        encoding="utf-8",
                    )
                else:
                    with self.portfile_path.open("a", encoding="utf-8") as stream:
                        stream.write(f"SHA512 {OLD_HASH}\n")
                self.assert_failed_without_writes()

    def test_missing_or_non_utf8_inputs_fail_before_either_write(self) -> None:
        """Reject missing and undecodable release inputs without partial writes."""
        for case in ("manifest-missing", "portfile-missing", "manifest-binary"):
            with self.subTest(case=case):
                self.write_port()
                if case == "manifest-missing":
                    self.manifest_path.unlink()
                elif case == "portfile-missing":
                    self.portfile_path.unlink()
                else:
                    self.manifest_path.write_bytes(b"\xff\xfe")
                before = tuple(
                    path.read_bytes() if path.exists() else None
                    for path in (self.manifest_path, self.portfile_path)
                )
                with mock.patch.object(release, "_commit_outputs") as commit_outputs:
                    self.assertEqual(self.run_release(), 2)
                commit_outputs.assert_not_called()
                after = tuple(
                    path.read_bytes() if path.exists() else None
                    for path in (self.manifest_path, self.portfile_path)
                )
                self.assertEqual(after, before)

    def test_wrong_port_name_fails_before_either_write(self) -> None:
        """Never apply libtmux policy to a renamed or unrelated manifest."""
        self.write_port()
        text = self.manifest_path.read_text(encoding="utf-8")
        self.manifest_path.write_text(
            text.replace('"name": "libtmux"', '"name": "other"'),
            encoding="utf-8",
        )
        self.assert_failed_without_writes()

    @unittest.skipIf(not hasattr(pathlib.Path, "symlink_to"), "symlinks unavailable")
    def test_symlinked_port_or_release_input_is_refused(self) -> None:
        """Keep atomic release writes inside the selected repository."""
        victim = self.root / "victim"
        victim.mkdir()
        self.write_port()
        victim_manifest = victim / "vcpkg.json"
        victim_manifest.write_bytes(self.manifest_path.read_bytes())
        self.manifest_path.unlink()
        try:
            self.manifest_path.symlink_to(victim_manifest)
        except OSError as error:
            self.skipTest(f"symlinks unavailable: {error}")
        before = victim_manifest.read_bytes()

        self.assertEqual(self.run_release(), 2)
        self.assertEqual(victim_manifest.read_bytes(), before)

        self.manifest_path.unlink()
        victim_portfile = victim / "portfile.cmake"
        victim_portfile.write_bytes(self.portfile_path.read_bytes())
        self.portfile_path.unlink()
        self.port_dir.rmdir()
        self.port_dir.symlink_to(victim, target_is_directory=True)
        before = victim_manifest.read_bytes()
        self.assertEqual(self.run_release(), 2)
        self.assertEqual(victim_manifest.read_bytes(), before)

    def test_stale_staging_files_are_removed_before_a_rerun(self) -> None:
        """Recover a clean tree after an abrupt process exit during staging."""
        self.write_port()
        stale = (
            self.port_dir / ".vcpkg.json.release-stale",
            self.port_dir / ".portfile.cmake.release-stale",
        )
        for path in stale:
            path.write_text("inert", encoding="utf-8")

        self.assertEqual(self.run_release(), 0)

        self.assertFalse(any(path.exists() for path in stale))

    def test_concurrent_release_writer_fails_before_cleanup_or_writes(self) -> None:
        """Never let one release invocation remove another writer's stages."""
        self.write_port()
        before = self.snapshot()

        with release._release_lock(self.root.resolve()):
            self.assertEqual(self.run_release(), 2)

        self.assertEqual(self.snapshot(), before)

    def test_second_replacement_failure_rolls_back_the_first(self) -> None:
        """Keep manifest and source hash together when replacement fails."""
        self.write_port()
        before = self.snapshot()
        real_replace = release.os.replace
        calls = 0

        def fail_second(source: pathlib.Path, destination: pathlib.Path) -> None:
            nonlocal calls
            calls += 1
            if calls == 2:
                message = "injected second replacement failure"
                raise OSError(message)
            real_replace(source, destination)

        with mock.patch.object(release.os, "replace", side_effect=fail_second):
            self.assertEqual(self.run_release(enable_windows=True), 2)

        self.assertEqual(self.snapshot(), before)
        self.assertEqual(list(self.port_dir.glob(".*.release-*")), [])

    def test_first_replacement_failure_changes_nothing(self) -> None:
        """Clean every staged file when the commit cannot begin."""
        self.write_port()
        before = self.snapshot()
        message = "injected first replacement failure"
        with mock.patch.object(release.os, "replace", side_effect=OSError(message)):
            self.assertEqual(self.run_release(enable_windows=True), 2)

        self.assertEqual(self.snapshot(), before)
        self.assertEqual(list(self.port_dir.glob(".*.release-*")), [])

    def test_staging_failure_removes_every_temporary(self) -> None:
        """Leave no staged replacement when preparing the second file fails."""
        self.write_port()
        before = self.snapshot()
        real_stage = release._stage_bytes
        calls = 0

        def fail_second(path: pathlib.Path, contents: bytes) -> pathlib.Path:
            nonlocal calls
            calls += 1
            if calls == 2:
                message = "injected staging failure"
                raise OSError(message)
            return real_stage(path, contents)

        with mock.patch.object(release, "_stage_bytes", side_effect=fail_second):
            self.assertEqual(self.run_release(enable_windows=True), 2)

        self.assertEqual(self.snapshot(), before)
        self.assertEqual(list(self.port_dir.glob(".*.release-*")), [])

    def test_cli_defaults_closed_and_forwards_the_opt_in(self) -> None:
        """Expose Windows enablement only through the explicit command flag."""
        for extra, expected in (([], False), (["--enable-windows"], True)):
            with (
                self.subTest(extra=extra),
                mock.patch.object(
                    vcpkg_main.release, "run", return_value=0
                ) as run_release,
                mock.patch(
                    "sys.argv",
                    [
                        "tools.vcpkg",
                        "--root",
                        str(self.root),
                        "release",
                        "--version",
                        NEW_VERSION,
                        "--sha512",
                        NEW_HASH,
                        *extra,
                    ],
                ),
            ):
                self.assertEqual(vcpkg_main.main(), 0)
                self.assertEqual(
                    run_release.call_args.kwargs["enable_windows"], expected
                )

    def test_cli_has_no_arbitrary_port_path(self) -> None:
        """Keep the writable release command confined to libtmux's port."""
        with (
            mock.patch(
                "sys.argv",
                [
                    "tools.vcpkg",
                    "--root",
                    str(self.root),
                    "release",
                    "--version",
                    NEW_VERSION,
                    "--sha512",
                    NEW_HASH,
                    "--port",
                    "../victim",
                ],
            ),
            contextlib.redirect_stderr(io.StringIO()),
            self.assertRaises(SystemExit) as raised,
        ):
            vcpkg_main.main()

        self.assertEqual(raised.exception.code, 2)

    def test_checkout_manifest_uses_the_release_support_policy(self) -> None:
        """Keep source-tree and released Windows package scope identical."""
        root = pathlib.Path(__file__).resolve().parents[2]
        manifest = json.loads((root / "vcpkg.json").read_text(encoding="utf-8"))

        self.assertEqual(manifest["supports"], release.WINDOWS_SUPPORTS)


if __name__ == "__main__":
    unittest.main()
