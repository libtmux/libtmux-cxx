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


if __name__ == "__main__":
    unittest.main()
