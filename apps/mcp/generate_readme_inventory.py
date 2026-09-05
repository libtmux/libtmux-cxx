#!/usr/bin/env python3
"""Generate the README tool inventory from the server's effective registry."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

BEGIN = "<!-- BEGIN GENERATED TOOL INVENTORY -->"
END = "<!-- END GENERATED TOOL INVENTORY -->"
TOOLSETS = ("inspect", "manage", "execute", "teardown")


def capability_document(server: Path) -> dict[str, object]:
    """Read the unfiltered capability resource from one server process."""
    requests = (
        {
            "jsonrpc": "2.0",
            "id": "initialize",
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-11-25",
                "capabilities": {},
                "clientInfo": {"name": "readme-generator", "version": "1"},
            },
        },
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {
            "jsonrpc": "2.0",
            "id": "capabilities",
            "method": "resources/read",
            "params": {"uri": "tmux://capabilities"},
        },
    )
    environment = os.environ.copy()
    for name in (
        "LIBTMUX_SOCKET",
        "LIBTMUX_SOCKET_PATH",
        "LIBTMUX_TMUX_CONFIG",
        "LIBTMUX_TOOLS",
        "LIBTMUX_EXCLUDE_TOOLS",
        "LIBTMUX_SAFETY",
        "TMUX",
    ):
        environment.pop(name, None)
    environment["LIBTMUX_TOOLSETS"] = ",".join(TOOLSETS)
    wire = "".join(
        json.dumps(request, separators=(",", ":")) + "\n" for request in requests
    )
    completed = subprocess.run(
        [str(server), "--socket-name", "libtmux-cxx-readme-no-dispatch"],
        check=True,
        input=wire,
        text=True,
        capture_output=True,
        timeout=15,
        env=environment,
    )
    replies = [json.loads(line) for line in completed.stdout.splitlines()]
    response = next(reply for reply in replies if reply.get("id") == "capabilities")
    return json.loads(response["result"]["contents"][0]["text"])


def inventory(document: dict[str, object]) -> str:
    """Render tool names in registry order, grouped by their declared toolset."""
    grouped = {toolset: [] for toolset in TOOLSETS}
    for tool in document["tools"]:
        grouped[tool["toolset"]].append(tool["name"])
    rows = [
        BEGIN,
        "<!-- Generated from tmux://capabilities; run this script to update. -->",
        "| Toolset | Tools |",
        "|---|---|",
    ]
    rows.extend(
        f"| `{toolset}` | " + ", ".join(f"`{name}`" for name in grouped[toolset]) + " |"
        for toolset in TOOLSETS
    )
    rows.append(END)
    return "\n".join(rows)


def main() -> int:
    """Update the generated block, or fail when check mode detects drift."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--readme", required=True, type=Path)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()

    text = arguments.readme.read_text(encoding="utf-8")
    start = text.index(BEGIN)
    finish = text.index(END, start) + len(END)
    updated = (
        text[:start] + inventory(capability_document(arguments.server)) + text[finish:]
    )
    if arguments.check:
        if updated != text:
            print("MCP README tool inventory is stale", file=sys.stderr)
            return 1
        return 0
    arguments.readme.write_text(updated, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
