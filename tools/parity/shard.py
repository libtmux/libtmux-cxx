"""Dependency-ordered ownership for Python parity observations."""

from __future__ import annotations

import dataclasses
import typing as t

SHARD_ORDER = (
    "metadata",
    "server-connection",
    "server-collections",
    "server-sessions",
    "server-shell-buffers",
    "server-commands",
    "session-values",
    "session-navigation",
    "session-lifecycle",
    "window-values",
    "window-layout",
    "window-panes",
    "window-lifecycle",
    "pane-values",
    "pane-io",
    "pane-layout",
    "pane-modes",
    "pane-topology",
    "client",
    "environment",
    "sparse-array",
    "options",
    "hooks",
    "query-neo",
    "common-version",
    "warnings-errors",
    "compatibility-protocols",
    "testing-support",
)
"""Fixed dependency order from the Python parity execution plan."""

_ENTITY_MEMBER_SHARDS = {
    "session": {
        "session-values": frozenset(
            {
                "session",
                "__eq__",
                "__getitem__",
                "__repr__",
                "_list_windows",
                "_windows",
                "active_pane",
                "active_window",
                "attached_pane",
                "attached_window",
                "children",
                "cmd",
                "default_hook_scope",
                "default_option_scope",
                "find_where",
                "from_env",
                "from_session_id",
                "get",
                "get_by_id",
                "id",
                "list_windows",
                "name",
                "panes",
                "refresh",
                "search_panes",
                "search_windows",
                "server",
                "where",
                "windows",
            }
        ),
        "session-navigation": frozenset(
            {
                "last_window",
                "next_window",
                "previous_window",
                "select_window",
                "switch_client",
            }
        ),
        "session-lifecycle": frozenset(
            {
                "__enter__",
                "__exit__",
                "attach",
                "attach_session",
                "detach_client",
                "kill",
                "kill_session",
                "kill_window",
                "lock_session",
                "new_window",
                "rename_session",
            }
        ),
    },
    "window": {
        "window-values": frozenset(
            {
                "window",
                "__eq__",
                "__getitem__",
                "__repr__",
                "_list_panes",
                "_panes",
                "active_pane",
                "attached_pane",
                "children",
                "cmd",
                "default_hook_scope",
                "default_option_scope",
                "display_message",
                "find_where",
                "from_env",
                "from_window_id",
                "get",
                "get_by_id",
                "height",
                "id",
                "index",
                "linked_sessions",
                "list_panes",
                "name",
                "panes",
                "refresh",
                "search_panes",
                "server",
                "session",
                "set_window_option",
                "show_window_option",
                "show_window_options",
                "where",
                "width",
            }
        ),
        "window-layout": frozenset(
            {
                "next_layout",
                "previous_layout",
                "resize",
                "rotate",
                "select",
                "select_layout",
                "select_window",
                "swap",
            }
        ),
        "window-panes": frozenset(
            {
                "last_pane",
                "new_pane",
                "new_window",
                "select_pane",
                "split",
                "split_window",
            }
        ),
        "window-lifecycle": frozenset(
            {
                "__enter__",
                "__exit__",
                "kill",
                "kill_window",
                "link",
                "move_window",
                "rename_window",
                "respawn",
                "unlink",
            }
        ),
    },
    "pane": {
        "pane-values": frozenset(
            {
                "pane",
                "__eq__",
                "__getitem__",
                "__repr__",
                "at_bottom",
                "at_left",
                "at_right",
                "at_top",
                "default_hook_scope",
                "default_option_scope",
                "from_env",
                "from_pane_id",
                "get",
                "height",
                "id",
                "index",
                "refresh",
                "server",
                "session",
                "title",
                "width",
                "window",
            }
        ),
        "pane-io": frozenset(
            {
                "capture_pane",
                "clear",
                "clear_history",
                "cmd",
                "display_message",
                "enter",
                "reset",
                "send_keys",
                "set_title",
            }
        ),
        "pane-layout": frozenset(
            {
                "new_pane",
                "resize",
                "resize_pane",
                "select",
                "select_pane",
                "set_height",
                "set_width",
                "split",
                "split_window",
            }
        ),
        "pane-modes": frozenset(
            {
                "choose_buffer",
                "choose_client",
                "choose_tree",
                "clock_mode",
                "copy_mode",
                "customize_mode",
                "display_panes",
                "display_popup",
                "find_window",
                "paste_buffer",
                "pipe",
                "send_prefix",
            }
        ),
        "pane-topology": frozenset(
            {
                "__enter__",
                "__exit__",
                "break_pane",
                "join",
                "kill",
                "move",
                "respawn",
                "swap",
            }
        ),
    },
}
"""Closed entity-member ownership derived from Plan 05 Tasks 8 through 13."""

_SERVER_MEMBER_SHARDS = {
    "server-connection": frozenset(
        {
            "server",
            "__enter__",
            "__eq__",
            "__exit__",
            "__init__",
            "__repr__",
            "_is_daemon_not_up_error",
            "cmd",
            "dashliteral",
            "from_env",
            "is_alive",
            "raise_if_dead",
        }
    ),
    "server-collections": frozenset(
        {
            "_fetch_or_empty",
            "_list_panes",
            "_list_sessions",
            "_list_windows",
            "_sessions",
            "_update_panes",
            "_update_windows",
            "attached_sessions",
            "children",
            "clients",
            "find_where",
            "get_by_id",
            "list_clients",
            "list_sessions",
            "panes",
            "search_panes",
            "search_sessions",
            "search_windows",
            "sessions",
            "where",
            "windows",
        }
    ),
    "server-sessions": frozenset(
        {
            "has_session",
            "kill",
            "kill_server",
            "kill_session",
            "new_session",
            "start_server",
        }
    ),
    "server-shell-buffers": frozenset(
        {
            "delete_buffer",
            "if_shell",
            "list_buffers",
            "load_buffer",
            "run_shell",
            "save_buffer",
            "set_buffer",
            "show_buffer",
            "source_file",
            "wait_for",
        }
    ),
    "server-commands": frozenset(
        {
            "attach_session",
            "bind_key",
            "clear_prompt_history",
            "command_prompt",
            "confirm_before",
            "detach_all_clients",
            "detach_client",
            "display_menu",
            "display_message",
            "list_commands",
            "list_keys",
            "lock_client",
            "lock_server",
            "refresh_client",
            "server_access",
            "show_messages",
            "show_prompt_history",
            "suspend_client",
            "switch_client",
            "unbind_key",
        }
    ),
}
"""Closed Server-member ownership derived from Plan 05 Tasks 2 and 4 through 7."""

_ENTITY_ROOTS = {
    "session": ("Session", "dataclass"),
    "window": ("Window", "dataclass"),
    "pane": ("Pane", "dataclass"),
}
"""Exact root declarations for entity modules."""


@dataclasses.dataclass(frozen=True, slots=True)
class ParityShard:
    """One dependency-ordered parity work queue.

    Attributes
    ----------
    name : str
        Stable shard identifier.
    depends_on : tuple[str, ...]
        Immediate predecessor required before this shard.
    entry_ids : tuple[str, ...]
        Sorted Python observation IDs owned by the shard.
    """

    name: str
    depends_on: tuple[str, ...]
    entry_ids: tuple[str, ...]


def assign_shard(entry: t.Mapping[str, object]) -> str:
    """Assign one observed Python entry to its Plan 05 behavior family.

    Parameters
    ----------
    entry : Mapping[str, object]
        Observation entry containing ``module`` and ``qualname``.

    Returns
    -------
    str
        One identifier from :data:`SHARD_ORDER`.

    Examples
    --------
    >>> assign_shard({"module": "libtmux.pane", "qualname": "Pane.send_keys"})
    'pane-io'
    >>> assign_shard({"module": "libtmux.formats", "qualname": "FORMAT"})
    'metadata'
    """
    module = str(entry.get("module", ""))
    qualname = str(entry.get("qualname", ""))
    kind = str(entry.get("kind", ""))
    if module == "conftest" or module.startswith("libtmux.test"):
        return "testing-support"
    if module == "libtmux.pytest_plugin":
        return "testing-support"
    if module in {"libtmux.exc"}:
        return "warnings-errors"
    if module in {"libtmux._compat", "libtmux._internal.control_mode"}:
        return "compatibility-protocols"
    if module in {"libtmux._internal.env"}:
        return "environment"
    if module in {"libtmux._internal.sparse_array"}:
        return "sparse-array"
    if module == "libtmux.options":
        return "options"
    if module == "libtmux.hooks":
        return "hooks"
    if module in {"libtmux.neo", "libtmux._internal.query_list"}:
        return "query-neo"
    if module in {
        "libtmux.common",
        "libtmux._vendor.version",
        "libtmux._vendor._structures",
    }:
        return "common-version"
    if module == "libtmux.client":
        return "client"
    if module == "libtmux.server":
        if qualname == "Server" or qualname.startswith("Server."):
            return _server_shard(qualname, kind)
        if "." in qualname:
            msg = f"unassigned server qualname: {qualname}"
            raise ValueError(msg)
        return "metadata"
    if module == "libtmux.session":
        return _entity_shard("session", qualname, kind)
    if module == "libtmux.window":
        return _entity_shard("window", qualname, kind)
    if module == "libtmux.pane":
        return _entity_shard("pane", qualname, kind)
    return "metadata"


def _server_shard(qualname: str, kind: str) -> str:
    """Classify a Server member by the fixed Plan 05 ownership table.

    Parameters
    ----------
    qualname : str
        Exact Server-qualified name.
    kind : str
        Observed declaration kind.

    Returns
    -------
    str
        Server shard identifier.

    Examples
    --------
    >>> _server_shard("Server.new_session", "function")
    'server-sessions'
    """
    parts = qualname.split(".")
    if parts[0] != "Server" or len(parts) not in {1, 2}:
        msg = f"unassigned server qualname: {qualname}"
        raise ValueError(msg)
    if len(parts) == 1:
        if kind != "class":
            msg = f"unassigned server root declaration: {qualname}"
            raise ValueError(msg)
        member = "server"
    else:
        member = parts[1]
    for shard, members in _SERVER_MEMBER_SHARDS.items():
        if member in members:
            return shard
    msg = f"unassigned server member: {member}"
    raise ValueError(msg)


def _entity_shard(entity: str, qualname: str, kind: str) -> str:
    """Classify Session, Window, and Pane members by behavior.

    Parameters
    ----------
    entity : str
        Entity family name.
    qualname : str
        Exact qualified name.
    kind : str
        Observed declaration kind.

    Returns
    -------
    str
        Matching entity shard.

    Raises
    ------
    ValueError
        Raised when Plan 05 has no explicit owner for the member.

    Examples
    --------
    >>> _entity_shard("window", "Window.select_layout", "function")
    'window-layout'
    """
    root, root_kind = _ENTITY_ROOTS[entity]
    parts = qualname.split(".")
    if len(parts) == 1:
        if parts[0] != root or kind != root_kind:
            msg = f"unassigned {entity} qualname: {qualname}"
            raise ValueError(msg)
        member = entity
    elif len(parts) == 2 and parts[0] == root:
        member = parts[1]
    else:
        msg = f"unassigned {entity} qualname: {qualname}"
        raise ValueError(msg)
    for shard, members in _ENTITY_MEMBER_SHARDS[entity].items():
        if member in members:
            return shard
    msg = f"unassigned {entity} member: {member}"
    raise ValueError(msg)


def build_shards(
    entries: t.Iterable[t.Mapping[str, object]],
) -> tuple[ParityShard, ...]:
    """Build every fixed shard with each entry assigned exactly once.

    Parameters
    ----------
    entries : Iterable[Mapping[str, object]]
        Deduplicated observation entries.

    Returns
    -------
    tuple[ParityShard, ...]
        All shards in fixed dependency order, including empty shards.

    Raises
    ------
    ValueError
        Raised when an entry ID is missing or duplicated.

    Examples
    --------
    >>> shards = build_shards(({"entry_id": "x", "module": "libtmux.formats"},))
    >>> shards[0].entry_ids
    ('x',)
    """
    assigned: dict[str, list[str]] = {name: [] for name in SHARD_ORDER}
    seen: set[str] = set()
    for entry in entries:
        entry_id = entry.get("entry_id")
        if not isinstance(entry_id, str) or not entry_id:
            msg = "parity entry lacks entry_id"
            raise ValueError(msg)
        if entry_id in seen:
            msg = f"duplicate parity entry_id: {entry_id}"
            raise ValueError(msg)
        seen.add(entry_id)
        assigned[assign_shard(entry)].append(entry_id)
    shards: list[ParityShard] = []
    for index, name in enumerate(SHARD_ORDER):
        depends_on = () if index == 0 else (SHARD_ORDER[index - 1],)
        shards.append(ParityShard(name, depends_on, tuple(sorted(assigned[name]))))
    return tuple(shards)


def shards_document(entries: t.Iterable[t.Mapping[str, object]]) -> dict[str, object]:
    """Serialize fixed shard ownership to a JSON-ready document.

    Parameters
    ----------
    entries : Iterable[Mapping[str, object]]
        Deduplicated observation entries.

    Returns
    -------
    dict[str, object]
        Versioned dependency and ownership document.

    Examples
    --------
    >>> document = shards_document(())
    >>> document["dependency_order"][0]
    'metadata'
    """
    shards = build_shards(entries)
    return {
        "schema_version": 1,
        "dependency_order": list(SHARD_ORDER),
        "shards": [
            {
                "name": shard.name,
                "depends_on": list(shard.depends_on),
                "entry_ids": list(shard.entry_ids),
            }
            for shard in shards
        ],
    }
