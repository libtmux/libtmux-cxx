"""Recovery and transaction contracts for the MCP config swapper."""

from __future__ import annotations

import dataclasses
import hashlib
import json
import pathlib
import tempfile
import unittest
from unittest import mock

from tools.mcp import mcp_swap


def _state_checksum(document: dict[str, object]) -> str:
    payload = {key: value for key, value in document.items() if key != "checksum"}
    encoded = json.dumps(
        payload,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode()
    return hashlib.sha256(encoded).hexdigest()


class SwapFixture:
    """Run the swapper against task-owned configs and recovery state."""

    def __init__(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="mcp-swap-recovery-")
        self.root = pathlib.Path(self.temporary.name)
        self.repo = self.root / "checkout"
        marker = self.repo / mcp_swap.CHECKOUT_MARKER
        marker.parent.mkdir(parents=True)
        marker.write_text(
            "set_target_properties(server PROPERTIES OUTPUT_NAME libtmux-mcp-server)\n",
            encoding="utf-8",
        )
        self.build = self.root / "build"
        self.binary = self.build / "apps" / "mcp" / "libtmux-mcp-server"
        self.binary.parent.mkdir(parents=True)
        self.binary.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        self.binary.chmod(0o755)
        self.configs = self.root / "configs"
        self.configs.mkdir()
        self.clis = {
            name: dataclasses.replace(
                info,
                config_path=self.configs / f"{name}{info.config_path.suffix}",
            )
            for name, info in mcp_swap.CLIS.items()
        }
        self.state_dir = self.root / "state"
        self.state_file = self.state_dir / "state.json"

    def close(self) -> None:
        """Remove the isolated fixture tree."""
        self.temporary.cleanup()

    def seed(self, cli: mcp_swap.CLIName) -> bytes:
        """Write one supported client config and return its bytes."""
        info = self.clis[cli]
        if info.fmt == "toml":
            contents = b'[mcp_servers.keep]\ncommand = "keep"\nargs = []\n'
        elif info.fmt == "jsonc" and cli == "opencode":
            contents = b'{\n  // keep this\n  "mcp": {},\n}\n'
        else:
            contents = b'{"mcpServers":{"keep":{"command":"keep"}}}\n'
        info.config_path.parent.mkdir(parents=True, exist_ok=True)
        info.config_path.write_bytes(contents)
        info.config_path.chmod(0o640)
        return contents

    def argv(
        self,
        *clients: mcp_swap.CLIName,
        dry_run: bool = False,
        socket: str = "/tmp/libtmux-swap-a",
        scope: mcp_swap.Scope | None = None,
    ) -> list[str]:
        """Build a use-local command for selected fixture clients."""
        argv = [
            "use-local",
            "--repo",
            str(self.repo),
            "--build-dir",
            str(self.build),
            "--socket",
            socket,
        ]
        if dry_run:
            argv.append("--dry-run")
        else:
            argv.append("--no-preflight")
        for cli in clients:
            argv.extend(("--cli", cli))
        if scope is not None:
            argv.extend(("--scope", scope))
        return argv

    def revert_argv(
        self,
        *clients: mcp_swap.CLIName,
        dry_run: bool = False,
        scope: mcp_swap.Scope | None = None,
    ) -> list[str]:
        """Build a revert command for selected fixture clients."""
        argv = ["revert"]
        if dry_run:
            argv.append("--dry-run")
        for cli in clients:
            argv.extend(("--cli", cli))
        if scope is not None:
            argv.extend(("--scope", scope))
        return argv

    def run(self, argv: list[str]) -> int:
        """Run the command with only fixture paths installed."""
        with (
            mock.patch.dict(mcp_swap.CLIS, self.clis, clear=True),
            mock.patch.object(mcp_swap, "STATE_DIR", self.state_dir),
            mock.patch.object(mcp_swap, "STATE_FILE", self.state_file),
        ):
            return mcp_swap.main(argv)

    def backups(self) -> list[pathlib.Path]:
        """Return every recovery backup under the fixture config root."""
        return sorted(self.configs.glob("*.bak.mcp-swap-*"))


class McpSwapApplyRecoveryTest(unittest.TestCase):
    """Require a fully planned, authenticated all-client apply."""

    def setUp(self) -> None:
        """Create one isolated swap fixture."""
        self.fixture = SwapFixture()

    def tearDown(self) -> None:
        """Remove the isolated swap fixture."""
        self.fixture.close()

    def test_dry_run_plans_backups_without_starting_the_server(self) -> None:
        """Validate the later backup before a server or file can change."""
        for cli in ("cursor", "gemini"):
            self.fixture.seed(cli)
        inspected: list[pathlib.Path] = []

        def check(path: pathlib.Path) -> None:
            inspected.append(path)
            if "gemini" in path.name:
                message = "synthetic later backup refusal"
                raise OSError(message)

        with (
            mock.patch.object(
                mcp_swap,
                "preflight_spec",
                return_value=None,
            ) as preflight,
            mock.patch.object(
                mcp_swap,
                "_check_backup_destination",
                side_effect=check,
            ),
        ):
            result = self.fixture.run(
                self.fixture.argv("cursor", "gemini", dry_run=True)
            )

        self.assertEqual(result, 1)
        preflight.assert_not_called()
        self.assertEqual(len(inspected), 2)
        self.assertFalse(self.fixture.state_dir.exists())
        self.assertEqual(self.fixture.backups(), [])

    def test_dry_run_rejects_duplicate_physical_configs(self) -> None:
        """Do not compose two selected clients into one physical file."""
        shared = self.fixture.configs / "shared.json"
        shared.write_bytes(b'{"mcpServers":{}}\n')
        shared.chmod(0o640)
        for cli in ("cursor", "gemini"):
            self.fixture.clis[cli].config_path.symlink_to(shared)

        with mock.patch.object(
            mcp_swap,
            "preflight_spec",
            return_value=None,
        ) as preflight:
            result = self.fixture.run(
                self.fixture.argv("cursor", "gemini", dry_run=True)
            )

        self.assertEqual(result, 1)
        preflight.assert_not_called()
        self.assertEqual(shared.read_bytes(), b'{"mcpServers":{}}\n')
        self.assertFalse(self.fixture.state_dir.exists())

    def test_apply_does_not_fall_back_after_a_late_backup_claim(self) -> None:
        """Leave a newly claimed backup path untouched without a side file."""
        before = self.fixture.seed("cursor")
        human = b"human backup\n"
        real_validate = mcp_swap._validate_missing_binding
        claimed = False

        def claim_late(
            path: pathlib.Path,
            binding: mcp_swap._MissingBinding,
        ) -> None:
            nonlocal claimed
            real_validate(path, binding)
            if mcp_swap.BACKUP_SUFFIX_PREFIX in path.name and not claimed:
                path.write_bytes(human)
                claimed = True

        with mock.patch.object(
            mcp_swap,
            "_validate_missing_binding",
            side_effect=claim_late,
        ):
            result = self.fixture.run(self.fixture.argv("cursor"))

        self.assertEqual(result, 1)
        self.assertEqual(self.fixture.clis["cursor"].config_path.read_bytes(), before)
        self.assertEqual(len(self.fixture.backups()), 1)
        self.assertEqual(self.fixture.backups()[0].read_bytes(), human)
        self.assertFalse(self.fixture.state_file.exists())

    def test_apply_rolls_back_every_earlier_config(self) -> None:
        """A later publish failure restores the complete selected set."""
        before = {cli: self.fixture.seed(cli) for cli in ("cursor", "gemini", "pi")}
        real_atomic_write = mcp_swap.atomic_write
        failed_path = self.fixture.clis["pi"].config_path.resolve()

        def fail_late(path: pathlib.Path, data: bytes) -> None:
            if path == failed_path:
                message = "synthetic Pi publish failure"
                raise OSError(message)
            real_atomic_write(path, data)

        with mock.patch.object(mcp_swap, "atomic_write", side_effect=fail_late):
            result = self.fixture.run(self.fixture.argv("cursor", "gemini", "pi"))

        self.assertEqual(result, 1)
        for cli, contents in before.items():
            self.assertEqual(self.fixture.clis[cli].config_path.read_bytes(), contents)
        self.assertFalse(self.fixture.state_file.exists())
        self.assertEqual(self.fixture.backups(), [])

    def test_apply_rechecks_each_config_before_publish(self) -> None:
        """Do not overwrite a later config changed during an earlier write."""
        cursor_before = self.fixture.seed("cursor")
        self.fixture.seed("gemini")
        cursor = self.fixture.clis["cursor"].config_path.resolve()
        gemini = self.fixture.clis["gemini"].config_path.resolve()
        human = b'{"mcpServers":{"human":{"command":"keep-me"}}}\n'
        real_atomic_write = mcp_swap.atomic_write

        def edit_later(path: pathlib.Path, data: bytes) -> None:
            real_atomic_write(path, data)
            if path == cursor:
                gemini.write_bytes(human)

        with mock.patch.object(mcp_swap, "atomic_write", side_effect=edit_later):
            result = self.fixture.run(self.fixture.argv("cursor", "gemini"))

        self.assertEqual(result, 1)
        self.assertEqual(cursor.read_bytes(), cursor_before)
        self.assertEqual(gemini.read_bytes(), human)
        self.assertFalse(self.fixture.state_file.exists())
        self.assertEqual(self.fixture.backups(), [])

    def test_apply_rollback_never_overwrites_an_intervening_edit(self) -> None:
        """Retain pending recovery when a changed config is no longer owned."""
        self.fixture.seed("cursor")
        self.fixture.seed("gemini")
        cursor = self.fixture.clis["cursor"].config_path.resolve()
        gemini = self.fixture.clis["gemini"].config_path.resolve()
        human = b'{"mcpServers":{"human":{"command":"keep-me"}}}\n'
        real_atomic_write = mcp_swap.atomic_write

        def fail_after_edit(path: pathlib.Path, data: bytes) -> None:
            if path == gemini:
                cursor.write_bytes(human)
                message = "synthetic later publish failure"
                raise OSError(message)
            real_atomic_write(path, data)

        with mock.patch.object(
            mcp_swap,
            "atomic_write",
            side_effect=fail_after_edit,
        ):
            result = self.fixture.run(self.fixture.argv("cursor", "gemini"))

        self.assertEqual(result, 1)
        self.assertEqual(cursor.read_bytes(), human)
        pending = json.loads(self.fixture.state_file.read_text())
        self.assertEqual(pending["transaction"]["kind"], "apply")
        self.assertEqual(len(self.fixture.backups()), 2)

    def test_apply_rollback_rechecks_each_config_before_restore(self) -> None:
        """Stop reverse rollback when a later owned config changes."""
        before = {cli: self.fixture.seed(cli) for cli in ("cursor", "gemini", "pi")}
        cursor = self.fixture.clis["cursor"].config_path.resolve()
        gemini = self.fixture.clis["gemini"].config_path.resolve()
        pi = self.fixture.clis["pi"].config_path.resolve()
        human = b'{"mcpServers":{"human":{"command":"keep-me"}}}\n'
        real_atomic_write = mcp_swap.atomic_write

        def fail_then_edit(path: pathlib.Path, data: bytes) -> None:
            if path == pi:
                message = "synthetic Pi publish failure"
                raise OSError(message)
            real_atomic_write(path, data)
            if path == gemini and data == before["gemini"]:
                cursor.write_bytes(human)

        with mock.patch.object(mcp_swap, "atomic_write", side_effect=fail_then_edit):
            result = self.fixture.run(self.fixture.argv("cursor", "gemini", "pi"))

        self.assertEqual(result, 1)
        self.assertEqual(cursor.read_bytes(), human)
        self.assertEqual(gemini.read_bytes(), before["gemini"])
        pending = json.loads(self.fixture.state_file.read_text())
        self.assertEqual(pending["transaction"]["kind"], "apply")
        self.assertEqual(len(self.fixture.backups()), 3)

    def test_apply_cleanup_failure_retains_complete_recovery(self) -> None:
        """Recreate removed backups before retaining a failed apply record."""
        before = {cli: self.fixture.seed(cli) for cli in ("cursor", "gemini", "pi")}
        pi = self.fixture.clis["pi"].config_path.resolve()
        real_atomic_write = mcp_swap.atomic_write
        real_remove = mcp_swap._remove_bound_file
        removed = 0

        def fail_publish(path: pathlib.Path, data: bytes) -> None:
            if path == pi:
                message = "synthetic Pi publish failure"
                raise OSError(message)
            real_atomic_write(path, data)

        def fail_cleanup(
            path: pathlib.Path,
            data: bytes,
            binding: mcp_swap._PathBinding,
        ) -> None:
            nonlocal removed
            if mcp_swap.BACKUP_SUFFIX_PREFIX in path.name:
                removed += 1
                if removed == 2:
                    message = "synthetic apply cleanup refusal"
                    raise OSError(message)
            real_remove(path, data, binding)

        with (
            mock.patch.object(mcp_swap, "atomic_write", side_effect=fail_publish),
            mock.patch.object(
                mcp_swap,
                "_remove_bound_file",
                side_effect=fail_cleanup,
            ),
        ):
            result = self.fixture.run(self.fixture.argv("cursor", "gemini", "pi"))

        self.assertEqual(result, 1)
        for cli, contents in before.items():
            self.assertEqual(self.fixture.clis[cli].config_path.read_bytes(), contents)
        backups = self.fixture.backups()
        self.assertEqual(len(backups), 3)
        self.assertEqual(
            sorted(path.read_bytes() for path in backups), sorted(before.values())
        )
        pending = json.loads(self.fixture.state_file.read_text())
        self.assertEqual(pending["transaction"]["kind"], "apply")
        for change in pending["transaction"]["changes"]:
            backup = pathlib.Path(change["backup_path"])
            self.assertEqual(
                change["backup_binding"]["target"]["inode"],
                backup.stat().st_ino,
            )

    def test_apply_cleanup_retains_backups_when_state_changes(self) -> None:
        """Recreate owned backups without overwriting replacement state."""
        before = {cli: self.fixture.seed(cli) for cli in ("cursor", "gemini", "pi")}
        pi = self.fixture.clis["pi"].config_path.resolve()
        human_state = b"human state\n"
        real_atomic_write = mcp_swap.atomic_write
        real_remove = mcp_swap._remove_bound_file
        removed = 0

        def fail_publish(path: pathlib.Path, data: bytes) -> None:
            if path == pi:
                message = "synthetic Pi publish failure"
                raise OSError(message)
            real_atomic_write(path, data)

        def replace_state_then_fail(
            path: pathlib.Path,
            data: bytes,
            binding: mcp_swap._PathBinding,
        ) -> None:
            nonlocal removed
            if mcp_swap.BACKUP_SUFFIX_PREFIX in path.name:
                removed += 1
                if removed == 2:
                    self.fixture.state_file.write_bytes(human_state)
                    message = "synthetic apply cleanup refusal"
                    raise OSError(message)
            real_remove(path, data, binding)

        with (
            mock.patch.object(mcp_swap, "atomic_write", side_effect=fail_publish),
            mock.patch.object(
                mcp_swap,
                "_remove_bound_file",
                side_effect=replace_state_then_fail,
            ),
        ):
            result = self.fixture.run(self.fixture.argv("cursor", "gemini", "pi"))

        self.assertEqual(result, 1)
        for cli, contents in before.items():
            self.assertEqual(self.fixture.clis[cli].config_path.read_bytes(), contents)
        self.assertEqual(len(self.fixture.backups()), 3)
        self.assertEqual(self.fixture.state_file.read_bytes(), human_state)

    def test_apply_refuses_a_retargeted_config_symlink(self) -> None:
        """Revalidate logical topology before the first config publish."""
        logical = self.fixture.clis["cursor"].config_path
        old_target = self.fixture.configs / "cursor-old.json"
        new_target = self.fixture.configs / "cursor-new.json"
        before = b'{"mcpServers":{}}\n'
        old_target.write_bytes(before)
        new_target.write_bytes(before)
        logical.symlink_to(old_target)
        real_backup = mcp_swap.write_new_backup
        retargeted = False

        def retarget(base: pathlib.Path, data: bytes) -> pathlib.Path:
            nonlocal retargeted
            if not retargeted:
                logical.unlink()
                logical.symlink_to(new_target)
                retargeted = True
            return real_backup(base, data)

        with mock.patch.object(mcp_swap, "write_new_backup", side_effect=retarget):
            result = self.fixture.run(self.fixture.argv("cursor"))

        self.assertEqual(result, 1)
        self.assertEqual(old_target.read_bytes(), before)
        self.assertEqual(new_target.read_bytes(), before)

    def test_apply_refuses_a_same_path_replacement(self) -> None:
        """Do not overwrite byte-identical content on an unobserved inode."""
        before = self.fixture.seed("cursor")
        target = self.fixture.clis["cursor"].config_path
        first_inode = target.stat().st_ino
        real_backup = mcp_swap.write_new_backup
        replaced = False

        def replace(base: pathlib.Path, data: bytes) -> pathlib.Path:
            nonlocal replaced
            if not replaced:
                replacement = target.with_name(target.name + ".replacement")
                replacement.write_bytes(before)
                replacement.chmod(0o640)
                replacement.replace(target)
                replaced = True
            return real_backup(base, data)

        with mock.patch.object(mcp_swap, "write_new_backup", side_effect=replace):
            result = self.fixture.run(self.fixture.argv("cursor"))

        self.assertEqual(result, 1)
        self.assertNotEqual(target.stat().st_ino, first_inode)
        self.assertEqual(target.read_bytes(), before)

    def test_stable_state_is_versioned_checksummed_and_bound(self) -> None:
        """Persist exact config and backup ownership for a later revert."""
        self.fixture.seed("cursor")

        self.assertEqual(self.fixture.run(self.fixture.argv("cursor")), 0)

        document = json.loads(self.fixture.state_file.read_text())
        self.assertEqual(document["version"], 2)
        self.assertEqual(document["checksum"], _state_checksum(document))
        self.assertIsNone(document["transaction"])
        entry = document["entries"]["cursor:user"]
        current = self.fixture.clis["cursor"].config_path.read_bytes()
        backup = pathlib.Path(entry["backup_path"])
        self.assertEqual(entry["expected_sha256"], hashlib.sha256(current).hexdigest())
        self.assertEqual(entry["expected_mode"], 0o640)
        self.assertEqual(
            entry["original_sha256"], hashlib.sha256(backup.read_bytes()).hexdigest()
        )
        self.assertEqual(entry["original_mode"], 0o640)
        self.assertEqual(
            entry["config_binding"]["target"]["inode"],
            pathlib.Path(entry["target_path"]).stat().st_ino,
        )
        self.assertEqual(
            entry["backup_binding"]["target"]["inode"], backup.stat().st_ino
        )
        self.assertEqual(self.fixture.state_file.stat().st_mode & 0o777, 0o600)

    def test_pending_state_survives_interruption_and_fails_closed(self) -> None:
        """Leave checksummed recovery evidence before config visibility."""
        before = self.fixture.seed("cursor")
        target = self.fixture.clis["cursor"].config_path.resolve()
        real_atomic_write = mcp_swap.atomic_write

        def interrupt(path: pathlib.Path, data: bytes) -> None:
            if path == target:
                raise KeyboardInterrupt
            real_atomic_write(path, data)

        with (
            mock.patch.object(mcp_swap, "atomic_write", side_effect=interrupt),
            self.assertRaises(KeyboardInterrupt),
        ):
            self.fixture.run(self.fixture.argv("cursor"))

        self.assertEqual(target.read_bytes(), before)
        pending_bytes = self.fixture.state_file.read_bytes()
        pending = json.loads(pending_bytes)
        self.assertEqual(pending["version"], 2)
        self.assertEqual(pending["checksum"], _state_checksum(pending))
        self.assertEqual(pending["transaction"]["kind"], "apply")

        # A byte change invalidates the pending record and must not be repaired
        # or overwritten by a later command.
        self.fixture.state_file.write_bytes(
            pending_bytes.replace(b'"apply"', b'"other"')
        )
        tampered = self.fixture.state_file.read_bytes()
        self.assertEqual(self.fixture.run(self.fixture.argv("cursor")), 1)
        self.assertEqual(self.fixture.state_file.read_bytes(), tampered)
        self.assertEqual(target.read_bytes(), before)

    def test_pending_updates_keep_one_transaction_identity(self) -> None:
        """Use one nonce for every durable phase of an apply transaction."""
        self.fixture.seed("cursor")
        real_save = mcp_swap.save_state
        transaction_ids: list[str] = []

        def observe(*args: object, **kwargs: object) -> mcp_swap._StateSnapshot:
            transaction = kwargs.get("transaction")
            if isinstance(transaction, dict):
                transaction_ids.append(transaction["id"])
            return real_save(*args, **kwargs)

        with mock.patch.object(mcp_swap, "save_state", side_effect=observe):
            self.assertEqual(self.fixture.run(self.fixture.argv("cursor")), 0)

        self.assertGreaterEqual(len(transaction_ids), 2)
        self.assertEqual(len(set(transaction_ids)), 1)

    def test_pending_schema_rejects_unknown_fields_with_a_valid_checksum(self) -> None:
        """Reject a checksummed pending record whose shape is not canonical."""
        self.fixture.seed("cursor")
        target = self.fixture.clis["cursor"].config_path.resolve()
        real_atomic_write = mcp_swap.atomic_write

        def interrupt(path: pathlib.Path, data: bytes) -> None:
            if path == target:
                raise KeyboardInterrupt
            real_atomic_write(path, data)

        with (
            mock.patch.object(mcp_swap, "atomic_write", side_effect=interrupt),
            self.assertRaises(KeyboardInterrupt),
        ):
            self.fixture.run(self.fixture.argv("cursor"))

        document = json.loads(self.fixture.state_file.read_text())
        document["transaction"]["unexpected"] = True
        document["checksum"] = _state_checksum(document)
        encoded = (json.dumps(document, indent=2) + "\n").encode()

        with self.assertRaises(ValueError):
            mcp_swap._decode_state(encoded)

    def test_dry_run_rejects_a_backup_alias_of_recovery_state(self) -> None:
        """Reject selected config, backup, and state cross-artifact aliases."""
        self.fixture.seed("cursor")
        self.fixture.state_dir.mkdir()

        with mock.patch.object(
            mcp_swap,
            "_next_backup_path",
            return_value=self.fixture.state_file,
        ):
            result = self.fixture.run(self.fixture.argv("cursor", dry_run=True))

        self.assertEqual(result, 1)
        self.assertFalse(self.fixture.state_file.exists())
        self.assertEqual(self.fixture.backups(), [])

    def test_repeat_use_keeps_the_original_backup_identity(self) -> None:
        """Reconfigure a swapped client without replacing its recovery copy."""
        original = self.fixture.seed("cursor")
        self.assertEqual(self.fixture.run(self.fixture.argv("cursor")), 0)
        first = json.loads(self.fixture.state_file.read_text())
        first_entry = first["entries"]["cursor:user"]
        backup = pathlib.Path(first_entry["backup_path"])
        identity = (backup.stat().st_dev, backup.stat().st_ino)

        self.assertEqual(
            self.fixture.run(self.fixture.argv("cursor", socket="/tmp/libtmux-swap-b")),
            0,
        )

        second = json.loads(self.fixture.state_file.read_text())
        second_entry = second["entries"]["cursor:user"]
        self.assertEqual(second_entry["backup_path"], str(backup))
        self.assertEqual(second_entry["seq_no"], first_entry["seq_no"])
        self.assertEqual(second_entry["swapped_at"], first_entry["swapped_at"])
        self.assertEqual((backup.stat().st_dev, backup.stat().st_ino), identity)
        self.assertEqual(backup.read_bytes(), original)


class McpSwapRevertRecoveryTest(unittest.TestCase):
    """Require one authenticated transaction for selected reverts."""

    def setUp(self) -> None:
        """Create one isolated swap fixture."""
        self.fixture = SwapFixture()

    def tearDown(self) -> None:
        """Remove the isolated swap fixture."""
        self.fixture.close()

    def test_revert_refuses_a_human_edit(self) -> None:
        """Never overwrite config bytes that changed after the swap."""
        self.fixture.seed("cursor")
        self.assertEqual(self.fixture.run(self.fixture.argv("cursor")), 0)
        target = self.fixture.clis["cursor"].config_path
        human = b'{"mcpServers":{"human":{"command":"keep-me"}}}\n'
        target.write_bytes(human)
        state = self.fixture.state_file.read_bytes()
        backups = self.fixture.backups()

        self.assertEqual(self.fixture.run(self.fixture.revert_argv("cursor")), 1)

        self.assertEqual(target.read_bytes(), human)
        self.assertEqual(self.fixture.state_file.read_bytes(), state)
        self.assertEqual(self.fixture.backups(), backups)

    def test_revert_refuses_a_same_path_replacement(self) -> None:
        """Never overwrite a byte-identical replacement on a new inode."""
        self.fixture.seed("cursor")
        self.assertEqual(self.fixture.run(self.fixture.argv("cursor")), 0)
        target = self.fixture.clis["cursor"].config_path
        swapped = target.read_bytes()
        previous_inode = target.stat().st_ino
        replacement = target.with_name(target.name + ".replacement")
        replacement.write_bytes(swapped)
        replacement.chmod(target.stat().st_mode & 0o777)
        replacement.replace(target)
        state = self.fixture.state_file.read_bytes()

        self.assertEqual(self.fixture.run(self.fixture.revert_argv("cursor")), 1)

        self.assertNotEqual(target.stat().st_ino, previous_inode)
        self.assertEqual(target.read_bytes(), swapped)
        self.assertEqual(self.fixture.state_file.read_bytes(), state)
        self.assertEqual(len(self.fixture.backups()), 1)

    def test_revert_refuses_a_config_mode_change(self) -> None:
        """Treat a post-swap permission change as human-owned state."""
        self.fixture.seed("cursor")
        self.assertEqual(self.fixture.run(self.fixture.argv("cursor")), 0)
        target = self.fixture.clis["cursor"].config_path
        swapped = target.read_bytes()
        target.chmod(0o600)
        state = self.fixture.state_file.read_bytes()

        self.assertEqual(self.fixture.run(self.fixture.revert_argv("cursor")), 1)

        self.assertEqual(target.read_bytes(), swapped)
        self.assertEqual(target.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.fixture.state_file.read_bytes(), state)
        self.assertEqual(len(self.fixture.backups()), 1)

    def test_revert_refuses_a_retargeted_config_symlink(self) -> None:
        """Keep both targets and recovery when the logical route changes."""
        logical = self.fixture.clis["cursor"].config_path
        old_target = self.fixture.configs / "cursor-old.json"
        new_target = self.fixture.configs / "cursor-new.json"
        original = b'{"mcpServers":{"old":{"command":"old"}}}\n'
        new_bytes = b'{"mcpServers":{"new":{"command":"new"}}}\n'
        old_target.write_bytes(original)
        old_target.chmod(0o640)
        new_target.write_bytes(new_bytes)
        new_target.chmod(0o640)
        logical.symlink_to(old_target)
        self.assertEqual(self.fixture.run(self.fixture.argv("cursor")), 0)
        swapped = old_target.read_bytes()
        state = self.fixture.state_file.read_bytes()
        logical.unlink()
        logical.symlink_to(new_target)

        self.assertEqual(self.fixture.run(self.fixture.revert_argv("cursor")), 1)

        self.assertEqual(old_target.read_bytes(), swapped)
        self.assertEqual(new_target.read_bytes(), new_bytes)
        self.assertEqual(self.fixture.state_file.read_bytes(), state)
        self.assertEqual(len(self.fixture.backups()), 1)

    def test_revert_authenticates_backup_bytes_mode_and_identity(self) -> None:
        """Refuse every unowned form of recovery-backup replacement."""
        for mutation in ("content", "mode", "identity"):
            with self.subTest(mutation=mutation):
                fixture = SwapFixture()
                try:
                    fixture.seed("cursor")
                    self.assertEqual(fixture.run(fixture.argv("cursor")), 0)
                    target = fixture.clis["cursor"].config_path
                    swapped = target.read_bytes()
                    state = fixture.state_file.read_bytes()
                    backup = fixture.backups()[0]
                    if mutation == "content":
                        backup.write_bytes(b"human backup edit\n")
                    elif mutation == "mode":
                        backup.chmod(0o640)
                    else:
                        data = backup.read_bytes()
                        replacement = backup.with_name(backup.name + ".replacement")
                        replacement.write_bytes(data)
                        replacement.chmod(0o600)
                        replacement.replace(backup)

                    self.assertEqual(fixture.run(fixture.revert_argv("cursor")), 1)
                    self.assertEqual(target.read_bytes(), swapped)
                    self.assertEqual(fixture.state_file.read_bytes(), state)
                    self.assertTrue(backup.exists())
                finally:
                    fixture.close()

    def test_dry_run_authenticates_every_selected_backup(self) -> None:
        """Reject a malformed later recovery artifact without any write."""
        for cli in ("cursor", "gemini"):
            self.fixture.seed(cli)
        self.assertEqual(
            self.fixture.run(self.fixture.argv("cursor", "gemini")),
            0,
        )
        swapped = {
            cli: self.fixture.clis[cli].config_path.read_bytes()
            for cli in ("cursor", "gemini")
        }
        state = self.fixture.state_file.read_bytes()
        self.fixture.backups()[-1].write_bytes(b"later backup edit\n")

        result = self.fixture.run(
            self.fixture.revert_argv("cursor", "gemini", dry_run=True)
        )

        self.assertEqual(result, 1)
        self.assertEqual(self.fixture.state_file.read_bytes(), state)
        for cli, contents in swapped.items():
            self.assertEqual(self.fixture.clis[cli].config_path.read_bytes(), contents)

    def test_late_revert_failure_rolls_every_config_forward(self) -> None:
        """Restore the complete selected set when a later restore fails."""
        for cli in ("cursor", "gemini", "pi"):
            self.fixture.seed(cli)
        self.assertEqual(
            self.fixture.run(self.fixture.argv("cursor", "gemini", "pi")),
            0,
        )
        swapped = {
            cli: self.fixture.clis[cli].config_path.read_bytes()
            for cli in ("cursor", "gemini", "pi")
        }
        backups = {
            path: (path.read_bytes(), path.stat().st_ino)
            for path in self.fixture.backups()
        }
        cursor = self.fixture.clis["cursor"].config_path.resolve()
        real_atomic_write = mcp_swap.atomic_write

        def fail_late(path: pathlib.Path, data: bytes) -> None:
            if path == cursor:
                message = "synthetic late restore failure"
                raise OSError(message)
            real_atomic_write(path, data)

        with mock.patch.object(mcp_swap, "atomic_write", side_effect=fail_late):
            result = self.fixture.run(
                self.fixture.revert_argv("pi", "gemini", "cursor")
            )

        self.assertEqual(result, 1)
        for cli, contents in swapped.items():
            self.assertEqual(self.fixture.clis[cli].config_path.read_bytes(), contents)
        self.assertEqual(
            {
                path: (path.read_bytes(), path.stat().st_ino)
                for path in self.fixture.backups()
            },
            backups,
        )
        self.assertEqual(
            self.fixture.run(self.fixture.argv("cursor", "gemini", "pi")),
            0,
        )

    def test_revert_rollback_rechecks_each_config_before_publish(self) -> None:
        """Stop roll-forward when another restored config changes."""
        before = {cli: self.fixture.seed(cli) for cli in ("cursor", "gemini", "pi")}
        self.assertEqual(
            self.fixture.run(self.fixture.argv("cursor", "gemini", "pi")),
            0,
        )
        swapped = {
            cli: self.fixture.clis[cli].config_path.read_bytes()
            for cli in ("cursor", "gemini", "pi")
        }
        cursor = self.fixture.clis["cursor"].config_path.resolve()
        gemini = self.fixture.clis["gemini"].config_path.resolve()
        pi = self.fixture.clis["pi"].config_path.resolve()
        human = b'{"mcpServers":{"human":{"command":"keep-me"}}}\n'
        real_atomic_write = mcp_swap.atomic_write

        def fail_then_edit(path: pathlib.Path, data: bytes) -> None:
            if path == cursor and data == before["cursor"]:
                message = "synthetic late restore failure"
                raise OSError(message)
            real_atomic_write(path, data)
            if path == gemini and data == swapped["gemini"]:
                pi.write_bytes(human)

        with mock.patch.object(mcp_swap, "atomic_write", side_effect=fail_then_edit):
            result = self.fixture.run(
                self.fixture.revert_argv("pi", "gemini", "cursor")
            )

        self.assertEqual(result, 1)
        self.assertEqual(pi.read_bytes(), human)
        pending = json.loads(self.fixture.state_file.read_text())
        self.assertEqual(pending["transaction"]["kind"], "revert")
        self.assertEqual(len(self.fixture.backups()), 3)

    def test_cleanup_failure_restores_the_swapped_transaction(self) -> None:
        """Roll forward when owned recovery cannot be removed completely."""
        self.fixture.seed("cursor")
        self.assertEqual(self.fixture.run(self.fixture.argv("cursor")), 0)
        target = self.fixture.clis["cursor"].config_path
        swapped = target.read_bytes()
        state = self.fixture.state_file.read_bytes()
        backup = self.fixture.backups()[0]
        real_unlink = pathlib.Path.unlink

        def refuse_backup(path: pathlib.Path, *args: object, **kwargs: object) -> None:
            if path == backup:
                message = "synthetic backup cleanup refusal"
                raise OSError(message)
            real_unlink(path, *args, **kwargs)

        with mock.patch.object(pathlib.Path, "unlink", new=refuse_backup):
            result = self.fixture.run(self.fixture.revert_argv("cursor"))

        self.assertEqual(result, 1)
        self.assertEqual(target.read_bytes(), swapped)
        self.assertEqual(self.fixture.state_file.read_bytes(), state)
        self.assertTrue(backup.exists())

    def test_partial_backup_cleanup_recreates_coherent_recovery(self) -> None:
        """Recreate already removed backups when a later cleanup fails."""
        for cli in ("cursor", "gemini", "pi"):
            self.fixture.seed(cli)
        self.assertEqual(
            self.fixture.run(self.fixture.argv("cursor", "gemini", "pi")),
            0,
        )
        swapped = {
            cli: self.fixture.clis[cli].config_path.read_bytes()
            for cli in ("cursor", "gemini", "pi")
        }
        backups = {path: path.read_bytes() for path in self.fixture.backups()}
        real_remove = mcp_swap._remove_bound_file
        removed = 0

        def fail_second(
            path: pathlib.Path,
            data: bytes,
            binding: mcp_swap._PathBinding,
        ) -> None:
            nonlocal removed
            if path in backups:
                removed += 1
                if removed == 2:
                    message = "synthetic later cleanup refusal"
                    raise OSError(message)
            real_remove(path, data, binding)

        with mock.patch.object(
            mcp_swap,
            "_remove_bound_file",
            side_effect=fail_second,
        ):
            result = self.fixture.run(
                self.fixture.revert_argv("cursor", "gemini", "pi")
            )

        self.assertEqual(result, 1)
        for cli, contents in swapped.items():
            self.assertEqual(self.fixture.clis[cli].config_path.read_bytes(), contents)
        self.assertEqual(
            {path: path.read_bytes() for path in self.fixture.backups()},
            backups,
        )
        self.assertEqual(
            self.fixture.run(
                self.fixture.argv(
                    "cursor",
                    "gemini",
                    "pi",
                    socket="/tmp/libtmux-swap-b",
                )
            ),
            0,
        )

    def test_state_removal_failure_recovers_the_previous_transaction(self) -> None:
        """Recover stable state when final state removal reports failure."""
        self.fixture.seed("cursor")
        self.assertEqual(self.fixture.run(self.fixture.argv("cursor")), 0)
        target = self.fixture.clis["cursor"].config_path
        swapped = target.read_bytes()
        backup_bytes = self.fixture.backups()[0].read_bytes()
        real_remove = mcp_swap._remove_state_snapshot

        def remove_then_fail(
            snapshot: mcp_swap._StateSnapshot,
        ) -> mcp_swap._StateSnapshot:
            real_remove(snapshot)
            message = "synthetic post-removal failure"
            raise OSError(message)

        with mock.patch.object(
            mcp_swap,
            "_remove_state_snapshot",
            side_effect=remove_then_fail,
        ):
            result = self.fixture.run(self.fixture.revert_argv("cursor"))

        self.assertEqual(result, 1)
        self.assertEqual(target.read_bytes(), swapped)
        self.assertTrue(self.fixture.state_file.exists())
        self.assertEqual(len(self.fixture.backups()), 1)
        self.assertEqual(self.fixture.backups()[0].read_bytes(), backup_bytes)
        self.assertEqual(
            self.fixture.run(self.fixture.argv("cursor", socket="/tmp/libtmux-swap-b")),
            0,
        )

    def test_revert_interruption_leaves_checksummed_pending_state(self) -> None:
        """Publish recovery evidence before the first restore can be visible."""
        self.fixture.seed("cursor")
        self.assertEqual(self.fixture.run(self.fixture.argv("cursor")), 0)
        target = self.fixture.clis["cursor"].config_path.resolve()
        swapped = target.read_bytes()
        real_atomic_write = mcp_swap.atomic_write

        def interrupt(path: pathlib.Path, data: bytes) -> None:
            if path == target:
                raise KeyboardInterrupt
            real_atomic_write(path, data)

        with (
            mock.patch.object(mcp_swap, "atomic_write", side_effect=interrupt),
            self.assertRaises(KeyboardInterrupt),
        ):
            self.fixture.run(self.fixture.revert_argv("cursor"))

        pending_bytes = self.fixture.state_file.read_bytes()
        pending = json.loads(pending_bytes)
        self.assertEqual(pending["checksum"], _state_checksum(pending))
        self.assertEqual(pending["transaction"]["kind"], "revert")
        self.assertEqual(target.read_bytes(), swapped)
        self.assertEqual(self.fixture.run(self.fixture.revert_argv("cursor")), 1)
        self.assertEqual(self.fixture.state_file.read_bytes(), pending_bytes)
        self.assertEqual(target.read_bytes(), swapped)

    def test_top_layer_reuse_preserves_two_scope_lifo(self) -> None:
        """Keep layered Claude recovery valid across a repeated top swap."""
        original = self.fixture.seed("claude")
        self.assertEqual(
            self.fixture.run(self.fixture.argv("claude", scope="project")),
            0,
        )
        self.assertEqual(
            self.fixture.run(
                self.fixture.argv(
                    "claude",
                    socket="/tmp/libtmux-swap-b",
                    scope="user",
                )
            ),
            0,
        )
        first_state = json.loads(self.fixture.state_file.read_text())
        top_backup = first_state["entries"]["claude:user"]["backup_path"]
        top_identity = pathlib.Path(top_backup).stat().st_ino
        before_lower_attempt = self.fixture.clis["claude"].config_path.read_bytes()

        self.assertEqual(
            self.fixture.run(
                self.fixture.argv(
                    "claude",
                    socket="/tmp/libtmux-swap-c",
                    scope="project",
                )
            ),
            1,
        )
        self.assertEqual(
            self.fixture.clis["claude"].config_path.read_bytes(),
            before_lower_attempt,
        )
        self.assertEqual(
            self.fixture.run(self.fixture.revert_argv("claude", scope="project")),
            1,
        )

        self.assertEqual(
            self.fixture.run(
                self.fixture.argv(
                    "claude",
                    socket="/tmp/libtmux-swap-c",
                    scope="user",
                )
            ),
            0,
        )
        second_state = json.loads(self.fixture.state_file.read_text())
        self.assertEqual(
            second_state["entries"]["claude:user"]["backup_path"],
            top_backup,
        )
        self.assertEqual(pathlib.Path(top_backup).stat().st_ino, top_identity)

        self.assertEqual(self.fixture.run(self.fixture.revert_argv("claude")), 0)
        self.assertEqual(self.fixture.clis["claude"].config_path.read_bytes(), original)
        self.assertFalse(self.fixture.state_file.exists())
        self.assertEqual(self.fixture.backups(), [])

    def test_symlinked_config_directory_restores_exactly(self) -> None:
        """Preserve logical parent topology while binding resolved artifacts."""
        linked = self.fixture.root / "linked-configs"
        linked.symlink_to(self.fixture.configs, target_is_directory=True)
        self.fixture.clis["cursor"] = dataclasses.replace(
            self.fixture.clis["cursor"],
            config_path=linked / "cursor.json",
        )
        original = self.fixture.seed("cursor")

        self.assertEqual(self.fixture.run(self.fixture.argv("cursor")), 0)
        self.assertEqual(self.fixture.run(self.fixture.revert_argv("cursor")), 0)

        self.assertTrue(linked.is_symlink())
        self.assertEqual(self.fixture.clis["cursor"].config_path.read_bytes(), original)
        self.assertFalse(self.fixture.state_file.exists())
        self.assertEqual(self.fixture.backups(), [])

    def test_all_eight_clients_restore_bytes_modes_and_state(self) -> None:
        """Complete one combined all-client transaction without residue."""
        before = {cli: self.fixture.seed(cli) for cli in mcp_swap.ALL_CLIS}
        modes = {
            cli: self.fixture.clis[cli].config_path.stat().st_mode & 0o777
            for cli in mcp_swap.ALL_CLIS
        }

        self.assertEqual(self.fixture.run(self.fixture.argv(*mcp_swap.ALL_CLIS)), 0)
        self.assertEqual(
            self.fixture.run(self.fixture.revert_argv(*reversed(mcp_swap.ALL_CLIS))),
            0,
        )

        for cli, contents in before.items():
            path = self.fixture.clis[cli].config_path
            self.assertEqual(path.read_bytes(), contents)
            self.assertEqual(path.stat().st_mode & 0o777, modes[cli])
        self.assertFalse(self.fixture.state_file.exists())
        self.assertEqual(self.fixture.backups(), [])


if __name__ == "__main__":
    unittest.main()
