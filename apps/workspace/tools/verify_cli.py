#!/usr/bin/env python3
"""Verify an installed workspace CLI on a private tmux socket."""

import argparse
import hashlib
import json
import os
import pathlib
import resource
import select
import shutil
import signal
import site
import statistics
import subprocess
import tempfile
import time


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
