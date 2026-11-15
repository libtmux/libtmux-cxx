"""The README inventory comes from the server's registry, and drift fails."""

from __future__ import annotations

import contextlib
import io
import json
import os
import pathlib
import sys
import typing as t

import mcp_inventory
import pytest

FAKE_SERVER = """#!{python}
import json, os, sys

seen = {{
    key: os.environ.get(key)
    for key in ("LIBTMUX_SOCKET", "LIBTMUX_TOOLS", "LIBTMUX_TOOLSETS", "TMUX")
}}
seen["argv"] = sys.argv[1:]
for line in sys.stdin:
    request = json.loads(line)
    if request.get("id") == "capabilities":
        document = {{"tools": {tools}, "seen": seen}}
        reply = {{
            "jsonrpc": "2.0",
            "id": "capabilities",
            "result": {{"contents": [{{"text": json.dumps(document)}}]}},
        }}
        print(json.dumps(reply), flush=True)
"""
TOOLS = [
    {"name": "list_panes", "toolset": "inspect"},
    {"name": "create_session", "toolset": "manage"},
    {"name": "capture_pane", "toolset": "inspect"},
    {"name": "send_keys", "toolset": "execute"},
]
README = f"""# Server

before
{mcp_inventory.BEGIN}
stale
{mcp_inventory.END}
after
"""


def fake_server(directory: pathlib.Path, tools: list[dict[str, str]]) -> pathlib.Path:
    """Write a server that answers the capability request with `tools`."""
    server = directory / "server"
    server.write_text(FAKE_SERVER.format(python=sys.executable, tools=tools))
    server.chmod(0o755)
    return server


def run(*arguments: str) -> tuple[int, str]:
    """Run `main` and return its status and stderr."""
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        status = mcp_inventory.main(list(arguments))
    return status, err.getvalue()


def test_the_inventory_groups_tools_by_toolset_in_registry_order() -> None:
    """Each row lists its toolset's tools as the registry ordered them."""
    block = mcp_inventory.inventory({"tools": TOOLS})
    inspect_row = (
        "| `inspect` | [`list_panes`][tool-list_panes], "
        "[`capture_pane`][tool-capture_pane] |"
    )
    assert inspect_row in block
    assert "| `manage` | [`create_session`][tool-create_session] |" in block
    assert "| `teardown` |  |" in block
    assert block.startswith(mcp_inventory.BEGIN)
    assert block.endswith(mcp_inventory.END)


def test_every_tool_links_to_its_page() -> None:
    """The reference definitions follow the table, toolset by toolset."""
    block = mcp_inventory.inventory({"tools": TOOLS})
    definitions = [line for line in block.splitlines() if line.startswith("[tool-")]
    assert definitions[0] == (
        "[tool-list_panes]: https://libtmux.org/en/cxx/latest/mcp/tools/list_panes/"
    )
    assert len(definitions) == len(TOOLS)


def test_a_tool_in_an_unknown_toolset_is_refused() -> None:
    """A toolset the README has no row for cannot be listed silently."""
    with pytest.raises(KeyError):
        mcp_inventory.inventory({"tools": [{"name": "x", "toolset": "mystery"}]})


def test_the_server_is_asked_with_a_clean_environment(
    tmp_path: pathlib.Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Variables that select a socket or a tool filter never reach the server."""
    for name in ("LIBTMUX_SOCKET", "LIBTMUX_TOOLS", "TMUX"):
        monkeypatch.setenv(name, "leak")
    document = mcp_inventory.capability_document(fake_server(tmp_path, TOOLS))
    seen: dict[str, t.Any] = document["seen"]
    assert seen["LIBTMUX_SOCKET"] is None
    assert seen["LIBTMUX_TOOLS"] is None
    assert seen["TMUX"] is None
    assert seen["LIBTMUX_TOOLSETS"] == "inspect,manage,execute,teardown"
    assert seen["argv"] == ["--socket-name", "libtmux-cxx-readme-no-dispatch"]
    assert "leak" not in json.dumps(document)
    assert os.environ["TMUX"] == "leak"


def test_check_fails_on_a_stale_readme_and_leaves_it_alone(
    tmp_path: pathlib.Path,
) -> None:
    """`--check` reports drift and writes nothing."""
    readme = tmp_path / "README.md"
    readme.write_text(README)
    status, err = run(
        "--server",
        str(fake_server(tmp_path, TOOLS)),
        "--readme",
        str(readme),
        "--check",
    )
    assert status == 1
    assert "tool inventory is stale" in err
    assert readme.read_text() == README


def test_writing_makes_the_check_pass(tmp_path: pathlib.Path) -> None:
    """The block the script writes is the block `--check` accepts."""
    readme = tmp_path / "README.md"
    readme.write_text(README)
    server = str(fake_server(tmp_path, TOOLS))
    assert run("--server", server, "--readme", str(readme))[0] == 0
    written = readme.read_text()
    assert written.startswith("# Server\n\nbefore\n")
    assert written.endswith("after\n")
    assert "[`send_keys`][tool-send_keys]" in written
    assert run("--server", server, "--readme", str(readme), "--check") == (0, "")
