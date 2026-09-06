#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["tomlkit==0.15.1"]
# ///
"""Swap MCP server configs across every installed agent CLI.

Use when you want every installed agent CLI to run a particular build of
``libtmux-mcp-server`` — one out of this checkout's build tree, or one
already installed — instead of whatever it is pointed at now.
``use-local`` rewrites each CLI's config to run that binary by absolute
path; ``revert`` restores from the timestamped backup the swap wrote.
Swapping a layer that is already swapped keeps that first backup rather
than taking a new one, so ``revert`` always lands on the pre-swap config.

The server registers as ``libtmux``, and the binary name comes from the
``OUTPUT_NAME`` its CMake target sets, so a rename in the build stays in
one place.

Examples
--------
```console
$ ./tools/mcp/mcp_swap.py detect
```

```console
$ ./tools/mcp/mcp_swap.py status
```

```console
$ ./tools/mcp/mcp_swap.py use-local --dry-run --socket /tmp/libtmux-agent/socket
```

```console
$ ./tools/mcp/mcp_swap.py use-local \
    --build-dir build/cxx-gcc \
    --socket /tmp/libtmux-agent/socket
```

```console
$ ./tools/mcp/mcp_swap.py use-local \
    --source published \
    --socket /tmp/libtmux-agent/socket
```

```console
$ ./tools/mcp/mcp_swap.py revert
```

Scope
-----
This script is best-effort and intentionally narrow:

- **POSIX client configs only.** The Windows psmux setup carries a
  task-owned config and high-entropy ``--socket-name`` through a PowerShell
  wrapper; this script neither models nor rewrites that wrapper.

- **Global configs only.** Writes to ``~/.cursor/mcp.json``,
  ``~/.claude.json``, ``~/.codex/config.toml``,
  ``~/.gemini/settings.json``, ``~/.grok/config.toml`` (TOML
  ``mcp_servers``, same shape as Codex),
  ``~/.gemini/config/mcp_config.json`` (agy / Antigravity CLI, JSON
  ``mcpServers`` — the shared-config file the CLI reads, sibling to the
  ``config.json`` it loads at startup),
  ``$XDG_CONFIG_HOME/opencode/opencode.jsonc`` (JSONC ``mcp``, comments
  preserved) and ``~/.pi/agent/mcp.json`` (JSONC too -- the adapter that
  reads it strips comments). Workspace / project-local
  configs (``$PWD/.cursor/mcp.json``, ``$PWD/.gemini/settings.json``,
  ``$PWD/opencode.json``, per-project ``projects.<abs>.mcpServers``
  entries inside ``~/.claude.json`` *are* recognised for Claude only)
  are NOT walked — workspace files for the others are silently ignored.
  When workspace precedence matters, run the CLI's own
  ``cursor mcp add ...`` / ``gemini mcp add ...`` directly. opencode has
  no non-interactive project-scope add -- ``opencode mcp add`` writes the
  global file -- so edit ``$PWD/opencode.json`` by hand for that.

- **opencode reads three global files.** ``config.json``,
  ``opencode.json`` and ``opencode.jsonc`` in the same directory are all
  loaded and merged, with ``.jsonc`` winning. This script owns
  ``.jsonc`` — the file opencode itself writes to — so its entry is the
  one that takes effect. A stale ``mcp.<name>`` left in a sibling
  ``opencode.json`` still merges underneath rather than being shadowed
  outright; remove it by hand if that matters.

- **pi has no MCP client of its own.** Its README says so, and the
  released build ships no MCP code. ``~/.pi/agent/mcp.json`` is read by
  the third-party ``pi-mcp-adapter`` extension, so a swap written there
  takes effect only once that package is installed. ``detect`` says as
  much rather than reporting a swap that cannot do anything.

- **Claude scope.** ``use-local`` and ``revert`` accept
  ``--scope {user,project}``. The default ``project`` writes the
  per-project entry under ``projects[<abs-repo>].mcpServers`` —
  only the current repo's directory sees the swap, matching
  pre-flag behaviour. ``--scope user`` writes Claude's top-level
  ``mcpServers`` fallback so every project that has no per-project
  override picks up the swap; useful when QA-ing a branch across
  many directories. Every other CLI here has no per-project layer in
  the config file this script writes; the flag is silently coerced to
  ``user`` for them. Both Claude scopes can coexist with
  independent backups; full ``revert`` unwinds in LIFO order.
- **Simple binary detection.** Probing is ``shutil.which(<binary>)``
  plus ``<config_path>.exists()``. Custom install locations
  (Homebrew, npm prefixes, ``~/.npm-global/bin``,
  ``~/.claude/local/claude``, ``~/.gemini/local/gemini``) are picked
  up only if the binary is on ``PATH``. FastMCP's installer probes
  these locations directly; this script does not.
- **Single config shape per CLI.** No fallback paths, no merge of
  multiple sources. If your setup deviates from the defaults above,
  use the CLI's native ``mcp`` subcommand instead.
"""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import difflib
import fcntl
import hashlib
import json
import os
import pathlib
import re
import secrets
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import typing as t

import tomlkit
import tomlkit.items

CLIName = t.Literal[
    "claude", "codex", "cursor", "gemini", "grok", "agy", "opencode", "pi"
]
ALL_CLIS: tuple[CLIName, ...] = (
    "claude",
    "codex",
    "cursor",
    "gemini",
    "grok",
    "agy",
    "opencode",
    "pi",
)

#: Width of the CLI-name column in ``detect`` output, derived rather
#: than hardcoded so adding a longer name cannot silently misalign it.
_CLI_COLUMN = max(len(name) for name in ALL_CLIS) + 1

#: Claude config scope: ``"user"`` targets the user/system-level top-level
#: ``mcpServers`` fallback that applies to every project without its own
#: override; ``"project"`` targets the project-level per-project
#: ``projects.<abs>.mcpServers`` node. Non-Claude CLIs have no
#: per-project scope in their config files, so for those CLIs the scope
#: is always normalised to ``"user"`` regardless of what was passed.
Scope = t.Literal["user", "project"]
ALL_SCOPES: tuple[Scope, ...] = ("user", "project")


def _normalize_scope(cli: CLIName, scope: Scope | None) -> Scope:
    """Coerce ``scope`` to the value that actually applies to ``cli``.

    Non-Claude CLIs have no per-project config layer — every write to
    them is necessarily user-level — so the flag is silently coerced to
    ``"user"`` for those. For Claude, ``None`` defaults to ``"project"``
    to preserve pre-flag behaviour where the script always wrote the
    per-project entry.
    """
    if cli != "claude":
        return "user"
    return scope if scope is not None else "project"


def _state_key(cli: CLIName, scope: Scope) -> str:
    """Compose the ``cli:scope`` key used inside the state file."""
    return f"{cli}:{scope}"


def _parse_state_key(key: str) -> tuple[CLIName, Scope] | None:
    """Decode the canonical internal ``cli:scope`` recovery key."""
    if ":" not in key:
        return None
    cli_str, _, scope_str = key.partition(":")
    if cli_str in ALL_CLIS and scope_str in ALL_SCOPES:
        return cli_str, scope_str
    return None


def _parse_state_entry(v: dict[str, t.Any]) -> SwapEntry | None:
    """Decode one exact internal entry; callers reject ``None`` fail-closed."""
    expected = {
        "config_path",
        "backup_path",
        "server",
        "action",
        "swapped_at",
        "seq_no",
        "target_path",
        "original_sha256",
        "original_mode",
        "expected_sha256",
        "expected_mode",
        "config_binding",
        "backup_binding",
    }
    if set(v) != expected:
        return None
    try:
        seq_no = v["seq_no"]
        if isinstance(seq_no, bool) or not isinstance(seq_no, int) or seq_no < 0:
            return None
        entry = SwapEntry(
            config_path=_required_string(v, "config_path"),
            backup_path=_required_string(v, "backup_path"),
            server=_required_string(v, "server"),
            action=v["action"],
            swapped_at=_required_string(v, "swapped_at"),
            seq_no=seq_no,
            target_path=_required_string(v, "target_path"),
            original_sha256=_required_digest(v, "original_sha256"),
            original_mode=_required_mode(v, "original_mode"),
            expected_sha256=_required_digest(v, "expected_sha256"),
            expected_mode=_required_mode(v, "expected_mode"),
            config_binding=_parse_path_binding(v["config_binding"]),
            backup_binding=_parse_path_binding(v["backup_binding"]),
        )
        if (
            entry.action not in ("replaced", "added")
            or not pathlib.Path(entry.config_path).is_absolute()
            or not pathlib.Path(entry.backup_path).is_absolute()
            or not pathlib.Path(entry.target_path).is_absolute()
            or entry.config_binding.resolved != entry.target_path
            or entry.config_binding.mode != entry.expected_mode
            or entry.backup_binding.mode != 0o600
        ):
            return None
    except (KeyError, TypeError, ValueError):
        return None
    else:
        return entry


def _xdg_state_home() -> pathlib.Path:
    """Resolve ``$XDG_STATE_HOME`` per the XDG Base Directory spec.

    Defaults to ``~/.local/state`` when the env var is unset or empty.
    State is the right XDG bucket here (vs. cache / config / data): the
    file is machine-written, must persist across runs so ``revert`` can
    locate the right backup, but is not safely deletable like cache nor
    user-edited like config.
    """
    env = os.environ.get("XDG_STATE_HOME")
    if env:
        return pathlib.Path(env)
    return pathlib.Path.home() / ".local" / "state"


# ``-dev`` suffix in the namespace makes it loud that this is dev-only
# tooling state, distinct from the runtime ``libtmux-mcp`` package.
STATE_DIR = _xdg_state_home() / "libtmux-mcp-dev" / "swap"
STATE_FILE = STATE_DIR / "state.json"

BACKUP_SUFFIX_PREFIX = ".bak.mcp-swap-"


# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------


#: Per-entry shape a CLI expects under its server map. ``standard`` is
#: the Claude-Desktop lineage every CLI here started from — scalar
#: ``command``, sibling ``args`` list, optional ``env`` table.
#: ``claude`` is that shape plus an explicit ``type``/``env`` that
#: Claude writes even when empty. ``opencode`` packs argv into a single
#: ``command`` array and spells the environment table ``environment``.
#: Dialects exist because the shape is not implied by the file format:
#: two CLIs sharing ``fmt="json"`` can still disagree about how one
#: entry is spelled.
Dialect = t.Literal["standard", "claude", "opencode"]


@dataclasses.dataclass(frozen=True)
class CLIInfo:
    """Static descriptor for a CLI's config file and discovery heuristics."""

    name: CLIName
    binary: str
    config_path: pathlib.Path
    fmt: t.Literal["json", "jsonc", "toml"]
    #: Key path from the document root down to the mapping of server
    #: name -> entry. A path rather than a single key so a CLI that
    #: nests deeper needs no new branch in the four functions that
    #: read, write, delete and enumerate entries.
    container: tuple[str, ...]
    #: Entry shape written and read back for this CLI.
    dialect: Dialect


def _xdg_config_home() -> pathlib.Path:
    """``$XDG_CONFIG_HOME`` when absolute, else ``~/.config``.

    The spec requires these variables to be absolute and says to ignore
    them otherwise. A relative value would resolve against the working
    directory, so the swap would record a backup path that revert could
    no longer find from anywhere else.
    """
    raw = os.environ.get("XDG_CONFIG_HOME")
    if raw and pathlib.Path(raw).is_absolute():
        return pathlib.Path(raw)
    return pathlib.Path.home() / ".config"


CLIS: dict[CLIName, CLIInfo] = {
    "claude": CLIInfo(
        name="claude",
        binary="claude",
        config_path=pathlib.Path.home() / ".claude.json",
        fmt="json",
        container=("mcpServers",),
        dialect="claude",
    ),
    "codex": CLIInfo(
        name="codex",
        binary="codex",
        config_path=pathlib.Path.home() / ".codex" / "config.toml",
        fmt="toml",
        container=("mcp_servers",),
        dialect="standard",
    ),
    "cursor": CLIInfo(
        name="cursor",
        binary="cursor-agent",
        config_path=pathlib.Path.home() / ".cursor" / "mcp.json",
        fmt="json",
        container=("mcpServers",),
        dialect="standard",
    ),
    "gemini": CLIInfo(
        name="gemini",
        binary="gemini",
        config_path=pathlib.Path.home() / ".gemini" / "settings.json",
        fmt="json",
        container=("mcpServers",),
        dialect="standard",
    ),
    "grok": CLIInfo(
        name="grok",
        binary="grok",
        config_path=pathlib.Path.home() / ".grok" / "config.toml",
        fmt="toml",
        container=("mcp_servers",),
        dialect="standard",
    ),
    "agy": CLIInfo(
        name="agy",
        binary="agy",
        config_path=(pathlib.Path.home() / ".gemini" / "config" / "mcp_config.json"),
        fmt="json",
        container=("mcpServers",),
        dialect="standard",
    ),
    "opencode": CLIInfo(
        name="opencode",
        binary="opencode",
        # opencode reads config.json, opencode.json and opencode.jsonc from
        # this directory and merges all three, with .jsonc winning. It writes
        # to the first that exists, defaulting to .jsonc — so that is the one
        # file a swap can own without being shadowed.
        config_path=_xdg_config_home() / "opencode" / "opencode.jsonc",
        fmt="jsonc",
        container=("mcp",),
        dialect="opencode",
    ),
    "pi": CLIInfo(
        name="pi",
        binary="pi",
        # Read by the pi-mcp-adapter extension, not by pi itself; see
        # PI_ADAPTER_DIR. Claude-Desktop schema, so the standard dialect.
        # The adapter parses through strip-json-comments with trailing
        # commas allowed, so the file is JSONC despite the .json suffix.
        config_path=pathlib.Path.home() / ".pi" / "agent" / "mcp.json",
        fmt="jsonc",
        container=("mcpServers",),
        dialect="standard",
    ),
}

#: Written into an opencode config this script creates from nothing.
#: opencode injects the same line itself on first load; seeding it here
#: keeps the swap from being followed by a surprise rewrite.
OPENCODE_SCHEMA_URL = "https://opencode.ai/config.json"

#: pi ships no MCP client — its README says "No MCP" outright, and the
#: released build contains no MCP code at all. MCP reaches pi only
#: through the third-party ``pi-mcp-adapter`` extension, which is what
#: reads ``~/.pi/agent/mcp.json``. The swap writes that file because it
#: is the one pi-family location with a settled schema, but until the
#: adapter is installed pi does not read it, so ``detect`` says so
#: instead of reporting a swap that cannot take effect.
PI_ADAPTER_DIR = (
    pathlib.Path.home() / ".pi" / "agent" / "npm" / "node_modules" / "pi-mcp-adapter"
)
PI_ADAPTER_HINT = "needs the pi-mcp-adapter package; pi has no built-in MCP client"


@dataclasses.dataclass
class McpServerSpec:
    """The portable shape shared across CLI configs."""

    command: str
    args: list[str] = dataclasses.field(default_factory=list)
    env: dict[str, str] = dataclasses.field(default_factory=dict)

    def to_entry_dict(self, dialect: Dialect = "standard") -> dict[str, t.Any]:
        """Serialize to the entry shape ``dialect`` expects."""
        # Claude's format always includes ``type`` and ``env`` (even when
        # empty); the standard shape omits both when there is nothing to say.
        if dialect == "claude":
            return {
                "type": "stdio",
                "command": self.command,
                "args": list(self.args),
                "env": dict(self.env),
            }
        if dialect == "opencode":
            # One array for argv, and the table is "environment" -- an
            # "env" key here is dropped in silence, and a scalar command
            # is a decode error that takes the whole config down with it.
            local: dict[str, t.Any] = {
                "type": "local",
                "command": [self.command, *self.args],
            }
            if self.env:
                local["environment"] = dict(self.env)
            return local
        out: dict[str, t.Any] = {"command": self.command, "args": list(self.args)}
        if self.env:
            out["env"] = dict(self.env)
        return out

    def binary_path(self) -> pathlib.Path | None:
        """Return the command as a path, or ``None`` when it is a bare name.

        A bare name is left to ``PATH`` by whichever CLI starts the
        server, so there is no path here to reason about.
        """
        if "/" not in self.command:
            return None
        return pathlib.Path(self.command)

    def is_under(self, root: pathlib.Path) -> bool:
        """Return True when the command is a binary inside ``root``.

        This is how a swapped-in build is recognised on the way back
        out: the config records an absolute path, and what makes it
        *this* checkout's build is living under it.
        """
        binary = self.binary_path()
        if binary is None:
            return False
        try:
            return root.resolve() in binary.resolve().parents
        except (OSError, ValueError):
            # ``status`` classifies whatever a config holds today, and a
            # command nothing here wrote can be unresolvable — an embedded
            # NUL raises ValueError rather than OSError. Either way it is
            # not a build in this checkout.
            return False


@dataclasses.dataclass
class SwapEntry:
    """One CLI's bookkeeping for a swap, written to the state file."""

    config_path: str
    backup_path: str
    server: str
    action: t.Literal["replaced", "added"]
    #: ``YYYYMMDDHHMMSS`` registration timestamp, human-readable for
    #: anyone inspecting ``state.json`` directly. Sort order is enforced
    #: separately via :attr:`seq_no` so this field stays purely
    #: descriptive.
    swapped_at: str
    #: Monotonic registration counter — the primary LIFO sort key for
    #: ``cmd_revert``. ``cmd_use_local`` computes the next value as
    #: ``max(existing seq_nos, default=-1) + 1`` so it strictly
    #: increases per swap regardless of wall-clock collisions or dict
    #: iteration order. Same explicit-counter pattern CPython's
    #: ``Lib/sched.py`` uses to break ties on ``Event(time, priority,
    #: sequence, …)``.
    seq_no: int
    #: Exact destination changed by the swap. ``config_path`` may be a
    #: symlink that is later repointed.
    target_path: str
    original_sha256: str
    original_mode: int
    expected_sha256: str
    expected_mode: int
    config_binding: _PathBinding
    backup_binding: _PathBinding


@dataclasses.dataclass(frozen=True)
class _PhysicalIdentity:
    """Kernel identity for one filesystem object."""

    device: int
    inode: int


@dataclasses.dataclass(frozen=True)
class _TopologyNode:
    """One directory or symlink on a logical destination route."""

    position: int
    kind: t.Literal["directory", "symlink"]
    link: str | None
    identity: _PhysicalIdentity


@dataclasses.dataclass(frozen=True)
class _PathBinding:
    """Logical topology, resolved target, identity, and mode for a file."""

    resolved: str
    nodes: tuple[_TopologyNode, ...]
    target: _PhysicalIdentity
    mode: int


@dataclasses.dataclass(frozen=True)
class _MissingBinding:
    """Resolved destination and existing-parent identity for a missing file."""

    resolved: str
    parent: str
    parent_identity: _PhysicalIdentity


@dataclasses.dataclass
class _StateSnapshot:
    """Authenticated state bytes plus their observed destination."""

    entries: dict[tuple[CLIName, Scope], SwapEntry]
    transaction: dict[str, t.Any] | None
    raw: bytes | None
    binding: _PathBinding | None
    missing: _MissingBinding | None


@dataclasses.dataclass
class _PreparedTarget:
    """One selected config rendered and recovery-planned before any write."""

    cli: CLIName
    scope: Scope
    label: str
    info: CLIInfo
    target_path: pathlib.Path
    original_bytes: bytes
    new_bytes: bytes
    action: t.Literal["replaced", "added"]
    config_binding: _PathBinding
    backup_path: pathlib.Path | None = None
    backup_missing: _MissingBinding | None = None
    backup_bytes: bytes | None = None
    backup_binding: _PathBinding | None = None
    prior_entry: SwapEntry | None = None
    state_entry: SwapEntry | None = None


@dataclasses.dataclass(frozen=True)
class _UseRequest:
    """Resolved immutable inputs shared by dry-run and locked planning."""

    repo: pathlib.Path
    server: str
    spec: McpServerSpec
    source: str
    targets: tuple[CLIName, ...]
    extra_env: dict[str, str]


@dataclasses.dataclass
class _ApplyPlan:
    """All selected config, backup, and state observations for one apply."""

    request: _UseRequest
    state: _StateSnapshot
    prepared: list[_PreparedTarget]
    timestamp: str


@dataclasses.dataclass(frozen=True)
class _RecoveryLayer:
    """One authenticated LIFO layer and its original-byte backup."""

    key: tuple[CLIName, Scope]
    label: str
    entry: SwapEntry
    backup_path: pathlib.Path
    backup_bytes: bytes
    backup_binding: _PathBinding


@dataclasses.dataclass
class _RevertTarget:
    """One physical config and the contiguous top layers to unwind."""

    info: CLIInfo
    config_path: pathlib.Path
    target_path: pathlib.Path
    current_bytes: bytes
    current_binding: _PathBinding
    layers: list[_RecoveryLayer]
    restored_bytes: bytes
    restored_mode: int
    restored_binding: _PathBinding | None = None


@dataclasses.dataclass
class _RevertPlan:
    """Every config, backup, and state observation for one revert."""

    state: _StateSnapshot
    targets: list[_RevertTarget]
    selected: set[tuple[CLIName, Scope]]


@dataclasses.dataclass(frozen=True)
class _PathClaim:
    """One physical config or recovery destination claimed by a plan."""

    owner: str
    kind: str
    resolved: str
    target: _PhysicalIdentity | None


class SwapStateError(RuntimeError):
    """Swap state is unsafe to use for a mutating operation."""


STATE_VERSION = 2
_DIGEST_PATTERN = re.compile(r"[0-9a-f]{64}")


def _invalid(message: str) -> t.NoReturn:
    """Raise a recovery-record validation failure."""
    raise ValueError(message)


def _unsafe(message: str) -> t.NoReturn:
    """Raise a filesystem ownership or transaction failure."""
    raise OSError(message)


def _unsafe_state(message: str) -> t.NoReturn:
    """Raise an authenticated recovery-state failure."""
    raise SwapStateError(message)


def _abort(message: str) -> t.NoReturn:
    """Raise a command-planning failure."""
    raise RuntimeError(message)


def _required_string(values: dict[str, t.Any], key: str) -> str:
    """Return one required nonempty string from an internal record."""
    value = values[key]
    if not isinstance(value, str) or not value:
        _invalid(key)
    return value


def _required_digest(values: dict[str, t.Any], key: str) -> str:
    """Return one canonical SHA-256 digest from an internal record."""
    value = _required_string(values, key)
    if _DIGEST_PATTERN.fullmatch(value) is None:
        _invalid(key)
    return value


def _required_mode(values: dict[str, t.Any], key: str) -> int:
    """Return one permission mode without accepting booleans."""
    value = values[key]
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 0o777:
        _invalid(key)
    return value


def _parse_identity(raw: t.Any) -> _PhysicalIdentity:
    """Decode a physical identity from recovery JSON."""
    if not isinstance(raw, dict) or set(raw) != {"device", "inode"}:
        _invalid("identity")
    device, inode = raw["device"], raw["inode"]
    if any(
        isinstance(value, bool) or not isinstance(value, int) or value < 0
        for value in (device, inode)
    ):
        _invalid("identity")
    return _PhysicalIdentity(device=device, inode=inode)


def _parse_path_binding(raw: t.Any) -> _PathBinding:
    """Decode one strict path binding from recovery JSON."""
    if not isinstance(raw, dict) or set(raw) != {"resolved", "nodes", "target", "mode"}:
        _invalid("path binding")
    resolved = raw["resolved"]
    nodes = raw["nodes"]
    if not isinstance(resolved, str) or not pathlib.Path(resolved).is_absolute():
        _invalid("path binding")
    if not isinstance(nodes, list):
        _invalid("path binding")
    parsed_nodes: list[_TopologyNode] = []
    positions: set[int] = set()
    for node in nodes:
        if not isinstance(node, dict) or set(node) != {
            "position",
            "kind",
            "link",
            "identity",
        }:
            _invalid("topology node")
        position = node["position"]
        kind = node["kind"]
        link = node["link"]
        if isinstance(position, bool) or not isinstance(position, int) or position < 0:
            _invalid("topology node")
        if kind not in ("directory", "symlink"):
            _invalid("topology node")
        if link is not None and not isinstance(link, str):
            _invalid("topology node")
        if (kind == "directory") != (link is None) or position in positions:
            _invalid("topology node")
        positions.add(position)
        parsed_nodes.append(
            _TopologyNode(
                position=position,
                kind=kind,
                link=link,
                identity=_parse_identity(node["identity"]),
            )
        )
    return _PathBinding(
        resolved=str(pathlib.Path(resolved)),
        nodes=tuple(parsed_nodes),
        target=_parse_identity(raw["target"]),
        mode=_required_mode(raw, "mode"),
    )


def _digest(data: bytes) -> str:
    """Return the canonical digest used by recovery records."""
    return hashlib.sha256(data).hexdigest()


def _physical_identity(details: os.stat_result) -> _PhysicalIdentity:
    """Extract stable POSIX identity fields from a stat result."""
    return _PhysicalIdentity(device=details.st_dev, inode=details.st_ino)


def _absolute_logical(path: pathlib.Path) -> pathlib.Path:
    """Make a path absolute without resolving its symlink topology."""
    return path.absolute()


def _capture_path_binding(path: pathlib.Path) -> _PathBinding:
    """Capture every logical route node and the resolved regular file."""
    absolute = _absolute_logical(path)
    nodes: list[_TopologyNode] = []
    current = pathlib.Path(absolute.anchor)
    for position, part in enumerate(absolute.parts[1:], start=1):
        current /= part
        details = current.lstat()
        last = current == absolute
        if last and stat.S_ISREG(details.st_mode):
            continue
        if stat.S_ISLNK(details.st_mode):
            kind: t.Literal["directory", "symlink"] = "symlink"
            link: str | None = str(current.readlink())
        elif stat.S_ISDIR(details.st_mode):
            kind = "directory"
            link = None
        else:
            _unsafe(f"destination topology contains a non-directory: {current}")
        nodes.append(
            _TopologyNode(
                position=position,
                kind=kind,
                link=link,
                identity=_physical_identity(details),
            )
        )
    resolved = absolute.resolve(strict=True)
    details = resolved.stat()
    if not stat.S_ISREG(details.st_mode):
        _unsafe(f"destination is not a regular file: {path}")
    return _PathBinding(
        resolved=str(resolved),
        nodes=tuple(nodes),
        target=_physical_identity(details),
        mode=stat.S_IMODE(details.st_mode),
    )


def _read_bound_file(path: pathlib.Path) -> tuple[bytes, _PathBinding]:
    """Read one file only while its topology and identity stay unchanged."""
    before = _capture_path_binding(path)
    with pathlib.Path(before.resolved).open("rb") as handle:
        data = handle.read()
    after = _capture_path_binding(path)
    if after != before:
        _unsafe(f"destination changed while it was read: {path}")
    return data, before


def _capture_missing_binding(path: pathlib.Path) -> _MissingBinding:
    """Bind a missing destination to its nearest existing directory."""
    absolute = _absolute_logical(path)
    if os.path.lexists(absolute):
        _unsafe(f"destination already exists: {path}")
    ancestor = absolute.parent
    tail = [absolute.name]
    while not os.path.lexists(ancestor):
        tail.insert(0, ancestor.name)
        ancestor = ancestor.parent
    resolved_parent = ancestor.resolve(strict=True)
    details = resolved_parent.stat()
    if not stat.S_ISDIR(details.st_mode):
        _unsafe(f"destination parent is not a directory: {ancestor}")
    if not os.access(resolved_parent, os.W_OK | os.X_OK):
        _unsafe(f"destination parent is not writable: {ancestor}")
    return _MissingBinding(
        resolved=str(resolved_parent.joinpath(*tail)),
        parent=str(resolved_parent),
        parent_identity=_physical_identity(details),
    )


def _validate_missing_binding(path: pathlib.Path, binding: _MissingBinding) -> None:
    """Require a planned missing path and its existing ancestor to stay put."""
    current = _capture_missing_binding(path)
    if current != binding:
        _unsafe(f"destination parent changed after planning: {path}")


def _validate_bound_contents(
    path: pathlib.Path,
    expected: bytes,
    binding: _PathBinding,
) -> None:
    """Require path topology, identity, mode, and bytes to match a plan."""
    data, current = _read_bound_file(path)
    if current != binding:
        _unsafe(f"destination identity changed after planning: {path}")
    if data != expected:
        _unsafe(f"destination content changed after planning: {path}")


def _check_bound_mutation(path: pathlib.Path, binding: _PathBinding) -> None:
    """Require the resolved parent needed for replace or removal."""
    parent = pathlib.Path(binding.resolved).parent
    if not parent.is_dir() or not os.access(parent, os.W_OK | os.X_OK):
        _unsafe(f"destination parent is not writable: {path}")


def _binding_claim(owner: str, kind: str, binding: _PathBinding) -> _PathClaim:
    """Describe one existing file for cross-artifact alias checks."""
    return _PathClaim(owner, kind, binding.resolved, binding.target)


def _missing_claim(owner: str, kind: str, binding: _MissingBinding) -> _PathClaim:
    """Describe one absent destination for cross-artifact alias checks."""
    return _PathClaim(owner, kind, binding.resolved, None)


def _validate_distinct_claims(claims: list[_PathClaim]) -> None:
    """Reject matching resolved paths or physical file identities."""
    for index, claim in enumerate(claims):
        for previous in claims[:index]:
            same_target = claim.target is not None and claim.target == previous.target
            if claim.resolved == previous.resolved or same_target:
                _unsafe(
                    f"{previous.owner} {previous.kind} and {claim.owner} "
                    f"{claim.kind} select the same physical path"
                )


def _remove_bound_file(
    path: pathlib.Path,
    data: bytes,
    binding: _PathBinding,
) -> None:
    """Remove exactly the observed file and durably publish its absence."""
    _validate_bound_contents(path, data, binding)
    pathlib.Path(binding.resolved).unlink()
    _sync_directory(pathlib.Path(binding.resolved).parent)


def _remove_state_snapshot(snapshot: _StateSnapshot) -> _StateSnapshot:
    """Remove exactly one authenticated recovery-state file."""
    if snapshot.raw is None or snapshot.binding is None:
        _unsafe_state("swap state has no authenticated file to remove")
    _remove_bound_file(STATE_FILE, snapshot.raw, snapshot.binding)
    return _StateSnapshot(
        entries={},
        transaction=None,
        raw=None,
        binding=None,
        missing=_capture_missing_binding(STATE_FILE),
    )


def _restore_known_state(
    snapshot: _StateSnapshot,
    alternatives: set[bytes | None],
) -> _StateSnapshot:
    """Restore pending state only from an authenticated transaction outcome."""
    if snapshot.raw is None or snapshot.binding is None:
        _unsafe_state("pending transaction has no authenticated state")
    if os.path.lexists(STATE_FILE):
        raw, binding = _read_bound_file(STATE_FILE)
        if raw == snapshot.raw and binding == snapshot.binding:
            return snapshot
        if raw not in alternatives or binding.mode != 0o600:
            _unsafe_state("recovery state changed outside the transaction")
    elif None not in alternatives:
        _unsafe_state("recovery state disappeared outside the transaction")
    atomic_write(STATE_FILE, snapshot.raw)
    raw, binding = _read_bound_file(STATE_FILE)
    if raw != snapshot.raw or binding.mode != 0o600:
        _unsafe_state("pending recovery state could not be restored")
    entries, transaction = _decode_state(raw)
    if transaction is None:
        _unsafe_state("restored recovery state is not pending")
    return _StateSnapshot(entries, transaction, raw, binding, None)


# ---------------------------------------------------------------------------
# JSONC — comments and trailing commas, edited without reserializing
# ---------------------------------------------------------------------------
#
# tomlkit gives TOML a format-preserving round trip; JSONC has no
# equivalent on PyPI that is safe to depend on here. ``json-five`` was
# measured first and rejected: it raises on ``"C:\\x"`` and silently
# decodes the literal six characters ``\u0041`` to ``"A"`` — both valid
# JSON that stdlib reads correctly, and the second is exactly the silent
# rewrite this script exists to avoid.
#
# So values come from stdlib ``json`` (correct escape semantics) and
# edits are applied as text splices located by an offset-preserving
# scanner. Every byte outside a replaced value survives untouched, which
# is the same technique opencode's own config writer uses via
# ``jsonc-parser``'s ``modify()``.

_JSON_WS = " \t\n\r"

#: Longest inline rendering of a scalar list before it is broken across
#: lines. A swapped ``command`` array is the common case and reads
#: better on one line, which is how these configs are written by hand.
_INLINE_WIDTH = 88


def _jsonc_blank_comments(text: str) -> str:
    """Replace comment bytes with spaces, preserving every offset.

    Scanning rather than matching a regex is the whole point: ``//``
    inside a URL and ``/*`` inside a Windows path are string content, not
    comments, and only a scanner that tracks string state can tell them
    apart. Offsets are preserved so a span found in the blanked text
    addresses the same bytes in the original.
    """
    out = list(text)
    i, n = 0, len(text)
    in_string = False
    while i < n:
        char = text[i]
        if in_string:
            if char == "\\":
                i += 2
                continue
            if char == '"':
                in_string = False
            i += 1
        elif char == '"':
            in_string = True
            i += 1
        elif char == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
        elif char == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            end = n if end == -1 else end + 2
            for j in range(i, end):
                if out[j] != "\n":
                    out[j] = " "
            i = end
        else:
            i += 1
    return "".join(out)


def _jsonc_blank_trailing_commas(blanked: str) -> str:
    """Blank trailing commas so stdlib :func:`json.loads` accepts the text."""
    out = list(blanked)
    i, n = 0, len(blanked)
    in_string = False
    last_comma = -1
    while i < n:
        char = blanked[i]
        if in_string:
            if char == "\\":
                i += 2
                continue
            if char == '"':
                in_string = False
            i += 1
            continue
        if char == '"':
            in_string = True
            last_comma = -1
        elif char == ",":
            last_comma = i
        elif char in "}]":
            if last_comma != -1:
                out[last_comma] = " "
            last_comma = -1
        elif char not in _JSON_WS:
            last_comma = -1
        i += 1
    return "".join(out)


def _jsonc_loads(text: str) -> t.Any:
    """Parse JSONC text into plain Python objects."""
    if not text.strip():
        return {}
    return json.loads(_jsonc_blank_trailing_commas(_jsonc_blank_comments(text)))


class _JsoncScanner:
    """Locate value spans inside comment-blanked JSON text."""

    def __init__(self, text: str) -> None:
        self.text = text
        self.pos = 0

    def skip_ws(self) -> None:
        """Advance past insignificant whitespace."""
        while self.pos < len(self.text) and self.text[self.pos] in _JSON_WS:
            self.pos += 1

    def read_string(self) -> str:
        """Consume one string token and return its raw text, quotes included."""
        start = self.pos
        self.pos += 1
        while self.pos < len(self.text):
            char = self.text[self.pos]
            if char == "\\":
                self.pos += 2
                continue
            self.pos += 1
            if char == '"':
                break
        return self.text[start : self.pos]

    def read_value(self) -> tuple[int, int]:
        """Consume one value and return its ``(start, end)`` span."""
        self.skip_ws()
        start = self.pos
        char = self.text[self.pos]
        if char == '"':
            self.read_string()
        elif char in "{[":
            self._read_container()
        else:
            while (
                self.pos < len(self.text)
                and self.text[self.pos] not in ",}]"
                and self.text[self.pos] not in _JSON_WS
            ):
                self.pos += 1
        return start, self.pos

    def _read_container(self) -> None:
        self.pos += 1
        depth = 1
        while self.pos < len(self.text) and depth:
            char = self.text[self.pos]
            if char == '"':
                self.read_string()
                continue
            if char in "{[":
                depth += 1
            elif char in "}]":
                depth -= 1
            self.pos += 1

    def read_members(self, obj_start: int) -> list[_JsoncMember]:
        """Enumerate an object's members. ``obj_start`` indexes its ``{``."""
        self.pos = obj_start + 1
        found: list[_JsoncMember] = []
        while True:
            self.skip_ws()
            if self.pos >= len(self.text) or self.text[self.pos] == "}":
                return found
            if self.text[self.pos] == ",":
                self.pos += 1
                continue
            member_start = self.pos
            raw_key = self.read_string()
            self.skip_ws()
            self.pos += 1  # the ':'
            value_start, value_end = self.read_value()
            found.append(
                _JsoncMember(
                    key=json.loads(raw_key),
                    start=member_start,
                    end=value_end,
                    value_start=value_start,
                    value_end=value_end,
                )
            )


class _JsoncMember(t.NamedTuple):
    """One ``"key": value`` pair located inside a JSONC document.

    Attributes
    ----------
    key : str
        The decoded member name.
    start : int
        Offset of the opening quote of the key.
    end : int
        Offset just past the value — the end of the whole member.
    value_start : int
        Offset of the first byte of the value.
    value_end : int
        Offset just past the last byte of the value.
    """

    key: str
    start: int
    end: int
    value_start: int
    value_end: int


def _jsonc_render(value: t.Any, depth: int, *, ensure_ascii: bool) -> str:
    """Render ``value`` as JSON text indented for nesting ``depth``."""
    pad = "  " * depth
    if isinstance(value, list) and all(
        isinstance(item, (str, int, float, bool)) or item is None for item in value
    ):
        inline = json.dumps(value, ensure_ascii=ensure_ascii)
        if len(inline) + len(pad) <= _INLINE_WIDTH:
            return inline
    return json.dumps(value, indent=2, ensure_ascii=ensure_ascii).replace(
        "\n", "\n" + pad
    )


def _jsonc_object_span(blanked: str, path: tuple[str, ...]) -> tuple[int, int] | None:
    """Return the span of the object reached by ``path``, or ``None``."""
    scanner = _JsoncScanner(blanked)
    scanner.skip_ws()
    if scanner.pos >= len(blanked) or blanked[scanner.pos] != "{":
        return None
    cursor = scanner.pos
    for key in path:
        match = next(
            (m for m in _JsoncScanner(blanked).read_members(cursor) if m.key == key),
            None,
        )
        if match is None or blanked[match.value_start] != "{":
            return None
        cursor = match.value_start
    tail = _JsoncScanner(blanked)
    tail.pos = cursor
    return tail.read_value()


def _jsonc_next_edit(
    text: str,
    data: t.Mapping[str, t.Any],
    path: tuple[str, ...],
    *,
    ensure_ascii: bool,
) -> tuple[int, int, str] | None:
    """Find the one next splice that brings ``path`` closer to ``data``."""
    blanked = _jsonc_blank_comments(text)
    span = _jsonc_object_span(blanked, path)
    if span is None:
        return None
    obj_start, obj_end = span
    members = _JsoncScanner(blanked).read_members(obj_start)
    by_key = {member.key: member for member in members}
    depth = len(path) + 1
    pad = "  " * depth

    for key, value in data.items():
        member = by_key.get(key)
        if member is None:
            body = _jsonc_render(value, depth, ensure_ascii=ensure_ascii)
            # Escape the key like any other value: written raw, a backslash
            # or quote in a server name emits text that cannot be parsed
            # back, so the member is never found and the merge re-inserts
            # it until the pass ceiling, holding the swap lock throughout.
            name = json.dumps(key, ensure_ascii=ensure_ascii)
            if members:
                tail = members[-1].end
                return tail, tail, f",\n{pad}{name}: {body}"
            if blanked[obj_start + 1 : obj_end - 1].strip():
                return None
            # Blanking hid any comment the object holds, so measure the
            # interior in the original text and splice after it, not over it.
            interior = text[obj_start + 1 : obj_end - 1]
            anchor = obj_start + 1 + len(interior.rstrip())
            closing = "  " * (depth - 1)
            return anchor, obj_end - 1, f"\n{pad}{name}: {body}\n{closing}"
        current = json.loads(
            _jsonc_blank_trailing_commas(blanked[member.value_start : member.value_end])
        )
        if isinstance(value, dict) and isinstance(current, dict):
            nested = _jsonc_next_edit(
                text, value, (*path, key), ensure_ascii=ensure_ascii
            )
            if nested is not None:
                return nested
        elif current != value:
            return (
                member.value_start,
                member.value_end,
                _jsonc_render(value, depth, ensure_ascii=ensure_ascii),
            )

    for index, member in enumerate(members):
        if member.key in data:
            continue
        # Exactly one delimiter leaves with the member: the comma before
        # it, or, for the first member which has none, the comma after.
        if index:
            return members[index - 1].end, member.end, ""
        # Read that comma out of the blanked text -- one inside a comment
        # is not a delimiter, and a real one behind a comment still is.
        trailing = blanked[member.end : obj_end]
        drop_to = member.end
        if trailing.lstrip(_JSON_WS).startswith(","):
            drop_to += trailing.index(",") + 1
        return obj_start + 1, drop_to, ""
    return None


def _jsonc_merge(text: str, data: t.Mapping[str, t.Any], *, ensure_ascii: bool) -> str:
    """Reconcile ``data`` into ``text``, rewriting only members that differ.

    Applies one splice at a time and rescans, so offsets are always
    computed against current text rather than patched up after the fact.
    Config files are small enough that the extra passes do not matter and
    the invariant is worth far more than the cycles.
    """
    if not text.strip():
        return json.dumps(dict(data), indent=2, ensure_ascii=ensure_ascii) + "\n"
    # One splice per member, plus slack; a config that needs more than
    # this has a pathology worth surfacing rather than looping on.
    for _ in range(10_000):
        edit = _jsonc_next_edit(text, data, (), ensure_ascii=ensure_ascii)
        if edit is None:
            return text
        start, end, replacement = edit
        text = text[:start] + replacement + text[end:]
    msg = "JSONC merge did not converge"
    raise RuntimeError(msg)


# ---------------------------------------------------------------------------
# Config IO — per format
# ---------------------------------------------------------------------------


def _parse_config_bytes(info: CLIInfo, raw: bytes) -> t.Any:
    """Parse JSON, JSONC or TOML config bytes into an editable structure.

    Empty JSON files are treated as empty objects so first-run MCP configs can
    be seeded with their initial server entry.
    """
    if info.fmt == "jsonc":
        return _jsonc_loads(raw.decode())
    if info.fmt == "json":
        text = raw.decode().strip()
        return json.loads(text) if text else {}
    return tomlkit.parse(raw.decode())


def load_config(info: CLIInfo) -> t.Any:
    """Read and parse one CLI config."""
    return _parse_config_bytes(info, info.config_path.read_bytes())


def _json_trailer(original: bytes) -> str:
    """Return the newline a rewritten JSON config should end with.

    Claude writes ``~/.claude.json`` without a trailing newline, so
    appending one unconditionally grows the file by a byte on every swap
    and shows as a diff hunk in a region the swap never touched. Empty
    bytes mean a file being seeded, which gets the conventional newline.
    """
    if not original:
        return "\n"
    return "\n" if original.endswith(b"\n") else ""


def dump_config_bytes(info: CLIInfo, config: t.Any, *, original: bytes) -> bytes:
    """Serialize an edited config back to bytes in its original format.

    ``original`` is the file's pre-edit bytes, or empty when seeding a
    new one. The parsed structure does not record the byte-level
    conventions of the file it came from, so they are carried over from
    the source instead. Required rather than defaulted: a caller that
    omitted it would silently start rewriting regions it never touched,
    which is the defect this parameter exists to prevent. tomlkit
    preserves those conventions itself; only the JSON writer needs it.
    """
    # Dispatched on the exact format rather than "not json": a third
    # format reaching the TOML writer by fall-through would silently
    # write TOML bytes into a JSON file.
    if info.fmt == "toml":
        return tomlkit.dumps(config).encode()
    if info.fmt == "jsonc":
        # The merge derives its output from the original text, so the
        # file's own trailing-newline convention carries over untouched
        # and needs no _json_trailer fixup.
        source = original.decode()
        try:
            return _jsonc_merge(source, config, ensure_ascii=False).encode()
        except UnicodeEncodeError:
            return _jsonc_merge(source, config, ensure_ascii=True).encode()
    trailer = _json_trailer(original)
    # ensure_ascii would re-escape every non-ASCII character in the file,
    # including config text the swap never read.
    text = json.dumps(config, indent=2, ensure_ascii=False) + trailer
    try:
        return text.encode()
    except UnicodeEncodeError:
        # A lone surrogate — a JS writer slicing a string mid-pair — has no
        # UTF-8 encoding. Escaping the document is then the only form that
        # can be written at all.
        return (json.dumps(config, indent=2) + trailer).encode()


def atomic_write(path: pathlib.Path, data: bytes) -> None:
    """Write bytes to ``path`` without replacing a symlinked config.

    Parameters
    ----------
    path : pathlib.Path
        Destination path. A symlink resolves to its final target so the
        write preserves every link in the chain.
    data : bytes
        Bytes to write atomically.
    """
    target = path.resolve() if path.is_symlink() else path
    target.parent.mkdir(parents=True, exist_ok=True)
    mode = stat.S_IMODE(target.stat().st_mode) if target.exists() else None
    fd, tmp_name = tempfile.mkstemp(prefix=target.name + ".", dir=str(target.parent))
    tmp = pathlib.Path(tmp_name)
    try:
        with os.fdopen(fd, "wb") as fh:
            if mode is not None:
                os.fchmod(fh.fileno(), mode)
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        tmp.replace(target)
        _sync_directory(target.parent)
    except Exception:
        tmp.unlink(missing_ok=True)
        raise


def _sync_directory(path: pathlib.Path) -> None:
    """Make a directory-entry update durable before returning."""
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def write_new_backup(path: pathlib.Path, data: bytes) -> pathlib.Path:
    """Create one preplanned backup without overwriting or falling back."""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as fh:
        fh.write(data)
        fh.flush()
        os.fsync(fh.fileno())
    _sync_directory(path.parent)
    return path


def _next_backup_path(base: pathlib.Path, reserved: set[pathlib.Path]) -> pathlib.Path:
    """Return the first unclaimed backup path without creating it."""
    candidate = base
    attempt = 0
    while candidate in reserved or os.path.lexists(candidate):
        attempt += 1
        candidate = base.with_name(f"{base.name}-{attempt}")
    return candidate


def _check_backup_destination(path: pathlib.Path) -> None:
    """Reject a backup destination whose existing parent is not writable."""
    parent = path.parent
    if not parent.is_dir():
        message = f"backup directory is not a directory: {parent}"
        raise OSError(message)
    if not os.access(parent, os.W_OK | os.X_OK):
        message = f"backup directory is not writable: {parent}"
        raise OSError(message)


# ---------------------------------------------------------------------------
# Per-CLI get / set / delete (the only CLI-specific logic)
# ---------------------------------------------------------------------------


def claude_project_key(repo: pathlib.Path) -> str:
    """Return the path Claude files a project's servers under.

    Claude keys by the main worktree, not by the directory it is run in, so
    inside a linked worktree the two differ and an entry written under the
    worktree's own path is never read. Git reports the main worktree as the
    parent of the common git directory; anything that is not a worktree, or
    a machine with no git, answers with the path itself.
    """
    try:
        common = subprocess.run(
            [
                "git",
                "-C",
                str(repo),
                "rev-parse",
                "--path-format=absolute",
                "--git-common-dir",
            ],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return str(repo.resolve())
    if not common:
        return str(repo.resolve())
    return str(pathlib.Path(common).parent.resolve())


@t.overload
def _claude_project_node(
    config: dict[str, t.Any],
    repo: pathlib.Path,
    *,
    create: t.Literal[True],
) -> dict[str, t.Any]: ...


@t.overload
def _claude_project_node(
    config: dict[str, t.Any],
    repo: pathlib.Path,
    *,
    create: t.Literal[False],
) -> dict[str, t.Any] | None: ...


def _claude_project_node(
    config: dict[str, t.Any], repo: pathlib.Path, *, create: bool
) -> dict[str, t.Any] | None:
    """Return (or create) the ``projects.<abs-repo>`` node Claude keys per-project.

    With ``create=True``, the node is unconditionally created if missing
    and the return type is statically narrowed to ``dict[str, t.Any]``;
    callers can drop runtime ``assert node is not None`` defensiveness.
    With ``create=False``, the absence of the node is a real return value
    and the type stays ``dict[str, t.Any] | None``.

    Raises ``RuntimeError`` if Claude's config layout is not the
    expected ``projects.<abs>.mcpServers`` mapping shape — the layout
    is undocumented Claude Code internal state, so a clear error before
    the atomic write beats a silent partial mutation that the backup
    defense would be asked to recover from.
    """
    key = claude_project_key(repo)
    projects_node = config.get("projects")
    if projects_node is not None and not isinstance(projects_node, dict):
        msg = (
            "Claude config layout appears to have changed; expected "
            f"'projects' to be a mapping but got "
            f"{type(projects_node).__name__}"
        )
        raise RuntimeError(msg)
    projects = (
        config.setdefault("projects", {}) if create else config.get("projects", {})
    )
    raw_node = projects.get(key)
    node: dict[str, t.Any] | None = None
    if isinstance(raw_node, dict):
        node = raw_node
    elif raw_node is not None:
        msg = (
            "Claude config layout appears to have changed; expected "
            f"'projects[{key!r}]' to be a mapping but got "
            f"{type(raw_node).__name__}"
        )
        raise RuntimeError(msg)
    if node is None and create:
        node = {"allowedTools": [], "mcpContextUris": [], "mcpServers": {}, "env": {}}
        projects[key] = node
    return node


@t.overload
def _claude_user_servers(
    config: dict[str, t.Any], *, create: t.Literal[True]
) -> dict[str, t.Any]: ...


@t.overload
def _claude_user_servers(
    config: dict[str, t.Any], *, create: t.Literal[False]
) -> dict[str, t.Any] | None: ...


def _claude_user_servers(
    config: dict[str, t.Any], *, create: bool
) -> dict[str, t.Any] | None:
    """Return (or create) the top-level ``mcpServers`` dict — Claude user scope.

    Mirrors :func:`_claude_project_node` for the user-scope path so the
    shape guard is centralised once and reused across read / write /
    delete instead of duplicated at each call site (or worse, missing
    on read and delete the way the inline write-side guard left them).
    Same reasoning applies as for the project-scope helper: Claude's
    config shape is undocumented internal state, so a clear
    ``RuntimeError`` before the atomic write beats an opaque
    ``AttributeError`` from ``.setdefault()`` on a non-dict.

    With ``create=True`` the dict is initialised when missing and the
    return type narrows to ``dict[str, t.Any]``. With ``create=False``
    a missing key returns ``None``.
    """
    raw = config.get("mcpServers")
    existing: dict[str, t.Any] | None = None
    if isinstance(raw, dict):
        existing = raw
    elif raw is not None:
        msg = (
            "Claude config layout appears to have changed; expected "
            f"'mcpServers' to be a mapping but got "
            f"{type(raw).__name__}"
        )
        raise RuntimeError(msg)
    if existing is None and create:
        existing = {}
        config["mcpServers"] = existing
    return existing


@t.overload
def _server_map(
    info: CLIInfo, config: t.Any, *, create: t.Literal[True]
) -> dict[str, t.Any]: ...


@t.overload
def _server_map(
    info: CLIInfo, config: t.Any, *, create: t.Literal[False]
) -> dict[str, t.Any] | None: ...


def _server_map(
    info: CLIInfo, config: t.Any, *, create: bool
) -> dict[str, t.Any] | None:
    """Walk ``info.container`` to the mapping holding this CLI's entries.

    Returns ``None`` when the path is absent and ``create`` is false.
    Intermediate levels are created on demand so a nested container needs
    no special case; TOML gets tomlkit tables so the written document
    keeps its formatting.

    Raises
    ------
    RuntimeError
        A key along the path holds something other than a mapping.
        Reported rather than overwritten — a swap must never discard
        config it cannot interpret.
    """
    node: dict[str, t.Any] = config
    for depth, key in enumerate(info.container):
        child = node.get(key)
        if child is None:
            if not create:
                return None
            child = tomlkit.table() if info.fmt == "toml" else {}
            node[key] = child
        elif not isinstance(child, dict):
            path = ".".join(info.container[: depth + 1])
            msg = (
                f"{info.config_path}: {path} is a {type(child).__name__}, "
                f"expected a table of server entries"
            )
            raise RuntimeError(msg)
        node = child
    return node


def _as_toml_table(entry: dict[str, t.Any]) -> tomlkit.items.Table:
    """Render one entry dict as a tomlkit table.

    Nested mappings (``env``) become sub-tables so the written document
    keeps TOML's own structure instead of an inline dict literal.
    """
    table = tomlkit.table()
    for key, value in entry.items():
        if isinstance(value, dict):
            sub = tomlkit.table()
            for sub_key, sub_value in value.items():
                sub[sub_key] = sub_value
            table[key] = sub
        else:
            table[key] = value
    return table


def get_server(
    cli: CLIName,
    config: t.Any,
    name: str,
    repo: pathlib.Path,
    *,
    scope: Scope = "project",
) -> McpServerSpec | None:
    """Fetch the MCP server entry for ``name`` from a CLI's config, if present.

    ``scope`` only affects Claude (see :data:`Scope` for the layered shape
    of ``~/.claude.json``); for Codex / Cursor / Gemini the parameter is
    accepted-but-ignored because their config has no per-project layer.
    """
    if cli == "claude":
        if scope == "user":
            servers = _claude_user_servers(config, create=False)
            entry = servers.get(name) if servers else None
        else:
            node = _claude_project_node(config, repo, create=False)
            if not node:
                return None
            entry = node.get("mcpServers", {}).get(name)
    else:
        servers = _server_map(CLIS[cli], config, create=False)
        entry = servers.get(name) if servers else None
    if entry is None:
        return None
    return _spec_from_entry(entry, info=CLIS[cli])


def set_server(
    cli: CLIName,
    config: t.Any,
    name: str,
    spec: McpServerSpec,
    repo: pathlib.Path,
    *,
    scope: Scope = "project",
) -> t.Literal["replaced", "added"]:
    """Write ``spec`` under ``name`` in a CLI's config, returning replaced/added.

    ``scope == "user"`` for Claude writes the top-level ``mcpServers``
    fallback used by every project that has no per-project override;
    ``"project"`` (the default, preserving pre-flag behaviour) writes
    under ``projects[abs(repo)].mcpServers``. The parameter is silently
    ignored for non-Claude CLIs.
    """
    if cli == "claude":
        if scope == "user":
            servers = _claude_user_servers(config, create=True)
            had = name in servers
            servers[name] = spec.to_entry_dict("claude")
            return "replaced" if had else "added"
        node = _claude_project_node(config, repo, create=True)
        servers = node.setdefault("mcpServers", {})
        had = name in servers
        servers[name] = spec.to_entry_dict("claude")
        return "replaced" if had else "added"
    info = CLIS[cli]
    if info.dialect == "opencode" and not config:
        # Seeding from nothing: opencode rewrites the file on load to add
        # this line, so writing it now avoids an immediate second edit.
        config["$schema"] = OPENCODE_SCHEMA_URL
    servers = _server_map(info, config, create=True)
    had = name in servers
    entry = spec.to_entry_dict(info.dialect)
    servers[name] = _as_toml_table(entry) if info.fmt == "toml" else entry
    return "replaced" if had else "added"


def delete_server(
    cli: CLIName,
    config: t.Any,
    name: str,
    repo: pathlib.Path,
    *,
    scope: Scope = "project",
) -> bool:
    """Remove the entry for ``name`` from a CLI's config; return whether it existed.

    See :func:`set_server` for the meaning of ``scope`` — the parameter
    is honoured for Claude and ignored for the other CLIs.
    """
    if cli == "claude":
        if scope == "user":
            servers = _claude_user_servers(config, create=False)
            if servers is not None and name in servers:
                del servers[name]
                return True
            return False
        node = _claude_project_node(config, repo, create=False)
        if not node:
            return False
        servers = node.get("mcpServers", {})
        return servers.pop(name, None) is not None
    servers = _server_map(CLIS[cli], config, create=False)
    if servers is None or name not in servers:
        return False
    del servers[name]
    return True


def _spec_from_entry(entry: t.Any, *, info: CLIInfo) -> McpServerSpec:
    """Convert a raw config entry (dict or tomlkit Table) into an McpServerSpec.

    Every dialect is normalised down to the portable scalar-command
    shape, so the helpers that reason about a spec —
    :meth:`McpServerSpec.binary_path`, :meth:`McpServerSpec.is_under`,
    ``_points_at`` — stay dialect-agnostic. Skipping this is not a
    cosmetic loss: an unsplit array command makes the "already local, no
    change" check miss, and every run rewrites a config it did not need
    to touch.
    """
    # tomlkit items quack like dicts/lists; coerce to plain Python for our spec.
    if info.fmt == "toml":
        entry = (
            tomlkit.items.Table.unwrap(entry)
            if isinstance(entry, tomlkit.items.Table)
            else dict(entry)
        )
    if info.dialect == "opencode":
        raw_command = entry.get("command", [])
        argv = (
            [str(part) for part in raw_command]
            if isinstance(raw_command, (list, tuple))
            else [str(raw_command)]
        )
        command, args = (argv[0], argv[1:]) if argv else ("", [])
        raw_env = entry.get("environment") or {}
    else:
        command = str(entry.get("command", ""))
        raw_args = entry.get("args", [])
        args = [str(a) for a in raw_args] if raw_args else []
        raw_env = entry.get("env") or {}
    env = {str(k): str(v) for k, v in dict(raw_env).items()}
    return McpServerSpec(command=command, args=args, env=env)


# ---------------------------------------------------------------------------
# Repo metadata
# ---------------------------------------------------------------------------


#: The file that identifies a libtmux checkout, relative to its root.
CHECKOUT_MARKER = pathlib.Path("apps/mcp/CMakeLists.txt")


def find_checkout(start: pathlib.Path) -> pathlib.Path:
    """Return the checkout root at or above ``start``.

    Walking up rather than requiring the root means the tool works from
    wherever the operator happens to be — the repository root, ``cxx/``
    where the build commands are run, or a build directory under it.
    """
    start = start.resolve()
    for candidate in (start, *start.parents):
        if (candidate / CHECKOUT_MARKER).is_file():
            return candidate
    msg = (
        f"no libtmux checkout at or above {start} — looked for "
        f"{CHECKOUT_MARKER} in it and each parent"
    )
    raise RuntimeError(msg)


def resolve_repo_meta(repo: pathlib.Path) -> tuple[str, str]:
    """Derive (server_name, binary_name) for this repo's MCP server.

    The server name is the registration slug used as the config-file key
    (``mcpServers.<slug>`` in JSON, ``[mcp_servers.<slug>]`` in TOML). The
    binary name is what the C++ package installs and what a published
    install puts on ``PATH``.

    Unlike the Python server this script came from, there is no
    ``[project.scripts]`` to read: the server is a compiled program, so the
    names are the ones the CMake target sets.
    """
    cmake = repo / CHECKOUT_MARKER
    if not cmake.is_file():
        msg = f"{cmake} not found — is {repo} the libtmux checkout?"
        raise RuntimeError(msg)
    text = cmake.read_text(encoding="utf-8")
    match = re.search(r"OUTPUT_NAME\s+([\w.-]+)", text)
    if match is None:
        msg = f"{cmake} sets no OUTPUT_NAME — cannot derive the binary name"
        raise RuntimeError(msg)
    return "libtmux", match.group(1)


#: Where the built server is looked for, in order, under a build directory.
BUILD_SUBPATHS: tuple[str, ...] = (
    "consumers/mcp",
    "apps/mcp",
)

#: Build directories tried by ``--source local``, most finished first.
DEFAULT_BUILD_DIRS: tuple[str, ...] = (
    "cxx/build/cxx-dev",
    "cxx/build/cxx-gcc",
    "cxx/build/cxx20",
    "build/cxx-dev",
)


def find_built_binary(
    repo: pathlib.Path, binary: str, build_dir: str | None = None
) -> pathlib.Path:
    """Return the built server under ``build_dir``, or the first that exists.

    A local build is a path, not a command: pointing an agent at the name
    alone would find whatever is on ``PATH``, which is the published copy
    and the opposite of what a local swap is for.
    """
    # An explicit build directory is an ordinary path, taken from where the
    # operator is standing, because that is where they just built it and how
    # every other tool reads one. Only the defaults are repo-relative, since
    # a default has no other frame of reference.
    roots = (
        [pathlib.Path(build_dir).expanduser().resolve()]
        if build_dir
        else [repo / d for d in DEFAULT_BUILD_DIRS]
    )
    for root in roots:
        for sub in BUILD_SUBPATHS:
            candidate = root / sub / binary
            if candidate.is_file() and os.access(candidate, os.X_OK):
                return candidate.resolve()
    looked = ", ".join(str(root) for root in roots)
    msg = (
        f"no built {binary} under {looked} — build it first:\n"
        f"  cmake --preset cxx-dev && "
        f"cmake --build --preset cxx-dev --target libtmux_mcp_server"
    )
    raise RuntimeError(msg)


def find_published_binary(binary: str, prefix: str | None = None) -> pathlib.Path:
    """Return an installed server, from ``prefix`` or from ``PATH``.

    A prefix is searched in both places a published server lands. ``cmake
    --install`` puts a program in ``bin/``; vcpkg moves it to
    ``tools/<port>/``, because a static triplet has no ``bin/`` for it to
    stay in. Looking in only one of them finds the server for half the
    people who installed it.
    """
    if prefix is not None:
        root = pathlib.Path(prefix).expanduser()
        looked = [root / "bin" / binary, root / "tools" / "libtmux" / binary]
        for candidate in looked:
            if candidate.is_file() and os.access(candidate, os.X_OK):
                return candidate.resolve()
        searched = " or ".join(str(path) for path in looked)
        msg = f"no executable {binary} at {searched}"
        raise RuntimeError(msg)
    found = shutil.which(binary)
    if found is None:
        msg = (
            f"{binary} is not on PATH — install the package, or use "
            f"--source local for a build in this checkout"
        )
        raise RuntimeError(msg)
    return pathlib.Path(found).resolve()


def build_binary_spec(
    binary: pathlib.Path,
    socket: str | None = None,
    env: dict[str, str] | None = None,
) -> McpServerSpec:
    """Build the spec that runs a compiled server.

    ``socket`` is passed as the server's POSIX-compatible positional path.
    Without it the server resolves its product-dedicated socket and bundled
    minimal configuration.
    """
    return McpServerSpec(
        command=str(binary),
        args=[] if socket is None else [socket],
        env=dict(env or {}),
    )


#: A complete legacy lifecycle and one operational request, newline-framed.
_PREFLIGHT_FRAMES = (
    "\n".join(
        json.dumps(frame)
        for frame in (
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-06-18",
                    "capabilities": {},
                    "clientInfo": {"name": "mcp_swap-preflight", "version": "1"},
                },
            },
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        )
    )
    + "\n"
)


def preflight_spec(spec: McpServerSpec, *, timeout: float = 300.0) -> str | None:
    """Launch ``spec`` and verify lifecycle negotiation plus tool discovery.

    Returns ``None`` when the server answered, otherwise a reason to
    show the operator. A pull-request spec resolves its dependencies at
    launch time, inside whichever agent starts it, so an unresolvable
    ref would otherwise land in every config and surface later as an
    opaque startup failure in each one.

    Closing stdin after the frame lets a well-behaved stdio server exit
    on its own, which keeps this free of signal handling.
    """
    try:
        proc = subprocess.Popen(
            [spec.command, *spec.args],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**os.environ, **spec.env},
            text=True,
        )
    except OSError as exc:
        return f"could not launch {spec.command}: {exc}"

    try:
        out, err = proc.communicate(_PREFLIGHT_FRAMES, timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()
        return f"no MCP response within {timeout:.0f}s"

    if proc.returncode != 0:
        tail = "\n".join(err.strip().splitlines()[-3:])
        return tail or f"server exited with status {proc.returncode}"

    replies: dict[object, list[dict[str, object]]] = {}
    for line in out.splitlines():
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            return "server wrote non-JSON data to the MCP stdout transport"
        if not isinstance(message, dict):
            return "server wrote a non-object JSON value to the MCP stdout transport"
        identifier = message.get("id")
        if identifier == 1 or identifier == 2:
            replies.setdefault(identifier, []).append(message)

    if any(len(messages) != 1 for messages in replies.values()):
        return "server answered an MCP preflight request more than once"

    initialized_reply = replies.get(1, [{}])[0]
    initialized = initialized_reply.get("result")
    if not isinstance(initialized, dict):
        tail = "\n".join(err.strip().splitlines()[-3:])
        return tail or "server exited without answering initialize"
    if initialized.get("protocolVersion") != "2025-06-18":
        return "server did not echo the requested MCP protocol version"
    server_info = initialized.get("serverInfo")
    if not isinstance(server_info, dict) or server_info.get("name") not in {
        "libtmux",
        "libtmux-cxx",
    }:
        return "initialize response did not identify a libtmux MCP server"

    listed_reply = replies.get(2, [{}])[0]
    listed = listed_reply.get("result")
    if not isinstance(listed, dict) or not isinstance(listed.get("tools"), list):
        return "server initialized but did not answer tools/list"
    tools = listed["tools"]
    if not all(
        isinstance(tool, dict) and isinstance(tool.get("name"), str) for tool in tools
    ):
        return "server returned a malformed MCP tool catalog"
    required = {
        "get_server_info",
        "list_panes",
        "list_sessions",
        "list_windows",
    }
    missing = sorted(required - {tool["name"] for tool in tools})
    if missing:
        return f"server tool catalog is missing: {', '.join(missing)}"
    return None


# ---------------------------------------------------------------------------
# State file
# ---------------------------------------------------------------------------


def _entry_document(entry: SwapEntry) -> dict[str, t.Any]:
    """Return one recovery entry as JSON-compatible values."""
    return dataclasses.asdict(entry)


def _state_payload(
    entries: dict[tuple[CLIName, Scope], SwapEntry],
    transaction: dict[str, t.Any] | None,
) -> dict[str, t.Any]:
    """Build the checksummed portion of the recovery document."""
    return {
        "version": STATE_VERSION,
        "entries": {
            _state_key(cli, scope): _entry_document(entry)
            for (cli, scope), entry in entries.items()
        },
        "transaction": transaction,
    }


def _canonical_json(value: t.Any) -> bytes:
    """Encode values identically for recovery checksums."""
    return json.dumps(
        value,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode()


def _encode_state(
    entries: dict[tuple[CLIName, Scope], SwapEntry],
    transaction: dict[str, t.Any] | None,
) -> bytes:
    """Encode one strict, versioned, checksummed recovery document."""
    payload = _state_payload(entries, transaction)
    document = {**payload, "checksum": _digest(_canonical_json(payload))}
    return (json.dumps(document, indent=2, ensure_ascii=False) + "\n").encode()


def _parse_transaction(raw: t.Any) -> dict[str, t.Any] | None:
    """Validate one pending transaction without interpreting its bytes."""
    if raw is None:
        return None
    if not isinstance(raw, dict) or set(raw) != {"id", "kind", "changes"}:
        _invalid("invalid pending transaction")
    identifier = raw["id"]
    if (
        not isinstance(identifier, str)
        or re.fullmatch(r"[0-9a-f]{32}", identifier) is None
        or raw["kind"] not in {"apply", "revert"}
        or not isinstance(raw["changes"], list)
        or not raw["changes"]
    ):
        _invalid("invalid pending transaction")
    fields = {
        "key",
        "config_path",
        "target_path",
        "before_sha256",
        "before_mode",
        "before_binding",
        "after_sha256",
        "after_mode",
        "backup_path",
        "backup_binding",
    }
    keys: set[str] = set()
    for change in raw["changes"]:
        if not isinstance(change, dict) or set(change) != fields:
            _invalid("invalid pending change")
        key = _required_string(change, "key")
        config_path = pathlib.Path(_required_string(change, "config_path"))
        target_path = pathlib.Path(_required_string(change, "target_path"))
        backup_path = pathlib.Path(_required_string(change, "backup_path"))
        before = _parse_path_binding(change["before_binding"])
        backup_raw = change["backup_binding"]
        backup = None if backup_raw is None else _parse_path_binding(backup_raw)
        if (
            _parse_state_key(key) is None
            or key in keys
            or not config_path.is_absolute()
            or not target_path.is_absolute()
            or not backup_path.is_absolute()
            or before.resolved != str(target_path)
            or before.mode != _required_mode(change, "before_mode")
            or (backup is not None and backup.mode != 0o600)
        ):
            _invalid("invalid pending change")
        _required_digest(change, "before_sha256")
        _required_digest(change, "after_sha256")
        _required_mode(change, "after_mode")
        keys.add(key)
    return raw


def _decode_state(
    raw: bytes,
) -> tuple[
    dict[tuple[CLIName, Scope], SwapEntry],
    dict[str, t.Any] | None,
]:
    """Validate and decode one recovery document."""
    document = json.loads(raw)
    if not isinstance(document, dict) or set(document) != {
        "version",
        "entries",
        "transaction",
        "checksum",
    }:
        _invalid("invalid document shape")
    if document["version"] != STATE_VERSION:
        _invalid("unsupported version")
    checksum = document["checksum"]
    if not isinstance(checksum, str) or _DIGEST_PATTERN.fullmatch(checksum) is None:
        _invalid("invalid checksum")
    payload = {key: value for key, value in document.items() if key != "checksum"}
    if not secrets.compare_digest(checksum, _digest(_canonical_json(payload))):
        _invalid("checksum mismatch")
    raw_entries = document["entries"]
    transaction = _parse_transaction(document["transaction"])
    if not isinstance(raw_entries, dict):
        _invalid("invalid entries")
    entries: dict[tuple[CLIName, Scope], SwapEntry] = {}
    sequences: set[int] = set()
    for key, value in raw_entries.items():
        parsed_key = _parse_state_key(key)
        parsed_entry = _parse_state_entry(value) if isinstance(value, dict) else None
        if (
            parsed_key is None
            or parsed_entry is None
            or parsed_entry.seq_no in sequences
        ):
            _invalid(f"invalid entry {key!r}")
        entries[parsed_key] = parsed_entry
        sequences.add(parsed_entry.seq_no)
    return entries, transaction


def _load_state_snapshot(
    *,
    strict: bool,
    allow_transaction: bool = False,
) -> _StateSnapshot:
    """Read recovery state while binding its bytes and physical identity."""
    if not os.path.lexists(STATE_FILE):
        return _StateSnapshot(
            entries={},
            transaction=None,
            raw=None,
            binding=None,
            missing=_capture_missing_binding(STATE_FILE),
        )
    try:
        raw, binding = _read_bound_file(STATE_FILE)
        if binding.mode != 0o600:
            _invalid("state mode is not 0600")
        entries, transaction = _decode_state(raw)
        if transaction is not None and not allow_transaction:
            _invalid("an interrupted transaction requires recovery")
        return _StateSnapshot(
            entries=entries,
            transaction=transaction,
            raw=raw,
            binding=binding,
            missing=None,
        )
    except (OSError, ValueError) as exc:
        message = f"swap state unreadable ({STATE_FILE}): {exc}"
        if strict:
            raise SwapStateError(message) from exc
        print(message, file=sys.stderr)
        return _StateSnapshot({}, None, None, None, None)


def load_state(*, strict: bool = False) -> dict[tuple[CLIName, Scope], SwapEntry]:
    """Return authenticated stable recovery entries, or an empty mapping."""
    return _load_state_snapshot(strict=strict).entries


@contextlib.contextmanager
def _state_lock() -> t.Iterator[None]:
    """Serialize config mutations that share the swap state file."""
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    fd = os.open(STATE_DIR / "state.lock", os.O_RDWR | os.O_CREAT, 0o600)
    with os.fdopen(fd, "a+b") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        yield


def _validate_state_snapshot(snapshot: _StateSnapshot) -> None:
    """Require recovery state to match its planning observation."""
    if snapshot.binding is not None and snapshot.raw is not None:
        _validate_bound_contents(STATE_FILE, snapshot.raw, snapshot.binding)
    elif snapshot.missing is not None:
        _validate_missing_binding(STATE_FILE, snapshot.missing)
    else:
        _unsafe_state("swap state has no authenticated identity")


def save_state(
    entries: dict[tuple[CLIName, Scope], SwapEntry],
    *,
    transaction: dict[str, t.Any] | None = None,
    expected: _StateSnapshot | None = None,
) -> _StateSnapshot:
    """Write recovery state atomically and return its new binding."""
    if expected is not None:
        _validate_state_snapshot(expected)
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    encoded = _encode_state(entries, transaction)
    atomic_write(STATE_FILE, encoded)
    raw, binding = _read_bound_file(STATE_FILE)
    if raw != encoded or binding.mode != 0o600:
        _unsafe("recovery state changed while it was written")
    decoded_entries, decoded_transaction = _decode_state(raw)
    return _StateSnapshot(
        decoded_entries,
        decoded_transaction,
        raw,
        binding,
        None,
    )


# ---------------------------------------------------------------------------
# Detection
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class Presence:
    """Detection outcome for a CLI: binary on PATH and config file present."""

    cli: CLIName
    binary_found: bool
    config_found: bool

    @property
    def present(self) -> bool:
        """Return True only when both the binary and the config file were found."""
        return self.binary_found and self.config_found


def detect_clis() -> list[Presence]:
    """Probe all supported CLIs and return their detection results."""
    return [
        Presence(
            cli=info.name,
            binary_found=shutil.which(info.binary) is not None,
            config_found=info.config_path.exists(),
        )
        for info in CLIS.values()
    ]


def present_clis() -> list[CLIName]:
    """Return the list of CLIs that have both a binary and a config present."""
    return [p.cli for p in detect_clis() if p.present]


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------


def cmd_detect(args: argparse.Namespace) -> int:
    """Print detection results for every supported CLI."""
    for p in detect_clis():
        flag = "yes" if p.present else " no"
        extra = []
        if not p.binary_found:
            extra.append("binary missing")
        if not p.config_found:
            extra.append(f"config missing: {CLIS[p.cli].config_path}")
        if p.cli == "pi" and not PI_ADAPTER_DIR.is_dir():
            extra.append(PI_ADAPTER_HINT)
        suffix = f"  ({', '.join(extra)})" if extra else ""
        print(f"  [{flag}] {p.cli:<{_CLI_COLUMN}}{suffix}")
    return 0


def cmd_status(args: argparse.Namespace) -> int:
    """Print the current MCP server entry per detected CLI.

    For Claude, prints separate lines for the user-level fallback
    (``[claude:user]``) and the per-project override
    (``[claude:project]``) when both exist; if only one exists, only
    that line shows. ``args.scope`` (when set) restricts Claude output
    to the matching layer only. Other CLIs print a single line as
    ``[<cli>]`` since their config has no scope concept and ignore
    ``args.scope``.
    """
    try:
        repo = find_checkout(pathlib.Path(args.repo))
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return 1
    server = args.server or resolve_repo_meta(repo)[0]
    scope_filter: Scope | None = args.scope
    for cli in args.cli or present_clis():
        info = CLIS[cli]
        if not info.config_path.exists():
            print(f"[{cli}] (no config at {info.config_path})")
            continue
        # Wrap the read + shape-guarded queries in try/except RuntimeError
        # so a malformed Claude config surfaces as a clean per-CLI error
        # instead of aborting status output for the rest of the CLIs.
        try:
            config = load_config(info)
            if cli == "claude":
                # Lazy reads: skip the get_server call entirely for the
                # filtered-out scope so a malformed projects node doesn't
                # raise when the user only asked about user scope.
                user_spec = (
                    get_server(cli, config, server, repo, scope="user")
                    if scope_filter in (None, "user")
                    else None
                )
                project_spec = (
                    get_server(cli, config, server, repo, scope="project")
                    if scope_filter in (None, "project")
                    else None
                )
                shown = False
                if user_spec is not None:
                    tag = _describe_spec(user_spec, repo)
                    print(
                        f"[claude:user] {server} = {user_spec.command} "
                        f"{' '.join(user_spec.args)}  ({tag})"
                    )
                    shown = True
                if project_spec is not None:
                    tag = _describe_spec(project_spec, repo)
                    print(
                        f"[claude:project] {server} = {project_spec.command} "
                        f"{' '.join(project_spec.args)}  ({tag})"
                    )
                    shown = True
                if not shown:
                    label = f"claude:{scope_filter}" if scope_filter else "claude"
                    print(f"[{label}] no entry for {server!r}")
            else:
                spec = get_server(cli, config, server, repo)
                if spec is None:
                    print(f"[{cli}] no entry for {server!r}")
                    continue
                tag = _describe_spec(spec, repo)
                print(
                    f"[{cli}] {server} = {spec.command} {' '.join(spec.args)}  ({tag})"
                )
        except (RuntimeError, ValueError, OSError) as exc:
            print(f"[{cli}] {exc}", file=sys.stderr)
            continue
    return 0


def _describe_spec(spec: McpServerSpec, repo: pathlib.Path) -> str:
    """Return a short label saying which copy of the server a spec runs."""
    binary = spec.binary_path()
    if binary is None:
        return f"on PATH: {spec.command}"
    if spec.is_under(repo):
        try:
            return f"local build: {binary.resolve().relative_to(repo.resolve())}"
        except ValueError:  # pragma: no cover - is_under just proved otherwise
            return f"local build: {binary}"
    return f"installed: {binary}"


def _points_at(
    current: McpServerSpec, target: McpServerSpec, repo: pathlib.Path
) -> bool:
    """Return True when ``current`` already runs what ``target`` describes.

    Argument equality is part of it, not a detail: the same binary told
    to use a different tmux socket is a different server to talk to.
    """
    del repo
    return current.command == target.command and current.args == target.args


def _resolve_use_request(args: argparse.Namespace) -> _UseRequest:
    """Resolve immutable command inputs before configuration planning."""
    repo = find_checkout(pathlib.Path(args.repo))
    server, default_entry = resolve_repo_meta(repo)
    server = args.server or server
    entry = args.entry or default_entry
    extra_env = dict(args.env or [])
    source = getattr(args, "source", "local")
    if getattr(args, "prefix", None) is not None:
        source = "published"
    if getattr(args, "build_dir", None) is not None:
        source = "local"
    if source == "published":
        binary = find_published_binary(entry, getattr(args, "prefix", None))
    else:
        binary = find_built_binary(repo, entry, getattr(args, "build_dir", None))
    targets = tuple(dict.fromkeys(args.cli or present_clis()))
    if not targets:
        _abort("no CLIs detected — nothing to do")
    return _UseRequest(
        repo=repo,
        server=server,
        spec=build_binary_spec(binary, getattr(args, "socket", None), extra_env),
        source=source,
        targets=targets,
        extra_env=extra_env,
    )


def _entry_config_path(
    key: tuple[CLIName, Scope],
    entry: SwapEntry,
) -> pathlib.Path:
    """Return the configured logical path only when it matches recovery."""
    cli, _scope = key
    info = CLIS[cli]
    if str(_absolute_logical(info.config_path)) != str(
        _absolute_logical(pathlib.Path(entry.config_path))
    ):
        _unsafe_state(f"{_state_key(*key)} config path changed")
    return info.config_path


def _authenticate_current(
    key: tuple[CLIName, Scope],
    entry: SwapEntry,
) -> tuple[bytes, _PathBinding]:
    """Authenticate the currently visible top recovery layer."""
    config_path = _entry_config_path(key, entry)
    config, config_binding = _read_bound_file(config_path)
    if (
        config_binding != entry.config_binding
        or config_binding.resolved != entry.target_path
        or config_binding.mode != entry.expected_mode
        or _digest(config) != entry.expected_sha256
    ):
        _unsafe_state(f"{_state_key(*key)} configuration changed after the swap")
    return config, config_binding


def _authenticate_backup(
    key: tuple[CLIName, Scope],
    entry: SwapEntry,
) -> tuple[bytes, _PathBinding]:
    """Authenticate one immutable original-byte recovery backup."""
    backup_path = pathlib.Path(entry.backup_path)
    backup, backup_binding = _read_bound_file(backup_path)
    if (
        backup_binding != entry.backup_binding
        or backup_binding.mode != 0o600
        or _digest(backup) != entry.original_sha256
    ):
        _unsafe_state(f"{_state_key(*key)} backup changed after the swap")
    return backup, backup_binding


def _recovery_groups(
    entries: dict[tuple[CLIName, Scope], SwapEntry],
) -> dict[str, list[tuple[CLIName, Scope]]]:
    """Group recovery keys by logical config in descending LIFO order."""
    groups: dict[str, list[tuple[CLIName, Scope]]] = {}
    for key, entry in entries.items():
        logical = str(_absolute_logical(_entry_config_path(key, entry)))
        groups.setdefault(logical, []).append(key)
    for keys in groups.values():
        keys.sort(key=lambda candidate: entries[candidate].seq_no, reverse=True)
    return groups


def _authenticate_recovery(
    state: _StateSnapshot,
) -> tuple[
    dict[tuple[CLIName, Scope], tuple[bytes, _PathBinding]],
    dict[str, tuple[bytes, _PathBinding]],
    dict[str, list[tuple[CLIName, Scope]]],
    list[_PathClaim],
]:
    """Authenticate every backup and each physical config's top layer."""
    groups = _recovery_groups(state.entries)
    backups = {
        key: _authenticate_backup(key, entry) for key, entry in state.entries.items()
    }
    current: dict[str, tuple[bytes, _PathBinding]] = {}
    claims = [_state_claim(state)]
    for logical, keys in groups.items():
        top = keys[0]
        current[logical] = _authenticate_current(top, state.entries[top])
        for index, key in enumerate(keys[:-1]):
            entry = state.entries[key]
            below = state.entries[keys[index + 1]]
            if (
                _digest(backups[key][0]) != below.expected_sha256
                or entry.original_mode != below.expected_mode
                or entry.target_path != below.target_path
            ):
                _unsafe_state(f"{_state_key(*key)} breaks the recovery chain")
        claims.append(
            _binding_claim(_state_key(*top), "configuration", current[logical][1])
        )
    claims.extend(
        _binding_claim(_state_key(*key), "backup", binding)
        for key, (_data, binding) in backups.items()
    )
    _validate_distinct_claims(claims)
    return backups, current, groups, claims


def _state_claim(snapshot: _StateSnapshot) -> _PathClaim:
    """Return the state file's existing or planned physical claim."""
    if snapshot.binding is not None:
        return _binding_claim("swap", "state", snapshot.binding)
    if snapshot.missing is not None:
        return _missing_claim("swap", "state", snapshot.missing)
    _unsafe_state("swap state has no authenticated destination")


def _prepare_apply_plan(
    request: _UseRequest,
    args: argparse.Namespace,
    timestamp: str,
    *,
    emit: bool,
) -> _ApplyPlan:
    """Read, render, and bind every selected config and recovery artifact."""
    state = _load_state_snapshot(strict=True)
    authenticated, current_configs, groups, claims = _authenticate_recovery(state)
    claimed_configs = {
        logical: binding for logical, (_data, binding) in current_configs.items()
    }

    prepared: list[_PreparedTarget] = []
    reserved_backups: set[pathlib.Path] = set()
    planned_keys = set(state.entries)
    selected_logicals: set[str] = set()
    for cli in request.targets:
        scope = _normalize_scope(cli, args.scope)
        key = (cli, scope)
        label = f"{cli}:{scope}" if cli == "claude" else cli
        info = CLIS[cli]
        if not os.path.lexists(info.config_path):
            _unsafe(f"[{label}] config not found at {info.config_path}")
        current_bytes, config_binding = _read_bound_file(info.config_path)
        logical = str(_absolute_logical(info.config_path))
        if logical in selected_logicals:
            _unsafe(f"[{label}] duplicates a selected configuration")
        selected_logicals.add(logical)
        if logical not in claimed_configs:
            claimed_configs[logical] = config_binding
            claims.append(_binding_claim(label, "configuration", config_binding))
        elif claimed_configs[logical] != config_binding or (
            logical in current_configs and current_configs[logical][0] != current_bytes
        ):
            _unsafe(f"[{label}] configuration identity is inconsistent")
        target_info = dataclasses.replace(
            info,
            config_path=pathlib.Path(config_binding.resolved),
        )
        config = _parse_config_bytes(target_info, current_bytes)
        existing = get_server(
            cli,
            config,
            request.server,
            request.repo,
            scope=scope,
        )
        if (
            existing
            and _points_at(existing, request.spec, request.repo)
            and all(
                existing.env.get(name) == value
                for name, value in request.extra_env.items()
            )
        ):
            if emit:
                print(
                    f"[{label}] already "
                    f"{_describe_spec(request.spec, request.repo)} — no change"
                )
            continue
        base_env = dict(existing.env) if existing else {}
        base_env.update(request.extra_env)
        cli_spec = (
            dataclasses.replace(request.spec, env=base_env)
            if existing or request.extra_env
            else request.spec
        )
        action = set_server(
            cli,
            config,
            request.server,
            cli_spec,
            request.repo,
            scope=scope,
        )
        new_bytes = dump_config_bytes(info, config, original=current_bytes)
        item = _PreparedTarget(
            cli=cli,
            scope=scope,
            label=label,
            info=info,
            target_path=pathlib.Path(config_binding.resolved),
            original_bytes=current_bytes,
            new_bytes=new_bytes,
            action=action,
            config_binding=config_binding,
            prior_entry=state.entries.get(key),
        )
        if item.prior_entry is not None:
            if groups[logical][0] != key:
                _unsafe_state(f"[{label}] is not the active recovery layer")
            item.backup_bytes, item.backup_binding = authenticated[key]
            item.backup_path = pathlib.Path(item.prior_entry.backup_path)
        else:
            suffix = f"{BACKUP_SUFFIX_PREFIX}{timestamp}"
            if cli == "claude":
                suffix += f"-{scope}"
            base = info.config_path.with_suffix(info.config_path.suffix + suffix)
            item.backup_path = _next_backup_path(base, reserved_backups)
            _check_backup_destination(item.backup_path)
            item.backup_missing = _capture_missing_binding(item.backup_path)
            reserved_backups.add(item.backup_path)
            claims.append(_missing_claim(label, "backup", item.backup_missing))
        prepared.append(item)
        planned_keys.add(key)

    _validate_distinct_claims(claims)
    if len(planned_keys) != len(state.entries) + sum(
        (item.cli, item.scope) not in state.entries for item in prepared
    ):
        _unsafe_state("recovery keys are not unique")
    return _ApplyPlan(request, state, prepared, timestamp)


def _pending_apply_document(
    plan: _ApplyPlan,
    transaction_id: str,
) -> dict[str, t.Any]:
    """Describe both sides of an apply before any config becomes visible."""
    return {
        "id": transaction_id,
        "kind": "apply",
        "changes": [
            {
                "key": _state_key(item.cli, item.scope),
                "config_path": str(item.info.config_path),
                "target_path": str(item.target_path),
                "before_sha256": _digest(item.original_bytes),
                "before_mode": item.config_binding.mode,
                "before_binding": dataclasses.asdict(item.config_binding),
                "after_sha256": _digest(item.new_bytes),
                "after_mode": item.config_binding.mode,
                "backup_path": str(item.backup_path),
                "backup_binding": (
                    dataclasses.asdict(item.backup_binding)
                    if item.backup_binding is not None
                    else None
                ),
            }
            for item in plan.prepared
        ],
    }


def _validate_apply_plan(plan: _ApplyPlan, state: _StateSnapshot) -> None:
    """Recheck every config, backup, and state immediately before writes."""
    _validate_state_snapshot(state)
    for item in plan.prepared:
        _validate_bound_contents(
            item.info.config_path,
            item.original_bytes,
            item.config_binding,
        )
        if item.backup_binding is not None and item.backup_bytes is not None:
            _validate_bound_contents(
                pathlib.Path(item.backup_path),
                item.backup_bytes,
                item.backup_binding,
            )


def _rollback_applied_configs(
    plan: _ApplyPlan,
    applied: list[_PreparedTarget],
) -> dict[tuple[CLIName, Scope], _PathBinding]:
    """Restore applied configs in reverse and return their new bindings."""
    for item in applied:
        if item.state_entry is None:
            _unsafe(f"[{item.label}] has no authenticated rollback identity")
        _validate_bound_contents(
            item.info.config_path,
            item.new_bytes,
            item.state_entry.config_binding,
        )
    restored: dict[tuple[CLIName, Scope], _PathBinding] = {}
    for item in reversed(applied):
        assert item.state_entry is not None
        _validate_bound_contents(
            item.info.config_path,
            item.new_bytes,
            item.state_entry.config_binding,
        )
        atomic_write(item.target_path, item.original_bytes)
        data, binding = _read_bound_file(item.info.config_path)
        if data != item.original_bytes or binding.mode != item.config_binding.mode:
            _unsafe(f"[{item.label}] rollback did not restore the config")
        restored[(item.cli, item.scope)] = binding
    return restored


def _restore_apply_state(
    plan: _ApplyPlan,
    state: _StateSnapshot,
    restored: dict[tuple[CLIName, Scope], _PathBinding],
) -> None:
    """Remove new backups and restore stable recovery state after rollback."""
    entries = dict(plan.state.entries)
    for key, binding in restored.items():
        item = next(
            candidate
            for candidate in plan.prepared
            if (candidate.cli, candidate.scope) == key
        )
        logical = str(_absolute_logical(item.info.config_path))
        matching = [
            candidate
            for candidate, entry in entries.items()
            if str(_absolute_logical(pathlib.Path(entry.config_path))) == logical
        ]
        if matching:
            top = max(matching, key=lambda candidate: entries[candidate].seq_no)
            entries[top] = dataclasses.replace(
                entries[top],
                config_binding=binding,
                expected_sha256=_digest(item.original_bytes),
                expected_mode=binding.mode,
                target_path=binding.resolved,
            )
    final_state = _encode_state(entries, None) if entries else None
    new_items = [item for item in plan.prepared if item.prior_entry is None]
    transaction = state.transaction
    if transaction is None:
        _unsafe_state("apply recovery lost its pending transaction")
    pending_state = _encode_state(
        plan.state.entries,
        _pending_apply_document(plan, transaction["id"]),
    )
    committed_entries = dict(plan.state.entries)
    committed_entries.update(
        {
            (item.cli, item.scope): item.state_entry
            for item in plan.prepared
            if item.state_entry is not None
        }
    )
    committed_state = _encode_state(committed_entries, None)
    try:
        for item in reversed(new_items):
            if item.backup_binding is None or item.backup_bytes is None:
                continue
            _remove_bound_file(
                item.backup_path,
                item.backup_bytes,
                item.backup_binding,
            )
        if entries:
            save_state(entries, expected=state)
        else:
            _remove_state_snapshot(state)
    except Exception:
        state_error: Exception | None = None
        try:
            state = _restore_known_state(
                state,
                {final_state, pending_state, committed_state},
            )
        except Exception as exc:
            state_error = exc
        for item in new_items:
            if item.backup_bytes is None or item.backup_binding is None:
                continue
            if os.path.lexists(item.backup_path):
                _validate_bound_contents(
                    item.backup_path,
                    item.backup_bytes,
                    item.backup_binding,
                )
                continue
            written = write_new_backup(item.backup_path, item.backup_bytes)
            if _absolute_logical(written) != _absolute_logical(item.backup_path):
                _unsafe(f"[{item.label}] backup could not be recreated in place")
            item.backup_bytes, item.backup_binding = _read_bound_file(item.backup_path)
            if item.backup_binding.mode != 0o600:
                _unsafe(f"[{item.label}] recreated backup is not owned")
        if state_error is not None:
            raise state_error from None
        restored_transaction = state.transaction
        if restored_transaction is None:
            _unsafe_state("apply recovery lost its pending transaction")
        save_state(
            entries,
            transaction=_pending_apply_document(plan, restored_transaction["id"]),
            expected=state,
        )
        raise


def _rollback_apply(
    plan: _ApplyPlan,
    applied: list[_PreparedTarget],
    state: _StateSnapshot,
    cause: Exception,
) -> int:
    """Reverse a failed apply, retaining pending evidence if recovery fails."""
    failures = [str(cause)]
    try:
        restored = _rollback_applied_configs(plan, applied)
        _restore_apply_state(plan, state, restored)
    except Exception as rollback_error:
        failures.append(f"rollback failed: {rollback_error}")
    print("swap failed: " + "; ".join(failures), file=sys.stderr)
    return 1


def _apply_plan(plan: _ApplyPlan) -> int:
    """Publish one fully planned apply transaction."""
    if not plan.prepared:
        return 0
    transaction_id = secrets.token_hex(16)
    state = save_state(
        plan.state.entries,
        transaction=_pending_apply_document(plan, transaction_id),
        expected=plan.state,
    )
    try:
        for item in plan.prepared:
            if item.backup_binding is not None:
                continue
            assert item.backup_path is not None
            assert item.backup_missing is not None
            _validate_missing_binding(item.backup_path, item.backup_missing)
            written = write_new_backup(item.backup_path, item.original_bytes)
            if _absolute_logical(written) != _absolute_logical(item.backup_path):
                _unsafe(f"[{item.label}] backup destination changed")
            item.backup_bytes, item.backup_binding = _read_bound_file(written)
            if (
                item.backup_bytes != item.original_bytes
                or item.backup_binding.mode != 0o600
            ):
                _unsafe(f"[{item.label}] backup changed while it was written")
        state = save_state(
            plan.state.entries,
            transaction=_pending_apply_document(plan, transaction_id),
            expected=state,
        )
        _validate_apply_plan(plan, state)
    except Exception as exc:
        return _rollback_apply(plan, [], state, exc)

    applied: list[_PreparedTarget] = []
    next_entries = dict(plan.state.entries)
    for item in plan.prepared:
        try:
            _validate_bound_contents(
                item.info.config_path,
                item.original_bytes,
                item.config_binding,
            )
            atomic_write(item.target_path, item.new_bytes)
            _revalidate(
                dataclasses.replace(
                    item.info,
                    config_path=pathlib.Path(item.target_path),
                )
            )
            current, binding = _read_bound_file(item.info.config_path)
            if current != item.new_bytes or binding.mode != item.config_binding.mode:
                _unsafe(f"[{item.label}] config changed while it was written")
            assert item.backup_path is not None
            assert item.backup_bytes is not None
            assert item.backup_binding is not None
            prior = item.prior_entry
            item.state_entry = SwapEntry(
                config_path=str(item.info.config_path),
                backup_path=str(item.backup_path),
                server=plan.request.server,
                action=item.action,
                swapped_at=prior.swapped_at if prior else plan.timestamp,
                seq_no=(
                    prior.seq_no
                    if prior
                    else max(
                        (entry.seq_no for entry in next_entries.values()), default=-1
                    )
                    + 1
                ),
                target_path=binding.resolved,
                original_sha256=(
                    prior.original_sha256 if prior else _digest(item.backup_bytes)
                ),
                original_mode=(
                    prior.original_mode if prior else item.config_binding.mode
                ),
                expected_sha256=_digest(current),
                expected_mode=binding.mode,
                config_binding=binding,
                backup_binding=item.backup_binding,
            )
            next_entries[(item.cli, item.scope)] = item.state_entry
            applied.append(item)
        except Exception as exc:  # noqa: PERF203 - only eight configs are possible
            try:
                current, _binding = _read_bound_file(item.info.config_path)
            except OSError:
                current = b""
            if current == item.new_bytes and item not in applied:
                applied.append(item)
            return _rollback_apply(plan, applied, state, exc)

    try:
        state = save_state(next_entries, expected=state)
    except Exception as exc:
        return _rollback_apply(plan, applied, state, exc)
    del state
    for item in plan.prepared:
        assert item.backup_path is not None
        note = (
            f"pre-swap backup kept: {item.backup_path}"
            if item.prior_entry is not None
            else f"backup: {item.backup_path}"
        )
        print(f"[{item.label}] {item.action}; {note}")
    return 0


def _show_apply_plan(plan: _ApplyPlan) -> None:
    """Print prepared diffs without touching server or filesystem state."""
    for item in plan.prepared:
        print(f"--- {item.info.config_path} (current)")
        print(f"+++ {item.info.config_path} (proposed)")
        diff = difflib.unified_diff(
            item.original_bytes.decode(errors="replace").splitlines(keepends=True),
            item.new_bytes.decode(errors="replace").splitlines(keepends=True),
            lineterm="",
        )
        sys.stdout.writelines(diff)


def cmd_use_local(args: argparse.Namespace) -> int:
    """Plan every selected config before starting or mutating anything."""
    try:
        request = _resolve_use_request(args)
        print(f"{request.source}: {request.spec.command}", file=sys.stderr)
        hint = _naming_hint(request.repo, request.server)
        if hint:
            print(hint, file=sys.stderr)
        timestamp = time.strftime("%Y%m%d%H%M%S")
        plan = _prepare_apply_plan(request, args, timestamp, emit=True)
        if args.dry_run:
            _show_apply_plan(plan)
            return 0
        if not args.no_preflight:
            print(
                f"preflight: {request.spec.command} {' '.join(request.spec.args)}",
                file=sys.stderr,
            )
            failure = preflight_spec(request.spec)
            if failure is not None:
                print(f"preflight failed, nothing written:\n{failure}", file=sys.stderr)
                return 1
        with _state_lock():
            plan = _prepare_apply_plan(request, args, timestamp, emit=False)
            return _apply_plan(plan)
    except SwapStateError as exc:
        print(exc, file=sys.stderr)
        return 1
    except (RuntimeError, ValueError, OSError) as exc:
        print(exc, file=sys.stderr)
        return 1


def _revalidate(info: CLIInfo) -> None:
    """Re-parse the file after writing; raise on failure."""
    load_config(info)


def _select_revert_keys(
    entries: dict[tuple[CLIName, Scope], SwapEntry],
    args: argparse.Namespace,
    *,
    emit: bool,
) -> set[tuple[CLIName, Scope]]:
    """Resolve requested recovery keys while preserving empty-target behavior."""
    clients = (
        list(dict.fromkeys(args.cli)) if args.cli else sorted({k[0] for k in entries})
    )
    if not clients:
        _abort("no recorded swaps — nothing to revert")
    selected: set[tuple[CLIName, Scope]] = set()
    for cli in clients:
        scopes = (
            (_normalize_scope(cli, args.scope),)
            if args.scope is not None
            else ALL_SCOPES
        )
        matches = {key for key in entries if key[0] == cli and key[1] in scopes}
        if not matches and emit:
            label = f"{cli}:{args.scope}" if args.scope and cli == "claude" else cli
            print(f"[{label}] no state entry — skip")
        selected.update(matches)
    return selected


def _recovery_label(key: tuple[CLIName, Scope]) -> str:
    """Render a scoped label only for Claude's layered config."""
    cli, scope = key
    return f"{cli}:{scope}" if cli == "claude" else cli


def _prepare_revert_plan(
    args: argparse.Namespace,
    *,
    emit: bool,
) -> _RevertPlan:
    """Authenticate all recovery and compose one write per physical config."""
    state = _load_state_snapshot(strict=True)
    backups, current, groups, _claims = _authenticate_recovery(state)
    if state.binding is None:
        _unsafe_state("recorded recovery has no state-file identity")
    _check_bound_mutation(STATE_FILE, state.binding)
    selected = _select_revert_keys(state.entries, args, emit=emit)
    targets: list[_RevertTarget] = []
    for logical, keys in groups.items():
        selected_keys = [key for key in keys if key in selected]
        if not selected_keys:
            continue
        if keys[: len(selected_keys)] != selected_keys:
            _unsafe_state(f"{logical} has a newer recovery layer")
        current_bytes, current_binding = current[logical]
        _check_bound_mutation(pathlib.Path(logical), current_binding)
        layers: list[_RecoveryLayer] = []
        for key in selected_keys:
            entry = state.entries[key]
            backup_bytes, backup_binding = backups[key]
            _check_bound_mutation(pathlib.Path(entry.backup_path), backup_binding)
            layers.append(
                _RecoveryLayer(
                    key=key,
                    label=_recovery_label(key),
                    entry=entry,
                    backup_path=pathlib.Path(entry.backup_path),
                    backup_bytes=backup_bytes,
                    backup_binding=backup_binding,
                )
            )
        oldest = layers[-1]
        targets.append(
            _RevertTarget(
                info=CLIS[keys[0][0]],
                config_path=pathlib.Path(state.entries[keys[0]].config_path),
                target_path=pathlib.Path(current_binding.resolved),
                current_bytes=current_bytes,
                current_binding=current_binding,
                layers=layers,
                restored_bytes=oldest.backup_bytes,
                restored_mode=oldest.entry.original_mode,
            )
        )
    targets.sort(key=lambda target: target.layers[0].entry.seq_no, reverse=True)
    return _RevertPlan(state, targets, selected)


def _pending_revert_document(
    plan: _RevertPlan,
    transaction_id: str,
) -> dict[str, t.Any]:
    """Describe every physical revert before its first config write."""
    return {
        "id": transaction_id,
        "kind": "revert",
        "changes": [
            {
                "key": _state_key(*target.layers[0].key),
                "config_path": str(target.config_path),
                "target_path": str(target.target_path),
                "before_sha256": _digest(target.current_bytes),
                "before_mode": target.current_binding.mode,
                "before_binding": dataclasses.asdict(target.current_binding),
                "after_sha256": _digest(target.restored_bytes),
                "after_mode": target.restored_mode,
                "backup_path": str(target.layers[-1].backup_path),
                "backup_binding": dataclasses.asdict(target.layers[-1].backup_binding),
            }
            for target in plan.targets
        ],
    }


def _validate_revert_plan(plan: _RevertPlan, state: _StateSnapshot) -> None:
    """Recheck selected config, backup, and state identities before writes."""
    _validate_state_snapshot(state)
    for target in plan.targets:
        _validate_bound_contents(
            target.config_path,
            target.current_bytes,
            target.current_binding,
        )
        for layer in target.layers:
            _validate_bound_contents(
                layer.backup_path,
                layer.backup_bytes,
                layer.backup_binding,
            )


def _publish_config(
    target: _RevertTarget,
    data: bytes,
    mode: int,
) -> _PathBinding:
    """Atomically publish and authenticate one planned config image."""
    atomic_write(target.target_path, data)
    target.target_path.chmod(mode)
    with target.target_path.open("rb") as handle:
        os.fsync(handle.fileno())
    _revalidate(dataclasses.replace(target.info, config_path=target.target_path))
    current, binding = _read_bound_file(target.config_path)
    if current != data or binding.mode != mode:
        _unsafe(f"[{target.layers[0].label}] config changed while it was written")
    return binding


def _selected_revert_layers(plan: _RevertPlan) -> list[_RecoveryLayer]:
    """Return selected recovery layers in global LIFO order."""
    return sorted(
        (layer for target in plan.targets for layer in target.layers),
        key=lambda layer: layer.entry.seq_no,
        reverse=True,
    )


def _next_recovery_entries(
    plan: _RevertPlan,
) -> dict[tuple[CLIName, Scope], SwapEntry]:
    """Remove selected layers and bind each newly visible top layer."""
    entries = {
        key: entry
        for key, entry in plan.state.entries.items()
        if key not in plan.selected
    }
    for target in plan.targets:
        if target.restored_binding is None:
            _unsafe_state("reverted config has no authenticated identity")
        remaining = [
            (key, entry)
            for key, entry in entries.items()
            if str(_absolute_logical(pathlib.Path(entry.config_path)))
            == str(_absolute_logical(target.config_path))
        ]
        if not remaining:
            continue
        key, entry = max(remaining, key=lambda pair: pair[1].seq_no)
        if (
            entry.expected_sha256 != _digest(target.restored_bytes)
            or entry.expected_mode != target.restored_binding.mode
        ):
            _unsafe_state(f"{_state_key(*key)} does not match the restored layer")
        entries[key] = dataclasses.replace(
            entry,
            target_path=target.restored_binding.resolved,
            config_binding=target.restored_binding,
        )
    return entries


def _recreate_removed_backups(
    layers: list[_RecoveryLayer],
    entries: dict[tuple[CLIName, Scope], SwapEntry],
) -> None:
    """Recreate only proven-owned backups removed by a failed transaction."""
    for layer in layers:
        if os.path.lexists(layer.backup_path):
            _validate_bound_contents(
                layer.backup_path,
                layer.backup_bytes,
                layer.backup_binding,
            )
            continue
        missing = _capture_missing_binding(layer.backup_path)
        _validate_missing_binding(layer.backup_path, missing)
        written = write_new_backup(layer.backup_path, layer.backup_bytes)
        if _absolute_logical(written) != _absolute_logical(layer.backup_path):
            _unsafe(f"[{layer.label}] backup could not be recreated in place")
        data, binding = _read_bound_file(layer.backup_path)
        if data != layer.backup_bytes or binding.mode != 0o600:
            _unsafe(f"[{layer.label}] recreated backup is not owned")
        entries[layer.key] = dataclasses.replace(
            entries[layer.key],
            backup_binding=binding,
        )


def _roll_revert_forward(
    plan: _RevertPlan,
    applied: list[_RevertTarget],
    state: _StateSnapshot,
    entries: dict[tuple[CLIName, Scope], SwapEntry],
    cause: Exception,
) -> int:
    """Undo a failed revert and retain authenticated recovery for retry."""
    failures = [str(cause)]
    try:
        for target in applied:
            if target.restored_binding is None:
                _unsafe_state("reverted config has no authenticated identity")
            _validate_bound_contents(
                target.config_path,
                target.restored_bytes,
                target.restored_binding,
            )
        for target in reversed(applied):
            assert target.restored_binding is not None
            _validate_bound_contents(
                target.config_path,
                target.restored_bytes,
                target.restored_binding,
            )
            binding = _publish_config(
                target,
                target.current_bytes,
                target.current_binding.mode,
            )
            top = target.layers[0].key
            entries[top] = dataclasses.replace(
                entries[top],
                target_path=binding.resolved,
                config_binding=binding,
            )
        _authenticate_recovery(dataclasses.replace(state, entries=entries))
        save_state(entries, expected=state)
    except Exception as rollback_error:
        failures.append(f"rollback failed: {rollback_error}")
    print("revert failed: " + "; ".join(failures), file=sys.stderr)
    return 1


def _apply_revert_plan(plan: _RevertPlan) -> int:
    """Publish one all-selected revert and clean recovery transactionally."""
    if not plan.targets:
        return 0
    transaction_id = secrets.token_hex(16)
    state = save_state(
        plan.state.entries,
        transaction=_pending_revert_document(plan, transaction_id),
        expected=plan.state,
    )
    try:
        _validate_revert_plan(plan, state)
    except Exception as exc:
        return _roll_revert_forward(plan, [], state, dict(plan.state.entries), exc)

    applied: list[_RevertTarget] = []
    for target in plan.targets:
        try:
            _validate_bound_contents(
                target.config_path,
                target.current_bytes,
                target.current_binding,
            )
            target.restored_binding = _publish_config(
                target,
                target.restored_bytes,
                target.restored_mode,
            )
            applied.append(target)
        except Exception as exc:  # noqa: PERF203 - at most eight configs are possible
            try:
                current, binding = _read_bound_file(target.config_path)
            except OSError:
                current, binding = b"", None
            if current == target.restored_bytes and binding is not None:
                target.restored_binding = binding
                applied.append(target)
            return _roll_revert_forward(
                plan,
                applied,
                state,
                dict(plan.state.entries),
                exc,
            )

    entries = dict(plan.state.entries)
    layers = _selected_revert_layers(plan)
    next_entries = _next_recovery_entries(plan)
    final_state = _encode_state(next_entries, None) if next_entries else None
    try:
        for layer in layers:
            _remove_bound_file(
                layer.backup_path,
                layer.backup_bytes,
                layer.backup_binding,
            )
        if next_entries:
            save_state(next_entries, expected=state)
        else:
            _remove_state_snapshot(state)
    except Exception as exc:
        try:
            state = _restore_known_state(state, {final_state})
        except Exception as state_error:
            print(f"state recovery failed: {state_error}", file=sys.stderr)
        try:
            _recreate_removed_backups(layers, entries)
        except Exception as recreation_error:
            print(
                f"backup recovery failed: {recreation_error}",
                file=sys.stderr,
            )
        return _roll_revert_forward(plan, applied, state, entries, exc)

    for layer in layers:
        print(f"[{layer.label}] restored from {layer.backup_path}")
    return 0


def _show_revert_plan(plan: _RevertPlan) -> None:
    """Print all authenticated LIFO layers without mutating recovery."""
    for layer in _selected_revert_layers(plan):
        print(
            f"[{layer.label}] would restore "
            f"{layer.entry.target_path} from {layer.backup_path}"
        )


def cmd_revert(args: argparse.Namespace) -> int:
    """Plan all selected recovery before publishing one revert transaction."""
    try:
        plan = _prepare_revert_plan(args, emit=True)
        if args.dry_run:
            _show_revert_plan(plan)
            return 0
        with _state_lock():
            plan = _prepare_revert_plan(args, emit=False)
            return _apply_revert_plan(plan)
    except SwapStateError as exc:
        print(exc, file=sys.stderr)
        return 1
    except (RuntimeError, ValueError, OSError) as exc:
        print(exc, file=sys.stderr)
        return 1


# ---------------------------------------------------------------------------
# doctor — read-only diagnostics
# ---------------------------------------------------------------------------

#: Env vars that, when set, override a CLI's stored subscription/login auth
#: with an API key — a frequent cause of "why is it billing / refusing?"
#: surprises when driving the CLI against a local server. Doctor only reports
#: presence; it never reads the value.
AUTH_ENV_VARS: dict[str, CLIName] = {
    "ANTHROPIC_API_KEY": "claude",
    "OPENAI_API_KEY": "codex",
    "GEMINI_API_KEY": "gemini",
    "GOOGLE_API_KEY": "gemini",
    "XAI_API_KEY": "grok",
    "GROK_API_KEY": "grok",
}


def _env_pair(raw: str) -> tuple[str, str]:
    """Parse a ``KEY=VALUE`` ``--env`` argument, or raise for argparse."""
    key, sep, value = raw.partition("=")
    if not sep or not key:
        msg = f"--env expects KEY=VALUE, got {raw!r}"
        raise argparse.ArgumentTypeError(msg)
    return key, value


def _config_present_clis() -> list[CLIName]:
    """CLIs whose config file exists — enough to *read* entries (no binary needed).

    Distinct from :func:`present_clis`, which also requires the binary on
    ``PATH``. Doctor and the naming hint only inspect config files, so a CLI
    whose binary is absent but whose config is present still has readable
    entries worth surfacing.
    """
    return [cli for cli in ALL_CLIS if CLIS[cli].config_path.exists()]


def _all_server_specs(
    cli: CLIName, config: t.Any, repo: pathlib.Path
) -> dict[str, McpServerSpec]:
    """Enumerate every MCP server entry visible in a CLI's config.

    Spans the scopes a CLI actually keys servers under: Claude's top-level
    user ``mcpServers`` plus this repo's per-project node, and the single
    ``mcpServers`` / ``mcp_servers`` table for the others. Used to detect the
    server-name footgun — the repo registered under a name other than the
    derived default — which a same-name-only lookup misses.
    """
    out: dict[str, McpServerSpec] = {}

    def _add(raw: t.Any) -> None:
        if not isinstance(raw, dict):
            return
        for name, entry in raw.items():
            if not isinstance(entry, dict):
                continue
            out[str(name)] = _spec_from_entry(entry, info=CLIS[cli])

    if cli == "claude":
        _add(_claude_user_servers(config, create=False))
        node = _claude_project_node(config, repo, create=False)
        if node:
            _add(node.get("mcpServers"))
    else:
        _add(_server_map(CLIS[cli], config, create=False))
    return out


def _repo_pointing_names(cli: CLIName, config: t.Any, repo: pathlib.Path) -> list[str]:
    """Server names in this CLI's config that run a binary built in ``repo``."""
    return sorted(
        name
        for name, spec in _all_server_specs(cli, config, repo).items()
        if spec.is_under(repo)
    )


def _naming_hint(repo: pathlib.Path, server: str) -> str | None:
    """Suggest ``--server <name>`` when the repo is registered under another name.

    The derived default (package name minus ``-mcp``) often doesn't match the
    slug the CLIs were actually registered under (e.g. ``tmux`` vs the derived
    ``libtmux``), so a bare run silently operates on a non-existent entry.
    Returns a one-line hint naming the real slug, or ``None`` when the derived
    name is already the registered one (or nothing points here).
    """
    names: set[str] = set()
    server_points = False
    for cli in _config_present_clis():
        try:
            config = load_config(CLIS[cli])
            pointing = _repo_pointing_names(cli, config, repo)
        except (RuntimeError, ValueError, OSError):
            continue
        for name in pointing:
            if name == server:
                server_points = True
            else:
                names.add(name)
    if server_points or not names:
        return None
    pick = min(names)
    return (
        f"note: nothing is registered under server {server!r}, but this repo is "
        f"registered as {sorted(names)} — pass --server {pick} to target it"
    )


def _orphaned_backups(config_path: pathlib.Path) -> list[pathlib.Path]:
    """All ``mcp-swap`` backups sitting next to ``config_path`` (any timestamp)."""
    pattern = config_path.name + BACKUP_SUFFIX_PREFIX + "*"
    return sorted(config_path.parent.glob(pattern))


def cmd_doctor(args: argparse.Namespace) -> int:
    """Report the effective MCP-swap environment without changing anything.

    Read-only. Surfaces the footguns that swap/status don't: the repo
    registered under an unexpected server name, un-reverted swaps and orphaned
    backups accumulating on disk, a state entry whose backup has gone missing
    (so revert would fail), and auth-overriding env vars. It deliberately does
    NOT model each CLI's config-merge behaviour — that is CLI-version-specific
    and lives in documentation, not here.
    """
    try:
        repo = find_checkout(pathlib.Path(args.repo))
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return 1
    server = args.server or resolve_repo_meta(repo)[0]
    print("mcp-swap doctor")
    print(f"  repo:   {repo}")
    print(f"  server: {server}  (derived default; override with --server)")

    print("  entries by CLI:")
    all_repo_names: set[str] = set()
    for cli in _config_present_clis():
        try:
            config = load_config(CLIS[cli])
            specs = _all_server_specs(cli, config, repo)
            pointing = _repo_pointing_names(cli, config, repo)
        except (RuntimeError, ValueError, OSError) as exc:
            print(f"    [{cli}] config unreadable: {exc}")
            continue
        spec = specs.get(server)
        if spec is not None:
            print(f"    [{cli}] {server} = {_describe_spec(spec, repo)}")
        all_repo_names.update(pointing)
        for name in pointing:
            if name != server:
                print(f"    [{cli}] {name} = local: this repo  (other name)")
    if not all_repo_names:
        print("    (no CLI currently points at this repo)")

    if all_repo_names and server not in all_repo_names:
        pick = min(all_repo_names)
        print(
            f"  ! server name mismatch: this repo is registered as "
            f"{sorted(all_repo_names)}, not {server!r} — use --server {pick}"
        )

    state = load_state()
    if state:
        print("  outstanding swaps (un-reverted):")
        for (cli, scope), entry in sorted(state.items(), key=lambda kv: kv[1].seq_no):
            flag = (
                ""
                if pathlib.Path(entry.backup_path).exists()
                else "  ! BACKUP MISSING — revert would fail for this entry"
            )
            print(f"    {cli}:{scope}  swapped_at={entry.swapped_at}{flag}")

    referenced = {e.backup_path for e in state.values()}
    orphans = [
        b
        for info in CLIS.values()
        for b in _orphaned_backups(info.config_path)
        if str(b) not in referenced
    ]
    if orphans:
        total = sum(b.stat().st_size for b in orphans if b.exists())
        print(
            f"  orphaned backups: {len(orphans)} file(s), {total} bytes not tracked "
            "by state — inspect before deleting: an untracked backup can be the "
            "only surviving pre-swap copy of a config"
        )

    auth_hits = [
        (var, cli) for var, cli in AUTH_ENV_VARS.items() if os.environ.get(var)
    ]
    if auth_hits:
        print("  auth-overriding env vars set:")
        for var, cli in auth_hits:
            print(
                f"    ! {var} overrides {cli}'s stored login — prefix with "
                f"`env -u {var}` to use the subscription/OAuth auth instead"
            )
    return 0


# ---------------------------------------------------------------------------
# argparse glue
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    """Construct the ``argparse`` parser for ``mcp_swap``."""
    p = argparse.ArgumentParser(prog="mcp_swap", description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser(
        "detect", help="list installed CLIs and their config presence"
    ).set_defaults(func=cmd_detect)

    ps = sub.add_parser("status", help="show the current MCP server entry per CLI")
    ps.add_argument(
        "--repo",
        default=".",
        help="where to look for the checkout; searched upward (default: .)",
    )
    ps.add_argument("--server", help="MCP server name (default: libtmux)")
    ps.add_argument(
        "--cli", action="append", choices=ALL_CLIS, help="limit to one or more CLIs"
    )
    ps.add_argument(
        "--scope",
        choices=ALL_SCOPES,
        default=None,
        help=(
            "Limit Claude output to one scope: 'user' shows only the "
            "top-level mcpServers fallback, 'project' shows only the "
            "projects.<abs>.mcpServers entry. Without this flag, both "
            "Claude scopes print when both have an entry. No-op for "
            "non-Claude CLIs (their config has no per-project layer)."
        ),
    )
    ps.set_defaults(func=cmd_status)

    pu = sub.add_parser(
        "use-local", help="rewrite configs to run a chosen build of the server"
    )
    pu.add_argument(
        "--repo",
        default=".",
        help="where to look for the checkout; searched upward (default: .)",
    )
    pu.add_argument(
        "--source",
        choices=("local", "published"),
        default="local",
        help=(
            "Which copy of the server to point the CLIs at. 'local' is a "
            "build in this checkout, found under the usual preset build "
            "directories or the one named by --build-dir. 'published' is an "
            "installed copy, from --prefix or from PATH. Default: local."
        ),
    )
    pu.add_argument(
        "--build-dir",
        metavar="DIR",
        help=(
            "Build directory to take the server from, as a path from here "
            "(for example build/cxx-gcc). Implies --source local, and is how "
            "a development build is selected over the default one."
        ),
    )
    pu.add_argument(
        "--prefix",
        metavar="DIR",
        help=(
            "Install prefix to take a published server from; its bin/ and "
            "tools/libtmux/ are searched, covering a cmake --install and a "
            "vcpkg install. Implies --source published."
        ),
    )
    pu.add_argument(
        "--socket",
        metavar="PATH",
        help=(
            "private POSIX tmux socket path, passed as the server's "
            "compatibility positional argument. Without it the server uses "
            "its product-dedicated default route."
        ),
    )
    pu.add_argument(
        "--no-preflight",
        action="store_true",
        help=(
            "Skip the MCP initialize round trip run before writing. The "
            "probe starts the server once, so a binary that cannot run fails "
            "here instead of inside every agent."
        ),
    )
    pu.add_argument("--server", help="MCP server name (default: libtmux)")
    pu.add_argument(
        "--entry", help="server binary name (default: from the CMake target)"
    )
    pu.add_argument(
        "--env",
        action="append",
        type=_env_pair,
        metavar="KEY=VALUE",
        help=(
            "Extra env var to write into the server entry (repeatable). "
            "Layered on top of any preserved existing env; explicit --env wins. "
            "Use to inject e.g. LIBTMUX_SOCKET without a manual post-edit."
        ),
    )
    pu.add_argument("--cli", action="append", choices=ALL_CLIS)
    pu.add_argument(
        "--scope",
        choices=ALL_SCOPES,
        default=None,
        help=(
            "Claude config scope: 'user' rewrites the top-level mcpServers "
            "fallback (every project without an override picks it up), "
            "'project' rewrites projects.<abs>.mcpServers under this repo. "
            "Default 'project'. Silently coerced to 'user' for non-Claude CLIs."
        ),
    )
    pu.add_argument("--dry-run", action="store_true")
    pu.set_defaults(func=cmd_use_local)

    pr = sub.add_parser("revert", help="restore each CLI's config from its swap backup")
    pr.add_argument("--cli", action="append", choices=ALL_CLIS)
    pr.add_argument(
        "--scope",
        choices=ALL_SCOPES,
        default=None,
        help=(
            "Limit revert to one Claude scope. Without this flag, every "
            "recorded scope for the targeted CLIs is reverted."
        ),
    )
    pr.add_argument("--dry-run", action="store_true")
    pr.set_defaults(func=cmd_revert)

    pd = sub.add_parser(
        "doctor", help="report the effective MCP-swap environment (read-only)"
    )
    pd.add_argument(
        "--repo",
        default=".",
        help="where to look for the checkout; searched upward (default: .)",
    )
    pd.add_argument("--server", help="MCP server name (default: libtmux)")
    pd.set_defaults(func=cmd_doctor)

    return p


def main(argv: list[str] | None = None) -> int:
    """Entry point — dispatches to the selected subcommand."""
    args = build_parser().parse_args(argv)
    return t.cast("int", args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
