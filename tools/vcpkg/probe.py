"""Install the port the way a consumer does, from the registry, and use it.

The check beside this one proves the versions database and the ports agree.
It cannot prove a consumer can resolve them: that needs a real vcpkg, a real
manifest naming this repository as a git registry, and a real build against
whatever comes back.

So this writes the two files a consumer writes, points them at this checkout,
and builds `examples/consume` -- the smallest complete consumer, which needs no
tmux server -- through the vcpkg toolchain. What it exercises that nothing else
does is resolution: the baseline commit, the git-tree behind it, and the
package config arriving somewhere `find_package` looks.
"""

from __future__ import annotations

import contextlib
import json
import os
import pathlib
import queue
import secrets
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import typing as t

from tools.vcpkg.check import port_version
from tools.vcpkg.check import run as check_registry
from tools.vcpkg.git import git

_BUILD_CONFIGURATION = "Release"
_PORT_NAME = "libtmux"
_PROTOCOL_VERSION = "2024-11-05"
_PROTOCOL_OUTPUT_LIMIT = 1024 * 1024
_LEGACY_ALPHA2_VERSION = "0.1.0-alpha.2"
_LEGACY_ALPHA2_PROTOCOL_VERSION = "2024-11-05"
_LEGACY_ALPHA2_INPUT_TYPES = {
    "capture_pane": {"target": "string"},
    "list_panes": {},
    "list_sessions": {},
    "new_window": {"name": "string", "session": "string"},
    "search_panes": {"text": "string"},
    "send_keys": {"keys": "string", "target": "string"},
    "send_text": {"target": "string", "text": "string"},
    "wait_for_text": {
        "target": "string",
        "text": "string",
        "timeout_ms": "string",
    },
}
_LEGACY_ALPHA2_REQUIRED = {
    "capture_pane": frozenset({"target"}),
    "list_panes": frozenset(),
    "list_sessions": frozenset(),
    "new_window": frozenset({"name", "session"}),
    "search_panes": frozenset({"text"}),
    "send_keys": frozenset({"keys", "target"}),
    "send_text": frozenset({"target", "text"}),
    "wait_for_text": frozenset({"target", "text"}),
}
_LEGACY_ALPHA2_TOOLS = frozenset(_LEGACY_ALPHA2_INPUT_TYPES)
_POSIX_TOOLS = frozenset(
    {
        "capture_pane",
        "create_session",
        "inspect_tmux",
        "list_panes",
        "list_session_panes",
        "list_sessions",
        "list_windows",
        "new_window",
        "search_panes",
        "send_keys",
        "send_text",
        "wait_for_text",
    }
)
_WINDOWS_TOOLS = frozenset(
    {"inspect_tmux", "list_session_panes", "list_sessions", "list_windows"}
)
_TOOL_INPUTS = {
    "capture_pane": frozenset({"target"}),
    "create_session": frozenset({"name"}),
    "inspect_tmux": frozenset(),
    "list_panes": frozenset(),
    "list_session_panes": frozenset({"session"}),
    "list_sessions": frozenset(),
    "list_windows": frozenset({"session"}),
    "new_window": frozenset({"name", "session"}),
    "search_panes": frozenset({"text"}),
    "send_keys": frozenset({"keys", "target"}),
    "send_text": frozenset({"target", "text"}),
    "wait_for_text": frozenset({"target", "text", "timeout_ms"}),
}
_TOOL_REQUIRED = {
    name: inputs - ({"timeout_ms"} if name == "wait_for_text" else set())
    for name, inputs in _TOOL_INPUTS.items()
}
_TOOL_INPUT_TYPES = {
    name: {key: "integer" if key == "timeout_ms" else "string" for key in inputs}
    for name, inputs in _TOOL_INPUTS.items()
}
_TOOL_OUTPUTS = {
    "capture_pane": frozenset({"pane_id", "text"}),
    "create_session": frozenset({"name", "session_id"}),
    "inspect_tmux": frozenset({"panes", "sessions", "windows"}),
    "list_panes": frozenset({"panes"}),
    "list_session_panes": frozenset({"panes"}),
    "list_sessions": frozenset({"sessions"}),
    "list_windows": frozenset({"windows"}),
    "new_window": frozenset({"session_id", "window_id"}),
    "search_panes": frozenset({"matches"}),
    "send_keys": frozenset({"pane_id"}),
    "send_text": frozenset({"pane_id"}),
    "wait_for_text": frozenset(
        {"elapsed_ms", "matched", "mode", "pane_id", "text", "timed_out"}
    ),
}
_TOOL_OUTPUT_TYPES = {
    "capture_pane": {"pane_id": "string", "text": "string"},
    "create_session": {"name": "string", "session_id": "string"},
    "inspect_tmux": {"panes": "array", "sessions": "array", "windows": "array"},
    "list_panes": {"panes": "array"},
    "list_session_panes": {"panes": "array"},
    "list_sessions": {"sessions": "array"},
    "list_windows": {"windows": "array"},
    "new_window": {"session_id": "string", "window_id": "string"},
    "search_panes": {"matches": "array"},
    "send_keys": {"pane_id": "string"},
    "send_text": {"pane_id": "string"},
    "wait_for_text": {
        "elapsed_ms": "integer",
        "matched": "boolean",
        "mode": "string",
        "pane_id": "string",
        "text": "string",
        "timed_out": "boolean",
    },
}
_READ_ONLY_TOOLS = frozenset(
    {
        "capture_pane",
        "inspect_tmux",
        "list_panes",
        "list_session_panes",
        "list_sessions",
        "list_windows",
        "search_panes",
        "wait_for_text",
    }
)
_TERMINAL_TOOLS = frozenset({"send_keys", "send_text"})


def _host_is_windows() -> bool:
    """Return whether programs built for this host carry ``.exe``."""
    return sys.platform == "win32"


def _paths_overlap(left: pathlib.Path, right: pathlib.Path) -> bool:
    """Return whether either path contains the other."""
    return left == right or left in right.parents or right in left.parents


def _platform_executable(
    label: str,
    directories: list[pathlib.Path],
    name: str,
    *,
    windows: bool,
) -> tuple[pathlib.Path | None, str | None]:
    """Select exactly one executable with the host platform's suffix."""
    wanted = f"{name}.exe" if windows else name
    other = name if windows else f"{name}.exe"
    expected = [directory / wanted for directory in directories]
    candidates = expected + [directory / other for directory in directories]
    present = [candidate for candidate in candidates if candidate.is_file()]

    if not present:
        choices = ", ".join(str(candidate) for candidate in expected)
        return None, f"{label}: missing; expected exactly one of {choices}"
    if len(present) != 1:
        choices = ", ".join(str(candidate) for candidate in present)
        return None, f"{label}: ambiguous artifacts: {choices}"
    if present[0] not in expected:
        choices = ", ".join(str(candidate) for candidate in expected)
        return (
            None,
            (
                f"{label}: {present[0]} has the wrong platform suffix; expected "
                f"exactly one of {choices}"
            ),
        )
    return present[0], None


def _installed_root(
    build: pathlib.Path,
    triplet: str | None,
) -> tuple[pathlib.Path | None, str | None]:
    """Select the one target package root produced by vcpkg."""
    base = build / "vcpkg_installed"
    if triplet is not None:
        expected = base / triplet
        if (expected / "share").is_dir():
            return expected, None
        return None, f"{expected}: target package root is missing"

    roots = sorted(path.parent for path in base.glob("*/share") if path.is_dir())
    if not roots:
        return None, f"{base}: no target package root was installed"
    if len(roots) != 1:
        choices = ", ".join(str(root) for root in roots)
        return None, f"{base}: ambiguous target package roots: {choices}"
    return roots[0], None


def _run(
    command: list[str],
    *,
    cwd: pathlib.Path | None = None,
    expected_stdout: str | None = None,
) -> int:
    """Run a command, streaming its output, and return its status."""
    printable = " ".join(command)
    print(f"$ {printable}", flush=True)
    if expected_stdout is None:
        return subprocess.run(command, cwd=cwd, check=False).returncode
    return _run_expected(
        command,
        cwd=cwd,
        expected_stdout=expected_stdout,
    )


def _identity_stdout_matches(actual: str, expected: str, *, windows: bool) -> bool:
    """Compare the identity line using only the host CRT newline convention."""
    comparable = actual.replace("\r\n", "\n") if windows else actual
    return comparable == expected


class _WindowsJob:
    """A kill-on-close Job Object containing one MCP server process tree."""

    def __init__(self, handle: int) -> None:
        self._handle = handle

    @classmethod
    def attach(cls, process: subprocess.Popen[bytes]) -> _WindowsJob:
        """Create and assign a kill-on-close job before protocol bytes are sent."""
        import ctypes
        from ctypes import wintypes

        class BasicLimitInformation(ctypes.Structure):
            _fields_ = [
                ("PerProcessUserTimeLimit", ctypes.c_int64),
                ("PerJobUserTimeLimit", ctypes.c_int64),
                ("LimitFlags", wintypes.DWORD),
                ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t),
                ("ActiveProcessLimit", wintypes.DWORD),
                ("Affinity", ctypes.c_size_t),
                ("PriorityClass", wintypes.DWORD),
                ("SchedulingClass", wintypes.DWORD),
            ]

        class IoCounters(ctypes.Structure):
            _fields_ = [
                ("ReadOperationCount", ctypes.c_uint64),
                ("WriteOperationCount", ctypes.c_uint64),
                ("OtherOperationCount", ctypes.c_uint64),
                ("ReadTransferCount", ctypes.c_uint64),
                ("WriteTransferCount", ctypes.c_uint64),
                ("OtherTransferCount", ctypes.c_uint64),
            ]

        class ExtendedLimitInformation(ctypes.Structure):
            _fields_ = [
                ("BasicLimitInformation", BasicLimitInformation),
                ("IoInfo", IoCounters),
                ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t),
                ("PeakJobMemoryUsed", ctypes.c_size_t),
            ]

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        kernel32.SetInformationJobObject.argtypes = [
            wintypes.HANDLE,
            ctypes.c_int,
            ctypes.c_void_p,
            wintypes.DWORD,
        ]
        kernel32.SetInformationJobObject.restype = wintypes.BOOL
        kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL

        def require(success: int) -> None:
            if not success:
                raise ctypes.WinError(ctypes.get_last_error())

        handle = kernel32.CreateJobObjectW(None, None)
        require(handle)
        information = ExtendedLimitInformation()
        information.BasicLimitInformation.LimitFlags = 0x00002000
        try:
            require(
                kernel32.SetInformationJobObject(
                    handle,
                    9,
                    ctypes.byref(information),
                    ctypes.sizeof(information),
                )
            )
            process_handle = wintypes.HANDLE(int(process._handle))  # type: ignore[attr-defined]
            require(kernel32.AssignProcessToJobObject(handle, process_handle))
        except BaseException:
            kernel32.CloseHandle(handle)
            raise
        return cls(int(handle))

    def close(self) -> None:
        """Close the job once, terminating every process still assigned to it."""
        if self._handle == 0:
            return
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL
        kernel32.CloseHandle(wintypes.HANDLE(self._handle))
        self._handle = 0


def _resume_windows_process(process: subprocess.Popen[bytes]) -> None:
    """Resume a process only after its kill-on-close job owns it."""
    import ctypes
    from ctypes import wintypes

    ntdll = ctypes.WinDLL("ntdll", use_last_error=True)
    ntdll.NtResumeProcess.argtypes = [wintypes.HANDLE]
    ntdll.NtResumeProcess.restype = wintypes.LONG
    status = int(
        ntdll.NtResumeProcess(
            wintypes.HANDLE(int(process._handle)),  # type: ignore[attr-defined]
        )
    )
    if status != 0:
        message = f"NtResumeProcess failed with NTSTATUS 0x{status & 0xFFFFFFFF:08x}"
        raise OSError(message)


def _terminate_process_tree(
    process: subprocess.Popen[bytes],
    job: _WindowsJob | None,
) -> None:
    """Stop the protocol process and descendants without using a default shell."""
    if job is not None:
        job.close()
    if os.name == "nt":
        with contextlib.suppress(OSError, subprocess.SubprocessError):
            subprocess.run(
                ["taskkill.exe", "/PID", str(process.pid), "/T", "/F"],
                check=False,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=1,
            )
    else:
        with contextlib.suppress(OSError):
            os.killpg(process.pid, signal.SIGKILL)
    with contextlib.suppress(OSError):
        process.kill()
    with contextlib.suppress(OSError, subprocess.SubprocessError):
        process.wait(timeout=0.5)


def _run_expected(
    command: list[str],
    *,
    cwd: pathlib.Path | None,
    expected_stdout: str,
    timeout: float = 30.0,
) -> int:
    """Run a small identity consumer with bounded time, memory, and descendants."""
    creation: dict[str, object] = {}
    if os.name == "nt":
        creation["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP | 0x00000004
    else:
        creation["start_new_session"] = True
    try:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            **creation,
        )
    except OSError as error:
        print(f"error: could not launch identity consumer: {error}", file=sys.stderr)
        return 1

    job: _WindowsJob | None = None
    if os.name == "nt":
        try:
            job = _WindowsJob.attach(process)
            _resume_windows_process(process)
        except OSError as error:
            _terminate_process_tree(process, None)
            print(
                f"error: could not contain identity consumer: {error}", file=sys.stderr
            )
            return 1

    chunks: tuple[list[bytes], list[bytes]] = ([], [])
    captured = 0
    lock = threading.Lock()
    overflow = threading.Event()

    def drain(pipe: t.BinaryIO, output: list[bytes]) -> None:
        nonlocal captured
        try:
            read = getattr(pipe, "read1", pipe.read)
            while chunk := read(16 * 1024):
                with lock:
                    remaining = max(0, _PROTOCOL_OUTPUT_LIMIT - captured)
                    output.append(chunk[:remaining])
                    captured += len(chunk)
                    if captured > _PROTOCOL_OUTPUT_LIMIT:
                        overflow.set()
                if overflow.is_set():
                    return
        finally:
            pipe.close()

    assert process.stdout is not None
    assert process.stderr is not None
    readers = tuple(
        threading.Thread(target=drain, args=(pipe, output), daemon=True)
        for pipe, output in zip((process.stdout, process.stderr), chunks, strict=True)
    )
    for reader in readers:
        reader.start()

    deadline = time.monotonic() + timeout
    failure: str | None = None
    while process.poll() is None:
        if overflow.is_set():
            failure = (
                f"identity consumer output exceeded {_PROTOCOL_OUTPUT_LIMIT} bytes"
            )
            break
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            failure = f"identity consumer timed out after {timeout:g}s"
            break
        with contextlib.suppress(subprocess.TimeoutExpired):
            process.wait(timeout=min(0.05, remaining))

    _terminate_process_tree(process, job)

    join_deadline = min(deadline, time.monotonic() + 0.25)
    for reader in readers:
        reader.join(timeout=max(0.0, join_deadline - time.monotonic()))
    if any(reader.is_alive() for reader in readers):
        _terminate_process_tree(process, job)
        failure = failure or "identity consumer output readers did not stop"
    if overflow.is_set():
        failure = f"identity consumer output exceeded {_PROTOCOL_OUTPUT_LIMIT} bytes"
    if failure is not None:
        print(f"error: {failure}", file=sys.stderr)
        return 1

    try:
        stdout = b"".join(chunks[0]).decode("utf-8")
        stderr = b"".join(chunks[1]).decode("utf-8")
    except UnicodeDecodeError as error:
        print(
            f"error: identity consumer output was not UTF-8: {error}", file=sys.stderr
        )
        return 1
    sys.stdout.write(stdout)
    sys.stderr.write(stderr)
    if process.returncode == 0 and not _identity_stdout_matches(
        stdout,
        expected_stdout,
        windows=os.name == "nt",
    ):
        print(
            f"error: expected stdout {expected_stdout!r}, got {stdout!r}",
            file=sys.stderr,
        )
        return 1
    return int(process.returncode)


def _initialize_problem(message: object, *, version: str) -> str | None:
    """Validate the initialize response before advancing the MCP lifecycle."""
    legacy_alpha2 = version == _LEGACY_ALPHA2_VERSION
    if not isinstance(message, dict):
        return "initialize reply is not a JSON-RPC object"
    if legacy_alpha2 and set(message) != {"id", "jsonrpc", "result"}:
        return "alpha.2 initialize returned unexpected fields"
    if type(message.get("id")) is not int or message.get("id") != 1:
        return "initialize reply did not preserve integer request id 1"
    if message.get("jsonrpc") != "2.0":
        return "initialize reply does not declare JSON-RPC 2.0"
    if "error" in message:
        return "initialize returned an error"
    initialized = message.get("result")
    if not isinstance(initialized, dict):
        return "server exited without answering initialize"
    expected_protocol = (
        _LEGACY_ALPHA2_PROTOCOL_VERSION if legacy_alpha2 else _PROTOCOL_VERSION
    )
    if initialized.get("protocolVersion") != expected_protocol:
        return "server did not negotiate the requested MCP protocol"
    if legacy_alpha2 and set(initialized) != {
        "capabilities",
        "protocolVersion",
        "serverInfo",
    }:
        return "alpha.2 initialize result returned unexpected fields"
    capabilities = initialized.get("capabilities")
    if not isinstance(capabilities, dict) or not isinstance(
        capabilities.get("tools"), dict
    ):
        return "initialize response has no capabilities object"
    if legacy_alpha2 and capabilities != {"tools": {}}:
        return "alpha.2 initialize returned unexpected capabilities"
    server_info = initialized.get("serverInfo")
    expected_name = "libtmux" if legacy_alpha2 else "libtmux-cxx"
    if legacy_alpha2 and (
        not isinstance(server_info, dict) or set(server_info) != {"name", "version"}
    ):
        return "alpha.2 serverInfo returned unexpected fields"
    if not isinstance(server_info, dict) or server_info.get("name") != expected_name:
        return f"initialize response does not identify {expected_name} {version}"
    if server_info.get("version") != version:
        return f"initialize response does not identify {expected_name} {version}"
    instructions = initialized.get("instructions")
    if not legacy_alpha2 and (not isinstance(instructions, str) or not instructions):
        return "initialize response has no server instructions"
    return None


def _run_protocol(
    command: list[str],
    initialize: str,
    after_initialize: str,
    *,
    version: str,
    timeout: float = 60.0,
) -> tuple[subprocess.CompletedProcess[list[str]] | None, int | None, str | None]:
    """Run one causal stdio lifecycle with a single deadline and output bound."""
    deadline = time.monotonic() + timeout
    creation: dict[str, object] = {}
    if os.name == "nt":
        creation["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP | 0x00000004
    else:
        creation["start_new_session"] = True
    try:
        process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            **creation,
        )
    except OSError as error:
        return None, None, f"could not launch: {error}"

    job: _WindowsJob | None = None
    if os.name == "nt":
        try:
            job = _WindowsJob.attach(process)
            _resume_windows_process(process)
        except OSError as error:
            _terminate_process_tree(process, None)
            return None, None, f"could not contain protocol process tree: {error}"

    chunks: tuple[list[bytes], list[bytes]] = ([], [])
    captured = 0
    lock = threading.Lock()
    overflow = threading.Event()
    stdout_lines: queue.Queue[bytes | None] = queue.Queue()

    def retain(chunk: bytes, output: list[bytes]) -> None:
        nonlocal captured
        with lock:
            remaining = max(0, _PROTOCOL_OUTPUT_LIMIT - captured)
            output.append(chunk[:remaining])
            captured += len(chunk)
            if captured > _PROTOCOL_OUTPUT_LIMIT:
                overflow.set()
                stdout_lines.put(None)

    def drain_stdout(pipe: t.BinaryIO, output: list[bytes]) -> None:
        pending = bytearray()
        try:
            read = getattr(pipe, "read1", pipe.read)
            while chunk := read(16 * 1024):
                retain(chunk, output)
                pending.extend(chunk)
                while (newline := pending.find(b"\n")) >= 0:
                    stdout_lines.put(bytes(pending[: newline + 1]))
                    del pending[: newline + 1]
                if overflow.is_set():
                    return
        finally:
            if pending:
                stdout_lines.put(bytes(pending))
            stdout_lines.put(None)
            pipe.close()

    def drain_stderr(pipe: t.BinaryIO, output: list[bytes]) -> None:
        try:
            read = getattr(pipe, "read1", pipe.read)
            while chunk := read(16 * 1024):
                retain(chunk, output)
                if overflow.is_set():
                    return
        finally:
            pipe.close()

    assert process.stdout is not None
    assert process.stderr is not None
    readers = (
        threading.Thread(
            target=drain_stdout,
            args=(process.stdout, chunks[0]),
            daemon=True,
        ),
        threading.Thread(
            target=drain_stderr,
            args=(process.stderr, chunks[1]),
            daemon=True,
        ),
    )
    for reader in readers:
        reader.start()

    def remaining() -> float:
        return max(0.0, deadline - time.monotonic())

    def next_stdout_line() -> bytes | None:
        try:
            return stdout_lines.get(timeout=remaining())
        except queue.Empty:
            return None

    failure: str | None = None
    assert process.stdin is not None
    try:
        try:
            process.stdin.write(initialize.encode("utf-8"))
            process.stdin.flush()
        except (BrokenPipeError, OSError) as error:
            failure = f"initialize could not be sent: {error}"

        line = None if failure is not None else next_stdout_line()
        if overflow.is_set():
            failure = f"protocol output exceeded {_PROTOCOL_OUTPUT_LIMIT} bytes"
        elif line is None and failure is None:
            failure = (
                f"timed out after {timeout:g}s waiting for initialize"
                if remaining() <= 0
                else "server exited without answering initialize"
            )
        elif line is not None and failure is None:
            try:
                initialized = json.loads(line)
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                failure = f"malformed initialize reply: {error}"
            else:
                failure = _initialize_problem(initialized, version=version)

        list_id = secrets.randbelow(2**63 - 2) + 2
        if failure is None:
            try:
                list_request = json.dumps(
                    {
                        "jsonrpc": "2.0",
                        "id": list_id,
                        "method": "tools/list",
                    },
                    separators=(",", ":"),
                )
                process.stdin.write(
                    (after_initialize + list_request + "\n").encode("utf-8")
                )
                process.stdin.flush()
            except (BrokenPipeError, OSError) as error:
                failure = f"initialized lifecycle could not be sent: {error}"

        listed_line = None if failure is not None else next_stdout_line()
        if overflow.is_set():
            failure = f"protocol output exceeded {_PROTOCOL_OUTPUT_LIMIT} bytes"
        elif listed_line is None and failure is None:
            failure = (
                f"timed out after {timeout:g}s waiting for tools/list"
                if remaining() <= 0
                else "server exited without answering tools/list"
            )
        elif listed_line is not None and failure is None:
            try:
                listed = json.loads(listed_line)
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                failure = f"malformed tools/list reply: {error}"
            else:
                if (
                    not isinstance(listed, dict)
                    or type(listed.get("id")) is not int
                    or listed.get("id") != list_id
                    or listed.get("jsonrpc") != "2.0"
                    or "error" in listed
                    or not isinstance(listed.get("result"), dict)
                    or not isinstance(listed["result"].get("tools"), list)
                ):
                    failure = "tools/list did not return the expected JSON-RPC reply"
        with contextlib.suppress(OSError):
            process.stdin.close()

        while failure is None and process.poll() is None:
            if overflow.is_set():
                failure = f"protocol output exceeded {_PROTOCOL_OUTPUT_LIMIT} bytes"
                break
            if remaining() <= 0:
                failure = f"timed out after {timeout:g}s"
                break
            with contextlib.suppress(subprocess.TimeoutExpired):
                process.wait(timeout=min(0.05, remaining()))
    except (OSError, subprocess.SubprocessError) as error:
        failure = f"protocol process failed: {error}"

    _terminate_process_tree(process, job)

    join_deadline = min(deadline, time.monotonic() + 0.25)
    for reader in readers:
        reader.join(timeout=max(0.0, join_deadline - time.monotonic()))
    if any(reader.is_alive() for reader in readers):
        _terminate_process_tree(process, job)
        for reader in readers:
            reader.join(timeout=0.05)

    if overflow.is_set() and failure is None:
        failure = f"protocol output exceeded {_PROTOCOL_OUTPUT_LIMIT} bytes"
    if any(reader.is_alive() for reader in readers) and failure is None:
        failure = "protocol output readers did not stop"
    if failure is not None:
        return None, None, failure
    try:
        stdout = b"".join(chunks[0]).decode("utf-8")
        stderr = b"".join(chunks[1]).decode("utf-8")
    except UnicodeDecodeError as error:
        return None, None, f"protocol output was not UTF-8: {error}"
    return (
        subprocess.CompletedProcess(command, process.returncode, stdout, stderr),
        list_id,
        None,
    )


def _catalog_problem(
    stdout: str,
    *,
    windows: bool,
    version: str,
    list_id: int,
) -> tuple[list[str], str | None]:
    """Validate the complete initialize and tools/list exchange."""
    legacy_alpha2 = version == _LEGACY_ALPHA2_VERSION
    if legacy_alpha2 and windows:
        return [], "the POSIX-only alpha.2 MCP contract cannot be a Windows package"
    try:
        messages = [json.loads(line) for line in stdout.splitlines() if line.strip()]
    except json.JSONDecodeError as error:
        return [], f"malformed JSON reply: {error}"
    if len(messages) != 2 or not all(isinstance(message, dict) for message in messages):
        return [], "expected exactly two JSON-RPC object replies"
    if any(type(message.get("id")) is not int for message in messages):
        return [], "MCP replies did not preserve integer request ids"
    replies = {message.get("id"): message for message in messages}
    if set(replies) != {1, list_id} or len(replies) != 2:
        return [], f"expected exactly one reply for request ids 1 and {list_id}"
    if any(message.get("jsonrpc") != "2.0" for message in messages):
        return [], "a reply does not declare JSON-RPC 2.0"
    if any("error" in message for message in messages):
        return [], "an MCP request returned an error"

    problem = _initialize_problem(replies[1], version=version)
    if problem is not None:
        return [], problem

    if legacy_alpha2 and set(replies[list_id]) != {"id", "jsonrpc", "result"}:
        return [], "alpha.2 tools/list returned unexpected fields"
    listed = replies[list_id].get("result")
    if not isinstance(listed, dict) or not isinstance(listed.get("tools"), list):
        return [], "server initialized but did not answer tools/list"
    if legacy_alpha2 and set(listed) != {"tools"}:
        return [], "alpha.2 tools/list result returned unexpected fields"
    tools = listed["tools"]
    names: list[str] = []
    for tool in tools:
        input_schema = tool.get("inputSchema") if isinstance(tool, dict) else None
        if (
            not isinstance(tool, dict)
            or not isinstance(tool.get("name"), str)
            or not tool["name"]
            or not isinstance(tool.get("description"), str)
            or not tool["description"]
            or not isinstance(input_schema, dict)
            or input_schema.get("type") != "object"
            or input_schema.get("additionalProperties") is not False
            or not isinstance(input_schema.get("properties"), dict)
        ):
            return [], "server returned a malformed MCP tool catalog"
        name = tool["name"]
        expected_input_types = (
            _LEGACY_ALPHA2_INPUT_TYPES if legacy_alpha2 else _TOOL_INPUT_TYPES
        )
        expected_required = _LEGACY_ALPHA2_REQUIRED if legacy_alpha2 else _TOOL_REQUIRED
        if name not in expected_input_types:
            return [], "server returned an unknown MCP tool contract"
        input_properties = frozenset(input_schema["properties"])
        input_required = input_schema.get("required")
        if (
            input_properties != frozenset(expected_input_types[name])
            or not isinstance(input_required, list)
            or not all(isinstance(item, str) for item in input_required)
            or len(input_required) != len(frozenset(input_required))
            or frozenset(input_required) != expected_required[name]
        ):
            return [], f"server returned the wrong schema for MCP tool {name}"
        if any(
            not isinstance(schema, dict)
            or schema.get("type") != expected_input_types[name][key]
            or not isinstance(schema.get("description"), str)
            or not schema["description"]
            or (legacy_alpha2 and set(schema) != {"description", "type"})
            for key, schema in input_schema["properties"].items()
        ):
            return [], f"server returned malformed properties for MCP tool {name}"
        if legacy_alpha2:
            if set(tool) != {"description", "inputSchema", "name"}:
                return [], f"alpha.2 returned unexpected tool fields for {name}"
            if set(input_schema) != {
                "additionalProperties",
                "properties",
                "required",
                "type",
            }:
                return [], f"alpha.2 returned unexpected input schema fields for {name}"
            names.append(name)
            continue

        output_schema = tool.get("outputSchema")
        annotations = tool.get("annotations")
        if (
            name not in _TOOL_OUTPUTS
            or not isinstance(tool.get("title"), str)
            or not tool["title"]
            or not isinstance(output_schema, dict)
            or output_schema.get("type") != "object"
            or output_schema.get("additionalProperties") is not False
            or not isinstance(output_schema.get("properties"), dict)
            or not isinstance(annotations, dict)
            or not isinstance(annotations.get("title"), str)
            or not annotations["title"]
            or any(
                type(annotations.get(annotation)) is not bool
                for annotation in (
                    "readOnlyHint",
                    "destructiveHint",
                    "idempotentHint",
                    "openWorldHint",
                )
            )
        ):
            return [], "server returned a malformed MCP tool catalog"
        output_properties = frozenset(output_schema["properties"])
        output_required = output_schema.get("required")
        if (
            output_properties != _TOOL_OUTPUTS[name]
            or not isinstance(output_required, list)
            or not all(isinstance(item, str) for item in output_required)
            or len(output_required) != len(frozenset(output_required))
            or frozenset(output_required) != _TOOL_OUTPUTS[name]
        ):
            return [], f"server returned the wrong schema for MCP tool {name}"
        if any(
            not isinstance(schema, dict)
            or schema.get("type") != _TOOL_OUTPUT_TYPES[name][key]
            for key, schema in output_schema["properties"].items()
        ):
            return [], f"server returned malformed properties for MCP tool {name}"
        actual_annotations = (
            annotations["readOnlyHint"],
            annotations["destructiveHint"],
            annotations["idempotentHint"],
            annotations["openWorldHint"],
        )
        expected_annotations = (
            (True, False, True, False)
            if name in _READ_ONLY_TOOLS
            else (False, True, False, True)
            if name in _TERMINAL_TOOLS
            else (False, False, False, False)
        )
        if actual_annotations != expected_annotations:
            return [], f"server returned the wrong annotations for MCP tool {name}"
        names.append(name)
    if len(names) != len(set(names)):
        return [], "server returned duplicate MCP tool names"
    expected = (
        _LEGACY_ALPHA2_TOOLS
        if legacy_alpha2
        else _WINDOWS_TOOLS
        if windows
        else _POSIX_TOOLS
    )
    if set(names) != expected:
        return names, "server returned the wrong platform MCP tool catalog"
    return names, None


def _manifest(port: str, version: str, features: list[str], builtin: str) -> str:
    """Render the vcpkg.json a consumer writes."""
    dependency: dict[str, t.Any] | str = port
    if features:
        dependency = {"name": port, "features": features}
    return json.dumps(
        {
            "name": "libtmux-registry-probe",
            "version": version,
            "builtin-baseline": builtin,
            "dependencies": [dependency],
        },
        indent=2,
    )


def _configuration(port: str, repository: str, baseline: str) -> str:
    """Render the configuration that names this repository as a registry."""
    return json.dumps(
        {
            "registries": [
                {
                    "kind": "git",
                    "repository": repository,
                    "baseline": baseline,
                    "packages": [port],
                },
            ],
        },
        indent=2,
    )


def _check_installed(
    installed: pathlib.Path,
    features: list[str],
    *,
    windows: bool,
    version: str,
) -> list[str]:
    """Report what the resolved package failed to deliver."""
    problems: list[str] = []

    usage = installed / "share" / _PORT_NAME / "usage"
    usage_text = ""
    if not usage.is_file():
        problems.append(f"{usage}: the port installs no usage text")
    else:
        usage_text = usage.read_text(encoding="utf-8")
        if "COMPONENTS testing" not in usage_text:
            problems.append(
                f"{usage}: does not show the testing component, so it is the "
                f"heuristic text vcpkg generates rather than the port's own",
            )

    if "mcp" in features:
        if (
            "<installed-root>" not in usage_text
            or "--socket-name" not in usage_text
            or "libtmux-mcp-server" not in usage_text
        ):
            problems.append(
                f"{usage}: does not explain MCP executable discovery and routing"
            )
        tool, problem = _platform_executable(
            "the mcp feature installed no server",
            [installed / "tools" / _PORT_NAME],
            "libtmux-mcp-server",
            windows=windows,
        )
        if problem is not None:
            problems.append(problem)
        elif tool is not None:
            protocol = (
                _LEGACY_ALPHA2_PROTOCOL_VERSION
                if version == _LEGACY_ALPHA2_VERSION
                else _PROTOCOL_VERSION
            )
            initialize = json.dumps(
                {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "initialize",
                    "params": {
                        "protocolVersion": protocol,
                        "capabilities": {},
                        "clientInfo": {"name": "probe", "version": "1"},
                    },
                },
                separators=(",", ":"),
            )
            initialize += "\n"
            ready = '{"jsonrpc":"2.0","method":"notifications/initialized"}\n'
            route_context: tempfile.TemporaryDirectory[str] | None = None
            if version == _LEGACY_ALPHA2_VERSION:
                route_context = tempfile.TemporaryDirectory(prefix="libtmux-vcpkg-mcp-")
                command = [str(tool), str(pathlib.Path(route_context.name) / "socket")]
            else:
                namespace = f"libtmux-vcpkg-probe-{secrets.token_hex(8)}"
                command = [str(tool), "--socket-name", namespace]
            try:
                done, list_id, problem = _run_protocol(
                    command,
                    initialize,
                    ready,
                    version=version,
                )
            finally:
                if route_context is not None:
                    route_context.cleanup()
            if problem is not None:
                problems.append(f"{tool}: could not complete protocol probe: {problem}")
            elif done is not None and done.returncode != 0:
                problems.append(
                    f"{tool}: exited {done.returncode}: {done.stderr.strip()}",
                )
            elif done is not None and list_id is not None:
                names, problem = _catalog_problem(
                    done.stdout,
                    windows=windows,
                    version=version,
                    list_id=list_id,
                )
                if problem is not None:
                    problems.append(f"{tool}: {problem}")
                else:
                    print(f"the installed server advertises: {', '.join(names)}")

    return problems


def run(
    root: pathlib.Path,
    vcpkg_root: pathlib.Path,
    *,
    triplet: str | None = None,
    features: list[str] | None = None,
    keep: pathlib.Path | None = None,
    repository: str | None = None,
) -> int:
    """Resolve the port from this repository as a registry, and build on it.

    ``repository`` overrides what the consumer is pointed at. It defaults to
    this checkout, which is what a gate wants: it tests the tree under review.
    Naming the public URL instead tests something the local path cannot -- that
    the baseline commit was actually pushed, and that vcpkg can fetch it over
    HTTPS from a machine that has never seen this working copy.
    """
    features = features or []
    windows = _host_is_windows()
    _vcpkg, problem = _platform_executable(
        "no vcpkg binary; bootstrap it first",
        [vcpkg_root],
        "vcpkg",
        windows=windows,
    )
    if problem is not None:
        print(problem, file=sys.stderr)
        return 2

    consumer = root / "examples" / "consume"
    if not (consumer / "CMakeLists.txt").is_file():
        print(f"{consumer}: missing, and it is the fixture", file=sys.stderr)
        return 2

    work: pathlib.Path | None = None
    if keep is not None:
        work = keep.resolve()
        protected = (root.resolve(), vcpkg_root.resolve())
        overlap = next(
            (candidate for candidate in protected if _paths_overlap(work, candidate)),
            None,
        )
        if overlap is not None:
            print(
                f"error: --keep {work} overlaps protected repository {overlap}",
                file=sys.stderr,
            )
            return 2
        if work.exists():
            print(f"error: --keep {work} already exists", file=sys.stderr)
            return 2

    if check_registry(root) != 0:
        print("error: refusing to probe an inconsistent registry", file=sys.stderr)
        return 2
    if git("status", "--porcelain", "--", "ports", "versions", repo=root):
        print(
            "error: refusing to probe uncommitted ports or versions",
            file=sys.stderr,
        )
        return 2

    declared = port_version(root / "ports" / _PORT_NAME)
    if declared is None:
        print(f"ports/{_PORT_NAME}: declares no version", file=sys.stderr)
        return 2
    _, version, _ = declared

    baseline = git("rev-parse", "HEAD", repo=root)
    builtin = git("rev-parse", "HEAD", repo=vcpkg_root)
    if not baseline or not builtin:
        print("both repositories must have a HEAD commit", file=sys.stderr)
        return 2

    context = None
    if keep is None:
        context = tempfile.TemporaryDirectory(prefix="libtmux-vcpkg-probe-")
        work = pathlib.Path(context.name)
    else:
        assert work is not None
        try:
            work.mkdir(parents=True, exist_ok=False)
        except FileExistsError:
            print(f"error: --keep {work} already exists", file=sys.stderr)
            return 2
    assert work is not None

    try:
        (work / "vcpkg.json").write_text(
            _manifest(_PORT_NAME, version, features, builtin) + "\n",
        )
        origin = repository or str(root)
        (work / "vcpkg-configuration.json").write_text(
            _configuration(_PORT_NAME, origin, baseline) + "\n",
        )
        print(f"registry {origin} @ {baseline[:12]} -> {_PORT_NAME} {version}")

        build = work / "build"
        configure = [
            "cmake",
            "-S",
            str(consumer),
            "-B",
            str(build),
            f"-DCMAKE_TOOLCHAIN_FILE={vcpkg_root / 'scripts/buildsystems/vcpkg.cmake'}",
            f"-DVCPKG_MANIFEST_DIR={work}",
            f"-DCMAKE_BUILD_TYPE={_BUILD_CONFIGURATION}",
        ]
        if not windows and shutil.which("ninja"):
            configure += ["-G", "Ninja"]
        if triplet:
            configure.append(f"-DVCPKG_TARGET_TRIPLET={triplet}")
        if _run(configure) != 0:
            print("error: the consumer did not configure", file=sys.stderr)
            if repository is not None:
                print(
                    f"note: {origin} must already carry commit {baseline}. "
                    f"An unpushed baseline fails exactly here.",
                    file=sys.stderr,
                )
            return 1

        if (
            _run(
                [
                    "cmake",
                    "--build",
                    str(build),
                    "--config",
                    _BUILD_CONFIGURATION,
                ],
            )
            != 0
        ):
            print("error: the consumer did not build", file=sys.stderr)
            return 1

        executable, problem = _platform_executable(
            "the consumer executable",
            [build, build / _BUILD_CONFIGURATION],
            "consume",
            windows=windows,
        )
        if problem is not None:
            print(f"error: {problem}", file=sys.stderr)
            return 1
        if (
            executable is None
            or _run(
                [str(executable)],
                expected_stdout=f"libtmux {version} consumed\n",
            )
            != 0
        ):
            print("error: the consumer built but did not run", file=sys.stderr)
            return 1

        installed, problem = _installed_root(build, triplet)
        if problem is not None:
            print(f"error: {problem}", file=sys.stderr)
            return 1

        if installed is None:
            print("error: nothing was installed", file=sys.stderr)
            return 1
        problems = _check_installed(
            installed,
            features,
            windows=windows,
            version=version,
        )
        if problems:
            for problem in problems:
                print(f"error: {problem}", file=sys.stderr)
            return 1
    finally:
        if context is not None:
            context.cleanup()

    asked = f"{_PORT_NAME}[{','.join(features)}]" if features else _PORT_NAME
    print(f"resolved and consumed {asked} {version} from the registry")
    return 0
