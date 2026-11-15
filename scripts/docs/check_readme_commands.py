"""Every command a README tells a reader to run is one the program accepts.

A pasted command that fails with `unknown option` is the first thing a new user
sees, and a README is the one place nothing exercises it. This reads the
`console` blocks and `json` client configurations in the READMEs and holds each
command to the program it names:

- `tmux-workspace` arguments are walked through the command tree the binary
  prints for `--command-tree`: subcommands, flags, and whether a flag takes a
  value.
- `libtmux-mcp-server` arguments, and the `LIBTMUX_` variables set beside them,
  are run for real: the server starts under a private `HOME` and
  `TMUX_TMPDIR`, answers `tools/list` and exits when its input ends. That
  covers the same command after `--` in a `claude mcp add` or `codex mcp add`
  line, and a `command`, `args` and `env` entry under `mcpServers`.
- `-DLIBTMUX_*` options and `--preset` names in a `cmake` command are looked up
  in the project's own CMake files and `CMakePresets.json`.

A program without a binary to hand is skipped by name, never silently passed.

    python3 tools/docs/check_readme_commands.py --cmake
    python3 tools/docs/check_readme_commands.py --workspace PATH --mcp PATH
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile
from collections.abc import Iterable, Sequence

WORKSPACE = "tmux-workspace"
SERVER = "libtmux-mcp-server"
CHAIN = frozenset({"&&", "||", ";", "|"})
FENCE = re.compile(
    r"^(?P<marks>```+)(?P<info>[^\n]*)\n(?P<body>.*?)^(?P=marks)[ \t]*$",
    re.MULTILINE | re.DOTALL,
)
OPTION_DEFINITION = re.compile(r"(?:option|set)\(\s*(?P<name>LIBTMUX_[A-Z0-9_]+)")
CMAKE_DEFINE = re.compile(r"^-D(?P<name>[A-Za-z0-9_]+)(?::\w+)?=")
ASSIGNMENT = re.compile(r"^(?P<name>[A-Za-z_][A-Za-z0-9_]*)=(?P<value>.*)$")

HANDSHAKE = (
    {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "protocolVersion": "2025-11-25",
            "capabilities": {},
            "clientInfo": {"name": "readme-commands", "version": "1"},
        },
    },
    {"jsonrpc": "2.0", "method": "notifications/initialized"},
    {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
)


@dataclasses.dataclass(frozen=True)
class Command:
    """One command from a README: where it is, its environment, and its argv."""

    readme: pathlib.Path
    line: int
    environment: dict[str, str]
    arguments: tuple[str, ...]

    @property
    def program(self) -> str:
        """The command's name without the directory it was spelled with."""
        return pathlib.PurePosixPath(self.arguments[0]).name if self.arguments else ""

    def where(self) -> str:
        """Return the location a problem is reported at."""
        return f"{self.readme}:{self.line}"


@dataclasses.dataclass(frozen=True)
class Problem:
    """A documented command the program does not accept."""

    where: str
    message: str

    def __str__(self) -> str:
        """Name the location first, as a compiler does."""
        return f"{self.where}: {self.message}"


def simple_commands(tokens: Sequence[str]) -> list[tuple[dict[str, str], list[str]]]:
    """Split a token list at shell operators, peeling off `NAME=value` prefixes."""
    commands: list[tuple[dict[str, str], list[str]]] = []
    current: list[str] = []
    for token in [*tokens, ";"]:
        if token not in CHAIN:
            current.append(token)
            continue
        environment: dict[str, str] = {}
        while current and (match := ASSIGNMENT.match(current[0])):
            environment[match.group("name")] = match.group("value")
            current = current[1:]
        if current:
            commands.append((environment, current))
        current = []
    return commands


def console_commands(readme: pathlib.Path, text: str) -> list[Command]:
    """Every `$ ` command in the `console` blocks, continuations joined."""
    found: list[Command] = []
    for block in FENCE.finditer(text):
        if block.group("info").strip() != "console":
            continue
        start_line = text.count("\n", 0, block.start("body")) + 1
        lines = block.group("body").split("\n")
        index = 0
        while index < len(lines):
            line = lines[index]
            if not line.startswith("$ "):
                index += 1
                continue
            joined = line[2:]
            first = index
            while joined.rstrip().endswith("\\") and index + 1 < len(lines):
                joined = joined.rstrip()[:-1] + " " + lines[index + 1].strip()
                index += 1
            index += 1
            try:
                tokens = shlex.split(joined)
            except ValueError:
                continue
            for environment, arguments in simple_commands(tokens):
                found.append(
                    Command(readme, start_line + first, environment, tuple(arguments))
                )
    return found


def client_commands(readme: pathlib.Path, text: str) -> list[Command]:
    """Return the server entries of every `mcpServers` object in a `json` block."""
    found: list[Command] = []
    for block in FENCE.finditer(text):
        if block.group("info").strip() != "json":
            continue
        try:
            document = json.loads(block.group("body"))
        except ValueError:
            continue
        servers = document.get("mcpServers") if isinstance(document, dict) else None
        if not isinstance(servers, dict):
            continue
        line = text.count("\n", 0, block.start("body")) + 1
        for entry in servers.values():
            if not isinstance(entry, dict) or "command" not in entry:
                continue
            environment = {
                str(name): str(value) for name, value in entry.get("env", {}).items()
            }
            arguments = (str(entry["command"]), *map(str, entry.get("args", [])))
            found.append(Command(readme, line, environment, arguments))
    return found


def registered_server(command: Command) -> Command | None:
    """Return the server a `claude mcp add` or `codex mcp add` line registers."""
    arguments = command.arguments
    if command.program not in {"claude", "codex"} or arguments[1:3] != ("mcp", "add"):
        return None
    if "--" not in arguments:
        return None
    split = arguments.index("--")
    environment = dict(command.environment)
    flags = arguments[3:split]
    for position, flag in enumerate(flags):
        if flag in {"-e", "--env"}:
            for value in flags[position + 1 :]:
                match = ASSIGNMENT.match(value)
                if value.startswith("-") or match is None:
                    break
                environment[match.group("name")] = match.group("value")
    return dataclasses.replace(
        command, environment=environment, arguments=arguments[split + 1 :]
    )


def find_option(node: dict, name: str) -> dict | None:
    """Return the option of a command-tree node that answers to `name`.

    A nameless child is a group of options that belongs to its parent, such as
    the console choices of `shell`, so its options count as the parent's.
    """
    for option in node.get("options", []):
        if name in option.get("aliases", []) or name == option.get("name"):
            return option
    for group in node.get("children", []):
        if not group["name"] and (option := find_option(group, name)) is not None:
            return option
    return None


def check_workspace(command: Command, tree: dict) -> Iterable[Problem]:
    """Walk `tmux-workspace` arguments through the command tree."""
    node = tree
    path = [WORKSPACE]
    positionals = 0
    arguments = command.arguments[1:]
    index = 0
    while index < len(arguments):
        token = arguments[index]
        index += 1
        if token.startswith("-") and len(token) > 1:
            name, equals, _ = token.partition("=")
            option = find_option(node, name) or find_option(tree, name)
            if option is None:
                yield Problem(
                    command.where(),
                    f"`{' '.join(path)}` has no option {name}",
                )
                continue
            if option.get("maximum", 0) >= 1 and not equals:
                if index >= len(arguments):
                    yield Problem(
                        command.where(), f"{name} needs a value but none follows"
                    )
                index += 1
            continue
        child = next((c for c in node.get("children", []) if c["name"] == token), None)
        if child is not None and positionals == 0:
            node = child
            path.append(token)
            continue
        positionals += 1
        if any(c["name"] for c in node.get("children", [])) and positionals == 1:
            yield Problem(
                command.where(), f"`{' '.join(path)}` has no subcommand {token}"
            )

    slots = [o for o in node.get("options", []) if o.get("positional")]
    capacity = sum(o.get("maximum", 0) for o in slots)
    if positionals > capacity and not any(c["name"] for c in node.get("children", [])):
        yield Problem(command.where(), f"`{' '.join(path)}` takes no such argument")
    needed = sum(o.get("minimum", 0) for o in slots if o.get("required"))
    if positionals < needed:
        yield Problem(command.where(), f"`{' '.join(path)}` needs {needed} argument(s)")


def names_a_socket(command: Command) -> bool:
    """Report whether a server command or its environment selects a socket."""
    arguments = command.arguments[1:]
    selects = any(
        argument in {"--socket", "--socket-path", "--socket-name"}
        for argument in arguments
    ) or any(
        not argument.startswith("-")
        and (index == 0 or not arguments[index - 1].startswith("-"))
        for index, argument in enumerate(arguments)
    )
    return selects or bool(
        {"LIBTMUX_SOCKET", "LIBTMUX_SOCKET_PATH"} & command.environment.keys()
    )


def server_environment(command: Command, home: pathlib.Path) -> dict[str, str]:
    """Return a private environment holding only the variables the README sets.

    A server given no socket leaves the choice to tmux, which would be the
    default socket under the check's `TMUX_TMPDIR`: a server that answers
    differently from one tmux release to the next. Such a command is given a
    private socket path instead, so its arguments are still run as written and
    nothing is started.
    """
    environment = {
        name: value
        for name, value in os.environ.items()
        if not name.startswith(("LIBTMUX_", "TMUX"))
    }
    environment["HOME"] = str(home)
    environment["TMUX_TMPDIR"] = str(home / "tmux")
    (home / "tmux").mkdir(exist_ok=True)
    if not names_a_socket(command):
        environment["LIBTMUX_SOCKET_PATH"] = str(home / "socket")
    environment.update(command.environment)
    return environment


def run_server(command: Command, binary: pathlib.Path) -> Iterable[Problem]:
    """Start the server as documented and require it to list its tools."""
    arguments = command.arguments[1:]
    informational = any(a in {"--help", "-h", "--version"} for a in arguments)
    with tempfile.TemporaryDirectory(prefix="readme-mcp-") as directory:
        home = pathlib.Path(directory)
        requests = "".join(
            json.dumps(request, separators=(",", ":")) + "\n" for request in HANDSHAKE
        )
        try:
            completed = subprocess.run(
                [str(binary), *arguments],
                input="" if informational else requests,
                capture_output=True,
                text=True,
                timeout=30,
                env=server_environment(command, home),
                check=False,
            )
        except subprocess.TimeoutExpired:
            yield Problem(command.where(), f"{' '.join(arguments)} did not exit")
            return
    spelled = " ".join(arguments) or "(no arguments)"
    if completed.returncode != 0:
        yield Problem(
            command.where(),
            f"{SERVER} {spelled} exited {completed.returncode}: "
            f"{completed.stderr.strip()[-200:]}",
        )
        return
    if informational:
        if not completed.stdout.strip():
            yield Problem(command.where(), f"{SERVER} {spelled} printed nothing")
        return
    listed = [
        reply
        for line in completed.stdout.splitlines()
        if (reply := json.loads(line)).get("id") == 2
    ]
    tools = listed[0].get("result", {}).get("tools") if listed else None
    if not tools:
        yield Problem(command.where(), f"{SERVER} {spelled} listed no tools")


def cmake_options(root: pathlib.Path) -> set[str]:
    """Every `LIBTMUX_*` option or cache variable the CMake files define."""
    files = [root / "CMakeLists.txt", *root.rglob("CMakeLists.txt")]
    files.extend((root / "cmake").glob("*.cmake"))
    found: set[str] = set()
    for path in files:
        if "build" in path.relative_to(root).parts[:1]:
            continue
        found.update(
            m.group("name")
            for m in OPTION_DEFINITION.finditer(path.read_text(encoding="utf-8"))
        )
    return found


def preset_names(root: pathlib.Path) -> set[str]:
    """Every configure, build and test preset the project defines."""
    document = json.loads((root / "CMakePresets.json").read_text(encoding="utf-8"))
    return {
        preset["name"]
        for kind in ("configurePresets", "buildPresets", "testPresets")
        for preset in document.get(kind, [])
    }


def check_cmake(
    command: Command, options: set[str], presets: set[str]
) -> Iterable[Problem]:
    """Hold a `cmake` command's project options and preset to the project."""
    arguments = command.arguments[1:]
    for index, argument in enumerate(arguments):
        match = CMAKE_DEFINE.match(argument)
        defined = match.group("name") if match else ""
        if defined.startswith("LIBTMUX_") and defined not in options:
            yield Problem(
                command.where(), f"{defined} is not an option of this project"
            )
        elif (
            argument == "--preset"
            and index + 1 < len(arguments)
            and arguments[index + 1] not in presets
        ):
            yield Problem(
                command.where(),
                f"--preset {arguments[index + 1]} is not in CMakePresets.json",
            )


# Pages that hold what the READMEs moved out, held to the same links and commands.
EXTRA_PAGES = (
    "docs/runtime.md",
    "docs/windows.md",
    "docs/compatibility.md",
    "docs/mcp-server.md",
    "docs/workspace-cli.md",
    "docs/design/commitments.md",
)


def tracked_readmes(root: pathlib.Path) -> list[pathlib.Path]:
    """Every README the repository tracks, or those under it without a checkout."""
    try:
        listing = subprocess.run(
            ["git", "-C", str(root), "ls-files", "*README.md"],
            capture_output=True,
            text=True,
            check=True,
        )
    except (OSError, subprocess.CalledProcessError):
        skipped = {"build", ".git", "_deps"}
        found = sorted(
            path
            for path in root.rglob("README.md")
            if not skipped.intersection(path.relative_to(root).parts)
        )
        return found + [root / name for name in EXTRA_PAGES if (root / name).is_file()]
    pages = [root / name for name in EXTRA_PAGES if (root / name).is_file()]
    return [root / name for name in listing.stdout.split()] + pages


def workspace_tree(binary: pathlib.Path) -> dict:
    """Return the command tree the workspace binary reports about itself."""
    completed = subprocess.run(
        [str(binary), "--command-tree"],
        capture_output=True,
        text=True,
        timeout=30,
        check=True,
    )
    return json.loads(completed.stdout)


def main() -> int:
    """Check every documented command the supplied binaries can vouch for."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("readmes", nargs="*", type=pathlib.Path)
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path())
    parser.add_argument("--workspace", type=pathlib.Path, help="tmux-workspace binary")
    parser.add_argument("--mcp", type=pathlib.Path, help="libtmux-mcp-server binary")
    parser.add_argument(
        "--cmake",
        action="store_true",
        help="hold cmake commands to the project's options and presets",
    )
    arguments = parser.parse_args()

    root = arguments.root.resolve()
    readmes = [p.resolve() for p in arguments.readmes] or tracked_readmes(root)
    tree = workspace_tree(arguments.workspace) if arguments.workspace else None
    options = cmake_options(root) if arguments.cmake else set()
    presets = preset_names(root) if arguments.cmake else set()

    problems: list[Problem] = []
    counts = {"workspace": 0, "server": 0, "cmake": 0}
    skipped: set[str] = set()
    for readme in readmes:
        text = readme.read_text(encoding="utf-8")
        commands = [*console_commands(readme, text), *client_commands(readme, text)]
        for command in commands:
            server = registered_server(command)
            if server is not None:
                command = server
            if command.program == WORKSPACE and tree is not None:
                counts["workspace"] += 1
                problems.extend(check_workspace(command, tree))
            elif command.program == SERVER and arguments.mcp is not None:
                counts["server"] += 1
                problems.extend(run_server(command, arguments.mcp))
            elif command.program == "cmake" and arguments.cmake:
                counts["cmake"] += 1
                problems.extend(check_cmake(command, options, presets))
            elif command.program in {WORKSPACE, SERVER}:
                skipped.add(command.program)

    for problem in problems:
        print(problem, file=sys.stderr)
    for name in sorted(skipped):
        print(f"skipped {name} commands: no binary given", file=sys.stderr)
    if problems:
        return 1
    print(
        f"{counts['workspace']} {WORKSPACE}, {counts['server']} {SERVER} and "
        f"{counts['cmake']} cmake commands match what they run against"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
