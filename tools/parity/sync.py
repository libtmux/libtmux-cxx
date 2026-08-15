"""Review-preserving parity synchronization and evidence refresh."""

from __future__ import annotations

import copy
import hashlib
import json
import pathlib
import subprocess
import typing as t
import xml.etree.ElementTree

from .check_manifest import (
    derive_unresolved_conflicts,
    validate_manifest,
    validate_manifest_schema,
)
from .generate import (
    canonical_json_bytes,
    canonical_sha256,
    is_safe_relative_path,
    is_sha256_digest,
    synchronize_mapping,
)
from .shard import shards_document

_SEMANTIC_MAPPING_FIELDS = (
    "status",
    "cpp_symbol",
    "cpp_api_id",
    "cpp_alias_of",
    "error_behavior",
    "tmux_versions",
    "semantic_delta",
    "oracle_id",
    "approval_id",
    "reconciliation",
    "inapplicability_proof",
)


def synchronize_manifest(
    release: t.Mapping[str, object],
    development: t.Mapping[str, object],
    mapping: t.Mapping[str, object],
    approvals: t.Mapping[str, object],
    evidence: t.Mapping[str, object],
) -> dict[str, object]:
    """Synchronize generated identities around unchanged reviewed fields.

    Parameters
    ----------
    release : Mapping[str, object]
        Pinned release observation.
    development : Mapping[str, object]
        Pinned development observation.
    mapping : Mapping[str, object]
        Reviewed mapping sidecar.
    approvals : Mapping[str, object]
        Semantic decision approvals.
    evidence : Mapping[str, object]
        Current parity evidence records.

    Returns
    -------
    dict[str, object]
        Complete manifest with full and evidence-independent bindings.

    Raises
    ------
    ValueError
        Raised when mapping, approval, evidence, or shard structure is invalid.

    Examples
    --------
    >>> callable(synchronize_manifest)
    True
    """
    _validate_sidecar_envelope(mapping, "mapping")
    _validate_sidecar_envelope(approvals, "approvals")
    _validate_sidecar_envelope(evidence, "evidence")
    observations = (release, development)
    synchronized_mapping = synchronize_mapping(observations, mapping)
    normalized_approvals = _normalized_sidecar(approvals, "approvals")
    normalized_evidence = _normalized_sidecar(evidence, "evidence")
    union_entries = _union_entries(observations)
    shards = shards_document(union_entries.values())
    manifest: dict[str, object] = {
        "schema_version": 1,
        "release": copy.deepcopy(dict(release)),
        "development": copy.deepcopy(dict(development)),
        "mapping": synchronized_mapping,
        "approvals": normalized_approvals,
        "evidence": normalized_evidence,
        "shards": shards,
        "unresolved_conflicts": derive_unresolved_conflicts(
            observations, synchronized_mapping
        ),
        "bindings": {
            "release_sha256": canonical_sha256(release),
            "development_sha256": canonical_sha256(development),
            "mapping_sha256": canonical_sha256(synchronized_mapping),
            "approvals_sha256": canonical_sha256(normalized_approvals),
            "evidence_sha256": canonical_sha256(normalized_evidence),
            "shards_sha256": canonical_sha256(shards),
        },
        "semantic_contract_sha256": "sha256:" + "0" * 64,
    }
    schema_errors = validate_manifest_schema(manifest)
    if schema_errors:
        raise ValueError("invalid parity schema:\n" + "\n".join(schema_errors))
    manifest["semantic_contract_sha256"] = semantic_contract_sha256(manifest)
    errors = validate_manifest(
        manifest,
        complete=False,
        allow_pending=True,
    )
    if errors:
        raise ValueError("invalid parity mapping:\n" + "\n".join(errors))
    return manifest


def semantic_contract_sha256(manifest: t.Mapping[str, object]) -> str:
    """Hash only the evidence-independent Task 3 semantic projection.

    Parameters
    ----------
    manifest : Mapping[str, object]
        Synchronized manifest or compatible in-memory document.

    Returns
    -------
    str
        Canonical semantic contract digest.

    Examples
    --------
    >>> semantic_contract_sha256({})[:7]
    'sha256:'
    """
    release = _object_field(manifest, "release")
    development = _object_field(manifest, "development")
    mapping = _object_field(manifest, "mapping")
    approvals = _object_field(manifest, "approvals")
    shards = _object_field(manifest, "shards")
    source_projection = [
        {
            "observation_id": observation.get("observation_id"),
            "source": copy.deepcopy(observation.get("source")),
        }
        for observation in (release, development)
    ]
    raw_rows = mapping.get("entries")
    rows = raw_rows if isinstance(raw_rows, list) else []
    entry_projection = []
    for row in sorted(
        (item for item in rows if isinstance(item, dict)),
        key=lambda item: str(item.get("entry_id", "")),
    ):
        projected = {
            "entry_id": row.get("entry_id"),
            "observed_in": copy.deepcopy(row.get("observed_in")),
            "observation_hashes": copy.deepcopy(row.get("observation_hashes")),
        }
        projected.update(
            {field: copy.deepcopy(row.get(field)) for field in _SEMANTIC_MAPPING_FIELDS}
        )
        entry_projection.append(projected)
    approval_records = approvals.get("approvals")
    approval_projection = sorted(
        (copy.deepcopy(item) for item in approval_records if isinstance(item, dict))
        if isinstance(approval_records, list)
        else (),
        key=lambda item: str(item.get("approval_id", "")),
    )
    order = shards.get("dependency_order")
    shard_records = shards.get("shards")
    owners: list[dict[str, object]] = []
    if isinstance(shard_records, list):
        for shard in shard_records:
            if not isinstance(shard, dict):
                continue
            entry_ids = shard.get("entry_ids")
            if not isinstance(entry_ids, list):
                continue
            owners.extend(
                {"entry_id": entry_id, "shard": shard.get("name")}
                for entry_id in entry_ids
                if isinstance(entry_id, str)
            )
    projection = {
        "sources": source_projection,
        "entries": entry_projection,
        "approvals": approval_projection,
        "shards": {
            "dependency_order": copy.deepcopy(order),
            "owners": sorted(owners, key=lambda item: str(item["entry_id"])),
        },
    }
    return canonical_sha256(projection)


def record_evidence(
    *,
    manifest: t.Mapping[str, object],
    shards: t.Mapping[str, object],
    evidence: t.Mapping[str, object],
    shard_name: str,
    gate: t.Mapping[str, object],
    differential: t.Mapping[str, object] | None,
    tmux_binary: pathlib.Path,
    repository: pathlib.Path,
) -> dict[str, object]:
    """Refresh only one shard from immutable verified execution records.

    Parameters
    ----------
    manifest : Mapping[str, object]
        Current synchronized manifest.
    shards : Mapping[str, object]
        Fixed shard ownership document.
    evidence : Mapping[str, object]
        Existing deterministic evidence sidecar.
    shard_name : str
        Exact shard to refresh.
    gate : Mapping[str, object]
        Immutable CTest gate record.
    differential : Mapping[str, object] | None
        Immutable differential result record for behavior shards.
    tmux_binary : pathlib.Path
        Selected executable whose bytes and raw version are verified.
    repository : pathlib.Path
        Root used to resolve immutable repository-relative artifacts.

    Returns
    -------
    dict[str, object]
        New evidence document; the input is never mutated.

    Raises
    ------
    ValueError
        Raised for stale, cross-shard, failed, mutable, or mismatched evidence.

    Examples
    --------
    >>> callable(record_evidence)
    True
    """
    manifest_errors = validate_manifest(
        manifest,
        complete=False,
        allow_pending=True,
    )
    if manifest_errors:
        msg = "record-evidence requires a current structural manifest:\n"
        raise ValueError(msg + "\n".join(manifest_errors))
    if manifest.get("shards") != shards:
        msg = "shard document differs from manifest"
        raise ValueError(msg)
    if manifest.get("evidence") != evidence:
        msg = "evidence sidecar differs from manifest"
        raise ValueError(msg)
    owned_entries = _owned_entries(shards, shard_name)
    owned_evidence = _owned_evidence_ids(manifest, owned_entries)
    behavior_ids = sorted(
        evidence_id
        for evidence_id, kind in owned_evidence.items()
        if kind == "behavior"
    )
    differential_ids = sorted(
        evidence_id
        for evidence_id, kind in owned_evidence.items()
        if kind == "differential"
    )
    raw_evidence_records = evidence.get("evidence")
    evidence_records = t.cast(list[dict[str, object]], raw_evidence_records)
    evidence_by_id = {
        t.cast(str, record["evidence_id"]): record for record in evidence_records
    }
    behavior_records = [evidence_by_id[evidence_id] for evidence_id in behavior_ids]
    _validate_gate_record(
        gate,
        shard_name,
        behavior_ids,
        behavior_records,
        tmux_binary,
        repository,
    )
    if differential_ids:
        if differential is None:
            msg = "shard differential record is required"
            raise ValueError(msg)
        _validate_differential_record(
            differential,
            shard_name,
            differential_ids,
            manifest,
            repository,
        )
    elif differential is not None:
        msg = "unexpected differential record for shard"
        raise TypeError(msg)
    normalized = _normalized_sidecar(evidence, "evidence")
    records_value = normalized["evidence"]
    records = t.cast(list[dict[str, object]], records_value)
    by_id = {t.cast(str, record["evidence_id"]): record for record in records}
    if set(behavior_ids + differential_ids) - set(by_id):
        msg = "immutable record names unknown evidence ID"
        raise TypeError(msg)
    refreshed = copy.deepcopy(normalized)
    refreshed_records = t.cast(list[dict[str, object]], refreshed["evidence"])
    refreshed_by_id = {
        t.cast(str, record["evidence_id"]): record for record in refreshed_records
    }
    for evidence_id in behavior_ids:
        record = refreshed_by_id[evidence_id]
        record.update(
            {
                "status": gate["status"],
                "registration_sha256": gate["registration_sha256"],
                "junit_sha256": gate["junit_sha256"],
                "result_sha256": gate["result_sha256"],
                "tmux_binary_sha256": gate["tmux_binary_sha256"],
                "tmux_version": gate["tmux_version"],
            }
        )
    if differential is not None:
        for evidence_id in differential_ids:
            record = refreshed_by_id[evidence_id]
            record.update(
                {
                    "status": differential["status"],
                    "result_sha256": differential["result_sha256"],
                    "scenario_record_sha256": differential["scenario_record_sha256"],
                    "semantic_contract_sha256": differential[
                        "semantic_contract_sha256"
                    ],
                    "tmux_binary_sha256": gate["tmux_binary_sha256"],
                    "tmux_version": gate["tmux_version"],
                }
            )
    refreshed_records.sort(key=lambda item: str(item["evidence_id"]))
    return refreshed


def write_json_atomic(path: pathlib.Path, value: object) -> None:
    """Atomically replace one deterministic JSON artifact.

    Parameters
    ----------
    path : pathlib.Path
        Destination path.
    value : object
        JSON-compatible value.

    Returns
    -------
    None
        The destination is replaced only after serialization succeeds.

    Examples
    --------
    >>> callable(write_json_atomic)
    True
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_bytes(canonical_json_bytes(value))
    temporary.replace(path)


def _normalized_sidecar(
    document: t.Mapping[str, object], key: str
) -> dict[str, object]:
    """Copy and sort one versioned sidecar without dropping duplicates.

    Parameters
    ----------
    document : Mapping[str, object]
        Approval or evidence sidecar.
    key : str
        Array key and record identity prefix.

    Returns
    -------
    dict[str, object]
        Versioned sidecar with records in ID order.

    Raises
    ------
    ValueError
        Raised when the sidecar array or records are malformed.

    Examples
    --------
    >>> _normalized_sidecar({"evidence": []}, "evidence")
    {'schema_version': 1, 'evidence': []}
    """
    records_value = document.get(key)
    if not isinstance(records_value, list):
        msg = f"{key} sidecar requires a {key} array"
        raise TypeError(msg)
    id_key = "approval_id" if key == "approvals" else "evidence_id"
    records: list[dict[str, object]] = []
    for record in records_value:
        if not isinstance(record, dict):
            msg = f"{key} sidecar records must be objects"
            raise TypeError(msg)
        records.append(copy.deepcopy(record))
    records.sort(key=lambda item: str(item.get(id_key, "")))
    return {"schema_version": 1, key: records}


def _validate_sidecar_envelope(
    document: t.Mapping[str, object],
    sidecar: str,
) -> None:
    """Reject version or top-level keys that normalization would erase.

    Parameters
    ----------
    document : Mapping[str, object]
        Raw mapping, approval, or evidence sidecar.
    sidecar : str
        Sidecar name and schema vocabulary.

    Returns
    -------
    None
        The raw document has the exact versioned envelope.

    Raises
    ------
    ValueError
        Raised when the envelope cannot satisfy its published schema.

    Examples
    --------
    >>> _validate_sidecar_envelope(
    ...     {"schema_version": 1, "entries": []}, "mapping"
    ... )
    """
    array_keys = {
        "mapping": "entries",
        "approvals": "approvals",
        "evidence": "evidence",
    }
    array_key = array_keys.get(sidecar)
    if array_key is None:
        msg = f"invalid {sidecar} sidecar schema"
        raise ValueError(msg)
    expected_keys = {"schema_version", array_key}
    records = document.get(array_key)
    if (
        set(document) != expected_keys
        or type(document.get("schema_version")) is not int
        or document.get("schema_version") != 1
        or not isinstance(records, list)
        or not all(isinstance(record, dict) for record in records)
    ):
        msg = f"invalid {sidecar} sidecar schema"
        raise ValueError(msg)


def _union_entries(
    observations: t.Sequence[t.Mapping[str, object]],
) -> dict[str, dict[str, object]]:
    """Return the first observation document for each unioned entry ID.

    Parameters
    ----------
    observations : Sequence[Mapping[str, object]]
        Ordered observations.

    Returns
    -------
    dict[str, dict[str, object]]
        Unioned entries by ID.

    Examples
    --------
    >>> _union_entries(({"entries": [{"entry_id": "x"}]},))["x"]
    {'entry_id': 'x'}
    """
    entries: dict[str, dict[str, object]] = {}
    for observation in observations:
        raw_entries = observation.get("entries")
        if not isinstance(raw_entries, list):
            continue
        for raw_entry in raw_entries:
            if isinstance(raw_entry, dict) and isinstance(
                raw_entry.get("entry_id"), str
            ):
                entries.setdefault(t.cast(str, raw_entry["entry_id"]), raw_entry)
    return entries


def _object_field(document: t.Mapping[str, object], key: str) -> t.Mapping[str, object]:
    """Return a mapping field or an empty projection for digest calculation.

    Parameters
    ----------
    document : Mapping[str, object]
        Parent document.
    key : str
        Object field name.

    Returns
    -------
    Mapping[str, object]
        Existing mapping value or an empty mapping.

    Examples
    --------
    >>> _object_field({"value": {"x": 1}}, "value")["x"]
    1
    """
    value = document.get(key)
    return value if isinstance(value, dict) else {}


def _owned_entries(shards: t.Mapping[str, object], shard_name: str) -> set[str]:
    """Resolve exact entry ownership for one fixed shard.

    Parameters
    ----------
    shards : Mapping[str, object]
        Shard ownership document.
    shard_name : str
        Requested shard ID.

    Returns
    -------
    set[str]
        Exact owned entry IDs.

    Raises
    ------
    ValueError
        Raised for an unknown or duplicate shard.

    Examples
    --------
    >>> _owned_entries({"shards": [{"name": "x", "entry_ids": ["a"]}]}, "x")
    {'a'}
    """
    raw_shards = shards.get("shards")
    if not isinstance(raw_shards, list):
        msg = "shards document requires a shards array"
        raise TypeError(msg)
    matches = [
        shard
        for shard in raw_shards
        if isinstance(shard, dict) and shard.get("name") == shard_name
    ]
    if len(matches) != 1:
        msg = f"unknown or duplicate shard: {shard_name}"
        raise TypeError(msg)
    entry_ids = matches[0].get("entry_ids")
    if not isinstance(entry_ids, list) or not all(
        isinstance(item, str) for item in entry_ids
    ):
        msg = f"malformed entry ownership for shard: {shard_name}"
        raise ValueError(msg)
    return set(t.cast(list[str], entry_ids))


def _owned_evidence_ids(
    manifest: t.Mapping[str, object], owned_entries: set[str]
) -> dict[str, str]:
    """Resolve behavior and differential evidence owned by selected entries.

    Parameters
    ----------
    manifest : Mapping[str, object]
        Current manifest.
    owned_entries : set[str]
        Selected shard entry IDs.

    Returns
    -------
    dict[str, str]
        Evidence IDs mapped to their required kind.

    Examples
    --------
    >>> _owned_evidence_ids({"mapping": {"entries": []}}, set())
    {}
    """
    mapping = _object_field(manifest, "mapping")
    rows = mapping.get("entries")
    if not isinstance(rows, list):
        msg = "manifest mapping requires entries"
        raise TypeError(msg)
    owned: dict[str, str] = {}
    for row in rows:
        if not isinstance(row, dict) or row.get("entry_id") not in owned_entries:
            continue
        behavior = row.get("behavior_tests")
        if isinstance(behavior, list):
            for evidence_id in behavior:
                if isinstance(evidence_id, str):
                    owned[evidence_id] = "behavior"
        boundaries = row.get("boundary_tests")
        if isinstance(boundaries, list):
            for evidence_id in boundaries:
                if isinstance(evidence_id, str):
                    owned[evidence_id] = "differential"
        oracle_id = row.get("oracle_id")
        if isinstance(oracle_id, str) and oracle_id:
            owned[oracle_id] = "differential"
    return owned


def _validate_gate_record(
    gate: t.Mapping[str, object],
    shard_name: str,
    behavior_ids: list[str],
    behavior_records: t.Sequence[t.Mapping[str, object]],
    tmux_binary: pathlib.Path,
    repository: pathlib.Path,
) -> None:
    """Validate immutable CTest registration, JUnit, fixture, and binary data.

    Parameters
    ----------
    gate : Mapping[str, object]
        Immutable gate record.
    shard_name : str
        Expected shard.
    behavior_ids : list[str]
        Exact shard-owned behavior evidence IDs.
    behavior_records : Sequence[Mapping[str, object]]
        Reviewed behavior identities in evidence-ID order.
    tmux_binary : pathlib.Path
        Selected tmux executable.
    repository : pathlib.Path
        Artifact resolution root.

    Returns
    -------
    None
        Validation succeeds without mutation.

    Raises
    ------
    ValueError
        Raised for any stale or mismatched gate field.

    Examples
    --------
    >>> callable(_validate_gate_record)
    True
    """
    if type(gate.get("schema_version")) is not int or gate.get("schema_version") != 1:
        msg = "unsupported CTest gate schema"
        raise ValueError(msg)
    if gate.get("shard") != shard_name:
        msg = "CTest gate belongs to another shard"
        raise ValueError(msg)
    if gate.get("gate_id") != f"parity-{shard_name}":
        msg = "CTest gate ID does not match shard"
        raise ValueError(msg)
    if gate.get("status") != "passed":
        msg = "CTest gate did not pass"
        raise ValueError(msg)
    if gate.get("real_tmux") is not True or gate.get("execution_mode") != "real_tmux":
        msg = "CTest gate did not use real tmux fixture mode"
        raise ValueError(msg)
    fixture_modes = gate.get("fixture_modes")
    if (
        not isinstance(fixture_modes, list)
        or not fixture_modes
        or not all(isinstance(mode, str) and mode for mode in fixture_modes)
        or len(fixture_modes) != len(set(t.cast(list[str], fixture_modes)))
        or not set(t.cast(list[str], fixture_modes)).issubset({"name", "path"})
    ):
        msg = "invalid CTest gate fixture modes"
        raise ValueError(msg)
    evidence_ids = gate.get("evidence_ids")
    if evidence_ids != behavior_ids:
        msg = "CTest gate evidence ownership does not match shard"
        raise ValueError(msg)
    ctest_names = gate.get("ctest_names")
    if (
        not isinstance(ctest_names, list)
        or len(ctest_names) != len(behavior_ids)
        or not all(isinstance(name, str) and name for name in ctest_names)
        or len(ctest_names) != len(set(t.cast(list[str], ctest_names)))
    ):
        msg = "CTest gate names do not match behavior evidence"
        raise ValueError(msg)
    for field in ("cmake_target", "ctest_label"):
        if not isinstance(gate.get(field), str) or not gate.get(field):
            msg = f"CTest gate lacks {field}"
            raise ValueError(msg)
    if not is_sha256_digest(gate.get("result_sha256")):
        msg = "invalid CTest gate result_sha256"
        raise ValueError(msg)
    registration_path = _verify_artifact_digest(
        repository,
        gate.get("registration_path"),
        gate.get("registration_sha256"),
        "registration",
    )
    junit_path = _verify_artifact_digest(
        repository,
        gate.get("junit_path"),
        gate.get("junit_sha256"),
        "JUnit",
    )
    _validate_ctest_registration(
        registration_path, gate, t.cast(list[str], ctest_names)
    )
    _validate_ctest_junit(junit_path, t.cast(list[str], ctest_names))
    reviewed_identity_matches = (
        len(behavior_records) == len(behavior_ids)
        and [record.get("evidence_id") for record in behavior_records] == behavior_ids
        and [record.get("ctest_name") for record in behavior_records] == ctest_names
        and all(
            record.get("cmake_target") == gate.get("cmake_target")
            and record.get("ctest_label") == gate.get("ctest_label")
            and record.get("execution_mode") == gate.get("execution_mode")
            and record.get("real_tmux") is gate.get("real_tmux")
            for record in behavior_records
        )
    )
    if not reviewed_identity_matches:
        msg = "CTest gate does not match reviewed evidence identity"
        raise ValueError(msg)
    binary_digest = _file_sha256(tmux_binary)
    if gate.get("tmux_binary_sha256") != binary_digest:
        msg = "selected tmux binary digest does not match gate"
        raise ValueError(msg)
    result = subprocess.run(
        [str(tmux_binary), "-V"],
        check=True,
        capture_output=True,
        text=True,
    )
    if gate.get("tmux_version") != result.stdout.strip():
        msg = "selected tmux version does not match gate"
        raise ValueError(msg)


def _validate_ctest_registration(
    path: pathlib.Path,
    gate: t.Mapping[str, object],
    ctest_names: list[str],
) -> None:
    """Validate named targets and labels in an immutable CTest registry.

    Parameters
    ----------
    path : pathlib.Path
        Immutable CTest JSON-v1 registry.
    gate : Mapping[str, object]
        Gate record naming the expected target and label.
    ctest_names : list[str]
        Exact selected CTest names.

    Returns
    -------
    None
        Every named test is registered to the declared target and label.

    Raises
    ------
    ValueError
        Raised for malformed or invented registration data.

    Examples
    --------
    >>> callable(_validate_ctest_registration)
    True
    """
    try:
        registry = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        msg = "invalid CTest registration"
        raise ValueError(msg) from exc
    if (
        not isinstance(registry, dict)
        or registry.get("kind") != "ctestInfo"
        or not isinstance(registry.get("version"), dict)
        or not isinstance(registry.get("tests"), list)
    ):
        msg = "invalid CTest registration"
        raise ValueError(msg)
    version = t.cast(dict[str, object], registry["version"])
    if (
        set(version) != {"major", "minor"}
        or type(version.get("major")) is not int
        or type(version.get("minor")) is not int
        or version["major"] != 1
        or t.cast(int, version["minor"]) < 0
    ):
        msg = "invalid CTest registration"
        raise ValueError(msg)
    raw_tests = t.cast(list[object], registry["tests"])
    tests: dict[str, dict[str, object]] = {}
    for raw_test in raw_tests:
        valid_test = isinstance(raw_test, dict) and isinstance(
            raw_test.get("name"), str
        )
        if not valid_test:
            msg = "invalid CTest registration"
            raise ValueError(msg)
        raw_test = t.cast(dict[str, object], raw_test)
        name = t.cast(str, raw_test["name"])
        if name in tests:
            msg = "invalid CTest registration"
            raise ValueError(msg)
        tests[name] = raw_test
    if sorted(tests) != sorted(ctest_names):
        msg = "CTest names do not match registration"
        raise ValueError(msg)
    target = gate.get("cmake_target")
    label = gate.get("ctest_label")
    for name in ctest_names:
        test = tests[name]
        command = test.get("command")
        if (
            not isinstance(command, list)
            or not command
            or not isinstance(command[0], str)
            or pathlib.PurePath(command[0]).name != target
        ):
            msg = "CTest target does not match registration"
            raise ValueError(msg)
        properties = test.get("properties")
        valid_properties = isinstance(properties, list)
        if not valid_properties:
            msg = "CTest label does not match registration"
            raise ValueError(msg)
        properties = t.cast(list[object], properties)
        labels: set[str] = set()
        for prop in properties:
            if not isinstance(prop, dict) or prop.get("name") != "LABELS":
                continue
            value = prop.get("value")
            if isinstance(value, str):
                labels.add(value)
            elif isinstance(value, list):
                labels.update(item for item in value if isinstance(item, str))
        if label not in labels:
            msg = "CTest label does not match registration"
            raise ValueError(msg)


def _validate_ctest_junit(path: pathlib.Path, ctest_names: list[str]) -> None:
    """Validate exact passing, non-skipped cases in immutable CTest JUnit.

    Parameters
    ----------
    path : pathlib.Path
        Immutable CTest JUnit XML result.
    ctest_names : list[str]
        Exact selected CTest names.

    Returns
    -------
    None
        Every selected test has one passing result.

    Raises
    ------
    ValueError
        Raised for malformed, missing, failed, errored, or skipped cases.

    Examples
    --------
    >>> callable(_validate_ctest_junit)
    True
    """
    try:
        root = xml.etree.ElementTree.fromstring(path.read_bytes())
    except (OSError, xml.etree.ElementTree.ParseError) as exc:
        msg = "invalid CTest JUnit"
        raise ValueError(msg) from exc
    root_tag = str(root.tag).rsplit("}", maxsplit=1)[-1]
    if root_tag not in {"testsuite", "testsuites"}:
        msg = "invalid CTest JUnit"
        raise ValueError(msg)
    if not _junit_hierarchy_valid(root):
        msg = "invalid CTest JUnit"
        raise ValueError(msg)
    testcases = [
        element
        for element in root.iter()
        if str(element.tag).rsplit("}", maxsplit=1)[-1] == "testcase"
    ]
    names = [element.get("name") for element in testcases]
    if not all(isinstance(name, str) for name in names) or sorted(
        t.cast(list[str], names)
    ) != sorted(ctest_names):
        msg = "CTest JUnit names do not match gate"
        raise ValueError(msg)
    totals = _junit_totals(root)
    if totals is None or any(
        totals[field] for field in ("errors", "failures", "skipped")
    ):
        msg = "CTest JUnit contains non-passing case"
        raise ValueError(msg)


def _junit_totals(element: xml.etree.ElementTree.Element) -> dict[str, int] | None:
    """Reconcile declared and recursive JUnit suite counts.

    Parameters
    ----------
    element : xml.etree.ElementTree.Element
        ``testsuite`` or ``testsuites`` element.

    Returns
    -------
    dict[str, int] | None
        Recursive test, error, failure, and skipped counts, or ``None`` when
        a suite declares malformed or inconsistent counts.

    Examples
    --------
    >>> root = xml.etree.ElementTree.fromstring(
    ...     '<testsuite tests="0" errors="0" failures="0" skipped="0" />'
    ... )
    >>> _junit_totals(root)
    {'tests': 0, 'errors': 0, 'failures': 0, 'skipped': 0}
    """
    fields = ("tests", "errors", "failures", "skipped")
    declared: dict[str, int] = {}
    for field in fields:
        value = element.get(field)
        if value is None or not value.isdecimal():
            return None
        declared[field] = int(value)
    totals = dict.fromkeys(fields, 0)
    for child in element:
        tag = str(child.tag).rsplit("}", maxsplit=1)[-1]
        if tag in {"testsuite", "testsuites"}:
            nested = _junit_totals(child)
            if nested is None:
                return None
            for field in fields:
                totals[field] += nested[field]
        elif tag == "testcase":
            totals["tests"] += 1
            child_tags = {
                str(grandchild.tag).rsplit("}", maxsplit=1)[-1] for grandchild in child
            }
            totals["errors"] += int("error" in child_tags)
            totals["failures"] += int("failure" in child_tags)
            totals["skipped"] += int("skipped" in child_tags)
    return totals if totals == declared else None


def _junit_hierarchy_valid(element: xml.etree.ElementTree.Element) -> bool:
    """Return whether one JUnit element has only permitted children.

    Parameters
    ----------
    element : xml.etree.ElementTree.Element
        Current JUnit XML element.

    Returns
    -------
    bool
        Whether the recursively closed JUnit element hierarchy is valid.

    Examples
    --------
    >>> root = xml.etree.ElementTree.fromstring('<testsuite />')
    >>> _junit_hierarchy_valid(root)
    True
    """
    tag = str(element.tag).rsplit("}", maxsplit=1)[-1]
    allowed_children = {
        "testsuites": {"testsuite", "testsuites"},
        "testsuite": {
            "testsuite",
            "testsuites",
            "testcase",
            "properties",
            "system-out",
            "system-err",
        },
        "testcase": {"failure", "error", "skipped", "system-out", "system-err"},
        "properties": {"property"},
        "failure": set(),
        "error": set(),
        "skipped": set(),
        "system-out": set(),
        "system-err": set(),
        "property": set(),
    }
    allowed = allowed_children.get(tag)
    if allowed is None:
        return False
    children = tuple(element)
    return all(
        str(child.tag).rsplit("}", maxsplit=1)[-1] in allowed
        and _junit_hierarchy_valid(child)
        for child in children
    )


def _validate_differential_record(
    differential: t.Mapping[str, object],
    shard_name: str,
    differential_ids: list[str],
    manifest: t.Mapping[str, object],
    repository: pathlib.Path,
) -> None:
    """Validate immutable differential ownership and semantic binding.

    Parameters
    ----------
    differential : Mapping[str, object]
        Differential result record.
    shard_name : str
        Expected shard.
    differential_ids : list[str]
        Exact shard-owned differential evidence IDs.
    manifest : Mapping[str, object]
        Current semantic contract.
    repository : pathlib.Path
        Artifact resolution root.

    Returns
    -------
    None
        Validation succeeds without mutation.

    Raises
    ------
    ValueError
        Raised for stale, failed, or cross-shard differential data.

    Examples
    --------
    >>> callable(_validate_differential_record)
    True
    """
    if (
        type(differential.get("schema_version")) is not int
        or differential.get("schema_version") != 1
    ):
        msg = "unsupported differential record schema"
        raise ValueError(msg)
    if differential.get("shard") != shard_name:
        msg = "differential record belongs to another shard"
        raise ValueError(msg)
    if differential.get("status") != "passed":
        msg = "differential record did not pass"
        raise ValueError(msg)
    if differential.get("evidence_ids") != differential_ids:
        msg = "differential evidence ownership does not match shard"
        raise ValueError(msg)
    if differential.get("semantic_contract_sha256") != manifest.get(
        "semantic_contract_sha256"
    ):
        msg = "differential semantic contract digest is stale"
        raise ValueError(msg)
    _verify_artifact_digest(
        repository,
        differential.get("scenario_record_path"),
        differential.get("scenario_record_sha256"),
        "scenario record",
    )
    if not is_sha256_digest(differential.get("result_sha256")):
        msg = "invalid differential result digest"
        raise ValueError(msg)


def _verify_artifact_digest(
    repository: pathlib.Path,
    relative_path: object,
    expected_digest: object,
    label: str,
) -> pathlib.Path:
    """Verify one immutable artifact without consulting CTest Testing state.

    Parameters
    ----------
    repository : pathlib.Path
        Artifact resolution root.
    relative_path : object
        Repository-relative immutable path.
    expected_digest : object
        Expected prefixed digest.
    label : str
        Stable diagnostic label.

    Returns
    -------
    pathlib.Path
        Contained regular file whose bytes matched the digest.

    Raises
    ------
    ValueError
        Raised for unsafe, mutable, missing, or stale artifacts.

    Examples
    --------
    >>> callable(_verify_artifact_digest)
    True
    """
    if not is_safe_relative_path(relative_path):
        msg = f"unsafe {label} artifact path"
        raise ValueError(msg)
    relative_path = t.cast(str, relative_path)
    if "Testing" in pathlib.PurePosixPath(relative_path).parts:
        msg = f"mutable CTest Testing path is not {label} evidence"
        raise ValueError(msg)
    artifact = repository
    for part in pathlib.PurePosixPath(relative_path).parts:
        artifact /= part
        if artifact.is_symlink():
            msg = f"{label} artifact path contains a symlink"
            raise ValueError(msg)
    root = repository.resolve()
    if not artifact.resolve(strict=False).is_relative_to(root):
        msg = f"{label} artifact escapes repository"
        raise ValueError(msg)
    if not artifact.is_file():
        msg = f"missing immutable {label} artifact"
        raise ValueError(msg)
    if _file_sha256(artifact) != expected_digest:
        msg = f"{label} digest does not match immutable artifact"
        raise ValueError(msg)
    return artifact


def _file_sha256(path: pathlib.Path) -> str:
    """Hash one file as a prefixed SHA-256 digest.

    Parameters
    ----------
    path : pathlib.Path
        File to read.

    Returns
    -------
    str
        Prefixed digest of exact bytes.

    Examples
    --------
    >>> _file_sha256(pathlib.Path("cxx/parity/inputs.json"))[:7]
    'sha256:'
    """
    return f"sha256:{hashlib.sha256(path.read_bytes()).hexdigest()}"
