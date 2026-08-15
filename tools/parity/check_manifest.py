"""Structural and completion validation for the Python parity ledger."""

from __future__ import annotations

import collections
import json
import pathlib
import re
import subprocess
import typing as t

import jsonschema
import referencing

from . import git_objects
from .drift import selected_field_digest
from .extract import extract_revision
from .generate import (
    MAPPING_FIELDS,
    canonical_json_bytes,
    canonical_sha256,
    is_safe_relative_path,
    is_sha256_digest,
    observation_document,
)
from .git_objects import rev_parse
from .model import InputSpec
from .shard import shards_document

_STATUSES = {"pending", "implemented", "adapted", "excluded"}
_EVIDENCE_KINDS = {
    "compile",
    "behavior",
    "differential",
    "documentation",
    "example",
    "version",
}
_DECISION_KINDS = {
    "adaptation",
    "exclusion",
    "pending_adaptation",
    "reconciliation",
}
_SHAPE_FIELDS = ("signature", "value_shape", "decorators", "bases")
_SEMANTIC_DECISION_FIELDS = (
    "entry_id",
    "status",
    "semantic_delta",
    "oracle_id",
    "reconciliation",
    "inapplicability_proof",
)
# Beside the module that validates against them, so neither moves
# without the other.
_SCHEMA_DIRECTORY = pathlib.Path(__file__).parent / "data"
_EVIDENCE_PATH_RULES = {
    "compile": (("tests/compile/",), frozenset({".cc", ".cpp", ".cxx"})),
    "behavior": (
        ("tests/integration/",),
        frozenset({".cc", ".cpp", ".cxx"}),
    ),
    "differential": (
        ("tests/differential/", "tests/cxx/differential/"),
        frozenset({".cpp", ".json", ".py"}),
    ),
    "documentation": (("docs/",), frozenset({".md", ".rst"})),
    "example": (("examples/",), frozenset({".cc", ".cpp", ".cxx"})),
    "version": (
        (
            "tests/compile/",
            "tests/integration/",
            "tests/differential/",
            "tests/cxx/differential/",
        ),
        frozenset({".cpp", ".json", ".py"}),
    ),
}
_APPROVAL_EVIDENCE_PATH_RULES = (
    ("docs/decisions/",),
    frozenset({".md", ".rst"}),
)


def validate_mapping(
    observations: t.Sequence[t.Mapping[str, object]],
    mapping: t.Mapping[str, object],
    approvals: t.Mapping[str, object],
    evidence: t.Mapping[str, object],
    complete: bool,
) -> list[str]:
    """Validate exact observation coverage and reviewed mapping decisions.

    Parameters
    ----------
    observations : Sequence[Mapping[str, object]]
        Ordered generated observation documents.
    mapping : Mapping[str, object]
        Reviewed Python-to-C++ mapping sidecar.
    approvals : Mapping[str, object]
        Explicit semantic decision approvals.
    evidence : Mapping[str, object]
        Compile, execution, documentation, example, and version evidence.
    complete : bool
        Whether pending classifications are prohibited.

    Returns
    -------
    list[str]
        Deterministic validation violations; an empty list is valid.

    Examples
    --------
    >>> validate_mapping(
    ...     (), {"entries": []}, {"approvals": []}, {"evidence": []}, False,
    ... )
    []
    """
    errors: list[str] = []
    observed, source_order = _observed_entries(observations, errors)
    rows, duplicate_mapping_ids = _mapping_rows(mapping, errors)
    row_by_id = {t.cast(str, row.get("entry_id")): row for row in rows}
    errors.extend(
        f"{duplicate}: duplicate mapping entry_id"
        for duplicate in sorted(duplicate_mapping_ids)
    )
    observed_ids = set(observed)
    mapping_ids = set(row_by_id)
    errors.extend(
        f"{entry_id}: observation lacks mapping entry"
        for entry_id in sorted(observed_ids - mapping_ids)
    )
    errors.extend(
        f"{entry_id}: stale mapping classification"
        for entry_id in sorted(mapping_ids - observed_ids)
    )
    shape_conflicts = {
        conflict["entry_id"] for conflict in derive_shape_conflicts(observations)
    }
    for entry_id in sorted(row_by_id):
        row = row_by_id[entry_id]
        errors.extend(_mapping_row_errors(entry_id, row, complete))
        if entry_id in observed:
            expected_in = [
                source for source in source_order if source in observed[entry_id]
            ]
            expected_hashes = {
                source: observed[entry_id][source]["hash"] for source in expected_in
            }
            if row.get("observed_in") != expected_in:
                errors.append(f"{entry_id}: observed_in does not match observations")
            if row.get("observation_hashes") != expected_hashes:
                errors.append(
                    f"{entry_id}: observation_hashes do not match observations"
                )
        if entry_id in shape_conflicts:
            errors.extend(_conflict_resolution_errors(entry_id, row))
        elif (
            row.get("status") == "implemented" and row.get("reconciliation") is not None
        ):
            errors.append(f"{entry_id}: implemented entry claims reconciliation")
    approval_index = _approval_records(approvals, errors)
    evidence_index = _evidence_records(evidence, errors)
    errors.extend(_cpp_ownership_errors(row_by_id))
    errors.extend(_approval_reference_errors(row_by_id, approval_index))
    errors.extend(_evidence_reference_errors(row_by_id, evidence_index, complete))
    return errors


def derive_shape_conflicts(
    observations: t.Sequence[t.Mapping[str, object]],
) -> list[dict[str, object]]:
    """Derive sorted same-ID signature, default, decorator, base, and value conflicts.

    Parameters
    ----------
    observations : Sequence[Mapping[str, object]]
        Ordered generated observation documents.

    Returns
    -------
    list[dict[str, object]]
        Exact conflict records sorted by entry ID.

    Examples
    --------
    >>> first = {
    ...     "observation_id": "a",
    ...     "entries": [{"entry_id": "x", "signature": "()"}],
    ... }
    >>> second = {
    ...     "observation_id": "b",
    ...     "entries": [{"entry_id": "x", "signature": "(x)"}],
    ... }
    >>> derive_shape_conflicts((first, second))[0]["fields"]
    ['signature']
    """
    errors: list[str] = []
    observed, source_order = _observed_entries(observations, errors)
    if errors:
        return []
    conflicts: list[dict[str, object]] = []
    for entry_id in sorted(observed):
        sources = [source for source in source_order if source in observed[entry_id]]
        if len(sources) < 2:
            continue
        differing = [
            field
            for field in _SHAPE_FIELDS
            if len(
                {
                    canonical_sha256(
                        t.cast(
                            dict[str, object],
                            observed[entry_id][source]["entry"],
                        ).get(field)
                    )
                    for source in sources
                }
            )
            > 1
        ]
        if differing:
            conflicts.append(
                {
                    "entry_id": entry_id,
                    "fields": sorted(differing),
                    "observation_hashes": {
                        source: observed[entry_id][source]["hash"] for source in sources
                    },
                }
            )
    return conflicts


def derive_unresolved_conflicts(
    observations: t.Sequence[t.Mapping[str, object]],
    mapping: t.Mapping[str, object],
) -> list[dict[str, object]]:
    """Return exact source conflicts whose reviewed rows remain pending.

    Parameters
    ----------
    observations : Sequence[Mapping[str, object]]
        Ordered generated observations.
    mapping : Mapping[str, object]
        Reviewed mapping document.

    Returns
    -------
    list[dict[str, object]]
        Derived unresolved records sorted by entry ID.

    Examples
    --------
    >>> derive_unresolved_conflicts((), {"entries": []})
    []
    """
    raw_rows = mapping.get("entries")
    rows = raw_rows if isinstance(raw_rows, list) else []
    status_by_id = {
        row.get("entry_id"): row.get("status") for row in rows if isinstance(row, dict)
    }
    return [
        conflict
        for conflict in derive_shape_conflicts(observations)
        if status_by_id.get(conflict["entry_id"]) == "pending"
    ]


def approval_decision_sha256(
    mapping: t.Mapping[str, object],
    approval: t.Mapping[str, object],
) -> str:
    """Hash the canonical reviewed payload accepted by one approval.

    Parameters
    ----------
    mapping : Mapping[str, object]
        Mapping containing the approval's exact scope.
    approval : Mapping[str, object]
        Approval record with decision kind and scope IDs.

    Returns
    -------
    str
        Canonical decision payload digest.

    Examples
    --------
    >>> row = {"entry_id": "x", "status": "excluded"}
    >>> approval = {
    ...     "decision_kind": "exclusion", "scope_entry_ids": ["x"],
    ... }
    >>> approval_decision_sha256({"entries": [row]}, approval)[:7]
    'sha256:'
    """
    raw_rows = mapping.get("entries")
    rows = raw_rows if isinstance(raw_rows, list) else []
    by_id = {
        row.get("entry_id"): row
        for row in rows
        if isinstance(row, dict) and isinstance(row.get("entry_id"), str)
    }
    raw_scope = approval.get("scope_entry_ids")
    scope = raw_scope if isinstance(raw_scope, list) else []
    decisions = []
    for entry_id in sorted(item for item in scope if isinstance(item, str)):
        row = by_id.get(entry_id, {})
        decisions.append({field: row.get(field) for field in _SEMANTIC_DECISION_FIELDS})
    return canonical_sha256(
        {
            "decision_kind": approval.get("decision_kind"),
            "scope_entry_ids": sorted(item for item in scope if isinstance(item, str)),
            "decisions": decisions,
        }
    )


def validate_manifest_schema(
    manifest: t.Mapping[str, object],
) -> list[str]:
    """Validate a manifest against the published Draft 2020-12 schemas.

    Parameters
    ----------
    manifest : Mapping[str, object]
        Candidate synchronized manifest.

    Returns
    -------
    list[str]
        Deterministically ordered schema violations.

    Examples
    --------
    >>> validate_manifest_schema({})[0].startswith("schema $")
    True
    """
    schema_documents = {
        name: json.loads((_SCHEMA_DIRECTORY / name).read_text(encoding="utf-8"))
        for name in (
            "manifest.schema.json",
            "approvals.schema.json",
            "evidence.schema.json",
        )
    }
    for schema in schema_documents.values():
        jsonschema.Draft202012Validator.check_schema(schema)
    resources = [
        (name, referencing.Resource.from_contents(schema))
        for name, schema in schema_documents.items()
        if name != "manifest.schema.json"
    ]
    registry = referencing.Registry().with_resources(resources)
    validator = jsonschema.Draft202012Validator(
        schema_documents["manifest.schema.json"],
        registry=registry,
    )
    violations: list[tuple[tuple[str, ...], str]] = []
    for error in validator.iter_errors(dict(manifest)):
        path = tuple(str(part) for part in error.absolute_path)
        location = "$"
        for part in error.absolute_path:
            location += f"[{part}]" if isinstance(part, int) else f".{part}"
        violations.append((path, f"schema {location}: {error.message}"))
    return [message for _, message in sorted(violations)]


def validate_manifest(
    manifest: t.Mapping[str, object],
    *,
    complete: bool,
    allow_pending: bool,
    repository: pathlib.Path | None = None,
) -> list[str]:
    """Validate a synchronized manifest, derived conflicts, shards, and bindings.

    Parameters
    ----------
    manifest : Mapping[str, object]
        Complete synchronized manifest document.
    complete : bool
        Whether all classifications and evidence must be closed.
    allow_pending : bool
        Whether structural verification permits untouched pending rows.
    repository : pathlib.Path | None, optional
        Repository used to resolve source Git identities and evidence files.

    Returns
    -------
    list[str]
        Deterministic manifest violations.

    Examples
    --------
    >>> bool(validate_manifest({}, complete=False, allow_pending=True))
    True
    """
    schema_errors = validate_manifest_schema(manifest)
    if schema_errors:
        return schema_errors
    errors: list[str] = []
    required_objects = (
        "release",
        "development",
        "mapping",
        "approvals",
        "evidence",
        "shards",
        "bindings",
    )
    if any(not isinstance(manifest.get(key), dict) for key in required_objects):
        return ["manifest lacks required object fields"]
    release = t.cast(dict[str, object], manifest["release"])
    development = t.cast(dict[str, object], manifest["development"])
    mapping = t.cast(dict[str, object], manifest["mapping"])
    approvals = t.cast(dict[str, object], manifest["approvals"])
    evidence = t.cast(dict[str, object], manifest["evidence"])
    shards = t.cast(dict[str, object], manifest["shards"])
    observations = (release, development)
    for observation in observations:
        errors.extend(_observation_input_errors(observation))
    if release.get("input_manifest") != development.get("input_manifest"):
        errors.append("manifest: source observations use different input boundaries")
    errors.extend(
        validate_mapping(
            observations,
            mapping,
            approvals,
            evidence,
            complete=complete or not allow_pending,
        )
    )
    expected_conflicts = derive_unresolved_conflicts(observations, mapping)
    actual_conflicts = manifest.get("unresolved_conflicts")
    if actual_conflicts != expected_conflicts:
        errors.append("manifest: unresolved_conflicts do not match observations")
    if isinstance(actual_conflicts, list):
        raw_rows = mapping.get("entries")
        rows = raw_rows if isinstance(raw_rows, list) else []
        by_id = {row.get("entry_id"): row for row in rows if isinstance(row, dict)}
        for conflict in actual_conflicts:
            if not isinstance(conflict, dict):
                errors.append("manifest: malformed unresolved observation conflict")
                continue
            entry_id = conflict.get("entry_id")
            row = by_id.get(entry_id)
            if complete or not isinstance(row, dict) or row.get("status") != "pending":
                errors.append(f"{entry_id}: unresolved observation conflict")
            elif _pending_conflict_claims(row):
                errors.append(
                    f"{entry_id}: pending conflict fabricates reconciliation "
                    "or evidence"
                )
    union_entries = _union_entry_documents(observations)
    try:
        expected_shards = shards_document(union_entries.values())
    except ValueError as exc:
        errors.append(str(exc))
    else:
        if shards != expected_shards:
            errors.append("manifest: shard assignment does not match observations")
    bindings = t.cast(dict[str, object], manifest["bindings"])
    for key, value in (
        ("release_sha256", release),
        ("development_sha256", development),
        ("mapping_sha256", mapping),
        ("approvals_sha256", approvals),
        ("evidence_sha256", evidence),
        ("shards_sha256", shards),
    ):
        if bindings.get(key) != canonical_sha256(value):
            errors.append(f"manifest: stale {key} binding")
    from .sync import semantic_contract_sha256

    if manifest.get("semantic_contract_sha256") != semantic_contract_sha256(manifest):
        errors.append("manifest: stale semantic_contract_sha256")
    # Artifact checks do not depend on the ledger being finished, and gating
    # them behind `complete` meant they had never run: a record could name a
    # test case nobody wrote for as long as one entry stayed pending.
    if repository is not None:
        errors.extend(_repository_evidence_errors(evidence, repository))
        errors.extend(_repository_approval_errors(approvals, repository))
    if complete:
        raw_evidence = evidence.get("evidence")
        evidence_records = raw_evidence if isinstance(raw_evidence, list) else []
        if any(
            isinstance(record, dict)
            and record.get("kind") == "differential"
            and record.get("status") == "passed"
            and record.get("semantic_contract_sha256")
            != manifest.get("semantic_contract_sha256")
            for record in evidence_records
        ):
            errors.append("differential evidence semantic contract is stale")
        if repository is not None:
            errors.extend(_source_identity_errors(observations, repository))
            errors.extend(_committed_observation_errors(observations, repository))
    return errors


def _observation_input_errors(observation: t.Mapping[str, object]) -> list[str]:
    """Match recorded Git objects to one observation's selector boundary.

    Parameters
    ----------
    observation : Mapping[str, object]
        Generated source observation.

    Returns
    -------
    list[str]
        Duplicate or mismatched input-object violations.

    Examples
    --------
    >>> _observation_input_errors({"observation_id": "empty"})
    ['empty: input objects do not match recorded input boundary']
    """
    observation_id = str(observation.get("observation_id", "observation"))
    raw_manifest = observation.get("input_manifest")
    raw_specs = raw_manifest.get("inputs") if isinstance(raw_manifest, dict) else None
    raw_inputs = observation.get("inputs")
    specs = raw_specs if isinstance(raw_specs, list) else []
    inputs = raw_inputs if isinstance(raw_inputs, list) else []
    expected = [
        (item.get("path"), item.get("kind")) for item in specs if isinstance(item, dict)
    ]
    actual = [
        (item.get("path"), item.get("kind"))
        for item in inputs
        if isinstance(item, dict)
    ]
    if not expected or not actual:
        return [f"{observation_id}: input objects do not match recorded input boundary"]
    if len(expected) != len(set(expected)) or len(actual) != len(set(actual)):
        return [f"{observation_id}: duplicate recorded input object"]
    if sorted(expected) != sorted(actual):
        return [f"{observation_id}: input objects do not match recorded input boundary"]
    return []


def _source_identity_errors(
    observations: t.Sequence[t.Mapping[str, object]],
    repository: pathlib.Path,
) -> list[str]:
    """Resolve each recorded commit and tree against the repository.

    Parameters
    ----------
    observations : Sequence[Mapping[str, object]]
        Generated source observations.
    repository : pathlib.Path
        Git repository containing the recorded objects.

    Returns
    -------
    list[str]
        Missing or mismatched Git identity violations.

    Examples
    --------
    >>> _source_identity_errors((), pathlib.Path.cwd())
    []
    """
    errors: list[str] = []
    for observation in observations:
        observation_id = str(observation.get("observation_id", "observation"))
        raw_source = observation.get("source")
        if not isinstance(raw_source, dict):
            continue
        commit = raw_source.get("commit")
        tree = raw_source.get("tree")
        if not isinstance(commit, str) or not isinstance(tree, str):
            continue
        try:
            resolved_commit = rev_parse(repository, f"{commit}^{{commit}}")
        except subprocess.CalledProcessError:
            errors.append(f"{observation_id}: source commit is not a Git commit")
            continue
        if resolved_commit != commit:
            errors.append(f"{observation_id}: source commit identity is stale")
        try:
            resolved_tree = rev_parse(repository, f"{commit}^{{tree}}")
        except subprocess.CalledProcessError:
            errors.append(f"{observation_id}: source tree cannot be resolved")
        else:
            if resolved_tree != tree:
                errors.append(f"{observation_id}: source tree identity is stale")
        raw_manifest = observation.get("input_manifest")
        raw_specs = (
            raw_manifest.get("inputs") if isinstance(raw_manifest, dict) else None
        )
        raw_inputs = observation.get("inputs")
        specs = t.cast(list[dict[str, object]], raw_specs)
        inputs = t.cast(list[dict[str, object]], raw_inputs)
        input_by_path = {t.cast(str, record["path"]): record for record in inputs}
        for spec in specs:
            path = t.cast(str, spec["path"])
            kind = t.cast(str, spec["kind"])
            record = input_by_path.get(path)
            if record is None:
                continue
            try:
                if kind == "toml_fields":
                    fields = tuple(t.cast(list[str], spec["fields"]))
                    expected_object_id = selected_field_digest(
                        git_objects.show(repository, commit, path),
                        fields,
                    )
                else:
                    entries = git_objects.ls_tree(
                        repository,
                        commit,
                        (path,),
                    )
                    if (
                        len(entries) != 1
                        or entries[0].path != path
                        or entries[0].kind != kind
                    ):
                        errors.append(
                            f"{observation_id}: input object cannot be resolved: {path}"
                        )
                        continue
                    expected_object_id = entries[0].object_id
            except (
                subprocess.CalledProcessError,
                UnicodeDecodeError,
                ValueError,
            ):
                errors.append(
                    f"{observation_id}: input object cannot be resolved: {path}"
                )
                continue
            if record.get("object_id") != expected_object_id:
                errors.append(
                    f"{observation_id}: input object identity is stale: {path}"
                )
    return errors


def _committed_observation_errors(
    observations: t.Sequence[t.Mapping[str, object]],
    repository: pathlib.Path,
) -> list[str]:
    """Re-extract each source commit and compare its complete stable observation.

    Parameters
    ----------
    observations : Sequence[Mapping[str, object]]
        Recorded release and development observations.
    repository : pathlib.Path
        Git repository containing their pinned commits.

    Returns
    -------
    list[str]
        Re-extraction failures or mismatched committed-observation violations.

    Examples
    --------
    >>> _committed_observation_errors((), pathlib.Path.cwd())
    []
    """
    errors: list[str] = []
    stable_source_fields = ("commit", "tree", "generator_version", "clean_policy")
    for observation in observations:
        observation_id = str(observation.get("observation_id", "observation"))
        source = observation.get("source")
        input_manifest = observation.get("input_manifest")
        if not isinstance(source, dict) or not isinstance(input_manifest, dict):
            continue
        commit = source.get("commit")
        raw_inputs = input_manifest.get("inputs")
        if not isinstance(commit, str) or not isinstance(raw_inputs, list):
            continue
        try:
            specs = tuple(
                InputSpec(
                    t.cast(str, item["path"]),
                    tuple(t.cast(list[str], item.get("fields", []))),
                )
                for item in raw_inputs
                if isinstance(item, dict)
            )
            extracted = observation_document(
                extract_revision(repository, commit, specs),
                observation_id,
                t.cast(dict[str, object], input_manifest),
            )
        except (
            subprocess.CalledProcessError,
            SyntaxError,
            UnicodeDecodeError,
            ValueError,
        ):
            errors.append(
                f"{observation_id}: committed observation cannot be extracted"
            )
            continue
        extracted_source = extracted.get("source")
        if not isinstance(extracted_source, dict):
            errors.append(
                f"{observation_id}: committed observation cannot be extracted"
            )
            continue
        recorded_replay = {
            "source": {field: source.get(field) for field in stable_source_fields},
            "inputs": observation.get("inputs"),
            "entries": observation.get("entries"),
        }
        extracted_replay = {
            "source": {
                field: extracted_source.get(field) for field in stable_source_fields
            },
            "inputs": extracted.get("inputs"),
            "entries": extracted.get("entries"),
        }
        if canonical_json_bytes(recorded_replay) != canonical_json_bytes(
            extracted_replay
        ):
            errors.append(
                f"{observation_id}: committed observation does not match extraction"
            )
    return errors


def _repository_evidence_errors(
    evidence: t.Mapping[str, object],
    repository: pathlib.Path,
) -> list[str]:
    """Validate kind-specific evidence files inside the repository.

    Parameters
    ----------
    evidence : Mapping[str, object]
        Evidence sidecar embedded in the manifest.
    repository : pathlib.Path
        Repository root used for artifact resolution.

    Returns
    -------
    list[str]
        Missing, unsafe, symlinked, or wrong-kind artifact violations.

    Examples
    --------
    >>> _repository_evidence_errors({"evidence": []}, pathlib.Path.cwd())
    []
    """
    raw_records = evidence.get("evidence")
    records = raw_records if isinstance(raw_records, list) else []
    errors: list[str] = []
    for record in records:
        if not isinstance(record, dict):
            continue
        evidence_id = str(record.get("evidence_id", "evidence"))
        kind = record.get("kind")
        prefixes, suffixes = _EVIDENCE_PATH_RULES.get(
            str(kind),
            ((), frozenset()),
        )
        error = _repository_artifact_error(
            repository,
            record.get("path"),
            prefixes,
            suffixes,
        )
        if error is not None:
            errors.append(f"{evidence_id}: {error}")
            continue
        case_error = _evidence_case_error(repository, record)
        if case_error is not None:
            errors.append(f"{evidence_id}: {case_error}")
    return errors


def _evidence_case_error(
    repository: pathlib.Path,
    record: t.Mapping[str, object],
) -> str | None:
    """Require an evidence record's ``case_id`` to exist where it claims.

    Path rules already place an artifact in the right directory, but nothing
    read it, so ``case_id`` was a free-text claim: a record could name a test
    that was never written and every gate would pass.  Each kind is resolved
    to the token its file would have to contain.

    Parameters
    ----------
    repository : pathlib.Path
        Repository root.
    record : Mapping[str, object]
        One evidence record.

    Returns
    -------
    str | None
        Violation text, or None when the case resolves or the kind has no
        resolution rule.

    Examples
    --------
    >>> _evidence_case_error(pathlib.Path.cwd(), {"kind": "unversioned"}) is None
    True
    """
    kind = record.get("kind")
    case_id = record.get("case_id")
    path = record.get("path")
    if not isinstance(kind, str) or not isinstance(case_id, str):
        return None
    if not isinstance(path, str) or not is_safe_relative_path(path):
        return None
    artifact = repository / path
    try:
        text = artifact.read_text(encoding="utf-8")
    except OSError:
        return None
    if kind == "behavior":
        suite, _, case = case_id.partition(".")
        if not case:
            return f"behavior case is not Suite.Case: {case_id}"
        wanted = re.compile(
            rf"TEST\w*\(\s*{re.escape(suite)}\s*,\s*{re.escape(case)}\s*\)"
        )
        if wanted.search(text) is None:
            return f"no TEST({suite}, {case}) in {path}"
        return None
    if kind == "documentation":
        member = case_id.rsplit("::", 1)[-1]
        if f"{member}(" not in text:
            return f"no declaration of {member} in {path}"
        return None
    if kind == "example":
        if "." not in case_id:
            # A type rather than a call: the example has to name it.
            if re.search(rf"\b{re.escape(case_id)}\b", text, re.IGNORECASE) is None:
                return f"no use of {case_id} in {path}"
            return None
        member = case_id.rsplit(".", 1)[-1]
        # Both spellings: an entity reached through `expected` or `optional`
        # is called through `->`, and requiring `.` would report a call that
        # is plainly there.
        if f".{member}(" not in text and f"->{member}(" not in text:
            return f"no call to {member} in {path}"
        return None
    if kind == "compile":
        if f"{case_id}(" not in text:
            return f"no {case_id} in {path}"
        return None
    return None


def _repository_approval_errors(
    approvals: t.Mapping[str, object],
    repository: pathlib.Path,
) -> list[str]:
    """Validate complete approval decision artifacts under their fixed root.

    Parameters
    ----------
    approvals : Mapping[str, object]
        Approval sidecar embedded in the manifest.
    repository : pathlib.Path
        Repository root used for decision-artifact containment.

    Returns
    -------
    list[str]
        Missing, unsafe, or wrong-root approval-artifact violations.

    Examples
    --------
    >>> _repository_approval_errors({"approvals": []}, pathlib.Path.cwd())
    []
    """
    raw_records = approvals.get("approvals")
    records = raw_records if isinstance(raw_records, list) else []
    prefixes, suffixes = _APPROVAL_EVIDENCE_PATH_RULES
    errors: list[str] = []
    for record in records:
        if not isinstance(record, dict):
            continue
        approval_id = str(record.get("approval_id", "approval"))
        error = _repository_artifact_error(
            repository,
            record.get("evidence_path"),
            prefixes,
            suffixes,
        )
        if error is not None:
            errors.append(f"{approval_id}: approval {error}")
    return errors


def _repository_artifact_error(
    repository: pathlib.Path,
    relative_path: object,
    prefixes: t.Sequence[str],
    suffixes: t.AbstractSet[str],
) -> str | None:
    """Return a violation for one repository-owned regular file.

    Parameters
    ----------
    repository : pathlib.Path
        Repository root used for containment.
    relative_path : object
        Normalized repository-relative path.
    prefixes : Sequence[str]
        Allowed repository subtrees for the artifact kind.
    suffixes : AbstractSet[str]
        Allowed lowercase suffixes for the artifact kind.

    Returns
    -------
    str | None
        Violation text, or ``None`` for a contained regular file.

    Examples
    --------
    >>> _repository_artifact_error(
    ...     pathlib.Path.cwd(),
    ...     "tools/parity/data/inputs.json",
    ...     ("tools/parity/data/",),
    ...     {".json"},
    ... ) is None
    True
    """
    if not is_safe_relative_path(relative_path):
        return "unsafe repository-relative evidence path"
    relative_path = t.cast(str, relative_path)
    if not any(relative_path.startswith(prefix) for prefix in prefixes) or (
        pathlib.PurePosixPath(relative_path).suffix.lower() not in suffixes
    ):
        return "evidence path does not match its kind"
    candidate = repository
    for part in pathlib.PurePosixPath(relative_path).parts:
        candidate /= part
        if candidate.is_symlink():
            return "evidence path contains a symlink"
    root = repository.resolve()
    resolved = candidate.resolve(strict=False)
    if not resolved.is_relative_to(root):
        return "evidence path escapes the repository"
    if not candidate.is_file():
        return "evidence path is not a regular file"
    return None


def _observed_entries(
    observations: t.Sequence[t.Mapping[str, object]],
    errors: list[str],
) -> tuple[dict[str, dict[str, dict[str, object]]], list[str]]:
    """Index observation entries by ID and source while retaining duplicates.

    Parameters
    ----------
    observations : Sequence[Mapping[str, object]]
        Generated observation documents.
    errors : list[str]
        Error accumulator.

    Returns
    -------
    tuple[dict[str, dict[str, dict[str, object]]], list[str]]
        Entry/source index and source order.

    Examples
    --------
    >>> _observed_entries((), [])[1]
    []
    """
    observed: dict[str, dict[str, dict[str, object]]] = {}
    source_order: list[str] = []
    seen_sources: set[str] = set()
    for observation in observations:
        source = observation.get("observation_id")
        if not isinstance(source, str) or not source:
            errors.append("observation lacks observation_id")
            continue
        if source in seen_sources:
            errors.append(f"{source}: duplicate observation_id")
            continue
        seen_sources.add(source)
        source_order.append(source)
        entries = observation.get("entries")
        if not isinstance(entries, list):
            errors.append(f"{source}: observation lacks entries array")
            continue
        counts: collections.Counter[str] = collections.Counter()
        for entry in entries:
            if not isinstance(entry, dict) or not isinstance(
                entry.get("entry_id"), str
            ):
                errors.append(f"{source}: malformed observation entry")
                continue
            entry_id = t.cast(str, entry["entry_id"])
            counts[entry_id] += 1
            if counts[entry_id] > 1:
                continue
            observed.setdefault(entry_id, {})[source] = {
                "entry": entry,
                "hash": canonical_sha256(entry),
            }
        for entry_id, count in sorted(counts.items()):
            if count > 1:
                errors.append(f"{entry_id}: duplicate observation entry_id in {source}")
    return observed, source_order


def _mapping_rows(
    mapping: t.Mapping[str, object],
    errors: list[str],
) -> tuple[list[dict[str, object]], set[str]]:
    """Read mapping rows and report malformed or duplicate IDs.

    Parameters
    ----------
    mapping : Mapping[str, object]
        Mapping sidecar.
    errors : list[str]
        Error accumulator.

    Returns
    -------
    tuple[list[dict[str, object]], set[str]]
        First row for each ID and duplicate ID set.

    Examples
    --------
    >>> _mapping_rows({"entries": []}, [])[0]
    []
    """
    raw_rows = mapping.get("entries")
    if not isinstance(raw_rows, list):
        errors.append("mapping requires an entries array")
        return [], set()
    rows: list[dict[str, object]] = []
    seen: set[str] = set()
    duplicates: set[str] = set()
    for row in raw_rows:
        if not isinstance(row, dict) or not isinstance(row.get("entry_id"), str):
            errors.append("mapping contains malformed entry")
            continue
        entry_id = t.cast(str, row["entry_id"])
        if entry_id in seen:
            duplicates.add(entry_id)
            continue
        seen.add(entry_id)
        rows.append(t.cast(dict[str, object], row))
    return rows, duplicates


def _mapping_row_errors(
    entry_id: str,
    row: t.Mapping[str, object],
    complete: bool,
) -> list[str]:
    """Validate the exact keys and status-specific fields of one row.

    Parameters
    ----------
    entry_id : str
        Mapping row ID.
    row : Mapping[str, object]
        Mapping row.
    complete : bool
        Whether pending is prohibited.

    Returns
    -------
    list[str]
        Row violations.

    Examples
    --------
    >>> bool(_mapping_row_errors("x", {"entry_id": "x"}, False))
    True
    """
    errors: list[str] = []
    missing = sorted(set(MAPPING_FIELDS) - set(row))
    extra = sorted(set(row) - set(MAPPING_FIELDS))
    if missing:
        errors.append(f"{entry_id}: mapping entry lacks keys: {', '.join(missing)}")
    if extra:
        errors.append(f"{entry_id}: mapping entry has unknown keys: {', '.join(extra)}")
    if not isinstance(row.get("entry_id"), str) or not row.get("entry_id"):
        errors.append(f"{entry_id}: invalid entry_id")
    observed_in = row.get("observed_in")
    if (
        not isinstance(observed_in, list)
        or not observed_in
        or not all(isinstance(item, str) and item for item in observed_in)
        or len(observed_in) != len(set(t.cast(list[str], observed_in)))
    ):
        errors.append(f"{entry_id}: invalid observed_in")
    observation_hashes = row.get("observation_hashes")
    if observation_hashes is not None and (
        not isinstance(observation_hashes, dict)
        or not observation_hashes
        or not all(
            isinstance(source, str) and source and is_sha256_digest(digest)
            for source, digest in observation_hashes.items()
        )
    ):
        errors.append(f"{entry_id}: invalid observation_hashes")
    for field in (
        "cpp_symbol",
        "cpp_api_id",
        "cpp_alias_of",
        "compile_probe",
        "doc_id",
        "example_id",
        "error_behavior",
        "semantic_delta",
        "oracle_id",
        "approval_id",
        "inapplicability_proof",
    ):
        value = row.get(field)
        if value is not None and (not isinstance(value, str) or not value):
            errors.append(f"{entry_id}: invalid {field}")
    for field in ("behavior_tests", "tmux_versions", "boundary_tests"):
        value = row.get(field)
        if (
            not isinstance(value, list)
            or not all(isinstance(item, str) and item for item in value)
            or len(value) != len(set(t.cast(list[str], value)))
        ):
            errors.append(f"{entry_id}: invalid {field}")
    reconciliation = row.get("reconciliation")
    if reconciliation is not None and (
        not isinstance(reconciliation, (dict, str)) or not reconciliation
    ):
        errors.append(f"{entry_id}: invalid reconciliation")
    status = row.get("status")
    if status not in _STATUSES:
        errors.append(f"{entry_id}: unknown status {status!r}")
        return errors
    if complete and status == "pending":
        errors.append(f"{entry_id}: pending entry is incomplete")
    is_alias = bool(row.get("cpp_alias_of"))
    behavior_value = row.get("behavior_tests")
    behavior_refs = (
        t.cast(list[object], behavior_value) if isinstance(behavior_value, list) else []
    )
    boundary_value = row.get("boundary_tests")
    boundary_refs = (
        t.cast(list[object], boundary_value) if isinstance(boundary_value, list) else []
    )
    implementation_refs = (
        row.get("compile_probe"),
        row.get("doc_id"),
        row.get("example_id"),
        *behavior_refs,
        *boundary_refs,
    )
    if status == "pending":
        errors.extend(
            f"{entry_id}: pending entry claims {field}"
            for field in (
                "cpp_symbol",
                "cpp_api_id",
                "cpp_alias_of",
                "error_behavior",
            )
            if row.get(field) is not None
        )
        if any(reference is not None for reference in implementation_refs):
            errors.append(f"{entry_id}: pending entry claims implementation evidence")
        if row.get("tmux_versions") not in ([], None):
            errors.append(f"{entry_id}: pending entry claims tmux_versions")
        errors.extend(
            f"{entry_id}: pending entry claims {field}"
            for field in ("reconciliation", "inapplicability_proof")
            if row.get(field) is not None
        )
        decision_fields = (
            row.get("semantic_delta"),
            row.get("oracle_id"),
            row.get("approval_id"),
        )
        if any(decision_fields) and not all(decision_fields):
            errors.append(f"{entry_id}: pending decision is partially bound")
    elif status in {"implemented", "adapted"}:
        errors.extend(
            f"{entry_id}: {status} entry lacks {field}"
            for field in ("cpp_symbol", "cpp_api_id", "error_behavior")
            if not row.get(field)
        )
        if not row.get("tmux_versions"):
            errors.append(f"{entry_id}: {status} entry lacks tmux_versions")
        if not is_alias:
            errors.extend(
                f"{entry_id}: {status} entry lacks {field}"
                for field in ("compile_probe", "doc_id", "example_id")
                if not row.get(field)
            )
        if not row.get("behavior_tests") and not is_alias:
            errors.append(f"{entry_id}: {status} entry lacks behavior_tests")
        if status == "implemented":
            errors.extend(
                f"{entry_id}: implemented entry claims {field}"
                for field in (
                    "semantic_delta",
                    "oracle_id",
                    "approval_id",
                    "inapplicability_proof",
                )
                if row.get(field) is not None
            )
        else:
            errors.extend(
                f"{entry_id}: adapted entry lacks {field}"
                for field in ("semantic_delta", "oracle_id", "approval_id")
                if not row.get(field)
            )
            if row.get("inapplicability_proof") is not None:
                errors.append(f"{entry_id}: adapted entry claims inapplicability_proof")
    else:
        errors.extend(
            f"{entry_id}: excluded entry claims {field}"
            for field in (
                "cpp_symbol",
                "cpp_api_id",
                "cpp_alias_of",
                "error_behavior",
            )
            if row.get(field) is not None
        )
        if any(reference is not None for reference in implementation_refs):
            errors.append(f"{entry_id}: excluded entry claims implementation evidence")
        if not row.get("inapplicability_proof"):
            errors.append(f"{entry_id}: excluded entry lacks inapplicability_proof")
        if not row.get("approval_id"):
            errors.append(f"{entry_id}: excluded entry lacks approval_id")
        errors.extend(
            f"{entry_id}: excluded entry claims {field}"
            for field in ("semantic_delta", "oracle_id", "reconciliation")
            if row.get(field) is not None
        )
        if row.get("tmux_versions") not in ([], None):
            errors.append(f"{entry_id}: excluded entry claims tmux_versions")
    return errors


def _conflict_resolution_errors(
    entry_id: str,
    row: t.Mapping[str, object],
) -> list[str]:
    """Validate pending and promoted handling of an observed source conflict.

    Parameters
    ----------
    entry_id : str
        Conflicting observation ID.
    row : Mapping[str, object]
        Reviewed mapping row.

    Returns
    -------
    list[str]
        Conflict-resolution violations.

    Examples
    --------
    >>> _conflict_resolution_errors("x", {"status": "pending"})
    []
    """
    status = row.get("status")
    if status == "pending":
        return (
            [f"{entry_id}: pending conflict fabricates reconciliation or evidence"]
            if _pending_conflict_claims(row)
            else []
        )
    if status == "adapted":
        return (
            []
            if _adapted_conflict_reconciliation(row)
            else [f"{entry_id}: adapted conflict lacks reconciliation"]
        )
    if status == "implemented" and _compatible_conflict_reconciliation(row):
        return []
    return [f"{entry_id}: {status} conflict remains unresolved"]


def _compatible_conflict_reconciliation(row: t.Mapping[str, object]) -> bool:
    """Return whether one reviewed C++ surface covers both observations.

    Parameters
    ----------
    row : Mapping[str, object]
        Implemented conflicting mapping row.

    Returns
    -------
    bool
        Whether the exact compatible-surface descriptor is present.

    Examples
    --------
    >>> _compatible_conflict_reconciliation({
    ...     "observed_in": ["release", "development"],
    ...     "reconciliation": {
    ...         "kind": "compatible",
    ...         "observations": ["release", "development"],
    ...     },
    ... })
    True
    """
    reconciliation = row.get("reconciliation")
    if not isinstance(reconciliation, dict) or set(reconciliation) != {
        "kind",
        "observations",
    }:
        return False
    return reconciliation.get("kind") == "compatible" and reconciliation.get(
        "observations"
    ) == row.get("observed_in")


def _adapted_conflict_reconciliation(row: t.Mapping[str, object]) -> bool:
    """Return whether a conflict declares an adapted or versioned surface.

    Parameters
    ----------
    row : Mapping[str, object]
        Adapted conflicting mapping row.

    Returns
    -------
    bool
        Whether a supported explicit reconciliation descriptor is present.

    Examples
    --------
    >>> _adapted_conflict_reconciliation(
    ...     {"reconciliation": {"kind": "versioned"}}
    ... )
    True
    """
    reconciliation = row.get("reconciliation")
    return isinstance(reconciliation, dict) and reconciliation.get("kind") in {
        "adapted",
        "versioned",
    }


def _pending_conflict_claims(row: t.Mapping[str, object]) -> bool:
    """Return whether a pending conflict claims a decision or implementation.

    Parameters
    ----------
    row : Mapping[str, object]
        Pending mapping row.

    Returns
    -------
    bool
        Whether the row violates the untouched-pending contract.

    Examples
    --------
    >>> _pending_conflict_claims({"status": "pending", "approval_id": None})
    False
    """
    scalar_claims = (
        "cpp_symbol",
        "cpp_api_id",
        "cpp_alias_of",
        "compile_probe",
        "doc_id",
        "example_id",
        "error_behavior",
        "semantic_delta",
        "oracle_id",
        "approval_id",
        "reconciliation",
        "inapplicability_proof",
    )
    list_claims = ("behavior_tests", "tmux_versions", "boundary_tests")
    return any(row.get(field) is not None for field in scalar_claims) or any(
        bool(row.get(field)) for field in list_claims
    )


def _cpp_ownership_errors(
    rows: t.Mapping[str, t.Mapping[str, object]],
) -> list[str]:
    """Validate canonical C++ declaration ownership and alias topology.

    Parameters
    ----------
    rows : Mapping[str, Mapping[str, object]]
        Mapping rows by entry ID.

    Returns
    -------
    list[str]
        Ownership and alias violations.

    Examples
    --------
    >>> _cpp_ownership_errors({})
    []
    """
    errors: list[str] = []
    owners: dict[str, list[str]] = {}
    for entry_id, row in rows.items():
        cpp_api_id = row.get("cpp_api_id")
        if isinstance(cpp_api_id, str) and cpp_api_id and not row.get("cpp_alias_of"):
            owners.setdefault(cpp_api_id, []).append(entry_id)
    for cpp_api_id, entry_ids in sorted(owners.items()):
        if len(entry_ids) != 1:
            errors.append(f"{cpp_api_id}: cpp_api_id lacks exactly one canonical owner")
    for entry_id, row in sorted(rows.items()):
        alias_of = row.get("cpp_alias_of")
        if not alias_of:
            continue
        target = rows.get(t.cast(str, alias_of))
        if target is None:
            errors.append(f"{entry_id}: alias target does not exist")
            continue
        if target.get("cpp_alias_of"):
            errors.append(f"{entry_id}: alias target is not a canonical owner")
        if row.get("cpp_api_id") != target.get("cpp_api_id"):
            errors.append(f"{entry_id}: alias cpp_api_id differs from canonical owner")
        errors.extend(
            f"{entry_id}: alias claims conflicting {field} evidence"
            for field in ("compile_probe", "doc_id", "example_id")
            if row.get(field) is not None
        )
    claimed_ids = {
        t.cast(str, row["cpp_api_id"])
        for row in rows.values()
        if isinstance(row.get("cpp_api_id"), str) and row.get("cpp_api_id")
    }
    errors.extend(
        f"{cpp_api_id}: cpp_api_id lacks exactly one canonical owner"
        for cpp_api_id in sorted(claimed_ids)
        if len(owners.get(cpp_api_id, [])) != 1
    )
    return list(dict.fromkeys(errors))


def _approval_records(
    approvals: t.Mapping[str, object], errors: list[str]
) -> dict[str, dict[str, object]]:
    """Validate and index normalized approval records.

    Parameters
    ----------
    approvals : Mapping[str, object]
        Approval sidecar.
    errors : list[str]
        Error accumulator.

    Returns
    -------
    dict[str, dict[str, object]]
        First record for each approval ID.

    Examples
    --------
    >>> _approval_records({"approvals": []}, [])
    {}
    """
    raw_records = approvals.get("approvals")
    if not isinstance(raw_records, list):
        errors.append("approvals requires an approvals array")
        return {}
    index: dict[str, dict[str, object]] = {}
    for record in raw_records:
        if not isinstance(record, dict) or not isinstance(
            record.get("approval_id"), str
        ):
            errors.append("malformed approval record")
            continue
        approval_id = t.cast(str, record["approval_id"])
        if approval_id in index:
            errors.append(f"{approval_id}: duplicate approval_id")
            continue
        index[approval_id] = record
        if record.get("decision_kind") not in _DECISION_KINDS:
            errors.append(f"{approval_id}: unknown approval decision kind")
        scope = record.get("scope_entry_ids")
        if (
            not isinstance(scope, list)
            or not scope
            or not all(isinstance(item, str) and item for item in scope)
            or scope != sorted(set(scope))
        ):
            errors.append(f"{approval_id}: invalid approval scope")
        digest = record.get("accepted_decision_sha256")
        if not is_sha256_digest(digest):
            errors.append(f"{approval_id}: invalid accepted decision hash")
        if not is_safe_relative_path(record.get("evidence_path")):
            errors.append(f"{approval_id}: unsafe repository-relative path in approval")
    return index


def _approval_reference_errors(
    rows: t.Mapping[str, t.Mapping[str, object]],
    approvals: t.Mapping[str, t.Mapping[str, object]],
) -> list[str]:
    """Validate approval scope ownership and current decision digests.

    Parameters
    ----------
    rows : Mapping[str, Mapping[str, object]]
        Mapping rows by ID.
    approvals : Mapping[str, Mapping[str, object]]
        Approval records by ID.

    Returns
    -------
    list[str]
        Approval reference violations.

    Examples
    --------
    >>> _approval_reference_errors({}, {})
    []
    """
    errors: list[str] = []
    references: dict[str, set[str]] = {}
    for entry_id, row in sorted(rows.items()):
        approval_id = row.get("approval_id")
        if not approval_id:
            continue
        if not isinstance(approval_id, str) or approval_id not in approvals:
            errors.append(f"{entry_id}: unknown approval reference {approval_id!r}")
            continue
        references.setdefault(approval_id, set()).add(entry_id)
        scope = approvals[approval_id].get("scope_entry_ids")
        if not isinstance(scope, list) or entry_id not in scope:
            errors.append(f"{entry_id}: approval scope does not own entry")
        status = row.get("status")
        expected_kind = {
            "pending": "pending_adaptation",
            "excluded": "exclusion",
        }.get(t.cast(str, status))
        if status == "adapted":
            expected_kind = (
                "reconciliation" if row.get("reconciliation") else "adaptation"
            )
        if (
            expected_kind
            and approvals[approval_id].get("decision_kind") != expected_kind
        ):
            errors.append(
                f"{entry_id}: approval decision kind does not match {status} entry"
            )
    mapping_document = {"entries": list(rows.values())}
    for approval_id, approval in sorted(approvals.items()):
        referenced = references.get(approval_id, set())
        scope_value = approval.get("scope_entry_ids")
        scope = set(scope_value) if isinstance(scope_value, list) else set()
        if not referenced:
            errors.append(f"{approval_id}: stale approval record")
        if referenced != scope:
            errors.append(f"{approval_id}: approval scope ownership mismatch")
        if approval.get("accepted_decision_sha256") != approval_decision_sha256(
            mapping_document, approval
        ):
            errors.append(f"{approval_id}: stale approval decision hash")
    return errors


def _evidence_records(
    evidence: t.Mapping[str, object], errors: list[str]
) -> dict[str, dict[str, object]]:
    """Validate and index normalized evidence records.

    Parameters
    ----------
    evidence : Mapping[str, object]
        Evidence sidecar.
    errors : list[str]
        Error accumulator.

    Returns
    -------
    dict[str, dict[str, object]]
        First record for each evidence ID.

    Examples
    --------
    >>> _evidence_records({"evidence": []}, [])
    {}
    """
    raw_records = evidence.get("evidence")
    if not isinstance(raw_records, list):
        errors.append("evidence requires an evidence array")
        return {}
    index: dict[str, dict[str, object]] = {}
    for record in raw_records:
        if not isinstance(record, dict) or not isinstance(
            record.get("evidence_id"), str
        ):
            errors.append("malformed evidence record")
            continue
        evidence_id = t.cast(str, record["evidence_id"])
        if evidence_id in index:
            errors.append(f"{evidence_id}: duplicate evidence_id")
            continue
        index[evidence_id] = record
        if record.get("kind") not in _EVIDENCE_KINDS:
            errors.append(f"{evidence_id}: unknown evidence kind")
        if not is_safe_relative_path(record.get("path")):
            errors.append(f"{evidence_id}: unsafe repository-relative path in evidence")
        if not isinstance(record.get("case_id"), str) or not record.get("case_id"):
            errors.append(f"{evidence_id}: evidence lacks case_id")
        if "status" in record and record.get("status") != "passed":
            errors.append(f"{evidence_id}: failed evidence record")
        errors.extend(
            f"{evidence_id}: invalid evidence {field}"
            for field in (
                "registration_sha256",
                "junit_sha256",
                "result_sha256",
                "tmux_binary_sha256",
                "scenario_record_sha256",
                "semantic_contract_sha256",
            )
            if field in record and not is_sha256_digest(record.get(field))
        )
    return index


def _evidence_reference_errors(
    rows: t.Mapping[str, t.Mapping[str, object]],
    evidence: t.Mapping[str, t.Mapping[str, object]],
    complete: bool,
) -> list[str]:
    """Validate typed, single-owner, current mapping evidence references.

    Parameters
    ----------
    rows : Mapping[str, Mapping[str, object]]
        Mapping rows by ID.
    evidence : Mapping[str, Mapping[str, object]]
        Evidence records by ID.
    complete : bool
        Whether executed evidence must carry every current result binding.

    Returns
    -------
    list[str]
        Evidence reference violations.

    Examples
    --------
    >>> _evidence_reference_errors({}, {}, False)
    []
    """
    errors: list[str] = []
    owners: dict[str, list[str]] = {}
    for entry_id, row in sorted(rows.items()):
        for evidence_id, expected_kinds in _row_evidence_references(row):
            owners.setdefault(evidence_id, []).append(entry_id)
            record = evidence.get(evidence_id)
            if record is None:
                errors.append(f"{entry_id}: unknown evidence reference {evidence_id!r}")
                continue
            if record.get("kind") not in expected_kinds:
                errors.append(f"{entry_id}: wrong evidence kind for {evidence_id}")
            if complete and record.get("kind") in {
                "behavior",
                "differential",
                "version",
            }:
                errors.extend(_executed_evidence_errors(evidence_id, record))
    for evidence_id, entry_ids in sorted(owners.items()):
        if len(set(entry_ids)) > 1:
            errors.append(f"{evidence_id}: multiply owned evidence")
    errors.extend(
        f"{evidence_id}: stale evidence record"
        for evidence_id in sorted(set(evidence) - set(owners))
    )
    return errors


def _row_evidence_references(
    row: t.Mapping[str, object],
) -> list[tuple[str, set[str]]]:
    """Return kind-constrained evidence references declared by one row.

    Parameters
    ----------
    row : Mapping[str, object]
        Mapping row.

    Returns
    -------
    list[tuple[str, set[str]]]
        Evidence IDs with their allowed kinds.

    Examples
    --------
    >>> _row_evidence_references({"compile_probe": "compile.x"})
    [('compile.x', {'compile'})]
    """
    references: list[tuple[str, set[str]]] = []
    for field, kinds in (
        ("compile_probe", {"compile"}),
        ("doc_id", {"documentation"}),
        ("example_id", {"example"}),
        ("oracle_id", {"differential"}),
    ):
        value = row.get(field)
        if isinstance(value, str) and value:
            references.append((value, kinds))
    for field, kinds in (
        ("behavior_tests", {"behavior"}),
        ("boundary_tests", {"differential", "version"}),
    ):
        values = row.get(field)
        if isinstance(values, list):
            references.extend(
                (value, kinds) for value in values if isinstance(value, str) and value
            )
    return references


def _executed_evidence_errors(
    evidence_id: str,
    record: t.Mapping[str, object],
) -> list[str]:
    """Validate immutable execution bindings for a runtime evidence record.

    Parameters
    ----------
    evidence_id : str
        Evidence record ID.
    record : Mapping[str, object]
        Executed evidence record.

    Returns
    -------
    list[str]
        Missing or invalid execution fields.

    Examples
    --------
    >>> bool(_executed_evidence_errors("x", {}))
    True
    """
    errors: list[str] = []
    errors = [
        f"{evidence_id}: executed evidence lacks {field}"
        for field in (
            "status",
            "result_sha256",
            "tmux_binary_sha256",
            "tmux_version",
        )
        if not record.get(field)
    ]
    if record.get("kind") == "behavior":
        errors.extend(
            f"{evidence_id}: behavior evidence lacks {field}"
            for field in (
                "cmake_target",
                "ctest_name",
                "ctest_label",
                "execution_mode",
                "registration_sha256",
                "junit_sha256",
            )
            if not record.get(field)
        )
        if record.get("real_tmux") is not True:
            errors.append(f"{evidence_id}: behavior evidence is not real tmux")
    if record.get("kind") == "differential" and not record.get(
        "scenario_record_sha256"
    ):
        errors.append(
            f"{evidence_id}: differential evidence lacks scenario_record_sha256"
        )
    if record.get("kind") == "differential" and not record.get(
        "semantic_contract_sha256"
    ):
        errors.append(
            f"{evidence_id}: differential evidence lacks semantic_contract_sha256"
        )
    return errors


def _union_entry_documents(
    observations: t.Sequence[t.Mapping[str, object]],
) -> dict[str, dict[str, object]]:
    """Return the first source document for each exact unioned entry ID.

    Parameters
    ----------
    observations : Sequence[Mapping[str, object]]
        Generated observations in source order.

    Returns
    -------
    dict[str, dict[str, object]]
        Entry documents keyed by ID.

    Examples
    --------
    >>> _union_entry_documents(({"entries": [{"entry_id": "x"}]},))["x"]
    {'entry_id': 'x'}
    """
    union: dict[str, dict[str, object]] = {}
    for observation in observations:
        entries = observation.get("entries")
        if not isinstance(entries, list):
            continue
        for entry in entries:
            if isinstance(entry, dict) and isinstance(entry.get("entry_id"), str):
                union.setdefault(t.cast(str, entry["entry_id"]), entry)
    return union
