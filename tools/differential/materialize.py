"""Materialize a pinned Python reference exclusively from recorded Git objects."""

from __future__ import annotations

import dataclasses
import pathlib
import shutil
import stat
import subprocess
import typing as t

from tools.parity import git_objects
from tools.parity.drift import selected_field_digest
from tools.parity.generate import canonical_sha256


@dataclasses.dataclass(frozen=True, slots=True)
class MaterializedSource:
    """Verified task-owned Python import tree.

    Attributes
    ----------
    root : pathlib.Path
        Owned materialization root.
    import_root : pathlib.Path
        Root inserted into ``sys.path`` for public ``libtmux`` import.
    source_commit : str
        Exact recorded commit object ID.
    input_manifest_sha256 : str
        Canonical digest of the authoritative input manifest.
    """

    root: pathlib.Path
    import_root: pathlib.Path
    source_commit: str
    input_manifest_sha256: str


def _safe_relative_path(raw: object) -> pathlib.PurePosixPath:
    """Validate one portable repository-relative input path.

    Parameters
    ----------
    raw : object
        Candidate path value.

    Returns
    -------
    pathlib.PurePosixPath
        Safe relative path.

    Raises
    ------
    ValueError
        Raised for absolute, empty, dot, parent, or backslash paths.

    Examples
    --------
    >>> str(_safe_relative_path('src/libtmux'))
    'src/libtmux'
    """
    if (
        not isinstance(raw, str)
        or not raw
        or "\\" in raw
        or any(part in {"", ".", ".."} for part in raw.split("/"))
    ):
        msg = f"unsafe input path {raw!r}"
        raise ValueError(msg)
    path = pathlib.PurePosixPath(raw)
    if path.is_absolute():
        msg = f"unsafe input path {raw!r}"
        raise ValueError(msg)
    return path


def _input_specs(document: t.Mapping[str, object]) -> dict[str, dict[str, object]]:
    """Validate and index one authoritative input manifest.

    Parameters
    ----------
    document : Mapping[str, object]
        Input manifest document.

    Returns
    -------
    dict[str, dict[str, object]]
        Specs keyed by safe repository path.

    Raises
    ------
    ValueError
        Raised for invalid versions, kinds, fields, or overlapping paths.

    Examples
    --------
    >>> sorted(_input_specs({'version': 1, 'inputs': [{'kind': 'blob', 'path': 'a'}]}))
    ['a']
    """
    if (
        set(document) != {"version", "inputs"}
        or type(document.get("version")) is not int
        or document["version"] != 1
    ):
        msg = "input manifest must be a closed version-one object"
        raise ValueError(msg)
    raw_inputs = document.get("inputs")
    if not isinstance(raw_inputs, list) or not raw_inputs:
        msg = "input manifest inputs must be a nonempty array"
        raise ValueError(msg)
    specs: dict[str, dict[str, object]] = {}
    for raw in raw_inputs:
        if not isinstance(raw, dict):
            msg = "input manifest row must be an object"
            raise TypeError(msg)
        kind = raw.get("kind")
        expected = (
            {"kind", "path", "fields"}
            if kind == "toml_fields"
            else {
                "kind",
                "path",
            }
        )
        if set(raw) != expected or kind not in {"blob", "tree", "toml_fields"}:
            msg = "input manifest row is not closed"
            raise ValueError(msg)
        path = str(_safe_relative_path(raw["path"]))
        if path in specs:
            msg = f"duplicate input path {path!r}"
            raise ValueError(msg)
        if kind == "toml_fields":
            fields = raw.get("fields")
            if (
                not isinstance(fields, list)
                or not fields
                or any(not isinstance(field, str) or not field for field in fields)
                or len(fields) != len(set(fields))
            ):
                msg = f"{path}: invalid selected TOML fields"
                raise ValueError(msg)
        specs[path] = t.cast(dict[str, object], raw)
    paths = sorted(specs)
    for index, path in enumerate(paths):
        prefix = f"{path}/"
        if any(other.startswith(prefix) for other in paths[index + 1 :]):
            msg = f"overlapping input path {path!r}"
            raise ValueError(msg)
    return specs


def _recorded_inputs(
    observation: t.Mapping[str, object],
) -> dict[str, dict[str, object]]:
    """Validate and index recorded input object identities.

    Parameters
    ----------
    observation : Mapping[str, object]
        Development observation document.

    Returns
    -------
    dict[str, dict[str, object]]
        Recorded objects keyed by safe path.

    Raises
    ------
    ValueError
        Raised for malformed or duplicate records.

    Examples
    --------
    >>> _recorded_inputs({'inputs': []})
    {}
    """
    raw_inputs = observation.get("inputs")
    if not isinstance(raw_inputs, list):
        msg = "observation inputs must be an array"
        raise TypeError(msg)
    recorded: dict[str, dict[str, object]] = {}
    for raw in raw_inputs:
        if not isinstance(raw, dict) or set(raw) != {"kind", "object_id", "path"}:
            msg = "recorded input row is not closed"
            raise ValueError(msg)
        path = str(_safe_relative_path(raw["path"]))
        if path in recorded:
            msg = f"duplicate recorded input {path!r}"
            raise ValueError(msg)
        if raw.get("kind") not in {"blob", "tree", "toml_fields"} or not isinstance(
            raw.get("object_id"), str
        ):
            msg = f"{path}: invalid recorded object identity"
            raise ValueError(msg)
        recorded[path] = t.cast(dict[str, object], raw)
    return recorded


def _entry_for(
    repository: pathlib.Path, commit: str, path: str
) -> git_objects.TreeEntry:
    """Resolve exactly one tree entry at a pinned commit.

    Parameters
    ----------
    repository : pathlib.Path
        Git repository.
    commit : str
        Pinned commit ID.
    path : str
        Repository-relative input path.

    Returns
    -------
    git_objects.TreeEntry
        Exact tree entry.

    Raises
    ------
    ValueError
        Raised when the path is missing or ambiguous.

    Examples
    --------
    >>> callable(_entry_for)
    True
    """
    try:
        entries = git_objects.ls_tree(repository, commit, (path,))
    except subprocess.CalledProcessError as error:
        msg = f"missing Git object for {path!r}"
        raise ValueError(msg) from error
    if len(entries) != 1 or entries[0].path != path:
        msg = f"missing Git object for {path!r}"
        raise ValueError(msg)
    return entries[0]


def _write_blob(
    repository: pathlib.Path,
    commit: str,
    entry: git_objects.TreeEntry,
    destination: pathlib.Path,
) -> None:
    """Write one verified regular Git blob under the owned root.

    Parameters
    ----------
    repository : pathlib.Path
        Git repository.
    commit : str
        Pinned commit ID.
    entry : git_objects.TreeEntry
        Verified regular blob entry.
    destination : pathlib.Path
        Exact owned output path.

    Raises
    ------
    ValueError
        Raised for symlink, submodule, or unsupported mode entries.

    Examples
    --------
    >>> callable(_write_blob)
    True
    """
    _validate_blob_entry(entry)
    try:
        payload = git_objects.show(repository, commit, entry.path)
    except subprocess.CalledProcessError as error:
        msg = f"missing Git object for {entry.path!r}"
        raise ValueError(msg) from error
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(payload)
    destination.chmod(
        stat.S_IRUSR | stat.S_IWUSR | (stat.S_IXUSR if entry.mode == "100755" else 0)
    )


def _validate_blob_entry(entry: git_objects.TreeEntry) -> None:
    """Reject a Git entry that cannot be materialized as a regular file.

    Parameters
    ----------
    entry : git_objects.TreeEntry
        Candidate Git tree entry.

    Raises
    ------
    ValueError
        Raised for symlink, submodule, tree, or unsupported mode entries.

    Examples
    --------
    >>> _validate_blob_entry(git_objects.TreeEntry('100644', 'blob', '0' * 40, 'a'))
    """
    if entry.mode == "120000":
        msg = f"symlink input is forbidden: {entry.path}"
        raise ValueError(msg)
    if entry.mode == "160000" or entry.kind == "commit":
        msg = f"submodule input is forbidden: {entry.path}"
        raise ValueError(msg)
    if entry.kind != "blob" or entry.mode not in {"100644", "100755"}:
        msg = f"unsupported Git input mode for {entry.path}"
        raise ValueError(msg)


def _owned_destination(
    root: pathlib.Path,
    raw_path: object,
    *,
    subtree: str | None = None,
) -> pathlib.Path:
    """Resolve one canonical descendant beneath the owned output root.

    Parameters
    ----------
    root : pathlib.Path
        Canonical absolute materialization root.
    raw_path : object
        Raw repository-relative path returned by Git.
    subtree : str | None, optional
        Declared tree input that must contain the descendant.

    Returns
    -------
    pathlib.Path
        Canonical destination proven to remain beneath ``root``.

    Raises
    ------
    ValueError
        Raised for an unsafe path, prefix escape, or destination escape.

    Examples
    --------
    >>> root = pathlib.Path('/tmp/materialized').resolve()
    >>> _owned_destination(root, 'src/libtmux/__init__.py', subtree='src/libtmux')
    PosixPath('/tmp/materialized/src/libtmux/__init__.py')
    """
    try:
        relative = _safe_relative_path(raw_path)
    except ValueError as error:
        msg = f"unsafe Git tree descendant {raw_path!r}"
        raise ValueError(msg) from error
    canonical = str(relative)
    if subtree is not None and not canonical.startswith(f"{subtree}/"):
        msg = f"unsafe Git tree descendant {canonical!r} outside {subtree!r}"
        raise ValueError(msg)
    try:
        destination = root.joinpath(*relative.parts).resolve(strict=False)
    except (OSError, RuntimeError) as error:
        msg = f"unsafe Git tree descendant {canonical!r}"
        raise ValueError(msg) from error
    if destination == root or root not in destination.parents:
        msg = f"unsafe Git tree descendant {canonical!r} outside owned root"
        raise ValueError(msg)
    return destination


def _populate_materialization(
    repository: pathlib.Path,
    commit: str,
    specs: t.Mapping[str, t.Mapping[str, object]],
    recorded: t.Mapping[str, t.Mapping[str, object]],
    destination: pathlib.Path,
) -> pathlib.Path:
    """Populate an owned root from already validated pinned inputs.

    Parameters
    ----------
    repository : pathlib.Path
        Git object database.
    commit : str
        Exact recorded commit.
    specs : Mapping[str, Mapping[str, object]]
        Authoritative input specifications by path.
    recorded : Mapping[str, Mapping[str, object]]
        Recorded input identities by path.
    destination : pathlib.Path
        Existing empty task-owned root.

    Returns
    -------
    pathlib.Path
        Materialized Python import root.

    Raises
    ------
    ValueError
        Raised for an object, kind, or selected-field mismatch.

    Examples
    --------
    >>> callable(_populate_materialization)
    True
    """
    root = destination.expanduser().resolve(strict=False)
    writes: list[tuple[git_objects.TreeEntry, pathlib.Path]] = []
    for path, spec in specs.items():
        entry = _entry_for(repository, commit, path)
        recorded_object = t.cast(str, recorded[path]["object_id"])
        if spec["kind"] == "toml_fields":
            if entry.kind != "blob" or entry.mode == "120000":
                msg = f"{path}: selected TOML input is not a regular blob"
                raise ValueError(msg)
            fields = tuple(t.cast(list[str], spec["fields"]))
            try:
                payload = git_objects.show(repository, commit, path)
            except subprocess.CalledProcessError as error:
                msg = f"missing Git object for {path!r}"
                raise ValueError(msg) from error
            digest = selected_field_digest(payload, fields)
            if digest != recorded_object:
                msg = f"{path}: selected-field object identity mismatch"
                raise ValueError(msg)
            continue
        if entry.object_id != recorded_object:
            msg = f"{path}: Git object identity mismatch"
            raise ValueError(msg)
        if spec["kind"] == "blob":
            _validate_blob_entry(entry)
            writes.append((entry, _owned_destination(root, path)))
            continue
        if entry.kind != "tree" or entry.mode != "040000":
            msg = f"{path}: recorded tree input is not a tree"
            raise ValueError(msg)
        try:
            descendants = git_objects.ls_tree(
                repository, commit, (path,), recursive=True
            )
        except subprocess.CalledProcessError as error:
            msg = f"missing Git object for tree {path!r}"
            raise ValueError(msg) from error
        for descendant in descendants:
            _validate_blob_entry(descendant)
            writes.append(
                (
                    descendant,
                    _owned_destination(
                        root,
                        descendant.path,
                        subtree=path,
                    ),
                )
            )
    destination.mkdir(parents=True)
    for entry, output_path in writes:
        _write_blob(repository, commit, entry, output_path)
    import_root = root / "src"
    if not (import_root / "libtmux/__init__.py").is_file():
        msg = "materialized inputs do not contain public libtmux"
        raise ValueError(msg)
    return import_root


def materialize_reference(
    repository: pathlib.Path,
    observation: t.Mapping[str, object],
    input_manifest: t.Mapping[str, object],
    destination: pathlib.Path,
) -> MaterializedSource:
    """Verify recorded Git identities and write only full recorded objects.

    Selected TOML fields are rehashed from the pinned blob but the unrecorded
    file content is not copied into the import tree.

    Parameters
    ----------
    repository : pathlib.Path
        Git repository containing the recorded commit and objects.
    observation : Mapping[str, object]
        Recorded development observation.
    input_manifest : Mapping[str, object]
        Authoritative configured inputs.
    destination : pathlib.Path
        Nonexistent task-owned materialization root.

    Returns
    -------
    MaterializedSource
        Verified root, import path, source commit, and manifest digest.

    Raises
    ------
    ValueError
        Raised for provenance drift, unsafe inputs, or unsupported Git kinds.

    Examples
    --------
    >>> callable(materialize_reference)
    True
    """
    embedded_manifest = observation.get("input_manifest")
    if embedded_manifest != input_manifest:
        msg = "observation input manifest does not match authoritative input manifest"
        raise ValueError(msg)
    specs = _input_specs(input_manifest)
    recorded = _recorded_inputs(observation)
    if set(specs) != set(recorded):
        msg = "recorded inputs do not match input manifest"
        raise ValueError(msg)
    for path, spec in specs.items():
        if recorded[path]["kind"] != spec["kind"]:
            msg = f"{path}: recorded input kind mismatch"
            raise ValueError(msg)

    source = observation.get("source")
    if not isinstance(source, dict):
        msg = "observation source must be an object"
        raise TypeError(msg)
    commit = source.get("commit")
    tree = source.get("tree")
    if not isinstance(commit, str) or not isinstance(tree, str):
        msg = "observation source lacks commit or tree"
        raise TypeError(msg)
    try:
        resolved_commit = git_objects.rev_parse(repository, f"{commit}^{{commit}}")
        resolved_tree = git_objects.rev_parse(repository, f"{commit}^{{tree}}")
    except subprocess.CalledProcessError as error:
        msg = f"missing Git object for recorded commit {commit}"
        raise ValueError(msg) from error
    if resolved_commit != commit:
        msg = "recorded commit is not an exact commit object ID"
        raise ValueError(msg)
    if resolved_tree != tree:
        msg = "recorded commit tree does not match commit tree"
        raise ValueError(msg)
    if destination.exists() or destination.is_symlink():
        msg = "materialization destination must not exist"
        raise ValueError(msg)

    try:
        import_root = _populate_materialization(
            repository,
            commit,
            specs,
            recorded,
            destination,
        )
    except Exception:
        if destination.exists():
            shutil.rmtree(destination)
        raise
    return MaterializedSource(
        destination.resolve(),
        import_root.resolve(),
        commit,
        canonical_sha256(input_manifest),
    )
