"""Run whole API programs and verify output, operation errors, and owned cleanup."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "examples/api/api-examples.json"


def digest(path: Path) -> str:
    """Identify the exact file supplied to a compiler or run."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_manifest(path: Path = MANIFEST, root: Path = ROOT) -> dict:
    """Reject incomplete files and ambiguous program-to-declaration mappings."""
    manifest = json.loads(path.read_text())
    if manifest.get("schemaVersion") != 1 or not manifest.get("programs"):
        message = "expected schemaVersion 1 and at least one program"
        raise ValueError(message)
    paths = [manifest["projectFile"]]
    names = set()
    for program in manifest["programs"]:
        name = program["id"]
        if not re.fullmatch(r"[a-z][a-z0-9-]*", name) or name in names:
            message = f"invalid or duplicate program: {name}"
            raise ValueError(message)
        names.add(name)
        symbols = program["symbols"]
        if not symbols or len(set(symbols)) != len(symbols):
            message = f"duplicate or empty targets: {name}"
            raise ValueError(message)
        if any(not re.fullmatch(r"libtmux::[A-Za-z_][\w:]*", s) for s in symbols):
            message = f"invalid declaration target: {name}"
            raise ValueError(message)
        for field in ("title", "description", "expectedOutput", "failureCommand"):
            if not isinstance(program[field], str) or not program[field].strip():
                message = f"missing {field}: {name}"
                raise ValueError(message)
        if not program["expectedOutput"].endswith("\n"):
            message = f"expected output must end with a newline: {name}"
            raise ValueError(message)
        paths.append(program["file"])
    if len(set(paths)) != len(paths):
        message = "each program must own a complete source file"
        raise ValueError(message)
    for name in paths:
        file = Path(name)
        if file.is_absolute() or ".." in file.parts or not (root / file).is_file():
            message = f"missing or invalid source file: {name}"
            raise ValueError(message)
        code = (root / file).read_text()
        if not code.endswith("\n"):
            message = f"source file must end with a newline: {name}"
            raise ValueError(message)
        if file.suffix == ".cpp" and "int main()" not in code:
            message = f"source file has no entry point: {name}"
            raise ValueError(message)
        for line in code.splitlines():
            if "//" in line and len(line) > 100:
                message = f"comment exceeds 100 columns: {name}"
                raise ValueError(message)
    return manifest


SHIM = """#!/usr/bin/env python3
import json, os, sys
args = sys.argv[1:]
failure = os.environ.get("API_FAIL_COMMAND", "")
refused = bool(failure) and failure in args
with open(os.environ["API_TMUX_TRACE"], "a") as output:
    entry = {"argv": args, "pid": os.getpid(), "refused": refused}
    output.write(json.dumps(entry) + "\\n")
if refused:
    sys.stderr.write("injected API example operation failure\\n")
    sys.exit(73)
os.execv(os.environ["API_REAL_TMUX"], [os.environ["API_REAL_TMUX"], *args])
"""


def alive(pid: int) -> bool:
    """Check a recorded owned server process after its fixture has exited."""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def run_program(program: dict, binary: Path, tmux: Path, failure: bool) -> dict:
    """Record the native outcome before any emergency cleanup."""
    with tempfile.TemporaryDirectory(prefix="lc-api-", dir="/tmp") as directory:
        root = Path(directory).resolve()
        fixtures = root / "fixtures"
        fixtures.mkdir()
        shim = root / "bin"
        shim.mkdir()
        (shim / "tmux").write_text(SHIM)
        (shim / "tmux").chmod(0o755)
        trace = root / "trace.jsonl"
        env = dict(os.environ)
        env.pop("TMUX", None)
        env.pop("TMUX_PANE", None)
        env.update(
            PATH=f"{shim}{os.pathsep}{env['PATH']}",
            TMPDIR=str(fixtures),
            TMUX_TMPDIR=str(fixtures),
            API_REAL_TMUX=str(tmux),
            API_TMUX_TRACE=str(trace),
            API_FAIL_COMMAND=program["failureCommand"] if failure else "",
        )
        started = time.monotonic()
        process = subprocess.Popen(
            [str(binary)],
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        timed_out = False
        try:
            stdout, stderr = process.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate(timeout=5)
        commands = (
            [json.loads(line) for line in trace.read_text().splitlines()]
            if trace.exists()
            else []
        )
        servers = []
        for command in commands:
            args = command["argv"]
            if "-D" in args and "-S" in args:
                socket = Path(args[args.index("-S") + 1])
                if not socket.is_relative_to(fixtures):
                    message = f"server escaped its private fixture root: {socket}"
                    raise ValueError(message)
                servers.append({"socket": str(socket), "pid": command["pid"]})
        leftovers = sorted(str(path) for path in fixtures.iterdir())
        living = [server for server in servers if alive(server["pid"])]
        if failure:
            expected = (
                process.returncode != 0
                and "injected API example operation failure" in stderr
            )
            expected = expected and any(command["refused"] for command in commands)
        else:
            expected = (
                process.returncode == 0
                and stdout == program["expectedOutput"]
                and not stderr
            )
        passed = (
            expected
            and not timed_out
            and len(servers) == 1
            and not leftovers
            and not living
        )
        result = {
            "id": program["id"],
            "failureInjected": failure,
            "command": [str(binary)],
            "binarySha256": digest(binary),
            "exitCode": process.returncode,
            "stdout": stdout,
            "stderr": stderr,
            "seconds": round(time.monotonic() - started, 3),
            "timedOut": timed_out,
            "fixtureRoot": str(fixtures),
            "servers": servers,
            "commands": commands,
            "leftoverPaths": leftovers,
            "liveServers": living,
            "passed": passed,
        }
        # Failed owned runs stay failures even if emergency cleanup succeeds.
        for server in living:
            subprocess.run(
                [
                    str(tmux),
                    "-N",
                    "-S",
                    server["socket"],
                    "if-shell",
                    "-F",
                    f"#{{==:#{{pid}},{server['pid']}}}",
                    "kill-server",
                    "",
                ],
                env=env,
                capture_output=True,
                timeout=5,
                check=False,
            )
        return result


def main() -> int:
    """Check source contracts, or run compiled programs selected from the manifest."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary-dir", type=Path)
    parser.add_argument("--program")
    parser.add_argument("--tmux", default="tmux")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    manifest = read_manifest()
    programs = [
        p for p in manifest["programs"] if not args.program or p["id"] == args.program
    ]
    if not programs:
        parser.error("the selected program is not in the manifest")
    if not args.binary_dir:
        print(f"Validated {len(programs)} complete API programs.")
        return 0
    resolved = shutil.which(args.tmux)
    if not resolved:
        parser.error(f"tmux executable not found: {args.tmux}")
    tmux = Path(resolved).resolve()
    version = subprocess.check_output([str(tmux), "-V"], text=True).strip()
    runs = []
    files = {manifest["projectFile"], *(p["file"] for p in programs)}
    for program in programs:
        binary = (args.binary_dir / f"api-{program['id']}").resolve()
        if not binary.is_file():
            parser.error(f"compiled program not found: {binary}")
        for failure in (False, True):
            result = run_program(program, binary, tmux, failure)
            runs.append(result)
            print(
                f"{program['id']} {'failure' if failure else 'success'}: "
                f"{'PASS' if result['passed'] else 'FAIL'}"
            )
            if not result["passed"]:
                print(json.dumps(result, indent=2))
    receipt = {
        "sourceRevision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "sourceDirty": bool(
            subprocess.check_output(
                ["git", "status", "--porcelain"], cwd=ROOT, text=True
            ).strip()
        ),
        "manifestSha256": digest(MANIFEST),
        "files": {name: digest(ROOT / name) for name in sorted(files)},
        "tmuxVersion": version,
        "tmuxBinary": str(tmux),
        "tmuxSha256": digest(tmux),
        "runs": runs,
        "passed": all(result["passed"] for result in runs),
    }
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(receipt, indent=2) + "\n")
    return 0 if receipt["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
