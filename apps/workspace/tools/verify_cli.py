#!/usr/bin/env python3
"""Verify an installed workspace CLI on a private tmux socket."""

import argparse
import hashlib
import json
import os
import pathlib
import resource
import select
import shlex
import shutil
import signal
import site
import statistics
import subprocess
import tempfile
import time
from contextlib import suppress


def run(command, env, cwd):
    """Run one checked command and measure wall time in milliseconds."""
    started = time.perf_counter_ns()
    result = subprocess.run(
        command, env=env, cwd=cwd, capture_output=True, timeout=5, check=False
    )
    elapsed = (time.perf_counter_ns() - started) / 1_000_000
    if result.returncode:
        message = f"command {command[0]} {command[1]} failed: {result.stderr!r}"
        raise RuntimeError(message)
    return result, elapsed


def spread(values):
    """Retain raw repeats and report their spread."""
    ordered = sorted(values)
    return {
        "samples_ms": values,
        "median_ms": statistics.median(values),
        "min_ms": min(values),
        "max_ms": max(values),
        "p95_ms": ordered[min(len(ordered) - 1, int(len(ordered) * 0.95))],
    }


def completions(binary, root, env):
    """Execute generated shell functions with partial command lines."""
    directory = root / "completion"
    directory.mkdir()
    (directory / "project space.yaml").touch()
    results = {}
    for name in ("bash", "zsh", "fish"):
        shell = shutil.which(name, path=env.get("PATH"))
        if shell is None:
            results[name] = "SKIP: shell unavailable"
            continue
        generated, _ = run([str(binary), "--generate-completion", name], env, root)
        script = directory / f"workspace.{name}"
        script.write_bytes(generated.stdout)
        run([shell, "-n", str(script)], env, root)
        results[name] = {"syntax": "PASS"}
        if name == "zsh":
            continue
        cases = [
            (["import", "tm"], ["tmuxinator"]),
            (["load", "--pr"], ["--progress-format", "--progress-lines"]),
            (["freeze", "-fj"], ["-fjson"]),
            (["--color=al"], ["--color=always"]),
            (["load", "-fproject"], ["-fproject space.yaml"]),
            (["freeze", "--save-to=project"], ["--save-to=project space.yaml"]),
        ]
        for words, expected in cases:
            if name == "bash":
                command = [
                    shell,
                    "--noprofile",
                    "--norc",
                    "-c",
                    (
                        'source "$1"; shift; COMP_WORDS=("$@"); '
                        "COMP_CWORD=$((${#COMP_WORDS[@]}-1)); "
                        '_tmux_workspace_complete; printf "%s\\n" "${COMPREPLY[@]}"'
                    ),
                    "completion",
                    str(script),
                    str(binary),
                    *words,
                ]
            else:
                command = [
                    shell,
                    "--no-config",
                    "-c",
                    'source "$argv[1]"; complete -C "$argv[2]"',
                    str(script),
                    "tmux-workspace " + " ".join(words),
                ]
            observed, _ = run(
                command, dict(env, PATH=f"{binary.parent}:{env['PATH']}"), directory
            )
            assert observed.stdout.decode().splitlines() == expected, (
                name,
                words,
                observed,
            )
        results[name]["candidates"] = "PASS"
    return results


def _limit_output_file():
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    resource.setrlimit(resource.RLIMIT_FSIZE, (256, 256))


def load_capture(program, workspace, socket, destination, env, cwd):
    """Measure matching load/capture commands and verify the saved topology."""
    destination.unlink(missing_ok=True)
    started = time.perf_counter_ns()
    run([program, "load", str(workspace), "-d", "-S", socket], env, cwd)
    run(
        [
            program,
            "freeze",
            "bench",
            "-S",
            socket,
            "-f",
            "json",
            "-o",
            str(destination),
            "-y",
            "-q",
        ],
        env,
        cwd,
    )
    elapsed = (time.perf_counter_ns() - started) / 1_000_000
    captured = json.loads(destination.read_text())
    assert captured["session_name"] == "bench"
    assert len(captured["windows"]) == 1
    assert len(captured["windows"][0]["panes"]) == 2
    return elapsed


def before_scripts(binary, root, env, prefix, append_env):
    """Verify live script records and cleanup through the executable boundary."""
    directory = root / "scripts"
    directory.mkdir()
    baseline, _ = run([*prefix, "list-panes", "-a", "-F", "#{pane_id}"], env, root)
    results = {}

    def wait_for(predicate):
        deadline = time.monotonic() + 2
        while not predicate():
            assert time.monotonic() < deadline, "script did not reach its checkpoint"
            time.sleep(0.005)

    for case, append, mode in [
        ("stream", False, "--ndjson"),
        ("interrupt", False, "--json"),
        ("leader-exit", False, "--json"),
        ("terminate", True, "--ndjson"),
        ("limit", False, "--json"),
        ("closed", False, "--ndjson"),
    ]:
        script = directory / f"{case}.sh"
        marker = directory / f"{case}.pids"
        release = directory / f"{case}.release"
        if case == "stream":
            body = (
                "printf '\\377\\351'\nsleep 0.02\n"
                "printf '\\233\\252\\t\\033[31m'\nprintf warning >&2\n"
            )
        elif case in ("interrupt", "terminate"):
            body = (
                'sh -c \'trap "" TERM; printf ready > "$1"; exec sleep 30\' sh "$2" &\n'
                'while ! test -f "$2"; do sleep 0.005; done\n'
                "printf '%s:%s' $$ $! > \"$1\"\n"
                "printf begin\nwait\n"
            )
        elif case == "leader-exit":
            body = "sleep 30 &\nprintf '%s:%s' $$ $! > \"$1\"\nprintf failed\nexit 7\n"
        elif case == "limit":
            body = "printf '%s' $$ > \"$1\"\nexec yes\n"
        else:
            body = "printf '%s' $$ > \"$1\"\n"
        if case in ("stream", "closed"):
            body += (
                'i=0\nwhile ! test -f "$2"; do i=$((i+1)); '
                'test "$i" -lt 200 || exit 9; sleep 0.01; done\n'
            )
            if case == "closed":
                body += "printf closed\nsleep 30\n"
        script.write_text(body)
        if case == "stream":
            script.write_text('printf "%s" $$ > "$1"\n' + body)
        workspace = directory / f"{case}.json"
        workspace.write_text(
            json.dumps(
                {
                    "session_name": "script-process",
                    "before_script": shlex.join(
                        ["/bin/sh", str(script), str(marker), str(release)]
                    ),
                    "windows": [{}],
                }
            )
        )
        process = subprocess.Popen(
            [
                binary,
                "load",
                str(workspace),
                "-S",
                prefix[2],
                mode,
                "--append" if append else "-d",
            ],
            cwd=directory,
            env=append_env if append else env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
        observed = bytearray()
        owned = []
        try:
            if case == "stream":

                def streamed(process=process, observed=observed):
                    ready, _, _ = select.select([process.stdout], [], [], 0.02)
                    if ready:
                        observed.extend(os.read(process.stdout.fileno(), 65536))
                    return any(
                        json.loads(line).get("event") == "script-output"
                        and "雪" in json.loads(line).get("text", "")
                        for line in observed.split(b"\n")[:-1]
                    )

                wait_for(streamed)
                release.touch()
            else:
                wait_for(
                    lambda marker=marker: marker.exists() and bool(marker.read_text())
                )
                owned = [int(value) for value in marker.read_text().split(":")]
                if case in ("interrupt", "terminate"):
                    process.send_signal(
                        signal.SIGINT if case == "interrupt" else signal.SIGTERM
                    )
                elif case == "closed":
                    process.stdout.close()
                    process.stdout = None
                    release.touch()
            output, error = process.communicate(timeout=2)
            observed.extend(output or b"")
            for pid in owned:
                state = subprocess.run(
                    ["ps", "-p", str(pid), "-o", "stat="],
                    capture_output=True,
                    text=True,
                    check=False,
                ).stdout.strip()
                assert not state or state.startswith("Z"), (case, pid, state)
            if case == "closed":
                assert process.returncode == 1, error
                diagnostics = [json.loads(line) for line in error.splitlines()]
                assert diagnostics and all(
                    item["code"] == "OUTPUT_CLOSED" for item in diagnostics
                ), error
                retained = diagnostics[-1]["retained_state"]
                assert retained["status"] == "error" and not retained["results"], error
                assert retained["errors"][0]["failed_stage"] == "before-script", error
            else:
                records = (
                    [json.loads(line) for line in observed.splitlines()]
                    if mode == "--ndjson"
                    else []
                )
                summary = records[-1] if mode == "--ndjson" else json.loads(observed)
                if case == "stream":
                    assert process.returncode == 0 and not error, (observed, error)
                    text = "".join(
                        item.get("text", "")
                        for item in records
                        if item.get("event") == "script-output"
                        and item["stream"] == "stdout"
                    )
                    assert text == "\ufffd雪\t\x1b[31m", text
                    assert (
                        sum(
                            item["event"] in ("completed", "failed") for item in records
                        )
                        == 1
                    )
                    run([*prefix, "kill-session", "-t", "=script-process:"], env, root)
                else:
                    problem = summary["errors"][0]
                    assert summary["status"] == ("partial" if append else "error"), (
                        summary
                    )
                    if case == "limit":
                        assert (
                            process.returncode == 1
                            and problem["code"] == "OUTPUT_LIMIT"
                        ), problem
                        assert problem["script_output"]["truncated"]
                        assert len(problem["script_output"]["stdout"]) <= 1024 * 1024
                    elif case == "leader-exit":
                        assert process.returncode == 1, (summary, error)
                        assert problem["code"] == "BEFORE_SCRIPT_FAILED", problem
                        assert problem["script_output"]["exit_code"] == 7, problem
                        assert problem["script_output"]["stdout"] == "failed", problem
                    else:
                        expected = 130 if case == "interrupt" else 143
                        assert process.returncode == expected, (summary, error)
                        assert problem["script_output"]["stdout"] == "begin", problem
            after, _ = run([*prefix, "list-panes", "-a", "-F", "#{pane_id}"], env, root)
            assert after.stdout == baseline.stdout, (case, after.stdout)
            results[case] = {"status": "PASS", "exit_code": process.returncode}
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=2)
            if not owned and marker.exists():
                owned = [int(value) for value in marker.read_text().split(":") if value]
            for pid in owned:
                with suppress(ProcessLookupError):
                    os.kill(pid, signal.SIGKILL)
            subprocess.run(
                [*prefix, "kill-session", "-t", "=script-process:"],
                env=env,
                cwd=root,
                capture_output=True,
                check=False,
            )
    return results


def main():
    """Verify the installed program and write machine-readable evidence."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--reference", help="Installed tmuxp executable")
    parser.add_argument("--iterations", type=int, default=5)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.iterations < 1:
        parser.error("--iterations must be positive")
    binary = args.binary.resolve(strict=True)
    reference = shutil.which(args.reference) if args.reference else None
    if args.reference and reference is None:
        parser.error("--reference executable was not found")
    binaries = {"cxx": str(binary)}
    if reference:
        binaries["tmuxp"] = reference
    report = {
        "schema_version": 1,
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "iterations": args.iterations,
        "checks": {},
        "timings": {},
    }
    with tempfile.TemporaryDirectory(prefix="cxx-ws-bench-") as temporary:
        root = pathlib.Path(temporary)
        configs = root / "config"
        configs.mkdir()
        env = dict(os.environ)
        for name in ("TMUX", "TMUX_PANE", "NO_COLOR", "FORCE_COLOR"):
            env.pop(name, None)
        env.update(
            HOME=str(root),
            XDG_CONFIG_HOME=str(root / "xdg"),
            TMUXP_CONFIGDIR=str(configs),
            TERM="xterm-256color",
            PYTHONUSERBASE=site.USER_BASE,
        )
        report["checks"]["completion"] = completions(binary, root, env)
        socket = str(root / "tmux.sock")
        prefix = ["tmux", "-S", socket, "-f", "/dev/null"]
        cleanup_prefixes = [prefix]
        cold_sockets = {name: str(root / (name + "-cold.sock")) for name in binaries}
        cleanup_prefixes.extend(
            ["tmux", "-S", value, "-f", "/dev/null"] for value in cold_sockets.values()
        )
        version, _ = run([*prefix, "-V"], env, root)
        report["tmux"] = version.stdout.decode().strip()
        run([*prefix, "new-session", "-d", "-s", "keeper"], env, root)
        try:
            context, _ = run(
                [
                    *prefix,
                    "display-message",
                    "-p",
                    "-t",
                    "keeper",
                    "#{pid},#{session_id},#{pane_id}",
                ],
                env,
                root,
            )
            pid, session, pane = context.stdout.decode().strip().split(",")
            append_env = dict(env, TMUX=f"{socket},{pid},{session[1:]}", TMUX_PANE=pane)
            report["checks"]["before_script"] = before_scripts(
                str(binary), root, env, prefix, append_env
            )
            workspace = configs / "bench.yaml"
            workspace.write_text(
                "session_name: bench\nwindows:\n"
                "  - window_name: editor\n    panes: [null, null]\n"
            )
            for name, program in binaries.items():
                version, _ = run([program, "--version"], env, root)
                report.setdefault("versions", {})[name] = (
                    version.stdout.decode().strip()
                )
                report["timings"][name] = {
                    key: []
                    for key in (
                        "startup",
                        "discovery",
                        "search",
                        "load_capture",
                        "cold_load_capture",
                        "append",
                    )
                }
            for _ in range(args.iterations):
                for name, program in binaries.items():
                    for key, arguments in (
                        ("startup", ["--version"]),
                        ("discovery", ["ls", "--json"]),
                        ("search", ["search", "name:bench", "--json"]),
                    ):
                        result, elapsed = run([program, *arguments], env, root)
                        if key == "discovery":
                            assert len(json.loads(result.stdout)["workspaces"]) == 1
                        if key == "search":
                            assert len(json.loads(result.stdout)) == 1
                        report["timings"][name][key].append(elapsed)
                    saved = root / f"{name}.json"
                    elapsed = load_capture(program, workspace, socket, saved, env, root)
                    report["timings"][name]["load_capture"].append(elapsed)
                    run([*prefix, "kill-session", "-t", "=bench"], env, root)
                    _, elapsed = run(
                        [program, "load", str(workspace), "--append", "-S", socket],
                        append_env,
                        root,
                    )
                    report["timings"][name]["append"].append(elapsed)
                    windows, _ = run(
                        [
                            *prefix,
                            "list-windows",
                            "-t",
                            "keeper",
                            "-F",
                            "#{window_id}:#{window_panes}:#{window_name}",
                        ],
                        env,
                        root,
                    )
                    rows = windows.stdout.decode().splitlines()
                    assert len(rows) == 2, rows
                    appended = [
                        row.split(":") for row in rows if row.endswith(":editor")
                    ]
                    assert len(appended) == 1 and appended[0][1] == "2", rows
                    run([*prefix, "kill-window", "-t", appended[0][0]], env, root)
                    elapsed = load_capture(
                        program, workspace, cold_sockets[name], saved, env, root
                    )
                    report["timings"][name]["cold_load_capture"].append(elapsed)
                    run(["tmux", "-S", cold_sockets[name], "kill-server"], env, root)
            report["checks"]["matched_boundaries_and_capture_topology"] = "PASS"
            report["checks"]["cold_startup_and_capture_topology"] = "PASS"
            report["checks"]["append_preserves_borrowed_session_and_topology"] = "PASS"
            stream_config = configs / "stream.yaml"
            stream_config.write_text(
                "session_name: stream\nwindows:\n  - panes:\n"
                "      - shell_command: [{cmd: '', sleep_after: 0.2}]\n"
            )
            process = subprocess.Popen(
                [
                    str(binary),
                    "load",
                    str(stream_config),
                    "-d",
                    "-S",
                    socket,
                    "--ndjson",
                ],
                env=env,
                cwd=root,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                bufsize=0,
            )
            if not select.select([process.stdout], [], [], 5)[0]:
                process.kill()
                process.communicate()
                message = "NDJSON first event timed out"
                raise RuntimeError(message)
            first = json.loads(process.stdout.readline())
            assert first["event"] == "started" and process.poll() is None
            stdout, stderr = process.communicate(timeout=5)
            assert process.returncode == 0, stderr
            events = [first, *[json.loads(line) for line in stdout.splitlines()]]
            assert [record["sequence"] for record in events] == list(
                range(1, len(events) + 1)
            ), events
            assert (
                sum(record["event"] in ("completed", "failed") for record in events)
                == 1
            )
            report["checks"]["ndjson_observable_before_completion"] = "PASS"
            metadata, _ = run([str(binary), "--command-tree"], env, root)
            tree = json.loads(metadata.stdout)
            assert len(tree["children"]) == 9
            report["checks"]["nine_root_commands"] = "PASS"
            large = root / "large.json"
            large.write_text(json.dumps({"data": "x" * 8192}))
            destination = root / "retained.json"
            destination.write_text("original")
            failed = subprocess.run(
                [
                    str(binary),
                    "convert",
                    str(large),
                    "--save-to",
                    str(destination),
                    "--workspace-format",
                    "json",
                    "--force",
                    "--json",
                ],
                cwd=root,
                env=env,
                capture_output=True,
                check=False,
                timeout=5,
                preexec_fn=_limit_output_file,
            )
            assert failed.returncode == 1 and not failed.stdout
            assert destination.read_text() == "original"
            assert not list(root.glob(".retained.json.*"))
            report["checks"]["failed_write_preserves_destination_and_cleans_temp"] = (
                "PASS"
            )

        finally:
            for cleanup in cleanup_prefixes:
                subprocess.run(
                    [*cleanup, "kill-server"],
                    env=env,
                    cwd=root,
                    capture_output=True,
                    timeout=5,
                    check=False,
                )
    for groups in report["timings"].values():
        for key, values in groups.items():
            groups[key] = spread(values)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            {
                "checks": report["checks"],
                "medians_ms": {
                    name: {key: value["median_ms"] for key, value in groups.items()}
                    for name, groups in report["timings"].items()
                },
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
