"""Validate the closed transport bakeoff decision and its evidence graph."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import pathlib
import re
import stat
import subprocess
import sys
import typing as t

from tools.bakeoff.measure_transport import (
    _MEASUREMENT_FAIRNESS,
    _TASK8_STATUS_ALLOWLIST,
    CANDIDATES,
    GateValidationError,
    MeasurementValidationError,
    _normalize_allocation,
    _normalize_batches,
    _validate_environment,
    _validate_helpers,
    canonical_json_bytes,
    validate_gate_record,
)

Json = dict[str, t.Any]

_DIGEST = re.compile(r"^sha256:[0-9a-f]{64}$")
_OBJECT_ID = re.compile(r"^[0-9a-f]{40}$")
_REVIEW_AXES = (
    "lifetime_ownership",
    "hidden_serialization",
    "diagnostic_reentrancy",
    "exception_containment",
    "timeout_certainty",
    "transport_leakage",
    "control_group_attribution",
    "engine_coupling",
    "measurement_fairness",
)
_DECISIVE_AXES = (
    "server_create_allocations",
    "wrapper_minus_common_allocations",
    "wrapper_minus_common_runtime",
)
_NON_DECISIVE_AXES = (
    "clean_compile_time",
    "controlled_incremental_time",
    "private_diagnostic_shape",
    "production_binary_sections",
    "production_source_footprint",
    "public_header_parse_time",
)
_LIMITATION_IDS = {
    "concurrent_hostile_build_mutation",
    "engine_ops_lifecycle",
    "engine_ops_materializer_publication",
    "engine_ops_not_claimed_transport_selection",
    "engine_ops_not_claimed_performance",
    "engine_ops_not_claimed_cross_version_tmux_behavior",
    "engine_ops_not_claimed_concrete_python_operation_parity",
    "engine_ops_process_adapter",
    "engine_ops_pre_3_7_real_runtime",
    "engine_ops_warning_channel_parity",
}
_EXPECTED_TRANSPORT_FILES = {
    "decision.json",
    "diagnostics",
    "measurements.json",
    "review.md",
    "scorecard.md",
}
_EXPECTED_DIAGNOSTICS = {f"{candidate}.txt" for candidate in CANDIDATES}
_EXPECTED_GRAFT_FILES = {
    "control-mode.json",
    "control-mode.md",
    "engine-ops-source.json",
    "engine-ops.json",
    "engine-ops.md",
}
_FOLLOW_UP_FILES = {"measurement.json", "result.json", "review.md"}
_MEASUREMENT_ID = "transport.measurements.v1"
_ENVIRONMENT_ID = "transport.environment.v1"
_TRANSPORT_DIRECTORY = "docs/bakeoffs/transport"
_GRAFT_DIRECTORY = "docs/bakeoffs/grafts"
_ENVIRONMENT_FILE = "docs/bakeoffs/environment.json"
_MEASUREMENTS_FILE = f"{_TRANSPORT_DIRECTORY}/measurements.json"
_FOLLOW_UP_PLAN_DIRECTORY = "docs/plans/followups"
_GATE_ORDER = ("transport-sanitize", "transport-tsan")
_GATE_KINDS = {"transport-sanitize": "sanitize", "transport-tsan": "tsan"}
_GATE_SELECTORS = {
    "transport-sanitize": ("cxx-sanitize", {"label": "transport"}),
    "transport-tsan": ("cxx-tsan", {"label": "concurrency"}),
}
_GRAFT_NAMES = ("control_mode", "engine_ops")
_GRAFT_PROBE_IDS = {"control_mode": "control-mode", "engine_ops": "engine-ops"}
_GRAFT_GATE_IDS = {
    "control_mode": ("graft-control-sanitize", "cxx-sanitize"),
    "engine_ops": ("graft-engine-ops", "cxx-dev"),
}
_GRAFT_CTEST_KEYS = {
    "executed_test_count",
    "execution_sha256",
    "gate_id",
    "gate_sha256",
    "junit_sha256",
    "preset",
    "registered_test_count",
    "registration_sha256",
    "selector",
    "status",
}
_CONTROL_GRAFT_KEYS = {
    "ctest_gate",
    "fail_fast_group",
    "golden_stream_ids",
    "probe_id",
    "schema_version",
    "socket_mode",
    "status",
    "tmux",
}
_ENGINE_GRAFT_KEYS = {
    "adapter_contract",
    "capability_adaptation",
    "ctest_gate",
    "entity_header_isolation",
    "limitations",
    "probe_id",
    "schema_version",
    "socket_mode",
    "source",
    "source_scope",
    "status",
    "tmux",
    "verification",
}
_SOURCE_LOCK_KEYS = {"commit", "files", "repository_uri", "schema_version", "tree"}
_MEASUREMENTS_KEYS = {
    "candidate_order",
    "candidates",
    "collection_observations_sha256",
    "environment",
    "environment_sha256",
    "hard_gate_helpers",
    "hard_gates",
    "measurement_fairness",
    "measurement_id",
    "repetitions",
    "schema_version",
    "source_context",
    "warmups",
}
_REVIEW_FIELDS = (
    "Status",
    "Unresolved findings",
    "Reviewed source commit",
    "Reviewed source tree",
    "Reviewed source manifest",
    "Decision core",
    "Findings",
)
_SCORECARD_FIELDS = (
    "Winner",
    "Method",
    "Decision core",
    "Decisive axes",
    "Non-decisive axes",
    "Fairness caveat",
)
_FOLLOW_UP_PLAN_FIELDS = (
    "Unknown",
    "Implementation",
    "Failing test",
    "Gates",
    "Measurement",
)
_FOLLOW_UP_REVIEW_FIELDS = (
    "Status",
    "Unresolved findings",
    "Source commit",
    "Source tree",
    "Source manifest",
    "Follow-up core",
)
_SELECTION_METHOD_PROSE = "evidence-led selection without numeric weights"
# Public evidence names its host only through a closed identity vocabulary. A
# value outside these shapes is treated as a disclosure rather than a typo,
# because a hostname or checkout path reaches the published artifact the same
# way an ordinary edit does.
_ENVIRONMENT_VALUE_SHAPES = {
    "architecture": r"[A-Za-z0-9_]+",
    "cmake_version": r"[0-9]+(\.[0-9]+){1,2}",
    "locale": r"[A-Za-z0-9_.@-]+",
    "ninja_version": r"[0-9]+(\.[0-9]+){1,2}",
    "operating_system": r"[A-Za-z]+",
    "stdlib": r"lib(c\+\+|stdc\+\+)",
    "tmux_version": r"[0-9]+\.[0-9]+[a-z]?",
}
_TMUX_VERSION_RAW = re.compile(r"tmux [0-9]+\.[0-9]+[a-z]?\n?")
_TMUX_CAPABILITY = re.compile(r"[a-z0-9][a-z0-9 .+-]*")
_METADATA_LINE = re.compile(r"^([A-Z][A-Za-z0-9 -]{0,39}): (.*)$")
_HOSTNAME = os.uname().nodename


class DecisionValidationError(ValueError):
    """Raised when transport selection evidence is incomplete or inconsistent."""


def _fail(detail: str) -> t.NoReturn:
    raise DecisionValidationError(detail)


def _sha256(payload: bytes) -> str:
    return f"sha256:{hashlib.sha256(payload).hexdigest()}"


def _digest(value: object, label: str) -> str:
    if not isinstance(value, str) or _DIGEST.fullmatch(value) is None:
        _fail(f"{label} digest is invalid")
    return value


def _object_id(value: object, label: str) -> str:
    if not isinstance(value, str) or _OBJECT_ID.fullmatch(value) is None:
        _fail(f"{label} is invalid")
    return value


def _exact_keys(value: object, expected: set[str], label: str) -> Json:
    if not isinstance(value, dict) or set(value) != expected:
        _fail(f"{label} has a closed schema")
    return t.cast(Json, value)


def _canonical_bytes(value: object) -> bytes:
    try:
        encoded = json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )
    except (TypeError, ValueError) as error:
        message = "decision contains an invalid JSON value"
        raise DecisionValidationError(message) from error
    return (encoded + "\n").encode()


def _read_regular(path: pathlib.Path, label: str) -> bytes:
    try:
        details = path.lstat()
    except OSError as error:
        message = f"{label} topology is incomplete"
        raise DecisionValidationError(message) from error
    if not stat.S_ISREG(details.st_mode) or path.is_symlink() or details.st_nlink != 1:
        _fail(f"{label} topology requires a regular single-link file")
    try:
        return path.read_bytes()
    except OSError as error:
        message = f"{label} cannot be read"
        raise DecisionValidationError(message) from error


def _parse_json(payload: bytes, label: str) -> Json:
    try:
        value = json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        message = f"{label} JSON is invalid"
        raise DecisionValidationError(message) from error
    if not isinstance(value, dict):
        _fail(f"{label} must be a JSON object")
    if payload != _canonical_bytes(value):
        _fail(f"{label} JSON is not canonical")
    return t.cast(Json, value)


def _read_json(path: pathlib.Path, label: str) -> Json:
    return _parse_json(_read_regular(path, label), label)


def decision_core_sha256(decision: Json) -> str:
    """Return the digest of the non-circular decision projection.

    >>> decision_core_sha256({"review": {}}).startswith("sha256:")
    True
    """
    projection = copy.deepcopy(decision)
    projection.pop("scorecard_sha256", None)
    review = projection.get("review")
    if isinstance(review, dict):
        review.pop("decision_core_sha256", None)
        review.pop("report_sha256", None)
    return _sha256(_canonical_bytes(projection))


def _evidence_ids(decision: Json) -> set[str]:
    result = {t.cast(str, decision["measurements_id"])}
    result.update(t.cast(str, row["gate_id"]) for row in decision["hard_gates"])
    result.update(t.cast(str, row["measurement_id"]) for row in decision["candidates"])
    result.update(t.cast(str, row["evidence_id"]) for row in decision["graft_evidence"])
    for finding in decision["review"]["findings"]:
        if isinstance(finding, dict) and isinstance(finding.get("finding_id"), str):
            result.add(t.cast(str, finding["finding_id"]))
    for unknown in decision["unknowns"]:
        if (
            isinstance(unknown, dict)
            and unknown.get("disposition") == "follow_up_complete"
        ):
            evidence = unknown.get("evidence_id")
            if isinstance(evidence, str):
                result.add(evidence)
    return result


def _validate_selection(decision: Json) -> None:
    if isinstance(decision["selection"], dict):
        if "weights" in decision["selection"]:
            _fail("selection weights are forbidden")
        if "winner_evidence" in decision["selection"]:
            _fail("non-decisive axes cannot select the winner")
    selection = _exact_keys(
        decision["selection"],
        {"decisive_axes", "method", "non_decisive_axes"},
        "selection",
    )
    if selection["method"] != "evidence_led_no_weights":
        _fail("selection uses a weighted method")
    decisive = selection["decisive_axes"]
    non_decisive = selection["non_decisive_axes"]
    if decisive != list(_DECISIVE_AXES):
        _fail("selection decisive axes do not match")
    if non_decisive != list(_NON_DECISIVE_AXES):
        _fail("selection non-decisive axes do not match")
    if set(t.cast(list[str], decisive)) & set(t.cast(list[str], non_decisive)):
        _fail("selection decisive axes overlap non-decisive axes")


def _validate_unknown(unknown: object) -> None:
    if not isinstance(unknown, dict):
        _fail("unknown row is invalid")
    materiality = unknown.get("materiality")
    disposition = unknown.get("disposition")
    if materiality == "material":
        if disposition != "follow_up_complete":
            _fail("material unknown remains open")
        follow_up = unknown.get("follow_up")
        required = {
            "commit",
            "committed_paths",
            "core_sha256",
            "diff_tree",
            "gate_transition",
            "hard_gates",
            "implementation",
            "measurement",
            "parent_commit",
            "plan",
            "result",
            "review",
            "source_sha256",
            "test",
            "tree",
        }
        if not isinstance(follow_up, dict) or set(follow_up) != required:
            _fail("follow-up result is incomplete")
    elif materiality == "non_material":
        rationale = unknown.get("contract_impact_rationale")
        if not isinstance(rationale, str) or not rationale.strip():
            _fail("non-material unknown lacks contract-impact rationale")
    else:
        _fail("unknown materiality is invalid")


def validate_decision(value: object, *, require_review_closed: bool) -> None:
    """Validate the closed in-memory decision schema and evidence references."""
    expected = {
        "accepted_grafts",
        "axis",
        "candidates",
        "environment_id",
        "environment_sha256",
        "graft_evidence",
        "hard_gates",
        "measurements_id",
        "measurements_sha256",
        "rejected_tradeoffs",
        "review",
        "schema_version",
        "scorecard_sha256",
        "selection",
        "source",
        "status",
        "unknowns",
        "winner",
    }
    if isinstance(value, dict) and "post_fix_hard_gates" in value:
        expected.add("post_fix_hard_gates")
    decision = _exact_keys(value, expected, "decision")
    if decision["schema_version"] != 1 or decision["axis"] != "transport":
        _fail("decision axis or schema is invalid")
    if decision["status"] != "selected" or decision["winner"] not in CANDIDATES:
        _fail("decision winner is invalid")
    if decision["accepted_grafts"] != ["control_mode", "engine_ops"]:
        _fail("accepted graft inventory is incomplete")
    candidates = decision["candidates"]
    if not isinstance(candidates, list) or [
        row.get("candidate_id") for row in candidates if isinstance(row, dict)
    ] != list(CANDIDATES):
        _fail("candidate inventory is incomplete")
    selected = [row for row in candidates if row.get("disposition") == "selected"]
    if len(selected) != 1 or selected[0].get("candidate_id") != decision["winner"]:
        _fail("candidate winner disposition is inconsistent")
    for row in candidates:
        if set(row) != {
            "candidate_id",
            "disposition",
            "hard_gate_ids",
            "measurement_id",
            "public_contract",
            "source_sha256",
        }:
            _fail("candidate row has a closed schema")
        if row["hard_gate_ids"] != ["transport-sanitize", "transport-tsan"]:
            _fail("candidate hard gate inventory is incomplete")
        if row["public_contract"] != "identical":
            _fail("candidate public contract differs")
        _digest(row["source_sha256"], "candidate source")
    gates = decision["hard_gates"]
    if not isinstance(gates, list) or len(gates) != 2:
        _fail("hard gate inventory is incomplete")
    if [row.get("gate_id") for row in gates if isinstance(row, dict)] != [
        "transport-sanitize",
        "transport-tsan",
    ]:
        _fail("hard gate inventory is incomplete")
    review = decision["review"]
    if not isinstance(review, dict):
        _fail("review is invalid")
    if require_review_closed and (
        review.get("status") != "closed" or review.get("unresolved_findings") != 0
    ):
        _fail("review is not closed")
    source = _exact_keys(
        decision["source"], {"commit", "transport_source_sha256", "tree"}, "source"
    )
    _object_id(source["commit"], "source commit")
    _object_id(source["tree"], "source tree")
    _digest(source["transport_source_sha256"], "source")
    if any(
        review.get(key) != source[source_key]
        for key, source_key in (
            ("reviewed_commit", "commit"),
            ("reviewed_tree", "tree"),
            ("reviewed_source_sha256", "transport_source_sha256"),
        )
    ):
        _fail("reviewed source does not match selected source")
    axes = review.get("axes")
    if not isinstance(axes, list) or [
        row.get("axis") for row in axes if isinstance(row, dict)
    ] != list(_REVIEW_AXES):
        _fail("review axes are incomplete")
    findings = review.get("findings")
    if not isinstance(findings, list):
        _fail("review findings are invalid")
    for finding in findings:
        if not isinstance(finding, dict):
            _fail("review finding is invalid")
        if (
            finding.get("impact") in {"correctness", "public_contract"}
            and finding.get("disposition") != "fixed"
        ):
            _fail("correctness or public-contract finding cannot be waived")
    _validate_selection(decision)
    unknowns = decision["unknowns"]
    if not isinstance(unknowns, list) or not unknowns:
        _fail("limitation inventory is incomplete")
    for unknown in unknowns:
        _validate_unknown(unknown)
    evidence = _evidence_ids(decision)
    references: list[str] = []
    for row in axes:
        references.extend(t.cast(list[str], row.get("evidence_ids", [])))
    for row in decision["rejected_tradeoffs"]:
        if isinstance(row, dict):
            references.extend(t.cast(list[str], row.get("evidence_ids", [])))
    references.extend(
        t.cast(str, row["evidence_id"]) for row in unknowns if isinstance(row, dict)
    )
    if any(reference not in evidence for reference in references):
        _fail("evidence ID does not resolve")
    claimed_core = review.get("decision_core_sha256")
    if claimed_core != decision_core_sha256(decision):
        _fail("decision core digest does not match")


def _git_output(root: pathlib.Path, *arguments: str) -> str:
    try:
        completed = subprocess.run(
            ["git", "-C", str(root), *arguments],
            check=True,
            close_fds=True,
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.SubprocessError) as error:
        message = f"Git command {arguments[0]!r} did not complete"
        raise DecisionValidationError(message) from error
    return completed.stdout


def _git(root: pathlib.Path, *arguments: str) -> str:
    return _git_output(root, *arguments).strip()


def _status_path(line: str) -> str:
    path = line[3:]
    if " -> " in path:
        path = path.split(" -> ", 1)[1]
    return path.strip('"')


def _reject_private_literals(payload: bytes, label: str, root: pathlib.Path) -> None:
    for secret in (str(root), _HOSTNAME):
        if len(secret) >= 4 and secret.encode() in payload:
            _fail(f"{label} discloses a private host or repository identity")


def _public_bytes(path: pathlib.Path, label: str, root: pathlib.Path) -> bytes:
    payload = _read_regular(path, label)
    _reject_private_literals(payload, label, root)
    return payload


def _public_document(
    path: pathlib.Path, label: str, root: pathlib.Path
) -> tuple[bytes, Json]:
    payload = _public_bytes(path, label, root)
    return payload, _parse_json(payload, label)


def _public_json(path: pathlib.Path, label: str, root: pathlib.Path) -> Json:
    return _public_document(path, label, root)[1]


def _decoded(payload: bytes, label: str) -> str:
    try:
        return payload.decode()
    except UnicodeDecodeError as error:
        message = f"{label} is not UTF-8 text"
        raise DecisionValidationError(message) from error


def _public_text(path: pathlib.Path, label: str, root: pathlib.Path) -> str:
    return _decoded(_public_bytes(path, label, root), label)


def _metadata_fields(text: str, label: str, allowed: tuple[str, ...]) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in text.splitlines():
        matched = _METADATA_LINE.match(line)
        if matched is None:
            continue
        key, value = matched.group(1), matched.group(2)
        if key not in allowed:
            _fail(f"{label} field {key!r} may disclose private data")
        if key in fields:
            _fail(f"{label} repeats the {key!r} field")
        fields[key] = value
    missing = [key for key in allowed if key not in fields]
    if missing:
        _fail(f"{label} omits the {missing[0]!r} field")
    return fields


def _require_field(fields: dict[str, str], key: str, expected: str, label: str) -> None:
    if fields[key] != expected:
        _fail(f"{label} field {key!r} does not match the decision")


def _closed_record(text: str, label: str, allowed: tuple[str, ...]) -> dict[str, str]:
    """Read a structured evidence record whose every line is a known field."""
    for line in text.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        if _METADATA_LINE.match(line) is None:
            _fail(f"{label} contains an unrecognized line")
    return _metadata_fields(text, label, allowed)


def _reject_private_environment(environment: object, label: str) -> None:
    if not isinstance(environment, dict):
        _fail(f"{label} must be a JSON object")
    for key, shape in _ENVIRONMENT_VALUE_SHAPES.items():
        value = environment.get(key)
        if not isinstance(value, str) or re.fullmatch(shape, value) is None:
            _fail(f"{label} field {key!r} may disclose private data")


def _reject_private_graft(document: Json, label: str) -> None:
    tmux = document.get("tmux")
    if not isinstance(tmux, dict):
        _fail(f"{label} omits its tmux identity")
    raw = tmux.get("version_raw")
    if not isinstance(raw, str) or _TMUX_VERSION_RAW.fullmatch(raw) is None:
        _fail(f"{label} field 'version_raw' may disclose private data")
    _digest(tmux.get("binary_sha256"), f"{label} tmux binary")
    capability = tmux.get("live_capability")
    if capability is not None and (
        not isinstance(capability, str)
        or _TMUX_CAPABILITY.fullmatch(capability) is None
    ):
        _fail(f"{label} field 'live_capability' may disclose private data")


def _directory_names(path: pathlib.Path, label: str) -> set[str]:
    try:
        return {entry.name for entry in path.iterdir()}
    except OSError as error:
        message = f"{label} topology is incomplete"
        raise DecisionValidationError(message) from error


def _require_topology(path: pathlib.Path, expected: set[str], label: str) -> None:
    if _directory_names(path, label) != expected:
        _fail(f"{label} topology is not the exact closed artifact set")


def _validate_gate_evidence(
    decision: Json, measurements: Json, root: pathlib.Path
) -> Json:
    """Revalidate both immutable gate records against their live build trees."""
    live: Json = {}
    for row, gate_id in zip(decision["hard_gates"], _GATE_ORDER, strict=True):
        preset, selector = _GATE_SELECTORS[gate_id]
        record = root / t.cast(str, row["record_path"])
        record_bytes = _read_regular(record, f"hard gate {gate_id} record")
        try:
            validated = validate_gate_record(
                record,
                source_dir=root / "cxx",
                expected_gate_id=gate_id,
                expected_preset=preset,
                expected_selector=selector,
            )
        except GateValidationError as error:
            message = f"hard gate {gate_id} record is invalid: {error}"
            raise DecisionValidationError(message) from error
        if _sha256(record_bytes) != row["record_sha256"]:
            _fail(f"hard gate {gate_id} record digest does not match the decision")
        for key in ("gate_sha256", "preset", "selector", "status"):
            if row[key] != validated[key]:
                _fail(f"hard gate {gate_id} {key} does not match the executed record")
        live[gate_id] = validated
    published = measurements.get("hard_gates")
    if not isinstance(published, dict) or set(published) != {"sanitize", "tsan"}:
        _fail("measured hard gate inventory is incomplete")
    for gate_id in _GATE_ORDER:
        if published[_GATE_KINDS[gate_id]] != live[gate_id]:
            _fail(f"measured hard gate {gate_id} identity does not match its record")
    return live


def _validate_candidate_samples(row: Json) -> None:
    workload = row["workload"]
    samples = row["samples"]
    try:
        _normalize_batches(
            samples["common_validation_batches"],
            "common validation",
            7,
            t.cast(int, workload["common_validation_iterations"]),
        )
        _normalize_batches(
            samples["wrapper_dispatch_batches"],
            "wrapper dispatch",
            7,
            t.cast(int, workload["dispatch_iterations"]),
        )
        _normalize_allocation(
            samples["allocations"]["common_validation"],
            "common validation",
            7,
            t.cast(int, workload["common_validation_iterations"]),
        )
        _normalize_allocation(
            samples["allocations"]["wrapper_dispatch"],
            "wrapper dispatch",
            7,
            t.cast(int, workload["dispatch_iterations"]),
        )
        _normalize_allocation(
            samples["allocations"]["server_create"],
            "server create",
            7,
            t.cast(int, workload["server_create_iterations"]),
        )
    except (MeasurementValidationError, KeyError, TypeError) as error:
        message = f"published measurement samples are invalid: {error}"
        raise DecisionValidationError(message) from error


def _validate_published_measurements(
    measurements: Json,
    decision: Json,
    *,
    root: pathlib.Path,
    environment: Json,
    environment_bytes: bytes,
) -> None:
    """Revalidate the published measurement against its live evidence inputs."""
    document = _exact_keys(measurements, _MEASUREMENTS_KEYS, "published measurement")
    if (
        document["schema_version"] != 1
        or document["repetitions"] != 7
        or document["warmups"] != 2
        or document["candidate_order"] != "round_robin"
    ):
        _fail("published measurement protocol is not the prescribed 2+7 round robin")
    if document["measurement_id"] != decision["measurements_id"]:
        _fail("measurement ID does not match the decision")
    if document["measurement_id"] != _MEASUREMENT_ID:
        _fail("measurement ID is not the frozen transport identity")
    candidates = document["candidates"]
    if not isinstance(candidates, list) or [
        row.get("candidate_id") for row in candidates if isinstance(row, dict)
    ] != list(CANDIDATES):
        _fail("published candidate inventory is incomplete")
    for row, decided in zip(candidates, decision["candidates"], strict=True):
        candidate = t.cast(str, row["candidate_id"])
        if row["measurement_id"] != f"transport.measurement.{candidate}":
            _fail("candidate measurement ID does not match its candidate")
        if row["measurement_id"] != decided["measurement_id"]:
            _fail("candidate measurement ID does not match the decision")
    compiler = t.cast(Json, document["environment"]).get("compiler")
    for row in candidates:
        if row["compiler"] != compiler:
            _fail("candidate compiler identity is not cross-bound")
    for kind in ("sanitize", "tsan"):
        if document["hard_gates"][kind]["compiler"] != compiler:
            _fail("hard gate compiler identity is not cross-bound")
    if environment_bytes != canonical_json_bytes(document["environment"]):
        _fail("environment does not match the published measurement environment")
    for row, decided in zip(candidates, decision["candidates"], strict=True):
        source = row["source"]
        if source["sha256"] != _sha256(canonical_json_bytes(source["files"])):
            _fail("candidate source manifest digest does not match its files")
        if source["sha256"] != decided["source_sha256"]:
            _fail("candidate source digest does not match the decision")
        if row["hard_gate_ids"] != list(_GATE_ORDER):
            _fail("candidate hard gate inventory is incomplete")
    headers = {row["source"]["files"][0]["sha256"] for row in candidates}
    if len(headers) != 1:
        _fail("candidate public headers are not byte-identical")
    try:
        _validate_environment(copy.deepcopy(document["environment"]))
    except MeasurementValidationError as error:
        message = f"published measurement environment is invalid: {error}"
        raise DecisionValidationError(message) from error
    if document["environment_sha256"] != environment["sha256"]:
        _fail("published measurement environment digest does not match")
    try:
        _validate_helpers(document["hard_gate_helpers"], root)
    except MeasurementValidationError as error:
        message = f"hard-gate helper evidence is invalid: {error}"
        raise DecisionValidationError(message) from error
    for row in candidates:
        _validate_candidate_samples(row)
    for row in candidates:
        candidate = t.cast(str, row["candidate_id"])
        diagnostic = row["diagnostic"]
        if diagnostic["path"] != f"diagnostics/{candidate}.txt":
            _fail(f"{candidate} diagnostic path is not the published location")
        published = root / _TRANSPORT_DIRECTORY / t.cast(str, diagnostic["path"])
        payload = _public_bytes(published, f"{candidate} diagnostic", root)
        if diagnostic["sha256"] != _sha256(payload):
            _fail(f"{candidate} diagnostic digest does not match the measurement")


def _validate_git_scope(
    decision: Json,
    measurements: Json,
    *,
    root: pathlib.Path,
    declared: set[str],
) -> None:
    """Bind the measured source identity to the live repository state.

    Only the measured source manifest is re-read from disk. The recorded
    allowed-change digests are collection-time values that publishing the
    decision, scorecard, and review legitimately supersedes, so live scope is
    proved from Git rather than from those digests.
    """
    context = _exact_keys(
        measurements["source_context"],
        {"exclusions", "files", "git", "sha256"},
        "measured source context",
    )
    rows = context["files"]
    if not isinstance(rows, list) or not rows:
        _fail("measured source context names no file")
    for item in rows:
        row = _exact_keys(item, {"path", "role", "sha256"}, "measured source file")
        path = root / t.cast(str, row["path"])
        if row["sha256"] != _sha256(_read_regular(path, "measured source file")):
            _fail("measured source file digest does not match the live file")
    if context["sha256"] != _sha256(canonical_json_bytes(rows)):
        _fail("measured source manifest digest does not match its files")
    git = _exact_keys(
        context["git"],
        {
            "allowed_changes",
            "commit",
            "scope_clean",
            "status_allowlist",
            "tree",
            "unexpected_paths",
        },
        "measured source Git identity",
    )
    if git["scope_clean"] is not True or git["unexpected_paths"] != []:
        _fail("measured source was not collected from a clean scope")
    if git["status_allowlist"] != list(_TASK8_STATUS_ALLOWLIST):
        _fail("measured source allowlist does not match the Task 8 scope")
    head = _git(root, "rev-parse", "HEAD")
    tree = _git(root, "rev-parse", "HEAD^{tree}")
    if git["commit"] != head or decision["source"]["commit"] != head:
        _fail("measured Git commit does not match the repository HEAD")
    if git["tree"] != tree or decision["source"]["tree"] != tree:
        _fail("measured Git tree does not match the repository HEAD")
    if context["sha256"] != decision["source"]["transport_source_sha256"]:
        _fail("measured source manifest does not match the selected source")
    allowed = set(_TASK8_STATUS_ALLOWLIST) | declared
    # Porcelain status columns are significant: stripping the output would
    # eat the leading space of the first " M path" row and shift its path.
    rows = [
        _status_path(line)
        for line in _git_output(
            root, "status", "--porcelain=v1", "--untracked-files=all"
        ).splitlines()
        if line
    ]
    unexpected = sorted(path for path in rows if path not in allowed)
    if unexpected:
        _fail(f"dirty source outside the Task 8 allowlist: {unexpected[0]}")


def _validate_graft_gate(document: Json, name: str, label: str) -> Json:
    gate = _exact_keys(document["ctest_gate"], _GRAFT_CTEST_KEYS, f"{label} ctest gate")
    gate_id, preset = _GRAFT_GATE_IDS[name]
    if gate["gate_id"] != gate_id or gate["preset"] != preset:
        _fail(f"{label} ctest gate identity does not match")
    if gate["status"] != "passed":
        _fail(f"{label} ctest gate did not pass")
    executed = gate["executed_test_count"]
    if type(executed) is not int or executed <= 0:
        _fail(f"{label} ctest gate executed no tests")
    if gate["registered_test_count"] != executed:
        _fail(f"{label} ctest gate registered and executed counts differ")
    for key in (
        "execution_sha256",
        "gate_sha256",
        "junit_sha256",
        "registration_sha256",
    ):
        _digest(gate[key], f"{label} ctest gate {key}")
    return gate


def _validate_grafts(decision: Json, *, root: pathlib.Path) -> Json:
    """Bind every accepted graft report, source lock, and executed gate."""
    documents: Json = {}
    grafts = root / _GRAFT_DIRECTORY
    for row, name in zip(decision["graft_evidence"], _GRAFT_NAMES, strict=True):
        label = f"graft {name}"
        if row["evidence_id"] != f"graft.{name}" or row["status"] != "accepted":
            _fail(f"{label} evidence is not accepted")
        probe = _GRAFT_PROBE_IDS[name]
        if row["path"] != f"{_GRAFT_DIRECTORY}/{probe}.json":
            _fail(f"{label} evidence path is not the published location")
        if row["report_path"] != f"{_GRAFT_DIRECTORY}/{probe}.md":
            _fail(f"{label} report path is not the published location")
        payload, document = _public_document(
            grafts / f"{probe}.json", "graft evidence", root
        )
        report_bytes = _public_bytes(grafts / f"{probe}.md", "graft report", root)
        report = _decoded(report_bytes, "graft report")
        if row["sha256"] != _sha256(payload):
            _fail(f"{label} evidence digest does not match the decision")
        if row["report_sha256"] != _sha256(report_bytes):
            _fail(f"graft report {probe}.md digest does not match the decision")
        _reject_private_graft(document, "graft evidence")
        expected = _CONTROL_GRAFT_KEYS if name == "control_mode" else _ENGINE_GRAFT_KEYS
        document = _exact_keys(document, expected, label)
        if (
            document["probe_id"] != probe
            or document["schema_version"] != 1
            or document["socket_mode"] != "path"
            or document["status"] != "passed"
        ):
            _fail(f"{label} evidence did not pass")
        gate = _validate_graft_gate(document, name, label)
        executed = t.cast(int, gate["executed_test_count"])
        if f"all {executed} registered tests" not in report:
            _fail(f"{label} report does not state the executed gate selection")
        if t.cast(str, gate["gate_sha256"]).removeprefix("sha256:") not in report:
            _fail(f"{label} report does not state the executed gate digest")
        documents[name] = document
    engine = documents["engine_ops"]
    engine_row = decision["graft_evidence"][1]
    if engine_row["source_path"] != f"{_GRAFT_DIRECTORY}/engine-ops-source.json":
        _fail("graft engine_ops source path is not the published location")
    lock_bytes, lock = _public_document(
        grafts / "engine-ops-source.json", "graft source", root
    )
    if engine_row["source_sha256"] != _sha256(lock_bytes):
        _fail("graft source digest does not match the decision")
    lock = _exact_keys(lock, _SOURCE_LOCK_KEYS, "graft source lock")
    if lock["schema_version"] != 1:
        _fail("graft source lock schema is invalid")
    files = lock["files"]
    if not isinstance(files, list) or not files:
        _fail("graft source lock names no file")
    for item in files:
        entry = _exact_keys(item, {"blob", "mode", "path"}, "graft source lock file")
        if (
            re.fullmatch(r"[0-9a-f]{40}", t.cast(str, entry["blob"])) is None
            or entry["mode"] != "100644"
        ):
            _fail("graft source lock file identity is invalid")
    source = engine["source"]
    if source["source_lock_canonical_sha256"] != _sha256(canonical_json_bytes(lock)):
        _fail("graft source lock digest does not match its locked objects")
    if source["commit"] != lock["commit"] or source["tree"] != lock["tree"]:
        _fail("graft source lock identity does not match the graft evidence")
    if source["repository_uri"] != lock["repository_uri"]:
        _fail("graft source lock repository does not match the graft evidence")
    if source["inspected_paths"] != [item["path"] for item in files]:
        _fail("graft source lock path inventory does not match the graft evidence")
    if engine["entity_header_isolation"] != list(CANDIDATES):
        _fail("graft engine_ops does not isolate every candidate entity header")
    return documents


def _slug(claim: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", claim.lower()).strip("_")


def _required_limitations(measurements: Json, engine: Json) -> dict[str, str]:
    """Return every limitation the measurement and graft evidence still carries."""
    required: dict[str, str] = {}
    for row in measurements["candidates"]:
        for item in row["limitations"]:
            identifier = t.cast(str, item["id"])
            required[identifier] = f"candidates.*.limitations.{identifier}"
    limitations = engine["limitations"]
    for key in ("lifecycle", "materializer_publication", "process_adapter"):
        required[f"engine_ops_{key}"] = f"limitations.{key}"
    for index, claim in enumerate(limitations["not_claimed"]):
        required[f"engine_ops_not_claimed_{_slug(claim)}"] = (
            f"limitations.not_claimed[{index}]"
        )
    for key in ("pre_3_7_real_runtime", "warning_channel_parity"):
        required[f"engine_ops_{key}"] = (
            f"capability_adaptation.kill_session_group.{key}"
        )
    return required


def _validate_limitations(
    decision: Json, measurements: Json, engine: Json, scorecard: str
) -> None:
    """Require every retained limitation in the decision and the scorecard."""
    required = _required_limitations(measurements, engine)
    if set(required) != _LIMITATION_IDS:
        _fail("retained limitation inventory changed without an approved decision")
    unknowns = {t.cast(str, row["id"]): row for row in decision["unknowns"]}
    for identifier, pointer in sorted(required.items()):
        row = unknowns.get(identifier)
        if row is None:
            _fail(f"retained limitation {identifier!r} is missing from the decision")
        if row.get("source_pointer") != pointer:
            _fail(f"retained limitation {identifier!r} names the wrong source")
        if row.get("disposition") != "accepted_non_material":
            _fail(f"retained limitation {identifier!r} is not accepted")
    for identifier in sorted(unknowns):
        if f"- {identifier}:" not in scorecard:
            _fail(f"scorecard limitation {identifier!r} is missing")


def _validate_review_report(decision: Json, fields: dict[str, str], text: str) -> None:
    label = "review report"
    review = decision["review"]
    source = decision["source"]
    if fields["Status"] != "Ready":
        _fail(f"{label} status is not ready")
    _require_field(
        fields, "Unresolved findings", str(review["unresolved_findings"]), label
    )
    _require_field(
        fields, "Reviewed source commit", t.cast(str, source["commit"]), label
    )
    _require_field(fields, "Reviewed source tree", t.cast(str, source["tree"]), label)
    _require_field(
        fields,
        "Reviewed source manifest",
        t.cast(str, source["transport_source_sha256"]),
        label,
    )
    _require_field(
        fields, "Decision core", t.cast(str, review["decision_core_sha256"]), label
    )
    _require_field(fields, "Findings", str(len(review["findings"])), label)
    for row in review["axes"]:
        evidence = ", ".join(t.cast(list[str], row["evidence_ids"]))
        line = f"- {row['axis']}: {row['disposition']} [{evidence}]"
        if line not in text:
            _fail(f"{label} omits the {row['axis']} axis disposition")


def _validate_scorecard(decision: Json, fields: dict[str, str], text: str) -> None:
    label = "scorecard"
    selection = decision["selection"]
    _require_field(fields, "Winner", t.cast(str, decision["winner"]), label)
    _require_field(fields, "Method", _SELECTION_METHOD_PROSE, label)
    _require_field(
        fields,
        "Decision core",
        t.cast(str, decision["review"]["decision_core_sha256"]),
        label,
    )
    _require_field(
        fields,
        "Decisive axes",
        ", ".join(t.cast(list[str], selection["decisive_axes"])),
        label,
    )
    _require_field(
        fields,
        "Non-decisive axes",
        ", ".join(t.cast(list[str], selection["non_decisive_axes"])),
        label,
    )
    if not fields["Fairness caveat"].strip():
        _fail(f"{label} states no fairness caveat")
    for row in decision["rejected_tradeoffs"]:
        if f"- {row['candidate_id']}: {row['reason']}" not in text:
            _fail(f"{label} omits the {row['candidate_id']} rejected tradeoff")


def _follow_up_slug(identifier: str) -> str:
    return identifier.replace("_", "-")


def _follow_up_core_sha256(follow_up: Json) -> str:
    projection = copy.deepcopy(follow_up)
    projection.pop("core_sha256", None)
    projection.pop("review", None)
    return _sha256(_canonical_bytes(projection))


def _validate_follow_up_result(
    result: Json, follow_up: Json, unknown: Json, *, root: pathlib.Path
) -> Json:
    """Require executed RED, GREEN, and live source evidence for the fix."""
    document = _exact_keys(
        result,
        {
            "evidence_id",
            "measurement_id",
            "schema_version",
            "source",
            "status",
            "test",
            "unknown_id",
        },
        "follow-up result",
    )
    if (
        document["schema_version"] != 1
        or document["status"] != "passed"
        or document["unknown_id"] != unknown["id"]
        or document["evidence_id"] != unknown["evidence_id"]
        or document["evidence_id"] != follow_up["result"]["evidence_id"]
    ):
        _fail("follow-up result identity does not match the closed unknown")
    if document["source"] != {
        "commit": follow_up["commit"],
        "tree": follow_up["tree"],
    }:
        _fail("follow-up result source does not match the atomic fix commit")
    test = document["test"]
    red = test["red"]
    green = test["green"]
    if red.get("status") != "failed_as_expected" or red.get("exit_code") == 0:
        _fail("follow-up RED evidence does not record an observed failure")
    if follow_up["test"]["red_observed"] is not True:
        _fail("follow-up RED evidence was not observed")
    compile_row = green["compile"]
    observations = green["deadline_observations"]
    if compile_row.get("status") != "passed" or compile_row.get("exit_code") != 0:
        _fail("follow-up GREEN evidence does not record a passing compile")
    if not isinstance(observations, list) or len(observations) != 7:
        _fail("follow-up GREEN evidence lacks seven controlled observations")
    if any(
        row.get("status") != "passed" or row.get("exit_code") != 0
        for row in observations
    ):
        _fail("follow-up GREEN evidence records a failed observation")
    if red.get("compiler_sha256") != green.get("compiler_sha256"):
        _fail("follow-up RED and GREEN evidence used different compilers")
    implementation = root / t.cast(str, follow_up["implementation"]["path"])
    live_source = _sha256(_read_regular(implementation, "follow-up implementation"))
    if (
        follow_up["implementation"]["sha256"] != live_source
        or green.get("implementation_sha256") != live_source
    ):
        _fail("follow-up implementation source digest does not match the live file")
    if red.get("implementation_sha256") == green.get("implementation_sha256"):
        _fail("follow-up implementation source did not change between RED and GREEN")
    live_test = _sha256(_read_regular(root / t.cast(str, test["path"]), "failing test"))
    if test["path"] != follow_up["test"]["path"]:
        _fail("follow-up failing test path does not match the plan")
    if (
        test["sha256"] != live_test
        or follow_up["test"]["sha256"] != live_test
        or red.get("test_sha256") != live_test
        or green.get("test_sha256") != live_test
    ):
        _fail("follow-up failing test digest does not match the executed evidence")
    return t.cast(Json, green)


def _validate_follow_up_measurement(
    measurement: Json,
    follow_up: Json,
    unknown: Json,
    *,
    measurements: Json,
    measurements_sha256: str,
    green: Json,
) -> None:
    """Require bounded, executed deadline evidence bound to the aggregate run."""
    document = _exact_keys(
        measurement,
        {
            "aggregate_measurement_id",
            "aggregate_path",
            "aggregate_sha256",
            "collection_observations_sha256",
            "deadline_probe",
            "evidence_id",
            "hard_gates",
            "schema_version",
            "source",
            "status",
            "unknown_id",
        },
        "follow-up measurement",
    )
    if (
        document["schema_version"] != 1
        or document["status"] != "passed"
        or document["unknown_id"] != unknown["id"]
        or document["evidence_id"] != follow_up["measurement"]["evidence_id"]
    ):
        _fail("follow-up measurement identity does not match the closed unknown")
    if (
        document["aggregate_measurement_id"] != measurements["measurement_id"]
        or document["aggregate_path"] != _MEASUREMENTS_FILE
        or document["aggregate_sha256"] != measurements_sha256
        or document["collection_observations_sha256"]
        != measurements["collection_observations_sha256"]
    ):
        _fail("follow-up measurement is not bound to the published measurement")
    if document["source"] != {
        "commit": follow_up["commit"],
        "sha256": follow_up["source_sha256"],
        "tree": follow_up["tree"],
    }:
        _fail("follow-up measurement source does not match the atomic fix commit")
    probe = _exact_keys(
        document["deadline_probe"],
        {"budget_us", "observations", "repetitions", "upper_bound_us"},
        "follow-up measurement deadline probe",
    )
    observations = probe["observations"]
    if probe["repetitions"] != 7 or len(t.cast(list[Json], observations)) != 7:
        _fail("follow-up measurement lacks seven controlled deadline observations")
    budget = t.cast(int, probe["budget_us"])
    bound = t.cast(int, probe["upper_bound_us"])
    for row in t.cast(list[Json], observations):
        elapsed = row.get("reported_elapsed_us")
        if not isinstance(elapsed, int) or not budget <= elapsed < bound:
            _fail("follow-up measurement deadline observation is out of bounds")
    if observations != green["deadline_observations"]:
        _fail("follow-up measurement observations differ from the GREEN evidence")
    if document["hard_gates"] != follow_up["hard_gates"]:
        _fail("follow-up measurement post-fix hard gate projection differs")


def _validate_post_fix_gates(
    decision: Json, follow_up: Json, *, measurements: Json, live: Json
) -> None:
    """Require both hard gates to be re-executed against the committed fix."""
    rows = decision.get("post_fix_hard_gates")
    if not isinstance(rows, list) or len(rows) != len(_GATE_ORDER):
        _fail("post-fix hard gate evidence is missing")
    source = decision["source"]
    projection: list[Json] = []
    for row, gate in zip(rows, decision["hard_gates"], strict=True):
        expected = set(gate) | {
            "measurement_id",
            "source_commit",
            "source_sha256",
            "source_tree",
        }
        post = _exact_keys(row, expected, "post-fix hard gate")
        if any(post[key] != gate[key] for key in gate):
            _fail("post-fix hard gate does not match the selected gate record")
        if (
            post["measurement_id"] != measurements["measurement_id"]
            or post["source_commit"] != source["commit"]
            or post["source_sha256"] != source["transport_source_sha256"]
            or post["source_tree"] != source["tree"]
            or post["status"] != "passed"
        ):
            _fail("post-fix hard gate is not bound to the committed fix")
        projection.append(
            {key: copy.deepcopy(gate[key]) for key in gate if key != "preset"}
        )
    if follow_up["hard_gates"] != projection:
        _fail("post-fix hard gate projection does not match the selected records")
    transition = _exact_keys(
        follow_up["gate_transition"],
        {"affected_executables", "after", "before", "selected_input"},
        "post-fix hard gate transition",
    )
    selected = _exact_keys(
        transition["selected_input"],
        {"after_sha256", "before_sha256", "path"},
        "post-fix hard gate transition input",
    )
    if selected["path"] != follow_up["implementation"]["path"]:
        _fail("post-fix hard gate transition names the wrong selected input")
    if selected["after_sha256"] != follow_up["implementation"]["sha256"]:
        _fail("post-fix hard gate transition input does not match the committed fix")
    if selected["before_sha256"] == selected["after_sha256"]:
        _fail("post-fix hard gate transition input did not change")
    before = t.cast(list[Json], transition["before"])
    after = t.cast(list[Json], transition["after"])
    if len(before) != len(_GATE_ORDER) or len(after) != len(_GATE_ORDER):
        _fail("post-fix hard gate transition is incomplete")
    for old, new, gate_id in zip(before, after, _GATE_ORDER, strict=True):
        if old["gate_id"] != gate_id or new["gate_id"] != gate_id:
            _fail("post-fix hard gate transition names the wrong gate")
        if new["gate_sha256"] != live[gate_id]["gate_sha256"]:
            _fail("post-fix hard gate transition does not name the executed record")
        if old["gate_sha256"] == new["gate_sha256"]:
            _fail("post-fix hard gate transition reused the pre-fix gate identity")
        if old["record_sha256"] == new["record_sha256"]:
            _fail("post-fix hard gate transition reused the pre-fix record")
        if old["executables"].keys() != new["executables"].keys():
            _fail("post-fix hard gate transition changed the executable closure")
        changed = sorted(
            path
            for path in t.cast(Json, old["executables"])
            if old["executables"][path] != new["executables"][path]
        )
        if transition["affected_executables"].get(gate_id) != changed:
            _fail("post-fix hard gate transition misreports the affected executables")


def _validate_follow_up(
    unknown: Json,
    *,
    root: pathlib.Path,
    decision: Json,
    measurements: Json,
    measurements_sha256: str,
    live: Json,
) -> None:
    """Validate one closed material unknown end to end."""
    follow_up = t.cast(Json, unknown["follow_up"])
    identifier = t.cast(str, unknown["id"])
    directory = f"{_TRANSPORT_DIRECTORY}/followups/{_follow_up_slug(identifier)}"
    if follow_up["plan"]["path"] != (
        f"{_FOLLOW_UP_PLAN_DIRECTORY}/{_follow_up_slug(identifier)}.md"
    ):
        _fail("follow-up plan is not tracked at its subordinate plan location")
    for key, name in (
        ("measurement", "measurement.json"),
        ("result", "result.json"),
        ("review", "review.md"),
    ):
        if follow_up[key]["path"] != f"{directory}/{name}":
            _fail(f"follow-up {key} is not published at its evidence location")
    source = decision["source"]
    if follow_up["commit"] != source["commit"]:
        _fail("follow-up commit does not match the selected source commit")
    if follow_up["tree"] != source["tree"]:
        _fail("follow-up tree does not match the selected source tree")
    if follow_up["source_sha256"] != source["transport_source_sha256"]:
        _fail("follow-up source manifest does not match the selected source")
    if (
        sorted(t.cast(list[str], follow_up["committed_paths"]))
        != follow_up["diff_tree"]
    ):
        _fail("follow-up committed paths do not match the recorded diff-tree")
    observed = sorted(
        _git(
            root,
            "diff-tree",
            "--no-commit-id",
            "--name-only",
            "-r",
            t.cast(str, follow_up["commit"]),
        ).splitlines()
    )
    if observed != follow_up["diff_tree"]:
        _fail("follow-up diff-tree does not match the recorded commit")
    parent = _git(root, "rev-parse", f"{follow_up['commit']}^")
    if parent != follow_up["parent_commit"]:
        _fail("follow-up parent commit does not match the recorded commit")
    committed = set(t.cast(list[str], follow_up["committed_paths"]))
    for key in ("implementation", "plan", "test"):
        if follow_up[key]["path"] not in committed:
            _fail(f"follow-up {key} is not part of the atomic fix commit")
    plan = _closed_record(
        _public_text(
            root / t.cast(str, follow_up["plan"]["path"]), "follow-up plan", root
        ),
        "follow-up plan",
        _FOLLOW_UP_PLAN_FIELDS,
    )
    if plan["Unknown"] != identifier:
        _fail("follow-up plan names a different unknown")
    _require_field(
        plan,
        "Implementation",
        t.cast(str, follow_up["implementation"]["path"]),
        "follow-up plan",
    )
    _require_field(
        plan, "Failing test", t.cast(str, follow_up["test"]["path"]), "follow-up plan"
    )
    if plan["Gates"] != " and ".join(_GATE_ORDER):
        _fail("follow-up plan does not name both hard gates")
    for key, label in (
        ("plan", "follow-up plan"),
        ("measurement", "follow-up measurement"),
        ("result", "follow-up result"),
        ("review", "follow-up review"),
    ):
        path = root / t.cast(str, follow_up[key]["path"])
        if follow_up[key]["sha256"] != _sha256(_read_regular(path, label)):
            _fail(f"{label} digest does not match its published bytes")
    review_text = _public_text(
        root / t.cast(str, follow_up["review"]["path"]), "follow-up review", root
    )
    review = _closed_record(review_text, "follow-up review", _FOLLOW_UP_REVIEW_FIELDS)
    if review["Status"] != "Ready":
        _fail("follow-up review status is not ready")
    if follow_up["review"]["status"] != "closed":
        _fail("follow-up review is not closed")
    _require_field(
        review,
        "Unresolved findings",
        str(follow_up["review"]["unresolved_findings"]),
        "follow-up review",
    )
    _require_field(
        review, "Source commit", t.cast(str, follow_up["commit"]), "follow-up review"
    )
    _require_field(
        review, "Source tree", t.cast(str, follow_up["tree"]), "follow-up review"
    )
    _require_field(
        review,
        "Source manifest",
        t.cast(str, follow_up["source_sha256"]),
        "follow-up review",
    )
    _require_field(
        review,
        "Follow-up core",
        t.cast(str, follow_up["core_sha256"]),
        "follow-up review",
    )
    result = _public_json(
        root / t.cast(str, follow_up["result"]["path"]), "follow-up result", root
    )
    green = _validate_follow_up_result(result, follow_up, unknown, root=root)
    if follow_up["result"]["status"] != "passed":
        _fail("follow-up result status is not passed")
    measurement = _public_json(
        root / t.cast(str, follow_up["measurement"]["path"]),
        "follow-up measurement",
        root,
    )
    if result["measurement_id"] != measurement.get("evidence_id"):
        _fail("follow-up result does not name its measurement")
    if follow_up["measurement"]["status"] != "passed":
        _fail("follow-up measurement status is not passed")
    _validate_follow_up_measurement(
        measurement,
        follow_up,
        unknown,
        measurements=measurements,
        measurements_sha256=measurements_sha256,
        green=green,
    )
    _validate_post_fix_gates(decision, follow_up, measurements=measurements, live=live)
    if follow_up["core_sha256"] != _follow_up_core_sha256(follow_up):
        _fail("follow-up core digest does not match its evidence")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="verify_decision.py")
    parser.add_argument("--axis", choices=["transport"], required=True)
    parser.add_argument("--require-review-closed", action="store_true", required=True)
    return parser


def main(argv: t.Sequence[str] | None = None) -> int:
    """Run the closed decision verifier from the repository root."""
    namespace = _parser().parse_args(argv)
    root = pathlib.Path.cwd()
    try:
        verify_decision(
            root=root,
            axis=namespace.axis,
            require_review_closed=namespace.require_review_closed,
        )
    except DecisionValidationError as error:
        print(error, file=sys.stderr)
        return 1
    return 0


def verify_decision(
    *, root: pathlib.Path, axis: str, require_review_closed: bool
) -> None:
    """Validate the complete repository-bound transport decision.

    Parameters
    ----------
    root : pathlib.Path
        Repository whose live bytes, Git state, and build trees the published
        evidence claims to describe.
    axis : str
        Bakeoff axis; only ``transport`` is closed.
    require_review_closed : bool
        Require a closed adversarial review with no unresolved finding.

    Raises
    ------
    DecisionValidationError
        When any artifact, identity, or disposition fails to bind.
    """
    if axis != "transport":
        _fail("only the transport axis is closed")
    root = root.resolve(strict=True)
    transport = root / _TRANSPORT_DIRECTORY
    decision_path = transport / "decision.json"
    decision = _public_json(decision_path, "decision", root)
    unknowns = decision.get("unknowns")
    material = [
        row
        for row in (unknowns if isinstance(unknowns, list) else [])
        if isinstance(row, dict) and row.get("materiality") == "material"
    ]
    expected = set(_EXPECTED_TRANSPORT_FILES)
    if material:
        expected.add("followups")
    _require_topology(transport, expected, "transport evidence")
    _require_topology(transport / "diagnostics", _EXPECTED_DIAGNOSTICS, "diagnostics")
    _require_topology(root / _GRAFT_DIRECTORY, _EXPECTED_GRAFT_FILES, "graft evidence")
    declared: set[str] = set()
    for row in material:
        follow_up = row.get("follow_up")
        if not isinstance(follow_up, dict):
            _fail("material unknown carries no follow-up result")
        slug = _follow_up_slug(t.cast(str, row["id"]))
        _require_topology(
            transport / "followups" / slug, _FOLLOW_UP_FILES, f"follow-up {slug}"
        )
        declared.update(
            t.cast(str, follow_up[key]["path"])
            for key in ("measurement", "result", "review")
            if isinstance(follow_up.get(key), dict)
        )
    if material and _directory_names(transport / "followups", "follow-up evidence") != {
        _follow_up_slug(t.cast(str, row["id"])) for row in material
    }:
        _fail("follow-up evidence topology is not the exact closed artifact set")
    environment_bytes, environment = _public_document(
        root / _ENVIRONMENT_FILE, "environment", root
    )
    _reject_private_environment(environment, "environment")
    measurements_bytes, measurements = _public_document(
        root / _MEASUREMENTS_FILE, "measurements", root
    )
    _reject_private_environment(measurements.get("environment"), "measurements")
    review_bytes = _public_bytes(transport / "review.md", "review", root)
    review_text = _decoded(review_bytes, "review")
    review_fields = _metadata_fields(review_text, "review report", _REVIEW_FIELDS)
    scorecard_bytes = _public_bytes(transport / "scorecard.md", "scorecard", root)
    scorecard_text = _decoded(scorecard_bytes, "scorecard")
    scorecard_fields = _metadata_fields(scorecard_text, "scorecard", _SCORECARD_FIELDS)
    if measurements.get("measurement_fairness") != _MEASUREMENT_FAIRNESS:
        _fail("published measurement fairness does not match the frozen normalization")

    validate_decision(decision, require_review_closed=require_review_closed)

    if decision["environment_id"] != _ENVIRONMENT_ID:
        _fail("environment identity is not the frozen transport identity")
    if decision["environment_sha256"] != _sha256(environment_bytes):
        _fail("environment digest does not match the decision")
    measurements_sha256 = _sha256(measurements_bytes)
    if decision["measurements_sha256"] != measurements_sha256:
        _fail("measurements digest does not match the decision")
    if decision["review"]["report_sha256"] != _sha256(review_bytes):
        _fail("review report digest does not match the decision")
    if decision["scorecard_sha256"] != _sha256(scorecard_bytes):
        _fail("scorecard digest does not match the decision")

    live = _validate_gate_evidence(decision, measurements, root)
    _validate_published_measurements(
        measurements,
        decision,
        root=root,
        environment=environment,
        environment_bytes=environment_bytes,
    )
    grafts = _validate_grafts(decision, root=root)
    _validate_limitations(decision, measurements, grafts["engine_ops"], scorecard_text)
    _validate_review_report(decision, review_fields, review_text)
    _validate_scorecard(decision, scorecard_fields, scorecard_text)
    for row in material:
        _validate_follow_up(
            row,
            root=root,
            decision=decision,
            measurements=measurements,
            measurements_sha256=measurements_sha256,
            live=live,
        )
    _validate_git_scope(decision, measurements, root=root, declared=declared)


if __name__ == "__main__":
    raise SystemExit(main())
