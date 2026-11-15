#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
r"""Smoke-test the MCP server against a native Windows psmux fixture.

It creates one psmux session in a private namespace, starts the server on it and
walks the protocol: initialize, the capability resource, the pinned tool
catalog, and four tools that read the fixture. It then closes the server's
input and requires a zero exit, kills the exact session and proves the
session's registry files and the isolated configuration are gone. A failed
cleanup names the socket, session, config and, if the server could not be shown
to have exited, its process id.

    python scripts/psmux/psmux_mcp_smoke.py --server PATH
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading
import time
import typing as t
import uuid
from collections.abc import Mapping, Sequence

import psmux_fixture as fixture

PROTOCOL_VERSION = "2025-06-18"
TOOLSETS = "inspect,manage,execute,teardown"
RESPONSE_SECONDS = 15.0
EXIT_SECONDS = 5.0
STOP_SECONDS = 3.0

# The authoritative Windows catalog. psmux advertises the same registry as POSIX
# and refuses what it cannot do, rather than hiding those tools.
EXPECTED_TOOLS = (
    "call_read_tools_batch",
    "capture_pane",
    "capture_since",
    "clear_pane_scrollback",
    "create_session",
    "create_window",
    "find_pane_by_position",
    "get_pane_info",
    "get_server_info",
    "get_session_info",
    "get_tmux_variables",
    "get_window_info",
    "kill_pane",
    "kill_session",
    "kill_window",
    "list_panes",
    "list_sessions",
    "list_windows",
    "move_window",
    "paste_text",
    "rename_session",
    "rename_window",
    "resize_pane",
    "resize_window",
    "respawn_pane",
    "run_shell_command",
    "search_panes",
    "select_layout",
    "select_pane",
    "select_window",
    "send_keys",
    "send_keys_batch",
    "set_history_limit",
    "set_mouse_enabled",
    "set_pane_title",
    "set_synchronize_panes",
    "show_environment",
    "show_hooks",
    "show_option",
    "signal_channel",
    "snapshot_pane",
    "split_window",
    "swap_pane",
    "wait_for_channel",
    "wait_for_text",
)

Json = dict[str, t.Any]


class SmokeError(Exception):
    """The server did not behave as the Windows contract requires."""


class Server:
    """The MCP server process and its line-delimited JSON-RPC pipes."""

    def __init__(
        self,
        command: Sequence[str],
        environment: Mapping[str, str],
        timeout: float = RESPONSE_SECONDS,
    ) -> None:
        """Start the server with piped stdio."""
        self.timeout = timeout
        self.process = subprocess.Popen(
            list(command),
            env=dict(environment),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
        )
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.stdin = self.process.stdin
        self.lines: queue.Queue[str | None] = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()
        self.stderr_chunks: list[str] = []
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def _read(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            self.lines.put(line)
        self.lines.put(None)

    def _read_stderr(self) -> None:
        assert self.process.stderr is not None
        self.stderr_chunks.append(self.process.stderr.read())

    def send(self, message: Json) -> None:
        """Write one request or notification."""
        self.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
        self.stdin.flush()

    def read_response(self, expected_id: str | int) -> Json:
        """Return the reply for ``expected_id``, skipping unrelated messages."""
        deadline = time.monotonic() + self.timeout
        while True:
            remaining = deadline - time.monotonic()
            try:
                line = self.lines.get(timeout=max(remaining, 0.001))
            except queue.Empty:
                message = f"timed out waiting for MCP response {expected_id}"
                raise SmokeError(message) from None
            if line is None:
                message = f"MCP server closed stdout before response {expected_id}"
                raise SmokeError(message)
            reply: Json = json.loads(line)
            if "id" in reply and str(reply["id"]) == str(expected_id):
                return reply

    def request(self, message: Json) -> Json:
        """Send a request and return its reply, failing on a JSON-RPC error."""
        self.send(message)
        reply = self.read_response(message["id"])
        if "error" in reply:
            raise SmokeError(json.dumps(reply["error"], separators=(",", ":")))
        return reply

    def tool(self, identifier: int, name: str, arguments: Json) -> Json:
        """Call a tool and return its structured content."""
        reply = self.request(
            {
                "jsonrpc": "2.0",
                "id": identifier,
                "method": "tools/call",
                "params": {"name": name, "arguments": arguments},
            }
        )
        result: Json = reply["result"]
        if result.get("isError"):
            raise SmokeError(json.dumps(result.get("content"), separators=(",", ":")))
        structured: Json = result["structuredContent"]
        return structured

    def stop(self, *, graceful: float, forceful: float) -> tuple[bool, str | None]:
        """End the process and report whether it is proven gone, and why not."""
        detail: str | None = None
        try:
            self.stdin.close()
        except OSError as error:
            detail = str(error)
        try:
            self.process.wait(timeout=graceful)
        except subprocess.TimeoutExpired:
            try:
                self.process.kill()
                self.process.wait(timeout=forceful)
            except (OSError, subprocess.TimeoutExpired) as error:
                detail = detail or str(error)
        return self.process.poll() is not None, detail


def check_capabilities(document: Json, namespace: str, text: str) -> None:
    """Require the capability resource to describe a psmux namespace, not a path."""
    connection = document["connection"]
    if connection.get("socketSelector") != f"name:{namespace}":
        message = "capability resource lost the operator's socket selector"
        raise SmokeError(message)
    if connection.get("namespaceSelector") != f"psmux:-L:{namespace}":
        message = "capability resource did not report the psmux namespace selector"
        raise SmokeError(message)
    if "resolvedSocketPath" not in connection or connection["resolvedSocketPath"]:
        message = "capability resource reported the psmux namespace as a socket path"
        raise SmokeError(message)
    if "attachCommand" not in connection or connection["attachCommand"]:
        message = "capability resource synthesized an unsupported psmux attach command"
        raise SmokeError(message)
    if "-S psmux:" in text:
        message = "capability resource emitted a POSIX route for psmux"
        raise SmokeError(message)


def converse(server: Server, namespace: str, session: str) -> None:
    """Walk the protocol and read the fixture through four tools."""
    initialized = server.request(
        {
            "jsonrpc": "2.0",
            "id": "initialize",
            "method": "initialize",
            "params": {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": "windows-psmux-smoke", "version": "1"},
            },
        }
    )
    if initialized["result"]["protocolVersion"] != PROTOCOL_VERSION:
        message = "unexpected MCP protocol version"
        raise SmokeError(message)
    server.send({"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})

    capabilities = server.request(
        {
            "jsonrpc": "2.0",
            "id": "capabilities",
            "method": "resources/read",
            "params": {"uri": "tmux://capabilities"},
        }
    )
    text = str(capabilities["result"]["contents"][0]["text"])
    check_capabilities(json.loads(text), namespace, text)

    listed = server.request(
        {"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}
    )
    actual = sorted(tool["name"] for tool in listed["result"]["tools"])
    if actual != sorted(EXPECTED_TOOLS):
        message = "Windows MCP tool catalog is not the authoritative inventory"
        raise SmokeError(message)

    sessions = server.tool(2, "list_sessions", {})["sessions"]
    owned = [entry for entry in sessions if entry["name"] == session]
    if len(owned) != 1:
        message = "fixture session was not listed exactly"
        raise SmokeError(message)
    session_id = str(owned[0]["id"])
    if not (session_id.startswith("$") and session_id[1:].isdigit()):
        message = "invalid session ID"
        raise SmokeError(message)

    windows = server.tool(3, "list_windows", {"session": session_id})["windows"]
    if not windows:
        message = "created session has no window"
        raise SmokeError(message)
    if any(window["session_id"] != session_id for window in windows):
        message = "window escaped its exact session"
        raise SmokeError(message)

    panes = server.tool(4, "list_panes", {})["panes"]
    mine = [pane for pane in panes if pane["session_id"] == session_id]
    if not mine:
        message = "created session has no pane"
        raise SmokeError(message)

    info = server.tool(5, "get_server_info", {})
    if not info.get("running"):
        message = "get_server_info reported a stopped server"
        raise SmokeError(message)
    if not info.get("version"):
        message = "get_server_info omitted the tmux version"
        raise SmokeError(message)


class Run:
    """How far a run got, which decides what its cleanup must prove."""

    def __init__(self) -> None:
        """Start with no fixture and no server."""
        self.attempted = False
        self.server: Server | None = None
        self.process_proven = True


def _exercise(
    command: Sequence[str],
    state: Run,
    *,
    tmux: str,
    namespace: str,
    session: str,
    config: pathlib.Path,
    registry: pathlib.Path,
    environment: Mapping[str, str],
    timeout: float,
    exit_timeout: float,
) -> None:
    """Create the fixture, start the server and run the whole conversation."""
    config.write_text("")
    if fixture.registry_entries(registry, f"{namespace}__{session}.*"):
        message = "psmux namespace already has registry state"
        raise SmokeError(message)
    state.attempted = True
    made = subprocess.run(
        [tmux, "-u", "-L", namespace, "new-session", "-d", "-s", session],
        env=dict(environment),
        capture_output=True,
        text=True,
        check=False,
    )
    if made.returncode != 0:
        output = "; ".join(
            part for part in (made.stdout.strip(), made.stderr.strip()) if part
        )
        message = f"failed to create exact psmux fixture: {output}"
        raise SmokeError(message)
    state.process_proven = False
    server = state.server = Server(command, environment, timeout)
    converse(server, namespace, session)
    server.stdin.close()
    try:
        code = server.process.wait(timeout=exit_timeout)
    except subprocess.TimeoutExpired:
        message = "MCP server did not stop after stdin closed"
        raise SmokeError(message) from None
    state.process_proven = True
    if code != 0:
        message = f"MCP server exited {code}: {''.join(server.stderr_chunks)}"
        raise SmokeError(message)


def smoke(
    server_path: str,
    *,
    tmux: str = "tmux.exe",
    environ: Mapping[str, str] | None = None,
    suffix: str | None = None,
    timeout: float = RESPONSE_SECONDS,
    exit_timeout: float = EXIT_SECONDS,
    stop_timeout: float = STOP_SECONDS,
) -> str | None:
    """Run the smoke test and its cleanup.

    Returns
    -------
    str | None
        ``None`` on success, otherwise what failed, with recovery details when
        the fixture or the server could not be proven gone.
    """
    base_environment = dict(os.environ if environ is None else environ)
    suffix = suffix or uuid.uuid4().hex
    namespace = f"libtmux-cxx-mcp-{suffix}"
    session = f"mcp-{suffix}"
    base = f"{namespace}__{session}"
    config = pathlib.Path(tempfile.gettempdir()) / f"libtmux-cxx-mcp-{suffix}.conf"
    try:
        registry = (
            pathlib.Path(fixture.resolve_profile_root(base_environment)) / ".psmux"
        )
    except fixture.PsmuxError as error:
        return str(error)
    environment = fixture.isolated_environment(base_environment, config)
    environment.pop("LIBTMUX_TOOLSETS", None)
    environment["LIBTMUX_TOOLSETS"] = TOOLSETS

    state = Run()
    failure: str | None = None
    try:
        _exercise(
            [server_path, "--socket-name", namespace],
            state,
            tmux=tmux,
            namespace=namespace,
            session=session,
            config=config,
            registry=registry,
            environment=environment,
            timeout=timeout,
            exit_timeout=exit_timeout,
        )
    except (SmokeError, fixture.PsmuxError, OSError, ValueError, KeyError) as error:
        failure = str(error)
    attempted, server, process_proven = (
        state.attempted,
        state.server,
        state.process_proven,
    )

    process_detail: str | None = None
    if server is not None and not process_proven:
        process_proven, process_detail = server.stop(
            graceful=stop_timeout, forceful=stop_timeout
        )
    if not process_proven:
        failure = failure or "MCP process exit could not be proven" + (
            f": {process_detail}" if process_detail else ""
        )

    fixture_proven = not attempted
    if attempted:
        kill_failure = fixture.kill_session(tmux, namespace, session, environment)
        try:
            fixture_proven, registry_failure = fixture.prove_registry_clean(
                registry, base, killed=kill_failure is None
            )
        except fixture.PsmuxError as error:
            fixture_proven, registry_failure = False, str(error)
        failure = failure or kill_failure or registry_failure
    config_failure: str | None = None
    if process_proven and fixture_proven:
        config_failure = fixture.remove_config(config)
        failure = failure or config_failure

    if failure is not None and (
        not (process_proven and fixture_proven) or config_failure is not None
    ):
        extra = ""
        if server is not None and not process_proven:
            extra = f" mcp-pid={server.process.pid}"
        return fixture.recovery(failure, namespace, session, config, extra)
    return failure


def main(argv: Sequence[str] | None = None) -> int:
    """Run the command line and return a process status."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True)
    parser.add_argument("--tmux-executable", default="tmux.exe")
    arguments = parser.parse_args(argv)
    failure = smoke(arguments.server, tmux=arguments.tmux_executable)
    if failure is not None:
        print(failure, file=sys.stderr)
        return 1
    print("PASS: native Windows MCP psmux smoke")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
