"""What the port covers, and what it does not.

Two questions, one place, because both turn on the same rename table.

*Which capabilities are missing* -- :func:`find_gaps`, over the public
callables of the hierarchy classes.

*How much of the Python surface those are* -- :func:`coverage`.  The ledger
holds 966 entries and counting them as one surface is misleading: a fifth of
them are ``libtmux.neo``, which the port deliberately does not mirror, and
another twentieth is a vendored copy of ``packaging``.  A pending count read
against that total says the port has barely started when the hierarchy it
exists to wrap is nearly covered, so the buckets are named and reported
rather than left for a reader to assume.

The whole difficulty is renames.  ``list_windows`` is ``windows()`` and
``capture_pane`` is ``capture()``, so a plain name diff reports both as
missing.  :data:`RENAMES` is the written-down answer, and every target in it
is checked against the headers before any gap is reported -- a rename map
whose targets are never verified is worse than no map at all, because a typo
there silently turns a real gap into a passing one.
"""

from __future__ import annotations

import collections
import json
import pathlib
import re
import typing as t

#: Hierarchy classes whose Python callables are surveyed.
CLASSES: t.Final = ("Server", "Session", "Window", "Pane", "Client")

#: Python callable name to the C++ name that covers it.  Every value must be
#: declared in the headers; :func:`find_gaps` refuses to run if one is not.
RENAMES: t.Final[t.Mapping[str, str]] = {
    "capture_pane": "capture",
    "clear": "clear_history",
    "cmd": "run",
    "copy_mode": "enter_copy_mode",
    "attach": "attach_command",
    "attach_session": "attach_command",
    "delete_buffer": "remove",
    "display_message": "show_message",
    "detach_client": "detach_clients",
    "has_session": "session",
    "kill_pane": "kill",
    "kill_server": "kill",
    "kill_session": "kill",
    "kill_window": "kill",
    "last_pane": "select_last_pane",
    "last_window": "select_last_window",
    "link": "link_to",
    "list_buffers": "buffers",
    "list_clients": "clients",
    "list_commands": "commands",
    "list_panes": "panes",
    "list_sessions": "sessions",
    "list_windows": "windows",
    "move_window": "move_to",
    "new_pane": "split",
    "next_window": "select_next_window",
    "paste_buffer": "paste",
    "pipe": "pipe_to",
    "previous_window": "select_previous_window",
    "raise_if_dead": "check_alive",
    "rename_session": "rename",
    "rename_window": "rename",
    "reset": "clear_history",
    "resize_pane": "set_width",
    "select_pane": "select",
    "select_window": "select",
    "set_window_option": "set_option",
    "send_keys": "send_text",
    "send_prefix": "send_key",
    "show_buffer": "contents",
    "show_hooks": "hooks",
    "show_option": "options",
    "show_options": "options",
    "show_window_option": "options",
    "show_window_options": "options",
    "split_window": "split",
    "start_server": "at_socket_name",
    "swap": "swap_with",
    "break_pane": "break_out",
}

#: Lookup and search callables the filter surface replaced wholesale rather
#: than translated one by one.
QUERY_SURFACE: t.Final = frozenset(
    {
        "filter",
        "find_where",
        "from_client_name",
        "from_pane_id",
        "from_session_id",
        "from_window_id",
        "get",
        "get_by_id",
        "search_panes",
        "search_sessions",
        "search_windows",
        "where",
    }
)

#: How one ledger entry is counted, in order: the first rule that matches
#: wins.  Each is a module prefix or a kind test, never a hand-listed set of
#: entries, so a symbol added upstream lands in a bucket without an edit here.
BUCKETS: t.Final = (
    (
        "vendored packaging, not libtmux",
        lambda module, kind, name: module.startswith("libtmux._vendor"),
    ),
    (
        "neo, which the port does not mirror",
        lambda module, kind, name: module == "libtmux.neo",
    ),
    (
        "tmux option and hook names",
        lambda module, kind, name: module == "libtmux._internal.constants",
    ),
    (
        "internal and test scaffolding",
        lambda module, kind, name: module.startswith(("libtmux._internal", "conftest")),
    ),
    ("exception types", lambda module, kind, name: module == "libtmux.exc"),
    (
        "public hierarchy callables",
        lambda module, kind, name: (
            kind in {"function", "property", "overload"}
            and name.split(".")[0] in CLASSES
        ),
    ),
    ("other public surface", lambda module, kind, name: True),
)

_ENTRY_ID = re.compile(r"([\w.]+):(\w+):(.+)")
_ENTRY = re.compile(r"libtmux\.\w+:function:(\w+)\.(\w+)$")
_CALLABLE = re.compile(r"\b(\w+)\s*\(")


class StaleRenameError(ValueError):
    """A recorded rename names a C++ callable the headers do not declare."""

    def __init__(self, names: t.Collection[str]) -> None:
        """Initialize an error naming every unverified rename target.

        Parameters
        ----------
        names : Collection[str]
            Rename targets absent from the C++ headers.

        Returns
        -------
        None
            The exception stores a deterministic error message.

        Examples
        --------
        >>> str(StaleRenameError({"typo_here"}))
        'renames name absent C++ callables: typo_here'
        """
        joined = ", ".join(sorted(names))
        super().__init__(f"renames name absent C++ callables: {joined}")


def cpp_callables(headers: pathlib.Path) -> frozenset[str]:
    """Return every identifier called or declared in the C++ headers.

    Deliberately class-agnostic.  A Python method answered by a differently
    owned C++ one -- ``Server.delete_buffer`` by ``Buffer::remove`` -- is a
    reshaping of the surface, not a missing capability.

    Parameters
    ----------
    headers : pathlib.Path
        Directory searched recursively for ``.hpp`` files.

    Returns
    -------
    frozenset[str]
        Identifiers appearing before an open parenthesis.

    Examples
    --------
    >>> "windows" in cpp_callables(pathlib.Path("include/libtmux"))
    True
    """
    found: set[str] = set()
    for header in sorted(headers.rglob("*.hpp")):
        found.update(_CALLABLE.findall(header.read_text(encoding="utf-8")))
    return frozenset(found)


def python_callables(mapping: t.Mapping[str, object]) -> dict[str, frozenset[str]]:
    """Return the public callables of each hierarchy class in the ledger.

    Parameters
    ----------
    mapping : Mapping[str, object]
        Parsed ``mapping.json`` holding one entry per Python symbol.

    Returns
    -------
    dict[str, frozenset[str]]
        Class name to its public callable names.

    Examples
    --------
    >>> ledger = {"entries": [{"entry_id": "libtmux.server:function:Server.kill"}]}
    >>> python_callables(ledger)
    {'Server': frozenset({'kill'})}
    """
    entries = mapping.get("entries")
    if not isinstance(entries, list):
        msg = "mapping requires a list of entries"
        raise TypeError(msg)
    by_class: dict[str, set[str]] = collections.defaultdict(set)
    for entry in entries:
        entry_id = entry.get("entry_id") if isinstance(entry, dict) else None
        match = _ENTRY.match(entry_id) if isinstance(entry_id, str) else None
        if match is None:
            continue
        owner, name = match.groups()
        if owner in CLASSES and not name.startswith("_"):
            by_class[owner].add(name)
    return {owner: frozenset(names) for owner, names in sorted(by_class.items())}


def find_gaps(
    mapping: t.Mapping[str, object],
    headers: pathlib.Path,
) -> dict[str, tuple[str, ...]]:
    """Return the Python callables no C++ name covers, by owning class.

    Parameters
    ----------
    mapping : Mapping[str, object]
        Parsed ``mapping.json``.
    headers : pathlib.Path
        Directory holding the C++ headers.

    Returns
    -------
    dict[str, tuple[str, ...]]
        Class name to its sorted absent callables.  Classes with no gap are
        absent from the result.

    Raises
    ------
    StaleRenameError
        Raised when a recorded rename names a callable the headers lack, so
        a typo cannot quietly erase a gap.

    Examples
    --------
    >>> ledger = {
    ...     "entries": [
    ...         {"entry_id": "libtmux.server:function:Server.lock_server"},
    ...         {"entry_id": "libtmux.server:function:Server.list_sessions"},
    ...     ]
    ... }
    >>> find_gaps(ledger, pathlib.Path("include/libtmux"))
    {'Server': ('lock_server',)}
    """
    declared = cpp_callables(headers)
    stale = {target for target in RENAMES.values() if target not in declared}
    if stale:
        raise StaleRenameError(stale)
    gaps: dict[str, tuple[str, ...]] = {}
    for owner, names in python_callables(mapping).items():
        absent = tuple(
            sorted(
                name
                for name in names
                if name not in QUERY_SURFACE and RENAMES.get(name, name) not in declared
            )
        )
        if absent:
            gaps[owner] = absent
    return gaps


def report(gaps: t.Mapping[str, t.Sequence[str]]) -> str:
    """Render one survey as the text the design note quotes.

    Parameters
    ----------
    gaps : Mapping[str, Sequence[str]]
        Class name to its absent callables.

    Returns
    -------
    str
        Total followed by one line per class holding a gap.

    Examples
    --------
    >>> print(report({"Server": ["lock_server", "unbind_key"]}))
    2 python callables with no C++ counterpart under any name
      Server (2): lock_server, unbind_key
    """
    total = sum(len(names) for names in gaps.values())
    lines = [f"{total} python callables with no C++ counterpart under any name"]
    lines.extend(
        f"  {owner} ({len(gaps[owner])}): {', '.join(gaps[owner])}"
        for owner in CLASSES
        if gaps.get(owner)
    )
    return "\n".join(lines)


def survey(mapping_path: pathlib.Path, headers: pathlib.Path) -> str:
    """Read a ledger and render its capability survey.

    Parameters
    ----------
    mapping_path : pathlib.Path
        Path to ``mapping.json``.
    headers : pathlib.Path
        Directory holding the C++ headers.

    Returns
    -------
    str
        Rendered survey.

    Examples
    --------
    >>> text = survey(
    ...     pathlib.Path("tools/parity/data/mapping.json"),
    ...     pathlib.Path("include/libtmux"),
    ... )
    >>> text.endswith("no C++ counterpart under any name") or "(" in text
    True
    """
    mapping = json.loads(mapping_path.read_text(encoding="utf-8"))
    if not isinstance(mapping, dict):
        msg = f"mapping must be an object: {mapping_path}"
        raise TypeError(msg)
    return report(find_gaps(t.cast("dict[str, object]", mapping), headers))


def bucket_of(entry_id: str) -> str:
    """Return which surface bucket one ledger entry belongs to.

    Parameters
    ----------
    entry_id : str
        Ledger entry identifier, ``module:kind:name``.

    Returns
    -------
    str
        Bucket label from :data:`BUCKETS`.

    Raises
    ------
    ValueError
        Raised when the identifier is not ``module:kind:name``.

    Examples
    --------
    >>> bucket_of("libtmux.neo:dataclass_field:Obj.pane_id")
    'neo, which the port does not mirror'
    >>> bucket_of("libtmux.server:function:Server.kill")
    'public hierarchy callables'
    """
    match = _ENTRY_ID.match(entry_id)
    if match is None:
        msg = f"malformed entry id: {entry_id}"
        raise ValueError(msg)
    module, kind, name = match.groups()
    for label, belongs in BUCKETS:
        if belongs(module, kind, name):
            return label
    msg = f"no bucket matched: {entry_id}"  # pragma: no cover - last rule is total
    raise ValueError(msg)


class Coverage(t.NamedTuple):
    """One measurement of the port against the Python surface.

    Attributes
    ----------
    buckets : dict[str, int]
        Ledger entry count for each label in :data:`BUCKETS`.
    total : int
        Public hierarchy callables, the surface the port aims at.
    answered : int
        Those some C++ name does the job of.
    classified : int
        Those whose ledger entry names that symbol and carries its
        compile, documentation, example and behavior evidence.
    """

    buckets: dict[str, int]
    total: int
    answered: int
    classified: int


def coverage(
    mapping: t.Mapping[str, object],
    headers: pathlib.Path,
) -> Coverage:
    """Measure the port against the part of the surface it aims at.

    Two counts are reported for the hierarchy rather than one.  *Answered*
    is what :func:`find_gaps` measures: some C++ name does the job.
    *Classified* is stricter: the ledger entry names that symbol and carries
    its compile, documentation, example and behavior evidence.  The distance
    between them is implemented work nobody has recorded, which is real debt
    and is hidden by reporting either number alone.

    Parameters
    ----------
    mapping : Mapping[str, object]
        Parsed ``mapping.json``.
    headers : pathlib.Path
        Directory holding the C++ headers.

    Returns
    -------
    Coverage
        Bucket counts and the three hierarchy numbers.

    Examples
    --------
    >>> measured = coverage(
    ...     json.loads(pathlib.Path("tools/parity/data/mapping.json").read_text()),
    ...     pathlib.Path("include/libtmux"),
    ... )
    >>> measured.classified <= measured.answered <= measured.total
    True
    """
    entries = mapping.get("entries")
    if not isinstance(entries, list):
        msg = "mapping requires a list of entries"
        raise TypeError(msg)
    buckets: collections.Counter[str] = collections.Counter()
    classified = 0
    for entry in entries:
        entry_id = entry.get("entry_id") if isinstance(entry, dict) else None
        if not isinstance(entry_id, str):
            msg = "ledger entry lacks an entry_id"
            raise TypeError(msg)
        label = bucket_of(entry_id)
        buckets[label] += 1
        if label == "public hierarchy callables" and entry.get("status") in {
            "implemented",
            "adapted",
        }:
            classified += 1
    absent = sum(len(names) for names in find_gaps(mapping, headers).values())
    total = buckets["public hierarchy callables"]
    return Coverage(
        buckets=dict(buckets),
        total=total,
        answered=total - absent,
        classified=classified,
    )


def coverage_report(measured: Coverage) -> str:
    """Render one coverage measurement.

    Parameters
    ----------
    measured : Coverage
        Result of :func:`coverage`.

    Returns
    -------
    str
        Bucket table followed by the hierarchy counts.

    Examples
    --------
    >>> print(coverage_report(Coverage(
    ...     buckets={"public hierarchy callables": 4},
    ...     total=4,
    ...     answered=3,
    ...     classified=2,
    ... )))
    4 ledger entries:
         4  ( 100%)  public hierarchy callables
    <BLANKLINE>
    Of the 4 public hierarchy callables:
         3   (75%)  answered by a C++ name
         2   (50%)  classified in the ledger, with evidence
    """
    buckets = measured.buckets
    total = sum(buckets.values())
    lines = [f"{total} ledger entries:"]
    lines.extend(
        f"  {count:>4}  ({100 * count // total:>4}%)  {label}"
        for label, count in sorted(buckets.items(), key=lambda item: -item[1])
    )
    reach = measured.total
    lines.append("")
    lines.append(f"Of the {reach} public hierarchy callables:")
    for label, count in (
        ("answered by a C++ name", measured.answered),
        ("classified in the ledger, with evidence", measured.classified),
    ):
        lines.append(f"  {count:>4}   ({100 * count // reach}%)  {label}")
    return "\n".join(lines)
