#!/usr/bin/env python3
"""Exercise workspace editor processes through a real controlling terminal."""

import argparse
import hashlib
import json
import os
import pathlib
import pty
import select
import signal
import subprocess
import tempfile
import time
from contextlib import suppress


def terminal_edit(binary, root, env):
    """Keep machine stdout separate while the editor exchanges terminal input."""
    output_read, output_write = os.pipe()
    pid, terminal = pty.fork()
    if pid == 0:
        os.close(output_read)
        os.dup2(output_write, 1)
        os.dup2(output_write, 2)
        os.close(output_write)
        os.chdir(root)
        result = subprocess.run(
            [binary, "edit", "dev.yaml", "--json"], env=env, check=False
        )
        assert os.tcgetpgrp(0) == os.getpgrp(), "terminal foreground was not restored"
        os._exit(result.returncode)
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
                os.write(terminal, b"terminal answer\n")
                sent = True
            ended, status = os.waitpid(pid, os.WNOHANG)
            if ended:
                reaped = True
                break
        else:
            message = "editor did not finish its terminal exchange"
            raise AssertionError(message)
        assert os.waitstatus_to_exitcode(status) == 7, (captured, machine)
        result = json.loads(machine)
        assert result["exit_code"] == 7 and result["stdout"] == ""
        assert b"EDITOR_READY" not in machine
        assert (root / "answer").read_text() == "terminal answer"
        return {"status": "PASS", "exit_code": result["exit_code"]}
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


def main():
    """Verify process behavior without opening any tmux server."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    binary = str(args.binary.resolve(strict=True))
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="cxx-workspace-process-") as temporary:
        root = pathlib.Path(temporary)
        (root / "dev.yaml").write_text("session_name: editor\nwindows: [{}]\n")
        script = root / "editor.sh"
        script.write_text(
            "echo $$ >editor.pid\nprintf EDITOR_READY\n"
            "IFS= read -r answer\nprintf '%s' \"$answer\" >answer\nexit 7\n"
        )
        env = dict(os.environ, EDITOR=f"/bin/sh {script}", VISUAL="")
        report = {
            "terminal_editor": terminal_edit(binary, root, env),
            "owned_child_cancellation": cancellation(binary, root, env),
            "captured_output_limit": output_limit(binary, root, env),
        }
    report["binary_sha256"] = hashlib.sha256(
        pathlib.Path(binary).read_bytes()
    ).hexdigest()
    report["elapsed_seconds"] = time.monotonic() - started
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
