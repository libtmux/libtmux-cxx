"""Each probe passes a well-behaved program and fails the ways it can misbehave."""

from __future__ import annotations

import contextlib
import io
import pathlib
import shutil
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


FAKE_CMAKE = """#!{python}
import os
import sys

status = int(os.environ.get("FAKE_STATUS", "1"))
print("-- Configuring incomplete")
print("CMake Error at CMakeLists.txt:3 (find_package):")
print("  the libtmux::testing component is POSIX-only and")
print("-- a status line lands between the halves")
print("  is unavailable in Windows packages")
if os.environ.get("FAKE_TEXT") == "other":
    print("a different failure")
sys.exit(status)
"""

FAKE_COMPLETION_CLI = """#!{python}
import os

reply = os.environ.get("FAKE_REPLY", "tmuxinator")
print("_tmux_workspace_complete() {{ COMPREPLY=(" + reply + "); }}")
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


@pytest.mark.parametrize(
    ("standard", "header", "package", "message"),
    [
        (20, True, False, None),
        (23, False, False, None),
        (20, False, False, "lacks its fallback header"),
        (23, True, False, "install carries the C.*20 fallback"),
        (23, False, True, "private CMake package"),
    ],
)
def test_the_package_layout_follows_the_standard(
    tmp_path: pathlib.Path,
    standard: int,
    header: bool,
    package: bool,
    message: str | None,
) -> None:
    """Only the C++20 install carries the fallback header, and none its package."""
    if header:
        (tmp_path / "include/tl").mkdir(parents=True)
        (tmp_path / "include/tl/expected.hpp").write_text("")
    if package:
        (tmp_path / "share/cmake/tl-expected").mkdir(parents=True)
    if message is None:
        assert installed_probe.package_layout(tmp_path, standard).startswith("C++")
    else:
        with pytest.raises(installed_probe.ProbeError, match=message):
            installed_probe.package_layout(tmp_path, standard)


def test_a_refused_testing_component_passes(tmp_path: pathlib.Path) -> None:
    """The diagnostic is found even with a status line inside it."""
    cmake = executable(tmp_path, "cmake", FAKE_CMAKE)
    report = installed_probe.testing_component([], cmake=str(cmake))
    assert "POSIX-only" in report


def test_a_testing_component_that_is_provided_fails(
    tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A package that serves the component to Windows is wrong."""
    monkeypatch.setenv("FAKE_STATUS", "0")
    cmake = executable(tmp_path, "cmake", FAKE_CMAKE)
    with pytest.raises(installed_probe.ProbeError, match="unexpectedly provided"):
        installed_probe.testing_component([], cmake=str(cmake))


def test_a_refusal_that_says_something_else_fails(tmp_path: pathlib.Path) -> None:
    """Failing is not enough; the failure has to name the reason."""
    cmake = executable(tmp_path, "cmake", FAKE_CMAKE.replace("POSIX-only", "broken"))
    with pytest.raises(installed_probe.ProbeError, match="missing testing-component"):
        installed_probe.testing_component([], cmake=str(cmake))


@pytest.mark.skipif(shutil.which("bash") is None, reason="needs bash")
def test_the_completion_probe_runs_the_installed_completion(
    tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """`import tm` completes to the one source that starts with it."""
    cli = executable(tmp_path, "tmux-workspace", FAKE_COMPLETION_CLI)
    assert "tmuxinator" in installed_probe.workspace_completion(cli)
    monkeypatch.setenv("FAKE_REPLY", "teamocil tmuxinator")
    with pytest.raises(installed_probe.ProbeError, match="completing `import tm`"):
        installed_probe.workspace_completion(cli)


def test_the_catalog_must_match_the_expected_tools(
    server: pathlib.Path, tmp_path: pathlib.Path
) -> None:
    """A server that advertises more or fewer tools than listed fails."""
    expected = tmp_path / "tools.txt"
    expected.write_text("capture_pane\nlist_panes\n")
    report = installed_probe.mcp_catalog(
        server, "probe", check_arguments=False, expect_tools=expected
    )
    assert report == "tools: capture_pane, list_panes"
    expected.write_text("capture_pane\nlist_windows\n")
    with pytest.raises(installed_probe.ProbeError, match="unexpected MCP tools"):
        installed_probe.mcp_catalog(
            server, "probe", check_arguments=False, expect_tools=expected
        )
