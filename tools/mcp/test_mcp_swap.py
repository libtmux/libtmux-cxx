"""Focused tests for the MCP config-swap safety preflight."""

from __future__ import annotations

import pathlib
import sys
import tempfile
import textwrap
import unittest

from tools.mcp import mcp_swap

_FAKE_SERVER = r"""
import json
import sys

mode = sys.argv[1]
frames = [json.loads(sys.stdin.readline()) for _ in range(3)]
if [frame.get("method") for frame in frames] != [
    "initialize",
    "notifications/initialized",
    "tools/list",
]:
    raise SystemExit(9)

version = "1900-01-01" if mode == "wrong-version" else "2025-06-18"
name = "another-server" if mode == "wrong-server" else "libtmux-cxx"
print(json.dumps({
    "jsonrpc": "2.0",
    "id": 1,
    "result": {
        "protocolVersion": version,
        "capabilities": {"tools": {}},
        "serverInfo": {"name": name, "version": "test"},
    },
}), flush=True)
if mode != "missing-tools":
    tools = [] if mode == "empty-tools" else [
        {"name": "inspect_tmux"},
        {"name": "list_session_panes"},
        {"name": "list_sessions"},
        {"name": "list_windows"},
    ]
    print(json.dumps({
        "jsonrpc": "2.0",
        "id": 2,
        "result": {"tools": tools},
    }), flush=True)
if mode == "duplicate-reply":
    print(json.dumps({"jsonrpc": "2.0", "id": 2, "result": {"tools": tools}}))
if mode == "garbage-stdout":
    print("not a protocol frame")
if mode == "crash-after-reply":
    print("synthetic crash", file=sys.stderr)
    raise SystemExit(7)
"""


class McpSwapPreflightTest(unittest.TestCase):
    """Exercise the lifecycle gate without touching a real agent config."""

    def setUp(self) -> None:
        """Materialize a task-owned fake stdio server."""
        self._temporary = tempfile.TemporaryDirectory(prefix="mcp-swap-test-")
        self._server = pathlib.Path(self._temporary.name) / "fake_server.py"
        self._server.write_text(textwrap.dedent(_FAKE_SERVER), encoding="utf-8")

    def tearDown(self) -> None:
        """Remove the task-owned fake server."""
        self._temporary.cleanup()

    def spec(self, mode: str) -> mcp_swap.McpServerSpec:
        """Return a test server spec for ``mode``."""
        return mcp_swap.McpServerSpec(
            command=sys.executable,
            args=[str(self._server), mode],
        )

    def test_complete_lifecycle_and_catalog_pass(self) -> None:
        """Accept a clean lifecycle and catalog exchange."""
        self.assertIsNone(mcp_swap.preflight_spec(self.spec("ok"), timeout=2.0))

    def test_protocol_version_must_be_echoed(self) -> None:
        """Reject a server that negotiates a different protocol."""
        failure = mcp_swap.preflight_spec(self.spec("wrong-version"), timeout=2.0)
        self.assertEqual(
            failure, "server did not echo the requested MCP protocol version"
        )

    def test_server_identity_must_match(self) -> None:
        """Reject a healthy but unrelated MCP server."""
        failure = mcp_swap.preflight_spec(self.spec("wrong-server"), timeout=2.0)
        self.assertEqual(
            failure,
            "initialize response did not identify a libtmux MCP server",
        )

    def test_tool_catalog_is_required(self) -> None:
        """Require one operational request after initialization."""
        failure = mcp_swap.preflight_spec(self.spec("missing-tools"), timeout=2.0)
        self.assertEqual(failure, "server initialized but did not answer tools/list")

    def test_required_tools_must_be_discoverable(self) -> None:
        """Reject an MCP endpoint that exposes no usable libtmux surface."""
        failure = mcp_swap.preflight_spec(self.spec("empty-tools"), timeout=2.0)
        self.assertEqual(
            failure,
            "server tool catalog is missing: inspect_tmux, list_session_panes, "
            "list_sessions, list_windows",
        )

    def test_duplicate_reply_is_rejected(self) -> None:
        """Require one response for each request ID."""
        failure = mcp_swap.preflight_spec(self.spec("duplicate-reply"), timeout=2.0)
        self.assertEqual(
            failure, "server answered an MCP preflight request more than once"
        )

    def test_stdout_must_remain_a_clean_protocol_stream(self) -> None:
        """Reject diagnostics or banners mixed into newline-framed JSON-RPC."""
        failure = mcp_swap.preflight_spec(self.spec("garbage-stdout"), timeout=2.0)
        self.assertEqual(
            failure, "server wrote non-JSON data to the MCP stdout transport"
        )

    def test_reply_followed_by_crash_is_rejected(self) -> None:
        """Do not install a server that crashes during shutdown."""
        failure = mcp_swap.preflight_spec(
            self.spec("crash-after-reply"),
            timeout=2.0,
        )
        self.assertEqual(failure, "synthetic crash")

    def test_binary_spec_never_invents_a_default_route(self) -> None:
        """Keep absent routes absent and explicit routes exact."""
        binary = pathlib.Path("/tmp/libtmux-mcp-server")
        inherited = mcp_swap.build_binary_spec(binary)
        explicit = mcp_swap.build_binary_spec(binary, "/tmp/libtmux-private/socket")
        self.assertEqual(inherited.args, [])
        self.assertEqual(explicit.args, ["/tmp/libtmux-private/socket"])


if __name__ == "__main__":
    unittest.main()
