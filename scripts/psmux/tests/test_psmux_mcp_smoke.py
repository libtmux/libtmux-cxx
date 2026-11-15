"""The MCP smoke test walks a stand-in server and cleans up what it created."""

from __future__ import annotations

import json
import pathlib
import sys
import tempfile
import typing as t

import psmux_mcp_smoke
import pytest

pytestmark = pytest.mark.skipif(
    sys.platform == "win32", reason="the stand-ins are shebang scripts"
)

SUFFIX = "abc123"
NAMESPACE = f"libtmux-cxx-mcp-{SUFFIX}"
SESSION = f"mcp-{SUFFIX}"

FAKE_TMUX = """#!{python}
import os, sys

with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(" ".join(sys.argv[1:]) + "\\n")
if sys.argv[4] == "kill-session":
    sys.exit(int(os.environ.get("FAKE_KILL_EXIT", "0")))
"""
FAKE_SERVER = """#!{python}
import json, os, sys, time

mode = os.environ.get("FAKE_MODE", "good")
namespace = sys.argv[2]
session = namespace.replace("libtmux-cxx-mcp-", "mcp-")
tools = json.load(open(os.environ["FAKE_TOOLS"]))
if mode == "bad-tools":
    tools = tools[:-1]
route = "-S psmux:x" if mode == "posix-route" else ""
with open(os.environ["FAKE_ENV_DUMP"], "w") as dump:
    toolsets = os.environ.get("LIBTMUX_TOOLSETS")
    json.dump({{"toolsets": toolsets, "tmux": "TMUX" in os.environ}}, dump)


def reply(identifier, result):
    message = {{"jsonrpc": "2.0", "id": identifier, "result": result}}
    print(json.dumps(message), flush=True)


def structured(body):
    return {{"structuredContent": body}}


for line in sys.stdin:
    request = json.loads(line)
    identifier, method = request.get("id"), request.get("method")
    if method == "initialize":
        reply(identifier, {{"protocolVersion": "2025-06-18"}})
    elif method == "resources/read":
        document = {{
            "connection": {{
                "socketSelector": "name:" + namespace,
                "namespaceSelector": "psmux:-L:" + namespace,
                "resolvedSocketPath": None,
                "attachCommand": None,
            }},
            "note": route,
        }}
        reply(identifier, {{"contents": [{{"text": json.dumps(document)}}]}})
    elif method == "tools/list":
        reply(identifier, {{"tools": [{{"name": name}} for name in tools]}})
    elif method == "tools/call":
        name = request["params"]["name"]
        if name == "list_sessions":
            body = {{"sessions": [{{"id": "$1", "name": session}}]}}
        elif name == "list_windows":
            body = {{"windows": [{{"session_id": "$1"}}]}}
        elif name == "list_panes":
            body = {{"panes": [{{"session_id": "$1"}}]}}
        else:
            body = {{"running": True, "version": "3.3"}}
        reply(identifier, structured(body))
if mode == "exit-nonzero":
    sys.stderr.write("server failed")
    sys.exit(3)
if mode == "hang":
    time.sleep(30)
"""


class Rig:
    """A profile, stand-in programs and the environment that reaches them."""

    def __init__(self, root: pathlib.Path, monkeypatch: pytest.MonkeyPatch) -> None:
        """Build the stand-ins under ``root``."""
        monkeypatch.setattr(tempfile, "tempdir", str(root))
        self.root = root
        self.log = root / "tmux.log"
        self.dump = root / "server-env.json"
        self.registry = root / ".psmux"
        self.config = root / f"libtmux-cxx-mcp-{SUFFIX}.conf"
        tools = root / "tools.json"
        tools.write_text(json.dumps(list(psmux_mcp_smoke.EXPECTED_TOOLS)))
        self.tmux = self.program("tmux", FAKE_TMUX)
        self.server = self.program("server", FAKE_SERVER)
        self.environ = {
            "USERPROFILE": str(root),
            "FAKE_LOG": str(self.log),
            "FAKE_ENV_DUMP": str(self.dump),
            "FAKE_TOOLS": str(tools),
            "TMUX": "/leak,1,0",
            "LIBTMUX_TOOLSETS": "leaked",
        }

    def program(self, name: str, template: str) -> pathlib.Path:
        """Write a Python program as an executable file."""
        path = self.root / name
        path.write_text(template.format(python=sys.executable))
        path.chmod(0o755)
        return path

    def smoke(self, **kwargs: t.Any) -> str | None:
        """Run the smoke test against the stand-ins."""
        return psmux_mcp_smoke.smoke(
            str(self.server),
            tmux=str(self.tmux),
            environ=self.environ,
            suffix=SUFFIX,
            timeout=5,
            **kwargs,
        )

    def invocations(self) -> list[str]:
        """Return each tmux command line the smoke test issued."""
        return self.log.read_text().splitlines() if self.log.exists() else []


@pytest.fixture
def rig(tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch) -> Rig:
    """Provide the stand-ins."""
    return Rig(tmp_path, monkeypatch)


def test_the_pinned_catalog_has_forty_five_distinct_tools() -> None:
    """The inventory the smoke test pins is the documented size."""
    tools = psmux_mcp_smoke.EXPECTED_TOOLS
    assert len(tools) == len(set(tools)) == 45


def test_a_well_behaved_server_passes_and_everything_is_removed(rig: Rig) -> None:
    """The run is green, the session killed once and the config deleted."""
    assert rig.smoke() is None
    assert rig.invocations() == [
        f"-u -L {NAMESPACE} new-session -d -s {SESSION}",
        f"-u -L {NAMESPACE} kill-session -t {SESSION}",
    ]
    assert not rig.config.exists()


def test_the_server_sees_every_toolset_and_no_routing_variable(rig: Rig) -> None:
    """The toolset list is the caller's to set, and TMUX never leaks in."""
    assert rig.smoke() is None
    seen = json.loads(rig.dump.read_text())
    assert seen == {"toolsets": psmux_mcp_smoke.TOOLSETS, "tmux": False}


@pytest.mark.parametrize(
    ("mode", "message"),
    [
        ("bad-tools", "authoritative inventory"),
        ("posix-route", "capability resource emitted a POSIX route for psmux"),
        ("exit-nonzero", "MCP server exited 3"),
    ],
)
def test_each_contract_failure_is_reported_and_still_cleaned_up(
    rig: Rig, monkeypatch: pytest.MonkeyPatch, mode: str, message: str
) -> None:
    """A server that breaks the contract fails the run after the cleanup."""
    monkeypatch.setitem(rig.environ, "FAKE_MODE", mode)
    failure = rig.smoke()
    assert failure is not None
    assert message in failure
    assert len(rig.invocations()) == 2
    assert not rig.config.exists()


def test_a_server_that_ignores_the_end_of_its_input_is_stopped(
    rig: Rig, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The run fails, the process is killed, and the cleanup still completes."""
    monkeypatch.setitem(rig.environ, "FAKE_MODE", "hang")
    failure = rig.smoke(exit_timeout=0.3, stop_timeout=0.3)
    assert failure == "MCP server did not stop after stdin closed"
    assert not rig.config.exists()


def test_a_failed_kill_keeps_the_config_and_names_the_recovery(
    rig: Rig, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The exact socket, session and config come back with the failure."""
    monkeypatch.setitem(rig.environ, "FAKE_KILL_EXIT", "47")
    failure = rig.smoke()
    assert failure is not None
    assert "cleanup failed with exit 47" in failure
    assert f"recovery socket={NAMESPACE} session={SESSION}" in failure
    assert f"config={rig.config}" in failure
    assert rig.config.exists()


def test_registry_state_for_the_namespace_is_never_adopted(rig: Rig) -> None:
    """A namespace that already has files is somebody else's."""
    rig.registry.mkdir()
    (rig.registry / f"{NAMESPACE}__{SESSION}.port").write_text("1")
    failure = rig.smoke()
    assert failure is not None
    assert "psmux namespace already has registry state" in failure
    assert rig.invocations() == []


def test_capabilities_that_expose_a_socket_path_are_refused() -> None:
    """A psmux namespace is a name, not a path."""
    document = {
        "connection": {
            "socketSelector": f"name:{NAMESPACE}",
            "namespaceSelector": f"psmux:-L:{NAMESPACE}",
            "resolvedSocketPath": "/tmp/sock",
            "attachCommand": None,
        }
    }
    with pytest.raises(psmux_mcp_smoke.SmokeError, match="as a socket path"):
        psmux_mcp_smoke.check_capabilities(document, NAMESPACE, json.dumps(document))
