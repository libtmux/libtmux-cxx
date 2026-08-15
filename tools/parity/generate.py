"""Deterministic generation of pinned Python parity observations."""

from __future__ import annotations

import copy
import dataclasses
import hashlib
import json
import math
import pathlib
import typing as t

from .extract import extract_revision
from .model import ApiObservation, InputSpec
from .shard import shards_document

MAPPING_FIELDS = (
    "entry_id",
    "observed_in",
    "observation_hashes",
    "status",
    "cpp_symbol",
    "cpp_api_id",
    "cpp_alias_of",
    "compile_probe",
    "behavior_tests",
    "doc_id",
    "example_id",
    "error_behavior",
    "tmux_versions",
    "boundary_tests",
    "semantic_delta",
    "oracle_id",
    "approval_id",
    "reconciliation",
    "inapplicability_proof",
)
"""Exact mapping-row key order before sorted JSON serialization."""


def canonical_json_bytes(value: object) -> bytes:
    r"""Serialize JSON deterministically with one trailing newline.

    Parameters
    ----------
    value : object
        JSON-compatible value.

    Returns
    -------
    bytes
        UTF-8, sorted, two-space-indented JSON.

    Examples
    --------
    >>> canonical_json_bytes({"b": 2, "a": 1})
    b'{\n  "a": 1,\n  "b": 2\n}\n'
    """
    return (
        json.dumps(
            value,
            sort_keys=True,
            indent=2,
            ensure_ascii=False,
            allow_nan=False,
        )
        + "\n"
    ).encode("utf-8")


def canonical_sha256(value: object) -> str:
    """Hash canonical compact sorted JSON.

    Parameters
    ----------
    value : object
        JSON-compatible semantic value.

    Returns
    -------
    str
        Lowercase SHA-256 digest with an algorithm prefix.

    Examples
    --------
    >>> canonical_sha256({"value": 1}).startswith("sha256:")
    True
    """
    payload = json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
        allow_nan=False,
    ).encode("utf-8")
    return f"sha256:{hashlib.sha256(payload).hexdigest()}"


def _json_ready(value: object) -> object:
    """Convert extracted Python containers to deterministic JSON values.

    Parameters
    ----------
    value : object
        Value returned by static extraction and dataclass conversion.

    Returns
    -------
    object
        Recursively JSON-compatible value with sets in canonical order.

    Examples
    --------
    >>> _json_ready({"values": {"beta", "alpha"}})
    {'values': ['alpha', 'beta']}
    """
    if value is Ellipsis:
        return {"python_type": "ellipsis"}
    if isinstance(value, bytes):
        return {"python_type": "bytes", "hex": value.hex()}
    if isinstance(value, float) and not math.isfinite(value):
        if math.isnan(value):
            float_value = "nan"
        elif value > 0:
            float_value = "inf"
        else:
            float_value = "-inf"
        return {"python_type": "float", "value": float_value}
    if isinstance(value, complex):
        return {
            "python_type": "complex",
            "real": _json_ready(value.real),
            "imag": _json_ready(value.imag),
        }
    if isinstance(value, dict):
        if all(isinstance(key, str) for key in value):
            return {key: _json_ready(item) for key, item in value.items()}
        normalized_items = [
            {"key": _json_ready(key), "value": _json_ready(item)}
            for key, item in value.items()
        ]
        normalized_items.sort(
            key=lambda item: json.dumps(
                item,
                sort_keys=True,
                separators=(",", ":"),
                ensure_ascii=False,
                allow_nan=False,
            )
        )
        return {"python_type": "mapping", "items": normalized_items}
    if isinstance(value, (list, tuple)):
        return [_json_ready(item) for item in value]
    if isinstance(value, (set, frozenset)):
        normalized_set = [_json_ready(item) for item in value]
        return sorted(
            normalized_set,
            key=lambda item: json.dumps(
                item,
                sort_keys=True,
                separators=(",", ":"),
                ensure_ascii=False,
                allow_nan=False,
            ),
        )
    return value


def is_sha256_digest(value: object) -> bool:
    """Return whether a value is a canonical prefixed SHA-256 digest.

    Parameters
    ----------
    value : object
        Candidate digest.

    Returns
    -------
    bool
        Whether the value is ``sha256:`` plus 64 lowercase hex digits.

    Examples
    --------
    >>> is_sha256_digest("sha256:" + "a" * 64)
    True
    """
    if not isinstance(value, str) or not value.startswith("sha256:"):
        return False
    digest = value.removeprefix("sha256:")
    return len(digest) == 64 and all(
        character in "0123456789abcdef" for character in digest
    )


def is_safe_relative_path(value: object) -> bool:
    """Return whether a value is a normalized repository-relative path.

    Parameters
    ----------
    value : object
        Candidate path string.

    Returns
    -------
    bool
        Whether the path is nonempty, relative, and contains no parent step.

    Examples
    --------
    >>> is_safe_relative_path("tests/example.cpp")
    True
    >>> is_safe_relative_path("../private")
    False
    """
    if not isinstance(value, str) or not value or "\\" in value:
        return False
    path = pathlib.PurePosixPath(value)
    windows_path = pathlib.PureWindowsPath(value)
    return (
        not path.is_absolute()
        and not windows_path.drive
        and ".." not in path.parts
        and value == path.as_posix()
    )


def load_input_manifest(
    path: pathlib.Path,
) -> tuple[dict[str, object], tuple[InputSpec, ...]]:
    """Load and validate the exact Git-object observation boundary.

    Parameters
    ----------
    path : pathlib.Path
        JSON input-manifest path.

    Returns
    -------
    tuple[dict[str, object], tuple[InputSpec, ...]]
        Normalized manifest and extractor specifications.

    Raises
    ------
    ValueError
        Raised for an unknown kind, duplicate path, unsafe path, or missing
        field selector.

    Examples
    --------
    >>> inputs = pathlib.Path("tools/parity/data/inputs.json")
    >>> manifest, specs = load_input_manifest(inputs)
    >>> manifest["version"], bool(specs)
    (1, True)
    """
    raw = json.loads(path.read_text(encoding="utf-8"))
    if (
        not isinstance(raw, dict)
        or type(raw.get("version")) is not int
        or raw.get("version") != 1
    ):
        msg = "input manifest requires version 1"
        raise TypeError(msg)
    raw_inputs = raw.get("inputs")
    if not isinstance(raw_inputs, list):
        msg = "input manifest requires an inputs array"
        raise TypeError(msg)
    normalized: list[dict[str, object]] = []
    specs: list[InputSpec] = []
    seen: set[str] = set()
    for raw_input in raw_inputs:
        if not isinstance(raw_input, dict):
            msg = "input manifest entries must be objects"
            raise TypeError(msg)
        input_path = raw_input.get("path")
        kind = raw_input.get("kind")
        if not is_safe_relative_path(input_path):
            msg = f"unsafe parity input path: {input_path!r}"
            raise ValueError(msg)
        input_path = t.cast(str, input_path)
        if input_path in seen:
            msg = f"duplicate parity input path: {input_path}"
            raise ValueError(msg)
        seen.add(input_path)
        if kind not in {"blob", "tree", "toml_fields"}:
            msg = f"unknown parity input kind: {kind!r}"
            raise ValueError(msg)
        fields_value = raw_input.get("fields", [])
        if not isinstance(fields_value, list) or not all(
            isinstance(field, str) and field for field in fields_value
        ):
            msg = f"invalid fields for parity input: {input_path}"
            raise ValueError(msg)
        fields = tuple(t.cast(list[str], fields_value))
        if kind == "toml_fields" and not fields:
            msg = f"toml_fields input requires nonempty fields: {input_path}"
            raise ValueError(msg)
        if kind != "toml_fields" and fields:
            msg = f"full-object input cannot select fields: {input_path}"
            raise ValueError(msg)
        if len(fields) != len(set(fields)):
            msg = f"duplicate field selector for parity input: {input_path}"
            raise ValueError(msg)
        item: dict[str, object] = {"path": input_path, "kind": kind}
        if fields:
            item["fields"] = list(fields)
        normalized.append(item)
        specs.append(InputSpec(input_path, fields))
    return {"version": 1, "inputs": normalized}, tuple(specs)


def observation_document(
    observation: ApiObservation,
    observation_id: str,
    input_manifest: dict[str, object],
    *,
    display_revision: str | None = None,
) -> dict[str, object]:
    """Convert an extractor result to its complete generated JSON document.

    Parameters
    ----------
    observation : ApiObservation
        Static Git-object extraction result.
    observation_id : str
        Stable source label used by mapping rows.
    input_manifest : dict[str, object]
        Normalized selector boundary.
    display_revision : str | None, optional
        Original human-facing revision retained during pinned regeneration.

    Returns
    -------
    dict[str, object]
        JSON-ready observation with full source and input identities.

    Examples
    --------
    >>> observed = extract_revision(pathlib.Path.cwd(), "HEAD", ("cxx",))
    >>> document = observation_document(observed, "development", {"version": 1})
    >>> document["observation_id"]
    'development'
    """
    converted = t.cast(dict[str, object], _json_ready(dataclasses.asdict(observation)))
    source = t.cast(dict[str, object], converted["source"])
    if display_revision is not None:
        source["revision"] = display_revision
    return {
        "schema_version": 1,
        "observation_id": observation_id,
        "source": source,
        "input_manifest": copy.deepcopy(input_manifest),
        "inputs": converted["inputs"],
        "entries": converted["entries"],
    }


def observation_entry_hashes(observation: t.Mapping[str, object]) -> dict[str, str]:
    """Derive canonical hashes for every observation entry.

    Parameters
    ----------
    observation : Mapping[str, object]
        Generated observation document.

    Returns
    -------
    dict[str, str]
        Entry IDs mapped to canonical entry digests.

    Raises
    ------
    ValueError
        Raised for malformed or duplicate entry IDs.

    Examples
    --------
    >>> observation_entry_hashes({"entries": [{"entry_id": "x"}]})["x"][:7]
    'sha256:'
    """
    entries = observation.get("entries")
    if not isinstance(entries, list):
        msg = "observation requires an entries array"
        raise TypeError(msg)
    hashes: dict[str, str] = {}
    for entry in entries:
        if not isinstance(entry, dict):
            msg = "observation entries must be objects"
            raise TypeError(msg)
        entry_id = entry.get("entry_id")
        if not isinstance(entry_id, str) or not entry_id:
            msg = "observation entry lacks entry_id"
            raise ValueError(msg)
        if entry_id in hashes:
            msg = f"duplicate observation entry_id: {entry_id}"
            raise ValueError(msg)
        hashes[entry_id] = canonical_sha256(entry)
    return hashes


def pending_mapping_entry(
    entry_id: str,
    observed_in: list[str],
    observation_hashes: dict[str, str],
) -> dict[str, object]:
    """Create one exact pending mapping row.

    Parameters
    ----------
    entry_id : str
        Python observation ID.
    observed_in : list[str]
        Ordered source labels containing the ID.
    observation_hashes : dict[str, str]
        Per-source entry digests.

    Returns
    -------
    dict[str, object]
        Mapping row with no fabricated implementation decision.

    Examples
    --------
    >>> pending_mapping_entry("x", ["development"], {"development": "h"})["status"]
    'pending'
    """
    return {
        "entry_id": entry_id,
        "observed_in": observed_in,
        "observation_hashes": observation_hashes,
        "status": "pending",
        "cpp_symbol": None,
        "cpp_api_id": None,
        "cpp_alias_of": None,
        "compile_probe": None,
        "behavior_tests": [],
        "doc_id": None,
        "example_id": None,
        "error_behavior": None,
        "tmux_versions": [],
        "boundary_tests": [],
        "semantic_delta": None,
        "oracle_id": None,
        "approval_id": None,
        "reconciliation": None,
        "inapplicability_proof": None,
    }


def synchronize_mapping(
    observations: t.Sequence[t.Mapping[str, object]],
    existing: t.Mapping[str, object] | None = None,
) -> dict[str, object]:
    """Preserve reviewed rows and add only newly observed IDs as pending.

    Parameters
    ----------
    observations : Sequence[Mapping[str, object]]
        Ordered release and development observation documents.
    existing : Mapping[str, object] | None, optional
        Existing reviewed mapping document.

    Returns
    -------
    dict[str, object]
        Deterministically sorted mapping with refreshed derived fields.

    Raises
    ------
    ValueError
        Raised for duplicate source labels or mapping IDs.

    Examples
    --------
    >>> observation = {"observation_id": "development", "entries": [{"entry_id": "x"}]}
    >>> synchronize_mapping((observation,))["entries"][0]["status"]
    'pending'
    """
    source_hashes: dict[str, dict[str, str]] = {}
    source_order: list[str] = []
    for observation in observations:
        observation_id = observation.get("observation_id")
        if not isinstance(observation_id, str) or not observation_id:
            msg = "observation requires observation_id"
            raise ValueError(msg)
        if observation_id in source_hashes:
            msg = f"duplicate observation_id: {observation_id}"
            raise ValueError(msg)
        source_order.append(observation_id)
        source_hashes[observation_id] = observation_entry_hashes(observation)
    existing_entries: dict[str, dict[str, object]] = {}
    if existing is not None:
        raw_entries = existing.get("entries")
        if not isinstance(raw_entries, list):
            msg = "mapping requires an entries array"
            raise ValueError(msg)
        for raw_entry in raw_entries:
            if not isinstance(raw_entry, dict):
                msg = "mapping entries must be objects"
                raise TypeError(msg)
            entry_id = raw_entry.get("entry_id")
            if not isinstance(entry_id, str) or not entry_id:
                msg = "mapping entry lacks entry_id"
                raise ValueError(msg)
            if entry_id in existing_entries:
                msg = f"duplicate mapping entry_id: {entry_id}"
                raise ValueError(msg)
            existing_entries[entry_id] = copy.deepcopy(raw_entry)
    union = set().union(*(set(hashes) for hashes in source_hashes.values()))
    rows: list[dict[str, object]] = []
    for entry_id in sorted(union | set(existing_entries)):
        observed_in = [
            source for source in source_order if entry_id in source_hashes[source]
        ]
        hashes = {source: source_hashes[source][entry_id] for source in observed_in}
        if entry_id in existing_entries:
            row = existing_entries[entry_id]
            if observed_in:
                row["observed_in"] = observed_in
                row["observation_hashes"] = hashes
        else:
            row = pending_mapping_entry(entry_id, observed_in, hashes)
        rows.append(row)
    return {"schema_version": 1, "entries": rows}


def generate_contract(
    repository: pathlib.Path,
    output: pathlib.Path,
    release_revision: str | None = None,
    development_revision: str | None = None,
    *,
    input_manifest_path: pathlib.Path | None = None,
    check: bool = False,
) -> tuple[str, ...]:
    """Generate or check pinned observations, mapping, sidecars, and shards.

    Parameters
    ----------
    repository : pathlib.Path
        Git repository read through Task 2 plumbing.
    output : pathlib.Path
        Parity artifact directory.
    release_revision : str | None, optional
        Release revision for a new generation.
    development_revision : str | None, optional
        Development revision for a new generation.
    input_manifest_path : pathlib.Path | None, optional
        Selector manifest, defaulting to ``output / "inputs.json"``.
    check : bool, default=False
        Compare byte-for-byte using embedded resolved commits without writing.

    Returns
    -------
    tuple[str, ...]
        Differing artifact names in check mode; otherwise an empty tuple.

    Raises
    ------
    ValueError
        Raised when revisions, inputs, or existing artifacts are malformed.

    Examples
    --------
    >>> output = pathlib.Path("tools/parity/data")
    >>> isinstance(generate_contract(pathlib.Path.cwd(), output, check=True), tuple)
    True
    """
    manifest_path = input_manifest_path or output / "inputs.json"
    if check:
        _checked_artifact_path(repository, manifest_path)
    input_manifest, specs = load_input_manifest(manifest_path)
    if check:
        release_path = _release_observation_path(output)
        for path in (
            release_path,
            output / "development.json",
            output / "mapping.json",
            output / "approvals.json",
            output / "evidence.json",
            output / "shards.json",
        ):
            _checked_artifact_path(repository, path)
        old_release = _read_json(release_path)
        old_development = _read_json(output / "development.json")
        release_revision = _source_commit(old_release)
        development_revision = _source_commit(old_development)
        release_display = _source_revision(old_release)
        development_display = _source_revision(old_development)
        release_id = _observation_id(old_release)
    else:
        if release_revision is None or development_revision is None:
            msg = "generation requires release and development revisions"
            raise ValueError(msg)
        release_display = release_revision
        development_display = development_revision
        release_id = f"release-{release_revision}"
        release_path = output / f"{release_id}.json"
    release = observation_document(
        extract_revision(repository, release_revision, specs),
        release_id,
        input_manifest,
        display_revision=release_display,
    )
    development = observation_document(
        extract_revision(repository, development_revision, specs),
        "development",
        input_manifest,
        display_revision=development_display,
    )
    mapping_path = output / "mapping.json"
    existing_mapping = _read_json(mapping_path) if mapping_path.exists() else None
    mapping = synchronize_mapping((release, development), existing_mapping)
    entries_by_id: dict[str, dict[str, object]] = {}
    for observation in (release, development):
        for entry in t.cast(list[dict[str, object]], observation["entries"]):
            entries_by_id.setdefault(t.cast(str, entry["entry_id"]), entry)
    expected: dict[pathlib.Path, object] = {
        release_path: release,
        output / "development.json": development,
        mapping_path: mapping,
        output / "shards.json": shards_document(entries_by_id.values()),
    }
    for path, empty in (
        (output / "approvals.json", {"schema_version": 1, "approvals": []}),
        (output / "evidence.json", {"schema_version": 1, "evidence": []}),
    ):
        expected[path] = _read_json(path) if path.exists() else empty
    differences = tuple(
        sorted(
            str(path.relative_to(output))
            for path, value in expected.items()
            if not path.exists() or path.read_bytes() != canonical_json_bytes(value)
        )
    )
    if check:
        return differences
    output.mkdir(parents=True, exist_ok=True)
    for path, value in expected.items():
        path.write_bytes(canonical_json_bytes(value))
    return ()


def _read_json(path: pathlib.Path) -> dict[str, object]:
    """Read one JSON object from UTF-8.

    Parameters
    ----------
    path : pathlib.Path
        JSON document path.

    Returns
    -------
    dict[str, object]
        Parsed object.

    Examples
    --------
    >>> _read_json(pathlib.Path("tools/parity/data/inputs.json"))["version"]
    1
    """
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        msg = f"JSON document must be an object: {path}"
        raise TypeError(msg)
    return t.cast(dict[str, object], value)


def _release_observation_path(output: pathlib.Path) -> pathlib.Path:
    """Find the sole generated release observation.

    Parameters
    ----------
    output : pathlib.Path
        Parity artifact directory.

    Returns
    -------
    pathlib.Path
        Unique release observation path.

    Raises
    ------
    ValueError
        Raised when the release observation is absent or ambiguous.

    Examples
    --------
    >>> data = pathlib.Path("tools/parity/data")
    >>> isinstance(_release_observation_path(data), pathlib.Path)
    True
    """
    expected = output / "release-v0.62.0.json"
    candidates = tuple(sorted(output.glob("release-*.json")))
    if candidates != (expected,) or not expected.is_file():
        msg = "parity directory requires exactly release-v0.62.0.json"
        raise ValueError(msg)
    return expected


def _checked_artifact_path(
    repository: pathlib.Path, path: pathlib.Path
) -> pathlib.Path:
    """Require one checked parity artifact to be a contained regular file.

    Parameters
    ----------
    repository : pathlib.Path
        Repository root used for lexical containment.
    path : pathlib.Path
        Candidate tracked parity artifact.

    Returns
    -------
    pathlib.Path
        Original path after contained non-symlink regular-file validation.

    Raises
    ------
    ValueError
        Raised when the path escapes the repository, is symlinked, or is not a
        regular file.

    Examples
    --------
    >>> artifact = _checked_artifact_path(
    ...     pathlib.Path.cwd(), pathlib.Path("tools/parity/data/inputs.json")
    ... )
    >>> artifact.name
    'inputs.json'
    """
    return contained_regular_file(
        repository,
        path,
        message="checked parity artifact must be a contained non-symlink regular file",
    )


def contained_regular_file(
    root: pathlib.Path,
    path: pathlib.Path,
    *,
    message: str,
) -> pathlib.Path:
    """Require one path to stay under a non-symlink regular-file boundary.

    Parameters
    ----------
    root : pathlib.Path
        Directory that lexically and physically contains ``path``.
    path : pathlib.Path
        Candidate file path.
    message : str
        Diagnostic used for every boundary violation.

    Returns
    -------
    pathlib.Path
        Absolute original path after symlink and containment validation.

    Raises
    ------
    ValueError
        Raised when ``root`` or ``path`` is symlinked, escaping, absent, or not
        a regular file.

    Examples
    --------
    >>> contained_regular_file(
    ...     pathlib.Path.cwd(),
    ...     pathlib.Path("tools/parity/data/inputs.json"),
    ...     message="bad",
    ... ).name
    'inputs.json'
    """
    boundary = root.absolute()
    for component in (*reversed(boundary.parents), boundary):
        if not component.exists() or component.is_symlink():
            raise ValueError(message)
    if not boundary.is_dir():
        raise ValueError(message)
    resolved_boundary = boundary.resolve()
    candidate = path if path.is_absolute() else boundary / path
    candidate = candidate.absolute()
    try:
        relative = candidate.relative_to(boundary)
    except ValueError as exc:
        raise ValueError(message) from exc
    ancestor = boundary
    for part in relative.parts:
        if part == "..":
            raise ValueError(message)
        ancestor /= part
        if ancestor.is_symlink():
            raise ValueError(message)
    if not candidate.is_file():
        raise ValueError(message)
    if not candidate.resolve(strict=False).is_relative_to(resolved_boundary):
        raise ValueError(message)
    return candidate


def _source_commit(observation: t.Mapping[str, object]) -> str:
    """Return a generated observation's resolved commit.

    Parameters
    ----------
    observation : Mapping[str, object]
        Observation document.

    Returns
    -------
    str
        Full commit object ID.

    Examples
    --------
    >>> _source_commit({"source": {"commit": "a"}})
    'a'
    """
    source = observation.get("source")
    if not isinstance(source, dict) or not isinstance(source.get("commit"), str):
        msg = "observation source requires commit"
        raise TypeError(msg)
    return t.cast(str, source["commit"])


def _source_revision(observation: t.Mapping[str, object]) -> str:
    """Return a generated observation's display revision.

    Parameters
    ----------
    observation : Mapping[str, object]
        Observation document.

    Returns
    -------
    str
        Original revision label.

    Examples
    --------
    >>> _source_revision({"source": {"revision": "HEAD"}})
    'HEAD'
    """
    source = observation.get("source")
    if not isinstance(source, dict) or not isinstance(source.get("revision"), str):
        msg = "observation source requires revision"
        raise TypeError(msg)
    return t.cast(str, source["revision"])


def _observation_id(observation: t.Mapping[str, object]) -> str:
    """Return a generated observation's stable mapping label.

    Parameters
    ----------
    observation : Mapping[str, object]
        Observation document.

    Returns
    -------
    str
        Stable observation ID.

    Examples
    --------
    >>> _observation_id({"observation_id": "development"})
    'development'
    """
    value = observation.get("observation_id")
    if not isinstance(value, str) or not value:
        msg = "observation requires observation_id"
        raise ValueError(msg)
    return value
