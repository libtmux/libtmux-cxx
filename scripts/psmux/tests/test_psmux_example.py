"""The example runner creates one exact session and proves it removed it."""

from __future__ import annotations

import json
import os
import pathlib
import sys
import typing as t

import psmux_example
import pytest

pytestmark = pytest.mark.skipif(
    sys.platform == "win32", reason="the stand-ins are shebang scripts"
)

FAKE_TMUX = """#!{python}
import os, sys

with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(" ".join(sys.argv[1:]) + "\\n")
if sys.argv[4] == "new-session":
    sys.exit(int(os.environ.get("FAKE_NEW_EXIT", "0")))
if sys.argv[4] == "kill-session":
    sys.stderr.write("kill said no")
    sys.exit(int(os.environ.get("FAKE_KILL_EXIT", "0")))
sys.exit(91)
"""
FAKE_EXAMPLE = """#!{python}
import json, os, sys

with open(os.environ["FAKE_ENV_DUMP"], "w") as dump:
    json.dump({{"argv": sys.argv[1:], "env": dict(os.environ)}}, dump)
sys.exit(int(os.environ.get("FAKE_EXAMPLE_EXIT", "0")))
"""
NAMESPACE = "libtmux-cxx-example-test"


class Rig:
    """A profile directory, stand-in programs and the environment that reaches them."""

    def __init__(self, root: pathlib.Path, **extra: str) -> None:
        """Build the stand-ins under ``root``."""
        self.root = root
        self.log = root / "tmux.log"
        self.dump = root / "env.json"
        self.config = root / "recovery.conf"
        self.registry = root / ".psmux"
        self.tmux = self.program("tmux", FAKE_TMUX)
        self.example = self.program("example", FAKE_EXAMPLE)
        self.environ = {
            "USERPROFILE": str(root),
            "FAKE_LOG": str(self.log),
            "FAKE_ENV_DUMP": str(self.dump),
            "TMUX": "/leak,1,0",
            "PSMUX_TARGET": "leak",
            **extra,
        }

    def program(self, name: str, template: str) -> pathlib.Path:
        """Write a Python program as an executable file."""
        path = self.root / name
        path.write_text(template.format(python=sys.executable))
        path.chmod(0o755)
        return path

    def run(self, *, config: pathlib.Path | None = None) -> int:
        """Run the example through the runner and return its status."""
        return psmux_example.run(
            str(self.example),
            tmux=str(self.tmux),
            config_path=str(config or self.config),
            environ=self.environ,
            namespace=NAMESPACE,
        )

    def invocations(self) -> list[str]:
        """Return each tmux command line the runner issued."""
        return self.log.read_text().splitlines() if self.log.exists() else []


def test_a_clean_run_creates_runs_kills_and_removes_the_config(
    tmp_path: pathlib.Path,
) -> None:
    """The happy path issues exactly two tmux commands and leaves nothing."""
    rig = Rig(tmp_path)
    assert rig.run() == 0
    assert rig.invocations() == [
        f"-u -L {NAMESPACE} new-session -d -s example",
        f"-u -L {NAMESPACE} kill-session -t example",
    ]
    assert not rig.config.exists()


def test_the_example_gets_the_namespace_and_no_routing_variables(
    tmp_path: pathlib.Path,
) -> None:
    """The program sees only the isolated environment."""
    rig = Rig(tmp_path)
    assert rig.run() == 0
    seen = json.loads(rig.dump.read_text())
    assert seen["argv"] == [NAMESPACE, "example"]
    assert "TMUX" not in seen["env"]
    assert "PSMUX_TARGET" not in seen["env"]
    assert seen["env"]["PSMUX_NO_WARM"] == "1"
    assert seen["env"]["PSMUX_CONFIG_FILE"] == str(rig.config)
    assert seen["env"]["PATHEXT"] == ".COM;.EXE;.BAT;.CMD"
    assert os.environ.get("PSMUX_NO_WARM") != "1"


def test_the_examples_exit_status_is_the_runners(tmp_path: pathlib.Path) -> None:
    """A failing example fails the run, after the cleanup."""
    rig = Rig(tmp_path, FAKE_EXAMPLE_EXIT="5")
    assert rig.run() == 5
    assert len(rig.invocations()) == 2
    assert not rig.config.exists()


def test_an_existing_config_is_refused_before_tmux_runs(tmp_path: pathlib.Path) -> None:
    """A collision never touches tmux and never overwrites the file."""
    rig = Rig(tmp_path)
    rig.config.write_text("do not overwrite")
    assert rig.run() != 0
    assert rig.invocations() == []
    assert rig.config.read_text() == "do not overwrite"


def test_a_failed_cleanup_keeps_the_config_and_says_how_to_recover(
    tmp_path: pathlib.Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """The exact socket, session and config are named, and the config is kept."""
    rig = Rig(tmp_path, FAKE_KILL_EXIT="47")
    assert rig.run() != 0
    assert rig.config.exists()
    assert rig.config.read_text() == ""
    assert rig.invocations() == [
        f"-u -L {NAMESPACE} new-session -d -s example",
        f"-u -L {NAMESPACE} kill-session -t example",
    ]
    error = capsys.readouterr().err
    assert "cleanup failed with exit 47" in error
    assert f"recovery socket={NAMESPACE}" in error
    assert "session=example" in error
    assert f"config={rig.config}" in error


def test_registry_state_that_outlives_the_session_fails_the_run(
    tmp_path: pathlib.Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """A live registry file after the kill means the server is still there."""
    monkeypatch.setattr("psmux_fixture.SETTLE_SECONDS", 0.05)
    monkeypatch.setattr("psmux_fixture.POLL_SECONDS", 0.01)
    rig = Rig(tmp_path)
    # The fake kill leaves this behind, as a psmux that did not stop would.
    original = psmux_example.fixture.kill_session

    def leave_a_port_file(*args: t.Any, **kwargs: t.Any) -> str | None:
        rig.registry.mkdir(exist_ok=True)
        (rig.registry / f"{NAMESPACE}__example.port").write_text("1")
        return original(*args, **kwargs)

    monkeypatch.setattr(psmux_example.fixture, "kill_session", leave_a_port_file)
    assert rig.run() != 0
    assert rig.config.exists()
    assert "psmux left 1 exact live registry file(s)" in capsys.readouterr().err


def test_a_session_file_psmux_leaves_after_a_clean_kill_is_removed(
    tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The exact `.sid` file is the one residue the runner deletes."""
    rig = Rig(tmp_path)
    rig.registry.mkdir()
    original = psmux_example.fixture.kill_session

    def leave_a_sid_file(*args: t.Any, **kwargs: t.Any) -> str | None:
        (rig.registry / f"{NAMESPACE}__example.sid").write_text("")
        (rig.registry / "other__session.sid").write_text("")
        return original(*args, **kwargs)

    monkeypatch.setattr(psmux_example.fixture, "kill_session", leave_a_sid_file)
    assert rig.run() == 0
    assert [p.name for p in rig.registry.iterdir()] == ["other__session.sid"]


def test_preexisting_registry_state_for_the_namespace_is_refused(
    tmp_path: pathlib.Path,
) -> None:
    """The runner never adopts a namespace someone else may own."""
    rig = Rig(tmp_path)
    rig.registry.mkdir()
    (rig.registry / f"{NAMESPACE}__example.port").write_text("1")
    assert rig.run() != 0
    assert rig.invocations() == []
    assert (rig.registry / f"{NAMESPACE}__example.port").exists()


def test_a_fixture_that_cannot_be_created_is_reported_and_cleaned_up(
    tmp_path: pathlib.Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """A refused `new-session` fails the run and still tries the exact kill."""
    rig = Rig(tmp_path, FAKE_NEW_EXIT="9")
    assert rig.run() != 0
    assert not rig.dump.exists()
    assert "psmux fixture creation failed" in capsys.readouterr().err


@pytest.mark.parametrize("path", ["", "relative.conf"])
def test_a_config_path_must_be_absolute(path: str, tmp_path: pathlib.Path) -> None:
    """A relative path would land wherever the caller happens to be."""
    rig = Rig(tmp_path)
    status = psmux_example.run(
        str(rig.example), tmux=str(rig.tmux), config_path=path, environ=rig.environ
    )
    assert status == 1
    assert rig.invocations() == []
