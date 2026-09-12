#!/usr/bin/env python3
"""Exercise workspace process ownership, terminal handoff and log failures."""

import argparse
import hashlib
import json
import os
import pathlib
import pty
import select
import shlex
import signal
import subprocess
import sys
import tempfile
import termios
import time
from contextlib import suppress


def terminal_edit(binary, root, env, *, cancelled=False):
    """Keep machine stdout separate while the editor exchanges terminal input."""
    output_read, output_write = os.pipe()
    pid, terminal = pty.fork()
    if pid == 0:
        os.close(output_read)
        os.dup2(output_write, 1)
        os.dup2(output_write, 2)
        os.close(output_write)
        os.chdir(root)
        settings = termios.tcgetattr(0)
        process = subprocess.Popen(
            [binary, "edit", "dev.yaml", "--json"],
            env=dict(env, TERMINAL_CANCEL="1" if cancelled else "0"),
        )
        (root / "cli.pid").write_text(str(process.pid))
        code = process.wait()
        restored = termios.tcgetattr(0)
        queued = os.read(0, 128) if select.select([0], [], [], 0.1)[0] else b""
        (root / "terminal-state.json").write_text(
            json.dumps(
                {
                    "restored": restored == settings,
                    "before_flags": settings[3],
                    "after_flags": restored[3],
                    "queued": queued.decode(),
                }
            )
        )
        assert os.tcgetpgrp(0) == os.getpgrp(), "terminal foreground was not restored"
        os._exit(code)
    os.close(output_write)
    captured = bytearray()
    machine = bytearray()
    sent = False
    status = None
    reaped = False
    boundary = time.monotonic() + 3
    try:
        while time.monotonic() < boundary:
            for fd in select.select([terminal, output_read], [], [], 0.02)[0]:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    chunk = b""
                (captured if fd == terminal else machine).extend(chunk)
            if b"EDITOR_READY" in captured and not sent:
                assert not termios.tcgetattr(terminal)[3] & (
                    termios.ECHO | termios.ICANON
                )
                os.write(
                    terminal,
                    b"queued answer\n"
                    if cancelled
                    else b"terminal answer\nqueued answer\n",
                )
                if cancelled:
                    os.kill(int((root / "cli.pid").read_text()), signal.SIGTERM)
                sent = True
            ended, status = os.waitpid(pid, os.WNOHANG)
            if ended:
                reaped = True
                break
        else:
            message = "editor did not finish its terminal exchange"
            raise AssertionError(message)
        expected = 143 if cancelled else 7
        assert os.waitstatus_to_exitcode(status) == expected, (captured, machine)
        result = json.loads(machine)
        assert result["exit_code"] == expected and result["stdout"] == ""
        assert b"EDITOR_READY" not in machine
        if not cancelled:
            assert (root / "answer").read_text() == "terminal answer"
        state = json.loads((root / "terminal-state.json").read_text())
        assert state["restored"], state
        assert state["queued"] == "queued answer\n", state
        return {"status": "PASS", "exit_code": result["exit_code"], **state}
    finally:
        child_pid = root / "editor.pid"
        if child_pid.exists():
            with suppress(ProcessLookupError):
                os.killpg(int(child_pid.read_text()), signal.SIGKILL)
        if not reaped:
            with suppress(ProcessLookupError):
                os.killpg(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
        os.close(terminal)
        os.close(output_read)


def cancellation(binary, root, env):
    """Cancel a child whose process group ignores SIGTERM and retains a descendant."""
    script = root / "cancel.sh"
    script.write_text("trap '' TERM\nsleep 30 &\necho $$:$! >cancel.pids\nwait\n")
    process = subprocess.Popen(
        [binary, "edit", "dev.yaml", "--json"],
        cwd=root,
        env=dict(env, EDITOR=f"/bin/sh {script}"),
        start_new_session=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    owned = []
    try:
        boundary = time.monotonic() + 1
        while not (root / "cancel.pids").exists():
            assert time.monotonic() < boundary, "editor did not start"
            time.sleep(0.005)
        owned = [
            int(value)
            for value in (root / "cancel.pids").read_text().strip().split(":")
        ]
        process.send_signal(signal.SIGTERM)
        output, error = process.communicate(timeout=1)
        assert process.returncode == 143, (output, error)
        assert json.loads(output)["exit_code"] == 143
        assert not error
        for child in owned:
            state = subprocess.run(
                ["ps", "-p", str(child), "-o", "stat="],
                capture_output=True,
                text=True,
                check=False,
                timeout=1,
            ).stdout.strip()
            assert not state or state.startswith("Z"), (child, state)
        return {"status": "PASS", "exit_code": process.returncode}
    finally:
        for child in owned:
            with suppress(ProcessLookupError):
                os.kill(child, signal.SIGKILL)
        if process.poll() is None:
            process.kill()
            process.wait(timeout=1)


def output_limit(binary, root, env):
    """Stop an editor that keeps writing after the retained capture limit."""
    script = root / "overflow.sh"
    script.write_text("echo $$ >overflow.pid\nexec yes\n")
    process = subprocess.Popen(
        [binary, "edit", "dev.yaml", "--json"],
        cwd=root,
        env=dict(env, EDITOR=f"/bin/sh {script}"),
        start_new_session=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        output, error = process.communicate(timeout=1)
        assert process.returncode == 1 and not output, (output, error)
        assert json.loads(error)["code"] == "OUTPUT_LIMIT", error
        return {"status": "PASS", "exit_code": process.returncode}
    finally:
        owned = root / "overflow.pid"
        if owned.exists():
            with suppress(ProcessLookupError):
                os.killpg(int(owned.read_text()), signal.SIGKILL)
        if process.poll() is None:
            process.kill()
            process.wait(timeout=1)


def terminal_load(binary, root, env, mode="detach", *, logging=False):
    """Publish loaded results before attaching, then retain the loaded session."""
    root.mkdir()
    socket = str(root / "tmux.sock")
    command = ["tmux", "-S", socket]
    env = dict(env, TMUX="", TMUX_PANE="", TERM="xterm-256color")
    subprocess.run(
        [*command, "-f", "/dev/null", "new-session", "-d", "-s", "keeper"],
        env=env,
        check=True,
        timeout=2,
    )
    (root / "load.yaml").write_text("session_name: loaded\nwindows: [{}]\n")
    arguments = [binary, "load", "-S", socket, str(root / "load.yaml")]
    if logging:
        (root / "load.log").write_bytes(b"x" * 2047 + b"\n")
        arguments[1:1] = ["--log-level", "info"]
        arguments.extend(["--log-file", str(root / "load.log")])
    output_read, output_write = os.pipe()
    pid, terminal = pty.fork()
    if pid == 0:
        os.close(output_read)
        os.dup2(output_write, 1)
        os.dup2(output_write, 2)
        os.close(output_write)
        settings = termios.tcgetattr(0)
        closed = None
        if mode == "closed":
            reader, closed = os.pipe()
            os.close(reader)
        process = subprocess.Popen(
            with_log_limit(arguments) if logging else arguments,
            env=env,
            stdin=subprocess.DEVNULL if mode == "unavailable" else None,
            stdout=closed,
        )
        if closed is not None:
            os.close(closed)
        (root / "cli.pid").write_text(str(process.pid))
        code = process.wait()
        (root / "restored.json").write_text(
            json.dumps(
                {
                    "settings": termios.tcgetattr(0) == settings,
                    "foreground": os.tcgetpgrp(0) == os.getpgrp(),
                }
            )
        )
        os._exit(code)
    os.close(output_write)
    output = bytearray()
    terminal_output = bytearray()
    attached = False
    reaped = False
    try:
        boundary = time.monotonic() + 3
        while time.monotonic() < boundary:
            for fd in select.select([terminal, output_read], [], [], 0.01)[0]:
                with suppress(OSError):
                    chunk = os.read(fd, 65536)
                    if fd == output_read:
                        output.extend(chunk)
                    else:
                        terminal_output.extend(chunk)
            if not attached:
                clients = (
                    subprocess.run(
                        [
                            *command,
                            "list-clients",
                            "-F",
                            "#{client_name}:#{session_name}",
                        ],
                        capture_output=True,
                        env=env,
                        check=True,
                        timeout=1,
                    )
                    .stdout.decode()
                    .splitlines()
                )
                if clients:
                    assert mode not in {"closed", "unavailable"}, clients
                    assert len(clients) == 1 and clients[0].endswith(":loaded"), clients
                    if select.select([output_read], [], [], 0)[0]:
                        output.extend(os.read(output_read, 65536))
                    assert b"created loaded $" in output, output
                    attached = True
                    if mode == "cancel":
                        os.kill(int((root / "cli.pid").read_text()), signal.SIGTERM)
                    else:
                        subprocess.run(
                            [
                                *command,
                                "detach-client",
                                "-t",
                                clients[0].rsplit(":", 1)[0],
                            ],
                            env=env,
                            check=True,
                            timeout=1,
                        )
            ended, status = os.waitpid(pid, os.WNOHANG)
            if ended:
                reaped = True
                break
        else:
            message = "load did not finish its terminal handoff"
            raise AssertionError(message)
        drain_until = time.monotonic() + 0.2
        while select.select([output_read], [], [], 0)[0]:
            assert time.monotonic() < drain_until, "load output did not finish draining"
            chunk = os.read(output_read, 65536)
            if not chunk:
                break
            output.extend(chunk)
        code = os.waitstatus_to_exitcode(status)
        expected = {"detach": 0, "cancel": 143, "unavailable": 2, "closed": 1}[mode]
        assert attached == (mode in {"detach", "cancel"}) and code == expected, (
            code,
            output,
            terminal_output,
        )
        state = json.loads((root / "restored.json").read_text())
        assert all(state.values()), state
        retained = subprocess.run(
            [*command, "has-session", "-t", "=loaded:"],
            env=env,
            capture_output=True,
            check=False,
            timeout=1,
        )
        assert retained.returncode == (1 if mode == "unavailable" else 0)
        if mode == "closed":
            assert b"Retained state:" in output, output
        if logging:
            assert output.count(b"log file disabled") == 1, output
            assert (root / "load.log").stat().st_size == 2048
        return {"status": "PASS", "exit_code": code, **state}
    finally:
        if not reaped:
            marker = root / "cli.pid"
            if marker.exists():
                with suppress(ProcessLookupError):
                    os.kill(int(marker.read_text()), signal.SIGTERM)
            with suppress(ProcessLookupError):
                os.killpg(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
        os.close(terminal)
        os.close(output_read)
        subprocess.run(
            [*command, "kill-server"],
            env=env,
            check=False,
            capture_output=True,
            timeout=1,
        )


def terminal_switch(binary, root, env, mode):
    """Select a unique pane's human client and recheck it after before_script."""
    root.mkdir()
    command = ["tmux", "-S", str(root / "tmux.sock")]
    env = dict(env, TMUX="", TMUX_PANE="", TERM="xterm-256color")

    def query(*arguments):
        return subprocess.run(
            [*command, *arguments],
            env=env,
            capture_output=True,
            text=True,
            check=True,
            timeout=1,
        ).stdout.strip()

    independent = mode in {
        "independent",
        "independent-detached",
        "independent-append",
        "independent-linked",
    }
    focus_case = independent or mode in {
        "gained-independent",
        "independent-other-window",
    }
    before = "touch ran"
    if mode == "gained-independent":
        before += "; " + shlex.join([*command, "refresh-client"])
        before += ' -t "$(cat client-name)" -f active-pane'
    if mode in {"changed", "replaced"}:
        before += "; " + shlex.join([*command, "detach-client", "-s", "origin"])
    if mode == "replaced":
        before += "; while ! test -f restarted; do sleep .01; done"
    (root / "first.json").write_text(
        json.dumps(
            {
                "session_name": "loaded",
                "before_script": shlex.join(["/bin/sh", "-c", before]),
                "windows": [{}],
            }
        )
    )
    (root / "last.yaml").write_text("session_name: destination\nwindows: [{}]\n")
    runner = root / "run.sh"
    arguments = [binary, "load", "first.json", "last.yaml", "-S", command[2]]
    if mode == "independent-detached":
        arguments.append("-d")
    if mode == "independent-append":
        arguments.append("--append")
    foreign = ["tmux", "-S", str(root / "foreign.sock")]
    if mode == "foreign":
        arguments[-1] = foreign[2]
    if mode == "stale":
        arguments = ["env", "TMUX=" + command[2] + ",0,0", *arguments]
    runner.write_text(
        "cd " + shlex.quote(str(root)) + "\n"
        "while ! test -f start; do sleep .01; done\n"
        + shlex.join(arguments)
        + " >load.out 2>load.err\nprintf '%s' $? >exit.tmp\n"
        "mv exit.tmp exit\nexec sleep 30\n"
    )
    subprocess.run(
        [
            *command,
            "-f",
            "/dev/null",
            "new-session",
            "-d",
            "-s",
            "origin",
            shlex.join(["/bin/sh", str(runner)]),
        ],
        env=env,
        check=True,
        timeout=2,
    )
    attached = []
    control = None
    try:
        if mode == "foreign":
            subprocess.run(
                [*foreign, "-f", "/dev/null", "new-session", "-d", "-s", "foreign"],
                env=env,
                check=True,
                timeout=2,
            )
        subprocess.run(
            [*command, "new-session", "-d", "-s", "destination"],
            env=env,
            check=True,
            timeout=1,
        )
        if focus_case:
            first_pane = query("display-message", "-p", "-t", "=origin:", "#{pane_id}")
            first_window = query(
                "display-message", "-p", "-t", first_pane, "#{window_id}"
            )
        if independent:
            second_pane = query(
                "split-window",
                "-d",
                "-t",
                first_pane,
                "-P",
                "-F",
                "#{pane_id}",
                "/bin/sh",
            )
        if mode in {"independent-linked", "independent-other-window"}:
            query("new-session", "-d", "-s", "other", "/bin/sh")
        if mode == "independent-linked":
            query("link-window", "-s", "=origin:", "-t", "=other:1")
            query("select-window", "-t", "=other:1")
        count = 2 if mode in {"ambiguous", "independent-other-window"} else 1
        if mode == "control":
            control = subprocess.Popen(
                [*command, "-C", "attach-session", "-t", "origin"],
                env=env,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        else:
            for index in range(count):
                target = (
                    "other"
                    if mode == "independent-linked"
                    or (mode == "independent-other-window" and index == 1)
                    else "origin"
                )
                attach = [*command, "attach-session", "-t", target]
                if independent or (mode == "independent-other-window" and index == 1):
                    attach += ["-f", "active-pane"]
                child, descriptor = pty.fork()
                if child == 0:
                    if mode == "replaced":
                        subprocess.run(
                            [*command, "attach-session", "-t", "origin"],
                            env=env,
                            check=False,
                        )
                        replacement = subprocess.Popen(
                            [*command, "attach-session", "-t", "origin"], env=env
                        )
                        (root / "replacement.pid").write_text(str(replacement.pid))
                        os._exit(replacement.wait())
                    os.execvpe("tmux", attach, env)
                attached.append((child, descriptor))
        boundary = time.monotonic() + 3
        while True:
            clients = (
                subprocess.run(
                    [*command, "list-clients", "-F", "#{session_name}"],
                    env=env,
                    capture_output=True,
                    check=True,
                    timeout=1,
                )
                .stdout.decode()
                .splitlines()
            )
            if len(clients) == count:
                break
            assert time.monotonic() < boundary, "clients did not attach"
            time.sleep(0.005)
        original = (
            subprocess.run(
                [*command, "list-clients", "-F", "#{client_name}|#{client_pid}"],
                env=env,
                capture_output=True,
                check=True,
                timeout=1,
            )
            .stdout.decode()
            .strip()
        )
        if mode == "gained-independent":
            (root / "client-name").write_text(original.split("|", 1)[0])
        focus = {}
        if independent:
            marker = root / "input-pane"
            keyboard = "printf '%s' \"$TMUX_PANE\" > " + shlex.quote(str(marker)) + "\n"
            os.write(attached[0][1], b"\x02o" + keyboard.encode())
            while not marker.exists() or marker.read_text() != second_pane:
                if select.select([attached[0][1]], [], [], 0.01)[0]:
                    with suppress(OSError):
                        os.read(attached[0][1], 65536)
                assert time.monotonic() < boundary, "independent input marker absent"
            projected = query(
                "list-clients", "-F", "#{pane_id}|#{window_id}|#{client_flags}"
            ).split("|")
            focus = {
                "input_pane": marker.read_text(),
                "projected_pane": projected[0],
                "window": projected[1],
                "flags": projected[2],
            }
            assert projected[0] == first_pane and projected[1] == first_window, focus
            assert "active-pane" in projected[2].split(","), focus
        elif mode == "independent-other-window":
            rows = [
                row.split("|")
                for row in query(
                    "list-clients", "-F", "#{window_id}|#{client_flags}"
                ).splitlines()
            ]
            assert len(rows) == 2 and any(
                window != first_window and "active-pane" in flags.split(",")
                for window, flags in rows
            ), rows
        if focus_case:
            initial_sessions = set(
                query("list-sessions", "-F", "#{session_name}").splitlines()
            )
            initial_windows = set(
                query(
                    "list-windows", "-a", "-F", "#{session_id}|#{window_id}"
                ).splitlines()
            )
        (root / "start").touch()
        while not (root / "exit").exists():
            for descriptor in select.select([fd for _, fd in attached], [], [], 0.01)[
                0
            ]:
                with suppress(OSError):
                    os.read(descriptor, 65536)
            if mode == "replaced" and (root / "replacement.pid").exists():
                current = (
                    subprocess.run(
                        [
                            *command,
                            "list-clients",
                            "-F",
                            "#{client_name}|#{client_pid}",
                        ],
                        env=env,
                        capture_output=True,
                        check=True,
                        timeout=1,
                    )
                    .stdout.decode()
                    .strip()
                )
                if current.endswith("|" + (root / "replacement.pid").read_text()):
                    assert current.split("|")[0] == original.split("|")[0]
                    assert current != original
                    (root / "restarted").touch()
            assert time.monotonic() < boundary, "load did not finish its switch"
        code = int((root / "exit").read_text())
        output, error = (root / "load.out").read_text(), (root / "load.err").read_text()
        if focus_case:
            sessions = set(query("list-sessions", "-F", "#{session_name}").splitlines())
            windows = set(
                query(
                    "list-windows", "-a", "-F", "#{session_id}|#{window_id}"
                ).splitlines()
            )
            client_rows = query(
                "list-clients", "-F", "#{session_name}|#{client_flags}"
            ).splitlines()
            ran = (root / "ran").exists()
            observed = {
                "exit_code": code,
                "stdout": output,
                "stderr": error,
                "sessions": sorted(sessions),
                "windows": sorted(windows),
                "clients": client_rows,
                "script_ran": ran,
                "focus": focus,
            }
            refused = mode in {"independent", "independent-linked"}
            failed = refused or mode == "gained-independent"
            assert code == (2 if failed else 0), observed
            assert ran != refused, observed
            assert sessions == (
                initial_sessions
                if refused or mode == "independent-append"
                else initial_sessions | {"loaded"}
            ), observed
            assert len(windows) == len(initial_windows) + (
                0 if refused else 2 if mode == "independent-append" else 1
            ), observed
            expected_clients = (
                ["other"]
                if mode == "independent-linked"
                else ["destination", "other"]
                if mode == "independent-other-window"
                else ["origin"]
            )
            assert (
                sorted(row.split("|")[0] for row in client_rows) == expected_clients
            ), observed
            if failed:
                assert "active-pane" in error and "-d" in error, observed
                if refused:
                    assert not output and windows == initial_windows, observed
                else:
                    assert (
                        "Retained state:" in error and "created loaded $" in output
                    ), observed
            if mode == "gained-independent":
                assert any(
                    "active-pane" in row.split("|")[1].split(",") for row in client_rows
                ), observed
            return {"status": "PASS", "mode": mode, **observed}
        refused = mode in {"ambiguous", "control", "foreign", "stale"}
        expected = 2 if mode in {"ambiguous", "control", "changed"} else 0
        if mode in {"foreign", "stale", "replaced"}:
            expected = 1
        assert code == expected, (code, output, error)
        sessions = (
            subprocess.run(
                [*command, "list-sessions", "-F", "#{session_name}"],
                env=env,
                capture_output=True,
                check=True,
                timeout=1,
            )
            .stdout.decode()
            .splitlines()
        )
        assert ("loaded" in sessions) != refused, (sessions, error)
        assert (root / "ran").exists() != refused
        if refused:
            assert not output and error, (output, error)
        else:
            assert "created loaded $" in output and "reused destination $" in output
            clients = (
                subprocess.run(
                    [*command, "list-clients", "-F", "#{session_name}"],
                    env=env,
                    capture_output=True,
                    check=True,
                    timeout=1,
                )
                .stdout.decode()
                .splitlines()
            )
            expected_clients = {"changed": [], "replaced": ["origin"]}.get(
                mode, ["destination"]
            )
            assert clients == expected_clients, clients
            if mode in {"changed", "replaced"}:
                assert "Retained state:" in error, error
        if mode == "foreign":
            sessions = (
                subprocess.run(
                    [*foreign, "list-sessions", "-F", "#{session_name}"],
                    env=env,
                    capture_output=True,
                    check=True,
                    timeout=1,
                )
                .stdout.decode()
                .splitlines()
            )
            assert sessions == ["foreign"], sessions
        return {"status": "PASS", "exit_code": code, "mode": mode}
    finally:
        subprocess.run(
            [*command, "kill-server"],
            env=env,
            capture_output=True,
            check=False,
            timeout=1,
        )
        if control is not None:
            control.communicate(timeout=1)
        if mode == "foreign":
            subprocess.run(
                [*foreign, "kill-server"],
                env=env,
                capture_output=True,
                check=False,
                timeout=1,
            )
        for child, descriptor in attached:
            with suppress(ProcessLookupError):
                os.kill(child, signal.SIGKILL)
            os.waitpid(child, 0)
            os.close(descriptor)


def with_log_limit(arguments):
    """Set EFBIG behavior inside the owned process, then exec the actual CLI."""
    return [
        sys.executable,
        "-c",
        (
            "import os, resource, signal, sys\n"
            "signal.signal(signal.SIGXFSZ, signal.SIG_IGN)\n"
            "resource.setrlimit(resource.RLIMIT_FSIZE, (2048, 2048))\n"
            "os.execv(sys.argv[1], sys.argv[1:])\n"
        ),
        *arguments,
    ]


def failed_log_file(binary, root, env, mode):
    """Keep process completion and workspace ownership after a real file error."""
    root.mkdir()
    command = ["tmux", "-S", str(root / "tmux.sock")]
    env = dict(env, TMUX="", TMUX_PANE="")
    subprocess.run(
        [*command, "-f", "/dev/null", "new-session", "-d", "-s", "keeper"],
        env=env,
        check=True,
        timeout=2,
    )
    borrowed = mode == "borrowed"
    failed = mode in {"failed", "failed-stderr", "failed-stdout", "borrowed", "cancel"}
    (root / "first.yaml").write_text("session_name: first\nwindows: [{}]\n")
    (root / "script.sh").write_text(
        "echo $$ >script.pid\n"
        "head -c 8192 /dev/zero | tr '\\000' x\n"
        "printf captured-err >&2\n"
        + ("exec sleep 30\n" if mode == "cancel" else "printf done >finished\n")
        + ("kill -TERM $$\n" if failed and mode != "cancel" else "")
    )
    (root / "second.json").write_text(
        json.dumps(
            {
                "session_name": "second",
                "before_script": "/bin/sh script.sh",
                "windows": [{}],
            }
        )
    )
    if borrowed:
        values = (
            subprocess.run(
                [
                    *command,
                    "display-message",
                    "-p",
                    "-t",
                    "=keeper:",
                    "#{pid},#{pane_id}",
                ],
                env=env,
                capture_output=True,
                text=True,
                check=True,
                timeout=1,
            )
            .stdout.strip()
            .split(",")
        )
        env.update(TMUX=f"{command[2]},{values[0]},0", TMUX_PANE=values[1])
    closed = None
    if mode in {"stderr", "failed-stderr", "stdout", "failed-stdout"}:
        reader, closed = os.pipe()
        os.close(reader)
    process = None
    try:
        process = subprocess.Popen(
            with_log_limit(
                [
                    binary,
                    "--log-level",
                    "debug",
                    "load",
                    "first.yaml",
                    "second.json",
                    "--append" if borrowed else "-d",
                    "-S",
                    command[2],
                    "--json",
                    "--log-file",
                    "operation.log",
                ]
            ),
            cwd=root,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=closed if mode.endswith("stdout") else subprocess.PIPE,
            stderr=closed if mode.endswith("stderr") else subprocess.PIPE,
            start_new_session=True,
        )
        if mode == "cancel":
            deadline = time.monotonic() + 2
            while (
                not (root / "operation.log").exists()
                or (root / "operation.log").stat().st_size < 2048
            ):
                assert time.monotonic() < deadline, "log write did not reach its limit"
                time.sleep(0.005)
            process.send_signal(signal.SIGTERM)
        output, diagnostic = process.communicate(timeout=3)
        expected = 143 if failed else 1 if mode == "stdout" else 0
        assert process.returncode == expected, (process.returncode, output, diagnostic)
        if mode != "cancel":
            assert (root / "finished").read_text() == "done"
        child = int((root / "script.pid").read_text())
        try:
            os.kill(child, 0)
        except ProcessLookupError:
            pass
        else:
            message = "owned script remains alive"
            raise AssertionError(message)
        sessions = subprocess.run(
            [*command, "list-sessions", "-F", "#{session_name}"],
            env=env,
            capture_output=True,
            text=True,
            check=True,
            timeout=1,
        ).stdout.splitlines()
        assert set(sessions) == (
            {"keeper"}
            if borrowed
            else {"keeper", "first"}
            if failed
            else {"keeper", "first", "second"}
        ), sessions
        windows = subprocess.run(
            [*command, "list-windows", "-a", "-F", "#{window_id}"],
            env=env,
            capture_output=True,
            text=True,
            check=True,
            timeout=1,
        ).stdout.splitlines()
        assert len(windows) == (2 if borrowed or failed else 3), windows
        if output:
            result = json.loads(output)
            assert result["exit_code"] == (143 if failed else 0)
            assert len(result["results"]) == (1 if failed else 2)
            script = result["errors" if failed else "results"][-1]["script_output"]
            if mode != "cancel":
                assert script["stdout"] == "x" * 8192
                assert script["stderr"] == "captured-err"
        if diagnostic is not None:
            records = [json.loads(line) for line in diagnostic.splitlines()]
            assert (
                sum(record["code"] == "LOG_FILE_WRITE_FAILED" for record in records)
                == 1
            ), records
            if mode.endswith("stdout"):
                retained = next(
                    record["retained_state"]
                    for record in records
                    if "retained_state" in record
                )
                assert len(retained["results"]) == (1 if failed else 2)
        log = (root / "operation.log").read_bytes()
        assert (
            len(log) == 2048 and json.loads(log.splitlines()[0])["event"] == "started"
        )
        return {
            "status": "PASS",
            "exit_code": process.returncode,
            "log_bytes": len(log),
        }
    finally:
        if closed is not None:
            os.close(closed)
        if process is not None and process.poll() is None:
            marker = root / "script.pid"
            if marker.exists():
                with suppress(ProcessLookupError):
                    os.killpg(int(marker.read_text()), signal.SIGKILL)
            with suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=1)
        subprocess.run(
            [*command, "kill-server"],
            env=env,
            capture_output=True,
            check=False,
            timeout=1,
        )


def main():
    """Verify process behavior, optionally using an owned tmux server for load."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--load", action="store_true")
    parser.add_argument("--logging", action="store_true")
    args = parser.parse_args()
    binary = str(args.binary.resolve(strict=True))
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="cxx-workspace-process-") as temporary:
        root = pathlib.Path(temporary)
        (root / "dev.yaml").write_text("session_name: editor\nwindows: [{}]\n")
        script = root / "editor.sh"
        script.write_text(
            "echo $$ >editor.pid\nstty -echo -icanon min 1 time 0\n"
            "printf EDITOR_READY\n"
            'if test "$TERMINAL_CANCEL" = 1; then sleep 30; fi\n'
            "IFS= read -r answer\nprintf '%s' \"$answer\" >answer\nexit 7\n"
        )
        env = dict(os.environ, EDITOR=f"/bin/sh {script}", VISUAL="")
        report = (
            {}
            if args.logging
            else {
                "terminal_editor": terminal_edit(binary, root, env),
                "terminal_editor_cancellation": terminal_edit(
                    binary, root, env, cancelled=True
                ),
                "owned_child_cancellation": cancellation(binary, root, env),
                "captured_output_limit": output_limit(binary, root, env),
            }
        )
        if args.logging:
            for mode in (
                "success",
                "failed",
                "stderr",
                "failed-stderr",
                "stdout",
                "failed-stdout",
                "borrowed",
                "cancel",
            ):
                report["log_" + mode] = failed_log_file(binary, root / mode, env, mode)
            for mode in ("detach", "cancel"):
                report["log_terminal_" + mode] = terminal_load(
                    binary, root / ("terminal-" + mode), env, mode, logging=True
                )
        if args.load:
            for mode in ("detach", "cancel", "unavailable", "closed"):
                report["terminal_load_" + mode] = terminal_load(
                    binary, root / ("load-" + mode), env, mode
                )
            for mode in (
                "normal",
                "ambiguous",
                "control",
                "changed",
                "replaced",
                "foreign",
                "stale",
                "independent",
                "independent-detached",
                "independent-append",
                "independent-linked",
                "independent-other-window",
                "gained-independent",
            ):
                report["terminal_switch_" + mode] = terminal_switch(
                    binary, root / ("switch-" + mode), env, mode
                )
    report["binary_sha256"] = hashlib.sha256(
        pathlib.Path(binary).read_bytes()
    ).hexdigest()
    report["elapsed_seconds"] = time.monotonic() - started
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
