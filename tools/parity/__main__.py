"""Unified command-line interface for the Python parity contract."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import typing as t

from .check_manifest import validate_manifest
from .drift import find_drift
from .gaps import coverage, coverage_report, survey
from .generate import (
    canonical_json_bytes,
    canonical_sha256,
    contained_regular_file,
    generate_contract,
    is_safe_relative_path,
    load_input_manifest,
)
from .model import InputSpec
from .sync import record_evidence, synchronize_manifest, write_json_atomic

_CURRENT_SIDECARS = (
    ("release-v0.62.0.json", "release", "release_sha256"),
    ("development.json", "development", "development_sha256"),
    ("mapping.json", "mapping", "mapping_sha256"),
    ("approvals.json", "approvals", "approvals_sha256"),
    ("evidence.json", "evidence", "evidence_sha256"),
    ("shards.json", "shards", "shards_sha256"),
)


def build_parser() -> argparse.ArgumentParser:
    """Build the five-command Task 3 parity CLI.

    Returns
    -------
    argparse.ArgumentParser
        Parser for generation, drift, synchronization, evidence, and verify.

    Examples
    --------
    >>> args = ["verify", "--manifest", "m", "--mode", "structural"]
    >>> build_parser().parse_args(args).command
    'verify'
    """
    parser = argparse.ArgumentParser(prog="python -m cxx.tools.parity")
    subparsers = parser.add_subparsers(dest="command", required=True)

    generate = subparsers.add_parser("generate")
    generate.add_argument("--release")
    generate.add_argument("--development")
    generate.add_argument(
        "--output", type=pathlib.Path, default=pathlib.Path("cxx/parity")
    )
    generate.add_argument("--inputs", type=pathlib.Path)
    generate.add_argument("--check", type=pathlib.Path)
    generate.set_defaults(handler=_run_generate)

    drift = subparsers.add_parser("drift")
    drift.add_argument("--manifest", type=pathlib.Path, required=True)
    drift.add_argument("--worktree", type=pathlib.Path, required=True)
    drift.set_defaults(handler=_run_drift)

    sync = subparsers.add_parser("sync")
    sync.add_argument("--release", type=pathlib.Path, required=True)
    sync.add_argument("--development", type=pathlib.Path, required=True)
    sync.add_argument("--mapping", type=pathlib.Path, required=True)
    sync.add_argument("--approvals", type=pathlib.Path, required=True)
    sync.add_argument("--evidence", type=pathlib.Path, required=True)
    sync.add_argument("--output", type=pathlib.Path, required=True)
    sync.set_defaults(handler=_run_sync)

    record = subparsers.add_parser("record-evidence")
    record.add_argument("--manifest", type=pathlib.Path, required=True)
    record.add_argument("--shards", type=pathlib.Path, required=True)
    record.add_argument("--evidence", type=pathlib.Path, required=True)
    record.add_argument("--shard", required=True)
    record.add_argument("--ctest-record", type=pathlib.Path, required=True)
    record.add_argument("--differential-record", type=pathlib.Path)
    record.add_argument("--tmux-bin", type=pathlib.Path, required=True)
    record.add_argument("--repository", type=pathlib.Path, default=pathlib.Path())
    record.set_defaults(handler=_run_record_evidence)

    gaps = subparsers.add_parser("gaps")
    gaps.add_argument(
        "--mapping", type=pathlib.Path, default=pathlib.Path("cxx/parity/mapping.json")
    )
    gaps.add_argument(
        "--headers",
        type=pathlib.Path,
        default=pathlib.Path("cxx/include/libtmux"),
    )
    gaps.set_defaults(handler=_run_gaps)

    measure = subparsers.add_parser("coverage")
    measure.add_argument(
        "--mapping", type=pathlib.Path, default=pathlib.Path("cxx/parity/mapping.json")
    )
    measure.add_argument(
        "--headers", type=pathlib.Path, default=pathlib.Path("cxx/include/libtmux")
    )
    measure.set_defaults(handler=_run_coverage)

    verify = subparsers.add_parser("verify")
    verify.add_argument("--manifest", type=pathlib.Path, required=True)
    verify.add_argument("--mode", choices=("structural", "complete"), required=True)
    verify.add_argument("--allow-pending", action="store_true")
    verify.set_defaults(handler=_run_verify)
    return parser


def main(argv: t.Sequence[str] | None = None) -> int:
    """Run one parity subcommand and return its process status.

    Parameters
    ----------
    argv : Sequence[str] | None, optional
        Arguments after the module name, defaulting to ``sys.argv``.

    Returns
    -------
    int
        Zero for success and nonzero for drift or validation errors.

    Examples
    --------
    >>> main(["generate", "--check", "cxx/parity"]) in {0, 1}
    True
    """
    namespace = build_parser().parse_args(argv)
    handler = t.cast(t.Callable[[argparse.Namespace], int], namespace.handler)
    try:
        return handler(namespace)
    except (OSError, TypeError, ValueError, subprocess.CalledProcessError) as exc:
        print(str(exc), file=sys.stderr)
        return 2


def _run_generate(namespace: argparse.Namespace) -> int:
    """Generate or byte-check parity artifacts.

    Parameters
    ----------
    namespace : argparse.Namespace
        Parsed generation arguments.

    Returns
    -------
    int
        Zero when files were written or reproduce byte-for-byte.

    Examples
    --------
    >>> callable(_run_generate)
    True
    """
    if namespace.check is not None:
        if namespace.release is not None or namespace.development is not None:
            msg = "generate --check does not accept revision overrides"
            raise ValueError(msg)
        output = t.cast(pathlib.Path, namespace.check)
        differences = generate_contract(pathlib.Path.cwd(), output, check=True)
    else:
        if namespace.release is None or namespace.development is None:
            msg = "generate requires --release and --development"
            raise ValueError(msg)
        output = t.cast(pathlib.Path, namespace.output)
        differences = generate_contract(
            pathlib.Path.cwd(),
            output,
            t.cast(str, namespace.release),
            t.cast(str, namespace.development),
            input_manifest_path=t.cast(pathlib.Path | None, namespace.inputs),
        )
    if differences:
        for difference in differences:
            print(difference, file=sys.stderr)
        return 1
    return 0


def _run_drift(namespace: argparse.Namespace) -> int:
    """Check only recorded Python input objects for working-tree drift.

    Parameters
    ----------
    namespace : argparse.Namespace
        Parsed manifest and worktree paths.

    Returns
    -------
    int
        Zero when recorded inputs match their pinned development commit.

    Examples
    --------
    >>> callable(_run_drift)
    True
    """
    manifest_path = t.cast(pathlib.Path, namespace.manifest)
    manifest = _read_json(manifest_path)
    errors = validate_manifest(manifest, complete=False, allow_pending=True)
    errors.extend(_current_sidecar_errors(manifest_path, manifest))
    errors.extend(_recorded_input_boundary_errors(manifest_path, manifest))
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    development = _object(manifest, "development")
    source = _object(development, "source")
    commit = source.get("commit")
    if not isinstance(commit, str) or not commit:
        msg = "development observation lacks source commit"
        raise TypeError(msg)
    input_manifest = _object(development, "input_manifest")
    specs = _embedded_input_specs(input_manifest)
    changes = find_drift(t.cast(pathlib.Path, namespace.worktree), commit, specs)
    for change in changes:
        print(change, file=sys.stderr)
    return int(bool(changes))


def _run_sync(namespace: argparse.Namespace) -> int:
    """Synchronize observations and reviewed sidecars into a manifest.

    Parameters
    ----------
    namespace : argparse.Namespace
        Parsed synchronization paths.

    Returns
    -------
    int
        Zero after an atomic deterministic manifest replacement.

    Examples
    --------
    >>> callable(_run_sync)
    True
    """
    manifest = synchronize_manifest(
        _read_json(t.cast(pathlib.Path, namespace.release)),
        _read_json(t.cast(pathlib.Path, namespace.development)),
        _read_json(t.cast(pathlib.Path, namespace.mapping)),
        _read_json(t.cast(pathlib.Path, namespace.approvals)),
        _read_json(t.cast(pathlib.Path, namespace.evidence)),
    )
    write_json_atomic(t.cast(pathlib.Path, namespace.output), manifest)
    return 0


def _run_record_evidence(namespace: argparse.Namespace) -> int:
    """Verify immutable records and atomically refresh one shard's evidence.

    Parameters
    ----------
    namespace : argparse.Namespace
        Parsed evidence-refresh paths and shard.

    Returns
    -------
    int
        Zero after the sidecar is safely replaced.

    Examples
    --------
    >>> callable(_run_record_evidence)
    True
    """
    repository = t.cast(pathlib.Path, namespace.repository)
    manifest_path = _record_evidence_path(
        repository, t.cast(pathlib.Path, namespace.manifest)
    )
    for filename, _, _ in _CURRENT_SIDECARS:
        _record_evidence_path(repository, manifest_path.parent / filename)
    shards_path = _record_evidence_path(
        repository, t.cast(pathlib.Path, namespace.shards)
    )
    evidence_path = _record_evidence_path(
        repository, t.cast(pathlib.Path, namespace.evidence)
    )
    if shards_path != manifest_path.parent / "shards.json":
        msg = "record-evidence shards path must be the manifest sidecar"
        raise ValueError(msg)
    if evidence_path != manifest_path.parent / "evidence.json":
        msg = "record-evidence evidence path must be the manifest sidecar"
        raise ValueError(msg)
    gate_path = _record_evidence_path(
        repository, t.cast(pathlib.Path, namespace.ctest_record)
    )
    differential_path = t.cast(pathlib.Path | None, namespace.differential_record)
    if differential_path is not None:
        differential_path = _record_evidence_path(repository, differential_path)
    manifest = _read_json(manifest_path)
    sidecar_errors = _current_sidecar_errors(manifest_path, manifest)
    if sidecar_errors:
        msg = "record-evidence requires current synchronized sidecars:\n"
        raise ValueError(msg + "\n".join(sidecar_errors))
    refreshed = record_evidence(
        manifest=manifest,
        shards=_read_json(shards_path),
        evidence=_read_json(evidence_path),
        shard_name=t.cast(str, namespace.shard),
        gate=_read_json(gate_path),
        differential=_read_json(differential_path) if differential_path else None,
        tmux_binary=t.cast(pathlib.Path, namespace.tmux_bin),
        repository=repository,
    )
    write_json_atomic(evidence_path, refreshed)
    return 0


def _run_gaps(namespace: argparse.Namespace) -> int:
    """Print the Python callables no C++ name covers.

    Parameters
    ----------
    namespace : argparse.Namespace
        Parsed mapping and header paths.

    Returns
    -------
    int
        Zero; an outstanding gap is a fact to report, not a failure.

    Examples
    --------
    >>> callable(_run_gaps)
    True
    """
    print(
        survey(
            t.cast("pathlib.Path", namespace.mapping),
            t.cast("pathlib.Path", namespace.headers),
        )
    )
    return 0


def _run_coverage(namespace: argparse.Namespace) -> int:
    """Print how much of each part of the Python surface the port covers.

    Parameters
    ----------
    namespace : argparse.Namespace
        Parsed mapping and header paths.

    Returns
    -------
    int
        Zero; this reports a measurement rather than enforcing one.

    Examples
    --------
    >>> callable(_run_coverage)
    True
    """
    mapping = _read_json(t.cast("pathlib.Path", namespace.mapping))
    print(coverage_report(coverage(mapping, t.cast("pathlib.Path", namespace.headers))))
    return 0


def _run_verify(namespace: argparse.Namespace) -> int:
    """Validate structural or complete parity state and current sidecars.

    Parameters
    ----------
    namespace : argparse.Namespace
        Parsed verification mode and manifest path.

    Returns
    -------
    int
        Zero only when no violation remains.

    Examples
    --------
    >>> callable(_run_verify)
    True
    """
    manifest_path = t.cast(pathlib.Path, namespace.manifest)
    manifest = _read_json(manifest_path)
    complete = namespace.mode == "complete"
    allow_pending = bool(namespace.allow_pending)
    if complete and allow_pending:
        msg = "complete verification cannot allow pending entries"
        raise TypeError(msg)
    errors = validate_manifest(
        manifest,
        complete=complete,
        allow_pending=allow_pending,
        repository=pathlib.Path.cwd(),
    )
    errors.extend(_current_sidecar_errors(manifest_path, manifest))
    if complete:
        errors.extend(_recorded_input_boundary_errors(manifest_path, manifest))
    for error in errors:
        print(error, file=sys.stderr)
    return int(bool(errors))


def _current_sidecar_errors(
    manifest_path: pathlib.Path,
    manifest: t.Mapping[str, object],
) -> list[str]:
    """Compare all six current sidecars with synchronized bindings.

    Parameters
    ----------
    manifest_path : pathlib.Path
        Synchronized manifest location.
    manifest : Mapping[str, object]
        Parsed synchronized manifest.

    Returns
    -------
    list[str]
        Missing or stale current-sidecar violations.

    Examples
    --------
    >>> errors = _current_sidecar_errors(pathlib.Path("missing/manifest.json"), {})
    >>> len(errors), errors[0]
    (6, 'manifest: missing current sidecar release-v0.62.0.json')
    """
    raw_bindings = manifest.get("bindings")
    bindings = raw_bindings if isinstance(raw_bindings, dict) else {}
    errors: list[str] = []
    contract_root = manifest_path.parent
    read_root = _contract_read_root(manifest_path)
    for filename, document_key, binding_key in _CURRENT_SIDECARS:
        path = (contract_root / filename).absolute()
        if not path.exists() and not path.is_symlink():
            errors.append(f"manifest: missing current sidecar {filename}")
            continue
        try:
            path = contained_regular_file(
                read_root,
                path,
                message=(
                    "manifest: current sidecar is not a contained "
                    f"non-symlink regular file: {filename}"
                ),
            )
        except ValueError as exc:
            errors.append(str(exc))
            continue
        current = _read_json(path)
        if current != manifest.get(document_key) or bindings.get(
            binding_key
        ) != canonical_sha256(current):
            errors.append(f"manifest: current sidecar binding is stale: {filename}")
    return errors


def _recorded_input_boundary_errors(
    manifest_path: pathlib.Path,
    manifest: t.Mapping[str, object],
) -> list[str]:
    """Compare embedded selectors with the authoritative tracked boundary.

    Parameters
    ----------
    manifest_path : pathlib.Path
        Synchronized manifest location beside ``inputs.json``.
    manifest : Mapping[str, object]
        Parsed synchronized manifest.

    Returns
    -------
    list[str]
        Missing, malformed, or divergent boundary violations.

    Examples
    --------
    >>> _recorded_input_boundary_errors(pathlib.Path("missing/manifest.json"), {})
    ['manifest: missing or invalid authoritative recorded input boundary']
    """
    try:
        input_path = (manifest_path.parent / "inputs.json").absolute()
        inputs_path = contained_regular_file(
            _contract_read_root(manifest_path),
            input_path,
            message="invalid authoritative recorded input boundary",
        )
        authoritative, _ = load_input_manifest(inputs_path)
    except (OSError, TypeError, ValueError):
        return ["manifest: missing or invalid authoritative recorded input boundary"]
    errors: list[str] = []
    for key in ("release", "development"):
        observation = manifest.get(key)
        embedded = (
            observation.get("input_manifest") if isinstance(observation, dict) else None
        )
        if canonical_json_bytes(embedded) != canonical_json_bytes(authoritative):
            errors.append(
                f"manifest: {key} recorded input boundary differs from inputs.json"
            )
    return errors


def _contract_read_root(manifest_path: pathlib.Path) -> pathlib.Path:
    """Return the containment root for contract reads.

    Parameters
    ----------
    manifest_path : pathlib.Path
        Manifest whose neighboring sidecars are being loaded.

    Returns
    -------
    pathlib.Path
        Current repository root when the manifest is lexically under it,
        otherwise the manifest directory for isolated fixtures.

    Examples
    --------
    >>> _contract_read_root(pathlib.Path("cxx/parity/manifest.json")).is_dir()
    True
    """
    cwd = pathlib.Path.cwd().absolute()
    candidate = manifest_path.absolute()
    try:
        candidate.relative_to(cwd)
    except ValueError:
        return manifest_path.parent
    return cwd


def _record_evidence_path(repository: pathlib.Path, path: pathlib.Path) -> pathlib.Path:
    """Require one record-evidence contract path to be contained and regular.

    Parameters
    ----------
    repository : pathlib.Path
        Non-symlink repository root containing all contract artifacts.
    path : pathlib.Path
        Existing contract document read before an evidence refresh.

    Returns
    -------
    pathlib.Path
        Absolute original path after containment and symlink validation.

    Raises
    ------
    ValueError
        Raised when the repository or path is unsafe for a contract refresh.

    Examples
    --------
    >>> callable(_record_evidence_path)
    True
    """
    root = repository.absolute()
    if root.is_symlink() or not root.is_dir():
        msg = "record-evidence repository must be a non-symlink directory"
        raise ValueError(msg)
    root = root.resolve()
    candidate = path.absolute()
    try:
        relative = candidate.relative_to(root)
    except ValueError as exc:
        msg = "record-evidence path escapes repository"
        raise ValueError(msg) from exc
    ancestor = root
    for part in relative.parts:
        if part == "..":
            msg = "record-evidence path escapes repository"
            raise ValueError(msg)
        ancestor /= part
        if ancestor.is_symlink():
            msg = "record-evidence path contains a symlink"
            raise ValueError(msg)
    if not candidate.is_file():
        msg = "record-evidence path is not a regular file"
        raise ValueError(msg)
    if not candidate.resolve().is_relative_to(root):
        msg = "record-evidence path escapes repository"
        raise ValueError(msg)
    return candidate


def _read_json(path: pathlib.Path) -> dict[str, object]:
    """Read one UTF-8 JSON object.

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
    >>> _read_json(pathlib.Path("cxx/parity/inputs.json"))["version"]
    1
    """
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        msg = f"JSON document must be an object: {path}"
        raise TypeError(msg)
    return t.cast(dict[str, object], value)


def _object(document: t.Mapping[str, object], key: str) -> dict[str, object]:
    """Require one object-valued document field.

    Parameters
    ----------
    document : Mapping[str, object]
        Parent document.
    key : str
        Required object key.

    Returns
    -------
    dict[str, object]
        Object value.

    Raises
    ------
    ValueError
        Raised when the field is absent or not an object.

    Examples
    --------
    >>> _object({"value": {"x": 1}}, "value")["x"]
    1
    """
    value = document.get(key)
    if not isinstance(value, dict):
        msg = f"document requires object field: {key}"
        raise TypeError(msg)
    return t.cast(dict[str, object], value)


def _embedded_input_specs(
    input_manifest: t.Mapping[str, object],
) -> tuple[InputSpec, ...]:
    """Rebuild Task 2 field-scoped specifications from embedded input JSON.

    Parameters
    ----------
    input_manifest : Mapping[str, object]
        Generated normalized input manifest.

    Returns
    -------
    tuple[InputSpec, ...]
        Full-object and field-scoped input specifications.

    Raises
    ------
    ValueError
        Raised when an embedded record is malformed.

    Examples
    --------
    >>> _embedded_input_specs(
    ...     {"version": 1, "inputs": [{"path": "api.py", "kind": "blob"}]}
    ... )
    (InputSpec(path='api.py', fields=()),)
    """
    if (
        type(input_manifest.get("version")) is not int
        or input_manifest.get("version") != 1
    ):
        msg = "embedded input manifest requires version 1"
        raise ValueError(msg)
    inputs = input_manifest.get("inputs")
    if not isinstance(inputs, list) or not inputs:
        msg = "embedded input manifest lacks inputs"
        raise ValueError(msg)
    specs: list[InputSpec] = []
    seen: set[str] = set()
    for item in inputs:
        if not isinstance(item, dict) or not is_safe_relative_path(item.get("path")):
            msg = "malformed embedded parity input"
            raise ValueError(msg)
        input_path = t.cast(str, item["path"])
        if input_path in seen:
            msg = "duplicate embedded parity input"
            raise ValueError(msg)
        seen.add(input_path)
        kind = item.get("kind")
        if kind not in {"blob", "tree", "toml_fields"}:
            msg = "malformed embedded parity input kind"
            raise ValueError(msg)
        fields_value = item.get("fields", [])
        if not isinstance(fields_value, list) or not all(
            isinstance(field, str) and field for field in fields_value
        ):
            msg = "malformed embedded parity selectors"
            raise ValueError(msg)
        fields = t.cast(list[str], fields_value)
        if len(fields) != len(set(fields)):
            msg = "duplicate embedded parity selector"
            raise ValueError(msg)
        if kind == "toml_fields" and not fields:
            msg = "embedded toml_fields input lacks selectors"
            raise ValueError(msg)
        if kind != "toml_fields" and fields:
            msg = "embedded full-object input claims selectors"
            raise ValueError(msg)
        specs.append(InputSpec(input_path, tuple(fields)))
    return tuple(specs)


if __name__ == "__main__":
    raise SystemExit(main())
