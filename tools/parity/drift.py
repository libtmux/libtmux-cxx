"""Recorded-input working-tree drift checks."""

from __future__ import annotations

import ast
import hashlib
import json
import pathlib
import typing as t

from . import git_objects
from .model import InputSpec


class MissingSelectorsError(ValueError):
    """Requested TOML selectors are absent from the metadata source."""

    def __init__(self, selectors: t.Collection[str]) -> None:
        """Initialize an error that names the absent selector set.

        Parameters
        ----------
        selectors : Collection[str]
            Requested TOML selector names absent from the source.

        Returns
        -------
        None
            The exception stores a deterministic error message.

        Examples
        --------
        >>> str(MissingSelectorsError({"project.version"}))
        'missing selectors: project.version'
        """
        message = f"missing selectors: {', '.join(sorted(selectors))}"
        super().__init__(message)


def find_drift(
    repo: pathlib.Path,
    revision: str,
    paths: t.Sequence[str | InputSpec],
) -> tuple[str, ...]:
    """Return selected working-tree changes relative to a Git revision.

    Only *paths* participate in the comparison.  This intentionally ignores
    C++ sources, build outputs, and unrelated documentation when they were
    not selected as parity inputs.

    Parameters
    ----------
    repo : pathlib.Path
        Repository whose working tree is compared.
    revision : str
        Git revision used as the recorded baseline.
    paths : Sequence[str | InputSpec]
        Full-object or field-scoped recorded inputs.

    Returns
    -------
    tuple[str, ...]
        Sorted change records limited to the selected inputs.

    Raises
    ------
    ValueError
        Raised when a requested TOML selector is absent.

    Examples
    --------
    >>> isinstance(find_drift(pathlib.Path.cwd(), "HEAD", ("cxx",)), tuple)
    True
    """
    specs = _input_specs(paths)
    object_paths = tuple(spec.path for spec in specs if not spec.fields)
    changes = _object_drift(repo, revision, object_paths)
    for spec in specs:
        if spec.fields and _selected_field_drift(repo, revision, spec):
            changes.add(f"modified:{spec.path}")
    return tuple(sorted(changes))


def _object_drift(
    repo: pathlib.Path,
    revision: str,
    paths: tuple[str, ...],
) -> set[str]:
    """Compare full recorded objects to a revision, including mode changes.

    Parameters
    ----------
    repo : pathlib.Path
        Repository whose working tree is compared.
    revision : str
        Git revision used as the comparison baseline.
    paths : tuple[str, ...]
        Full-object input paths.

    Returns
    -------
    set[str]
        Unsorted rendered change records.

    Examples
    --------
    >>> isinstance(_object_drift(pathlib.Path.cwd(), "HEAD", ("cxx",)), set)
    True
    """
    changes: set[str] = set()
    if not paths:
        return changes
    for line in git_objects.raw_diff(repo, revision, paths).splitlines():
        metadata, changed_path = line.split("\t", maxsplit=1)
        old_mode, new_mode, _old_id, _new_id, status = metadata[1:].split()
        path = changed_path.rsplit("\t", maxsplit=1)[-1]
        changes.add(f"{_raw_change_kind(status, old_mode, new_mode)}:{path}")
    for path in git_objects.untracked_paths(repo, paths):
        changes.add(f"added:{path}")
    for path in paths:
        if _root_type_changed(repo, revision, path):
            changes = {change for change in changes if not _is_under_path(change, path)}
            changes.add(f"type_changed:{path}")
    return changes


def _root_type_changed(repo: pathlib.Path, revision: str, path: str) -> bool:
    """Detect a file, tree, symlink, or submodule replacement at an input root.

    Parameters
    ----------
    repo : pathlib.Path
        Repository whose input root is inspected.
    revision : str
        Git revision providing the recorded root type.
    path : str
        Repository-relative input root.

    Returns
    -------
    bool
        Whether the working root has a different object kind.

    Examples
    --------
    >>> _root_type_changed(pathlib.Path.cwd(), "HEAD", "pyproject.toml")
    False
    """
    entries = git_objects.ls_tree(repo, revision, (path,))
    if len(entries) != 1:
        return False
    current = repo / path
    if not current.exists() and not current.is_symlink():
        return False
    return _entry_kind(entries[0]) != _working_kind(repo, current)


def _entry_kind(entry: git_objects.TreeEntry) -> str:
    """Classify a recorded Git entry by its object and file mode.

    Parameters
    ----------
    entry : git_objects.TreeEntry
        Tree entry to classify.

    Returns
    -------
    str
        ``"file"``, ``"tree"``, ``"symlink"``, or ``"submodule"``.

    Examples
    --------
    >>> _entry_kind(git_objects.TreeEntry("120000", "blob", "a", "link"))
    'symlink'
    """
    if entry.mode == "120000":
        return "symlink"
    if entry.mode == "160000" or entry.kind == "commit":
        return "submodule"
    if entry.kind == "tree":
        return "tree"
    return "file"


def _working_kind(repo: pathlib.Path, path: pathlib.Path) -> str:
    """Classify an existing working-tree path without following symlinks.

    Parameters
    ----------
    repo : pathlib.Path
        Repository containing *path*.
    path : pathlib.Path
        Existing working-tree path to classify.

    Returns
    -------
    str
        ``"file"``, ``"tree"``, ``"symlink"``, or ``"submodule"``.

    Examples
    --------
    >>> _working_kind(pathlib.Path.cwd(), pathlib.Path.cwd())
    'tree'
    """
    if path.is_symlink():
        return "symlink"
    if path.is_dir():
        relative_path = str(path.relative_to(repo))
        return "submodule" if git_objects.is_submodule(repo, relative_path) else "tree"
    return "file"


def _is_under_path(change: str, path: str) -> bool:
    """Return whether a rendered drift record belongs to an input path.

    Parameters
    ----------
    change : str
        Rendered change record in ``kind:path`` form.
    path : str
        Repository-relative input path.

    Returns
    -------
    bool
        Whether the change path equals or is nested below *path*.

    Examples
    --------
    >>> _is_under_path("added:src/api.py", "src")
    True
    """
    changed_path = change.split(":", maxsplit=1)[1]
    return changed_path == path or changed_path.startswith(f"{path}/")


def _selected_field_drift(
    repo: pathlib.Path,
    revision: str,
    spec: InputSpec,
) -> bool:
    """Compare only the configured TOML fields of one metadata file.

    Parameters
    ----------
    repo : pathlib.Path
        Repository whose metadata file is compared.
    revision : str
        Git revision providing the recorded metadata.
    spec : InputSpec
        Field-scoped metadata input specification.

    Returns
    -------
    bool
        Whether selected metadata differs from the revision.

    Raises
    ------
    ValueError
        Raised when a requested TOML selector is absent.

    Examples
    --------
    >>> _selected_field_drift(pathlib.Path.cwd(), "HEAD", InputSpec("pyproject.toml"))
    False
    """
    working_path = repo / spec.path
    if not working_path.is_file() or working_path.is_symlink():
        return True
    committed = selected_field_digest(
        git_objects.show(repo, revision, spec.path), spec.fields
    )
    working = selected_field_digest(working_path.read_bytes(), spec.fields)
    return committed != working


def selected_field_digest(data: bytes, fields: tuple[str, ...]) -> str:
    r"""Hash canonical selected TOML metadata fields without loading TOML code.

    Parameters
    ----------
    data : bytes
        UTF-8 encoded TOML source.
    fields : tuple[str, ...]
        Scalar fields or complete table selectors to record.

    Returns
    -------
    str
        SHA-256 digest with a ``sha256:`` prefix.

    Raises
    ------
    UnicodeDecodeError
        Raised when *data* is not UTF-8.
    ValueError
        Raised when a selector is absent or a selected value is invalid.

    Examples
    --------
    >>> source = b'[project]\nrequires-python = "3.10"\n'
    >>> selected_field_digest(source, ("project.requires-python",))[:7]
    'sha256:'
    """
    selected = _selected_toml_fields(data.decode("utf-8"), fields)
    encoded = json.dumps(selected, sort_keys=True, separators=(",", ":")).encode()
    return f"sha256:{hashlib.sha256(encoded).hexdigest()}"


def _selected_toml_fields(source: str, fields: tuple[str, ...]) -> dict[str, object]:
    r"""Read the small field subset that participates in the parity contract.

    Parameters
    ----------
    source : str
        TOML source containing the requested selectors.
    fields : tuple[str, ...]
        Scalar fields or complete table selectors to retain.

    Returns
    -------
    dict[str, object]
        Canonical scalar values and selected table mappings.

    Raises
    ------
    ValueError
        Raised when a selector is absent or a selected value is invalid.

    Examples
    --------
    >>> source = '[project]\nrequires-python = "3.10"\n'
    >>> _selected_toml_fields(source, ("project.requires-python",))
    {'project.requires-python': '3.10'}
    """
    wanted = set(fields)
    values: dict[str, object] = {}
    section = ""
    lines = iter(source.splitlines())
    for raw_line in lines:
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            if section in wanted:
                values[section] = {}
            continue
        if "=" not in line:
            continue
        key, value = (part.strip() for part in line.split("=", maxsplit=1))
        field = f"{section}.{key}" if section else key
        if field not in wanted and section not in wanted:
            continue
        if value.startswith("[") and not value.endswith("]"):
            collected = [value]
            for raw_continuation in lines:
                collected.append(raw_continuation.strip())
                if raw_continuation.strip().endswith("]"):
                    break
            value = "\n".join(collected)
        parsed = ast.literal_eval(value)
        if field == "project.classifiers" and isinstance(parsed, list):
            parsed = [
                item
                for item in parsed
                if isinstance(item, str)
                and (
                    item == "Framework :: Pytest"
                    or item.startswith("Programming Language :: Python ::")
                )
            ]
        if field in wanted:
            values[field] = parsed
        if section in wanted:
            table = t.cast(dict[str, object], values[section])
            table[key] = parsed
    missing = wanted.difference(values)
    if missing:
        raise MissingSelectorsError(missing)
    return values


def _input_specs(paths: t.Sequence[str | InputSpec]) -> tuple[InputSpec, ...]:
    """Normalize path strings to immutable input specifications.

    Parameters
    ----------
    paths : Sequence[str | InputSpec]
        Configured full-object paths or specifications.

    Returns
    -------
    tuple[InputSpec, ...]
        Immutable specifications in the supplied order.

    Examples
    --------
    >>> _input_specs(("src",))[0]
    InputSpec(path='src', fields=())
    """
    return tuple(
        path if isinstance(path, InputSpec) else InputSpec(path) for path in paths
    )


def _raw_change_kind(status: str, old_mode: str, new_mode: str) -> str:
    """Normalize raw Git differences to content, mode, and type categories.

    Parameters
    ----------
    status : str
        Raw Git change status.
    old_mode : str
        Recorded Git file mode.
    new_mode : str
        Working-tree Git file mode.

    Returns
    -------
    str
        Normalized drift category.

    Examples
    --------
    >>> _raw_change_kind("M", "100644", "100755")
    'mode_changed'
    >>> _raw_change_kind("T", "100644", "120000")
    'type_changed'
    """
    if status[0] == "M" and old_mode != new_mode:
        if _mode_kind(old_mode) != _mode_kind(new_mode):
            return "type_changed"
        return "mode_changed"
    match status[0]:
        case "A":
            return "added"
        case "D":
            return "deleted"
        case "T":
            return "type_changed"
        case _:
            return "modified"


def _mode_kind(mode: str) -> str:
    """Classify a Git mode as regular, symlink, or submodule content.

    Parameters
    ----------
    mode : str
        Git file mode string.

    Returns
    -------
    str
        ``"file"``, ``"symlink"``, or ``"submodule"``.

    Examples
    --------
    >>> _mode_kind("120000")
    'symlink'
    """
    if mode == "120000":
        return "symlink"
    if mode == "160000":
        return "submodule"
    return "file"
