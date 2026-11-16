#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
r"""Probe the installed workspace CLI and MCP server the way a user starts them.

``ci.yml`` runs these against the installed package, and ``release.yml`` against
the binary it is about to publish. None of them needs a tmux server: the
requests are answered by the protocol layer alone.

    uv run scripts/installed_probe/installed_probe.py workspace-tree \
        --binary build/prefix/bin/tmux-workspace
    uv run scripts/installed_probe/installed_probe.py mcp-catalog \
        --server build/prefix/bin/libtmux-mcp-server --socket-name probe
    uv run scripts/installed_probe/installed_probe.py mcp-hostile \
        --server build/prefix/bin/libtmux-mcp-server --socket-name probe
    uv run scripts/installed_probe/installed_probe.py workspace-completion \
        --binary build/prefix/bin/tmux-workspace
    uv run scripts/installed_probe/installed_probe.py package-layout \
        --prefix build/prefix --standard 20
    uv run scripts/installed_probe/installed_probe.py testing-component \
        -- -DCMAKE_PREFIX_PATH=build/windows-prefix
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
import typing as t

Message = dict[str, t.Any]

PROTOCOL_VERSION = "2025-06-18"
SERVER_NAME = "libtmux-cxx"
ROOT_COMMANDS = 9
IMPORT_COMMANDS = 2
BAD_ARGUMENT_ERROR = {"code": -32602, "message": "argument paneId must be a string"}

# The completion function for `tmux-workspace import tm`, run in the bash that
# sources the completion script the binary prints: the script under test is bash.
COMPLETION_PROGRAM = """\
set -e
source <("$WORKSPACE_BINARY" --generate-completion bash)
COMP_WORDS=("$WORKSPACE_BINARY" import tm)
COMP_CWORD=2
_tmux_workspace_complete
printf '%s' "${COMPREPLY[*]}"
"""
COMPLETION_REPLY = "tmuxinator"

# The header the C++20 configuration installs for its `tl::expected` fallback.
FALLBACK_STANDARD = 20
FALLBACK_HEADER = "include/tl/expected.hpp"
FALLBACK_CMAKE_PACKAGE = "share/cmake/tl-expected"

# What a Windows package says to a consumer that asks for the POSIX-only
# `testing` component.
TESTING_REFUSAL = (
    "the libtmux::testing component is POSIX-only and is unavailable in "
    "Windows packages"
)
TESTING_CONSUMER = """\
cmake_minimum_required(VERSION 3.25)
project(libtmux_windows_testing_consumer LANGUAGES CXX)
find_package(libtmux REQUIRED COMPONENTS testing)
"""

# Well-formed JSON that is not a well-formed request. Each of these parsed
# cleanly and then threw out of a typed read, which once ended the process, so a
# peer could stop the server with a line containing `[1,2,3]`. The last line is
# a good request: surviving means answering it, not merely not crashing.
HOSTILE_LINES = (
    "[1,2,3]",
    '"a string"',
    "42",
    "null",
    '{"jsonrpc":2.0,"id":1,"method":"ping"}',
    '{"jsonrpc":"2.0","id":{"not":"scalar"},"method":"ping"}',
    '{"jsonrpc":"2.0","id":1}',
    '{"jsonrpc":"2.0","id":2,"method":42}',
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":[]}',
    "{not json at all",
    '{"jsonrpc":"2.0","id":99,"method":"ping"}',
)
FINAL_ID = 99


class ProbeError(Exception):
    """The installed program did not behave as a user would need."""


class McpSession:
    """One server process spoken to over line-delimited JSON-RPC on stdio.

    A thread per pipe reads the server's stdout and stderr as they arrive. A
    reply is therefore available the moment its line is complete, however the
    server grouped its writes, and a server that writes a lot to stderr is never
    blocked on a full pipe. The threads make it work the same on Windows, where
    ``select`` does not accept pipes.
    """

    def __init__(self, server: pathlib.Path, socket_name: str, timeout: float) -> None:
        """Start ``server`` on the socket namespace ``socket_name``."""
        self.timeout = timeout
        self.process = subprocess.Popen(
            [str(server), "--socket-name", socket_name],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        assert self.process.stderr is not None
        self.stdin = self.process.stdin
        self._lines: queue.Queue[str | None] = queue.Queue()
        self._stderr: list[str] = []
        self._readers = [
            threading.Thread(
                target=self._read_lines, args=(self.process.stdout,), daemon=True
            ),
            threading.Thread(
                target=self._read_stderr, args=(self.process.stderr,), daemon=True
            ),
        ]
        for reader in self._readers:
            reader.start()

    def _read_lines(self, stream: t.TextIO) -> None:
        """Queue each line of stdout, then ``None`` at end of file."""
        for line in stream:
            self._lines.put(line)
        self._lines.put(None)

    def _read_stderr(self, stream: t.TextIO) -> None:
        """Keep everything the server writes to stderr."""
        for chunk in iter(lambda: stream.read(4096), ""):
            self._stderr.append(chunk)

    def _stderr_text(self) -> str:
        """Return stderr once the server has closed it."""
        self._readers[1].join(timeout=self.timeout)
        return "".join(self._stderr)

    def send(self, message: Message) -> None:
        """Write one request or notification."""
        self.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
        self.stdin.flush()

    def receive(self, expected_id: int) -> Message:
        """Read the next reply, which must answer ``expected_id``."""
        try:
            line = self._lines.get(timeout=self.timeout)
        except queue.Empty:
            message = f"timed out waiting for response {expected_id}"
            raise TimeoutError(message) from None
        if line is None:
            self._lines.put(None)
            code = self.process.wait(timeout=self.timeout)
            message = (
                f"the server exited {code} before response {expected_id}: "
                f"{self._stderr_text()}"
            )
            raise ProbeError(message)
        reply: Message = json.loads(line)
        if reply.get("id") != expected_id:
            message = f"expected response {expected_id}, got {reply}"
            raise ProbeError(message)
        return reply

    def close(self) -> str:
        """End the conversation and return the server's stderr.

        Raises
        ------
        ProbeError
            The server did not exit when its input ended, or exited nonzero.
        """
        self.stdin.close()
        try:
            code = self.process.wait(timeout=self.timeout)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=self.timeout)
            message = "the server did not exit when its input ended"
            raise ProbeError(message) from None
        stderr = self._stderr_text()
        if code != 0:
            raise ProbeError(stderr or f"the server exited {code}")
        return stderr

    def abort(self) -> None:
        """Stop the server after a failure, whatever state it is in."""
        self.process.kill()
        self.process.wait()


def workspace_tree(binary: pathlib.Path) -> str:
    """Check the command tree an installed ``tmux-workspace`` prints."""
    done = subprocess.run(
        [str(binary), "--command-tree"], capture_output=True, text=True, check=False
    )
    if done.returncode != 0:
        message = f"{binary} --command-tree exited {done.returncode}: {done.stderr}"
        raise ProbeError(message)
    tree = json.loads(done.stdout)
    children = tree["children"]
    if len(children) != ROOT_COMMANDS:
        message = f"expected {ROOT_COMMANDS} root commands, got {len(children)}"
        raise ProbeError(message)
    imports = next((node for node in children if node["name"] == "import"), None)
    if imports is None or len(imports["children"]) != IMPORT_COMMANDS:
        message = f"expected {IMPORT_COMMANDS} import sources"
        raise ProbeError(message)
    return f"{len(children)} root commands, {len(imports['children'])} import sources"


def mcp_catalog(
    server: pathlib.Path,
    socket_name: str,
    *,
    check_arguments: bool,
    expect_tools: pathlib.Path | None = None,
    timeout: float = 10,
) -> str:
    """Initialize the server, list its tools and, if asked, call one badly.

    With ``expect_tools``, a file of tool names, the server must advertise
    exactly those.
    """
    session = McpSession(server, socket_name, timeout)
    try:
        session.send(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": PROTOCOL_VERSION,
                    "capabilities": {},
                    "clientInfo": {"name": "probe", "version": "1"},
                },
            }
        )
        initialized = session.receive(1)
        session.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        session.send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        listed = session.receive(2)
        called: Message | None = None
        if check_arguments:
            session.send(
                {
                    "jsonrpc": "2.0",
                    "id": 3,
                    "method": "tools/call",
                    "params": {
                        "name": "capture_pane",
                        "arguments": {"paneId": 99},
                        "_meta": {
                            "progressToken": "probe",
                            "claudeCode": {"version": "probe"},
                        },
                    },
                }
            )
            called = session.receive(3)
    except BaseException:
        session.abort()
        raise
    session.close()

    info = initialized["result"]["serverInfo"]
    if info["name"] != SERVER_NAME:
        message = f"unexpected serverInfo: {info}"
        raise ProbeError(message)
    names = sorted(tool["name"] for tool in listed["result"]["tools"])
    if not names:
        message = "the server advertised no tools"
        raise ProbeError(message)
    if called is not None and called.get("error") != BAD_ARGUMENT_ERROR:
        message = f"tools/call lost arguments or _meta: {called}"
        raise ProbeError(message)
    if expect_tools is not None:
        expected = sorted(expect_tools.read_text().split())
        if names != expected:
            missing = sorted(set(expected) - set(names))
            extra = sorted(set(names) - set(expected))
            message = f"unexpected MCP tools: missing {missing}, extra {extra}"
            raise ProbeError(message)
    return "tools: " + ", ".join(names)


def workspace_completion(binary: pathlib.Path) -> str:
    """Complete ``import tm`` with the bash completion an installed CLI prints."""
    done = subprocess.run(
        ["bash", "--noprofile", "--norc"],
        input=COMPLETION_PROGRAM,
        env={**os.environ, "WORKSPACE_BINARY": str(binary)},
        capture_output=True,
        text=True,
        check=False,
    )
    if done.returncode != 0:
        message = f"the completion script failed ({done.returncode}): {done.stderr}"
        raise ProbeError(message)
    if done.stdout != COMPLETION_REPLY:
        message = (
            f"completing `import tm` gave {done.stdout!r}, not {COMPLETION_REPLY!r}"
        )
        raise ProbeError(message)
    return f"`import tm` completes to {COMPLETION_REPLY}"


def package_layout(prefix: pathlib.Path, standard: int) -> str:
    """Check which ``tl::expected`` files an install for ``standard`` carries."""
    fallback = prefix / FALLBACK_HEADER
    if standard == FALLBACK_STANDARD and not fallback.is_file():
        message = f"C++20 install lacks its fallback header {fallback}"
        raise ProbeError(message)
    if standard != FALLBACK_STANDARD and fallback.exists():
        message = f"C++{standard} install carries the C++20 fallback {fallback}"
        raise ProbeError(message)
    package = prefix / FALLBACK_CMAKE_PACKAGE
    if package.exists():
        message = f"the install carries a private CMake package {package}"
        raise ProbeError(message)
    return f"C++{standard} install layout is as designed"


def testing_component(cmake_arguments: t.Sequence[str], *, cmake: str = "cmake") -> str:
    """Require a consumer of the ``testing`` component to be refused, by name.

    ``cmake_arguments`` select the package, as a consumer's would: the
    generator, the prefix or toolchain file. They follow the consumer's source
    and build directories.
    """
    with tempfile.TemporaryDirectory() as temporary:
        source = pathlib.Path(temporary) / "source"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(TESTING_CONSUMER)
        done = subprocess.run(
            [
                cmake,
                "-S",
                str(source),
                "-B",
                str(pathlib.Path(temporary) / "build"),
                *cmake_arguments,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
    if done.returncode == 0:
        message = "the package unexpectedly provided libtmux::testing"
        raise ProbeError(message)
    # CMake writes its status lines to the same merged stream, and one can land
    # between two halves of the diagnostic. The diagnostic is a FATAL_ERROR and
    # never starts with `--`.
    kept = [
        line for line in done.stdout.splitlines() if not line.lstrip().startswith("--")
    ]
    if TESTING_REFUSAL not in " ".join(" ".join(kept).split()):
        message = f"missing testing-component diagnostic:\n{done.stdout}"
        raise ProbeError(message)
    return TESTING_REFUSAL


def mcp_hostile(server: pathlib.Path, socket_name: str, *, timeout: float = 30) -> str:
    """Send valid JSON of the wrong shape, then require an answer to a good request."""
    try:
        done = subprocess.run(
            [str(server), "--socket-name", socket_name],
            input="".join(line + "\n" for line in HOSTILE_LINES),
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        message = "the server did not exit when its input ended"
        raise ProbeError(message) from None
    if done.returncode != 0:
        message = done.stderr or f"the server exited {done.returncode}"
        raise ProbeError(message)
    replies = [json.loads(line) for line in done.stdout.splitlines() if line.strip()]
    for reply in replies:
        if "error" not in reply and reply.get("id") != FINAL_ID:
            message = f"expected an error reply, got {reply}"
            raise ProbeError(message)
    final = [reply for reply in replies if reply.get("id") == FINAL_ID]
    if len(final) != 1 or "result" not in final[0]:
        message = f"the server stopped answering: {replies}"
        raise ProbeError(message)
    return f"answered {len(replies)} lines and stayed up"


def main(argv: t.Sequence[str] | None = None) -> int:
    """Run one probe, print what it saw, and return a process status."""
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    tree = commands.add_parser("workspace-tree", help="check the command tree")
    tree.add_argument("--binary", type=pathlib.Path, required=True)

    for name, help_text in (
        ("mcp-catalog", "initialize the server and list its tools"),
        ("mcp-hostile", "send malformed requests and require an answer"),
    ):
        command = commands.add_parser(name, help=help_text)
        command.add_argument("--server", type=pathlib.Path, required=True)
        command.add_argument("--socket-name", required=True)
        if name == "mcp-catalog":
            command.add_argument(
                "--check-arguments",
                action="store_true",
                help="also call capture_pane with a bad argument and read the error",
            )
            command.add_argument(
                "--expect-tools",
                type=pathlib.Path,
                help="a file of tool names the server must advertise, exactly",
            )

    completion = commands.add_parser(
        "workspace-completion", help="complete `import tm` with the bash completion"
    )
    completion.add_argument("--binary", type=pathlib.Path, required=True)

    layout = commands.add_parser(
        "package-layout", help="check the tl::expected files an install carries"
    )
    layout.add_argument("--prefix", type=pathlib.Path, required=True)
    layout.add_argument("--standard", type=int, required=True)

    component = commands.add_parser(
        "testing-component",
        help="require a consumer of the testing component to be refused",
    )
    component.add_argument(
        "cmake_arguments",
        nargs=argparse.REMAINDER,
        help="after `--`: the cmake options that select the package",
    )

    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "workspace-tree":
            report = workspace_tree(arguments.binary)
        elif arguments.command == "workspace-completion":
            report = workspace_completion(arguments.binary)
        elif arguments.command == "package-layout":
            report = package_layout(arguments.prefix, arguments.standard)
        elif arguments.command == "testing-component":
            options = [a for a in arguments.cmake_arguments if a != "--"]
            report = testing_component(options)
        elif arguments.command == "mcp-catalog":
            report = mcp_catalog(
                arguments.server,
                arguments.socket_name,
                check_arguments=arguments.check_arguments,
                expect_tools=arguments.expect_tools,
            )
        else:
            report = mcp_hostile(arguments.server, arguments.socket_name)
    except (ProbeError, TimeoutError, OSError, ValueError, KeyError) as error:
        print(f"{arguments.command}: {error}", file=sys.stderr)
        return 1
    print(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
