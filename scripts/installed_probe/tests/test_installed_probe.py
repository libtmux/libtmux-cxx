"""Each probe passes a well-behaved program and fails the ways it can misbehave."""

from __future__ import annotations

import contextlib
import io
import pathlib
import sys

import installed_probe
import pytest

FAKE_SERVER = """#!{python}
import json
import os
import sys

mode = os.environ.get("FAKE_MODE", "good")
if mode == "exit-nonzero":
    sys.stderr.write("fake server failed")
    sys.exit(3)
if mode == "silent":
    sys.stdin.read()
    sys.exit(0)
if mode == "noisy-stderr":
    sys.stderr.write("x" * 1_000_000)
    sys.stderr.flush()
name = "other" if mode == "wrong-name" else "libtmux-cxx"
tools = [{{"name": "list_panes"}}, {{"name": "capture_pane"}}]
if mode == "no-tools":
    tools = []
bad_call = "argument paneId must be a string"
if mode == "lost-arguments":
    bad_call = "wrong text"


def reply(identifier, **body):
    print(json.dumps({{"jsonrpc": "2.0", "id": identifier, **body}}), flush=True)


def invalid(identifier):
    reply(identifier, error={{"code": -32600, "message": "invalid request"}})


for line in sys.stdin:
    try:
        request = json.loads(line)
    except ValueError:
        invalid(None)
        continue
    well_formed = (
        isinstance(request, dict)
        and request.get("jsonrpc") == "2.0"
        and isinstance(request.get("method"), str)
        and isinstance(request.get("id", 0), (int, str))
        and isinstance(request.get("params", {{}}), dict)
    )
    if not well_formed:
        if mode == "crash":
            sys.exit(0)
        if mode == "accepts-anything":
            reply(1, result={{}})
        else:
            invalid(request.get("id") if isinstance(request, dict) else None)
        continue
    method, identifier = request["method"], request.get("id")
    if identifier is None:
        continue
    if method == "initialize" and mode == "batched":
        info = {{"serverInfo": {{"name": name}}}}
        first = {{"jsonrpc": "2.0", "id": identifier, "result": info}}
        second = {{"jsonrpc": "2.0", "id": 2, "result": {{"tools": tools}}}}
        sys.stdout.write(json.dumps(first) + "\\n" + json.dumps(second) + "\\n")
        sys.stdout.flush()
    elif method == "initialize":
        reply(identifier, result={{"serverInfo": {{"name": name}}}})
    elif method == "tools/list" and mode == "batched":
        pass
    elif method == "tools/list":
        reply(identifier, result={{"tools": tools}})
    elif method == "tools/call":
        reply(identifier, error={{"code": -32602, "message": bad_call}})
    else:
        reply(identifier, result={{}})
"""

FAKE_CLI = """#!{python}
import json
import os

import_children = [{{"name": "tmuxinator"}}, {{"name": "teamocil"}}]
count = int(os.environ.get("FAKE_ROOTS", "9"))
imports = int(os.environ.get("FAKE_IMPORTS", "2"))
children = [{{"name": "import", "children": import_children[:imports]}}]
children += [{{"name": f"command-{{n}}"}} for n in range(count - 1)]
print(json.dumps({{"children": children}}))
"""


def executable(directory: pathlib.Path, name: str, template: str) -> pathlib.Path:
    """Write a Python program that runs as an executable file."""
    path = directory / name
    path.write_text(template.format(python=sys.executable))
    path.chmod(0o755)
    return path


@pytest.fixture
def server(tmp_path: pathlib.Path) -> pathlib.Path:
    """Provide a stand-in MCP server whose misbehaviour `FAKE_MODE` selects."""
    return executable(tmp_path, "server", FAKE_SERVER)


def test_a_server_that_lists_tools_passes_the_catalog_probe(
    server: pathlib.Path,
) -> None:
    """A good server names its tools."""
    report = installed_probe.mcp_catalog(server, "probe", check_arguments=False)
    assert report == "tools: capture_pane, list_panes"


def test_a_bad_argument_keeps_its_arguments_and_meta(server: pathlib.Path) -> None:
    """The error names the argument, so the call arrived intact."""
    report = installed_probe.mcp_catalog(server, "probe", check_arguments=True)
    assert report.startswith("tools:")


@pytest.mark.parametrize(
    ("mode", "message"),
    [
        ("wrong-name", "unexpected serverInfo"),
        ("no-tools", "the server advertised no tools"),
        ("lost-arguments", "tools/call lost arguments or _meta"),
        ("exit-nonzero", "fake server failed"),
    ],
)
def test_the_catalog_probe_fails_on_each_way_a_server_can_be_wrong(
    server: pathlib.Path, monkeypatch: pytest.MonkeyPatch, mode: str, message: str
) -> None:
    """Each misbehaviour is reported by name."""
    monkeypatch.setenv("FAKE_MODE", mode)
    with pytest.raises((installed_probe.ProbeError, TimeoutError), match=message):
        installed_probe.mcp_catalog(server, "probe", check_arguments=True, timeout=2)


def test_a_reply_already_buffered_is_not_a_timeout(
    server: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Two replies written together are both read, the second without waiting."""
    monkeypatch.setenv("FAKE_MODE", "batched")
    report = installed_probe.mcp_catalog(
        server, "probe", check_arguments=False, timeout=1
    )
    assert report == "tools: capture_pane, list_panes"


def test_a_server_that_fills_stderr_still_answers(
    server: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The probe drains stderr while it waits, so a noisy server is not blocked."""
    monkeypatch.setenv("FAKE_MODE", "noisy-stderr")
    report = installed_probe.mcp_catalog(
        server, "probe", check_arguments=False, timeout=5
    )
    assert report == "tools: capture_pane, list_panes"


def test_a_server_that_never_answers_times_out(
    server: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The probe gives up instead of waiting for the job timeout."""
    monkeypatch.setenv("FAKE_MODE", "silent")
    with pytest.raises(TimeoutError, match="waiting for response 1"):
        installed_probe.mcp_catalog(server, "probe", check_arguments=False, timeout=0.3)


def test_a_server_that_survives_hostile_input_passes(server: pathlib.Path) -> None:
    """Every malformed line is refused and the last good request is answered."""
    report = installed_probe.mcp_hostile(server, "probe")
    assert report == "answered 11 lines and stayed up"


@pytest.mark.parametrize(
    ("mode", "message"),
    [
        ("crash", "the server stopped answering"),
        ("accepts-anything", "expected an error reply"),
        ("exit-nonzero", "fake server failed"),
    ],
)
def test_the_hostile_probe_fails_when_the_server_does_not_cope(
    server: pathlib.Path, monkeypatch: pytest.MonkeyPatch, mode: str, message: str
) -> None:
    """Dying, accepting garbage and a bad exit status are each a failure."""
    monkeypatch.setenv("FAKE_MODE", mode)
    with pytest.raises(installed_probe.ProbeError, match=message):
        installed_probe.mcp_hostile(server, "probe")


def test_the_workspace_tree_passes_with_nine_commands_and_two_sources(
    tmp_path: pathlib.Path,
) -> None:
    """The documented command tree is the one the binary prints."""
    binary = executable(tmp_path, "tmux-workspace", FAKE_CLI)
    assert installed_probe.workspace_tree(binary) == (
        "9 root commands, 2 import sources"
    )


@pytest.mark.parametrize(
    ("roots", "imports", "message"),
    [("8", "2", "expected 9 root commands"), ("9", "1", "expected 2 import sources")],
)
def test_the_workspace_tree_fails_when_a_command_is_added_or_lost(
    tmp_path: pathlib.Path,
    monkeypatch: pytest.MonkeyPatch,
    roots: str,
    imports: str,
    message: str,
) -> None:
    """A change to the command tree has to be made here too, on purpose."""
    monkeypatch.setenv("FAKE_ROOTS", roots)
    monkeypatch.setenv("FAKE_IMPORTS", imports)
    binary = executable(tmp_path, "tmux-workspace", FAKE_CLI)
    with pytest.raises(installed_probe.ProbeError, match=message):
        installed_probe.workspace_tree(binary)


def test_main_prints_the_report_and_returns_zero(
    server: pathlib.Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """The command line wraps the same checks."""
    status = installed_probe.main(
        ["mcp-catalog", "--server", str(server), "--socket-name", "probe"]
    )
    assert status == 0
    assert capsys.readouterr().out.startswith("tools:")


def test_main_reports_a_failure_on_stderr_and_returns_one(
    server: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A failed probe is a failed job step."""
    monkeypatch.setenv("FAKE_MODE", "crash")
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        status = installed_probe.main(
            ["mcp-hostile", "--server", str(server), "--socket-name", "probe"]
        )
    assert status == 1
    assert err.getvalue().startswith("mcp-hostile: the server stopped answering")
