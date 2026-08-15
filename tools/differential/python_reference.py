"""Pinned public-libtmux differential reference adapter."""

from __future__ import annotations

import argparse
import copy
import importlib
import pathlib
import re
import sys
import tempfile
import typing as t

from cxx.tools.parity.generate import canonical_json_bytes

from .canonicalize import canonicalize
from .materialize import materialize_reference
from .model import (
    ScenarioRecord,
    ScenarioRegistry,
    SocketEndpoint,
    load_registry,
    load_scenario,
    scenario_record_from_document,
    validate_observation,
)
from .runner import _load_json_object, resolve_tmux_binary

_DIGEST = re.compile(r"sha256:[0-9a-f]{64}\Z")


def _verify_materialized_imports(import_root: pathlib.Path) -> None:
    """Require every loaded ``libtmux`` module to stay under one root.

    Parameters
    ----------
    import_root : pathlib.Path
        Verified materialized Python import root.

    Raises
    ------
    ValueError
        Raised for a sourceless or out-of-root ``libtmux`` module.

    Examples
    --------
    >>> callable(_verify_materialized_imports)
    True
    """
    for name, imported in tuple(sys.modules.items()):
        if name != "libtmux" and not name.startswith("libtmux."):
            continue
        source = getattr(imported, "__file__", None)
        if source is None:
            msg = f"materialized import {name} lacks a source file"
            raise ValueError(msg)
        try:
            pathlib.Path(source).resolve().relative_to(import_root)
        except ValueError as error:
            msg = f"libtmux import escaped materialized root: {source}"
            raise ValueError(msg) from error


def list_operations_document(registry: ScenarioRegistry) -> dict[str, object]:
    """Return Python dispatcher tags bound to the registry digest.

    Parameters
    ----------
    registry : ScenarioRegistry
        Closed registry to report.

    Returns
    -------
    dict[str, object]
        Canonically sortable operation listing.

    Examples
    --------
    >>> root = pathlib.Path('cxx/tests/differential')
    >>> document = list_operations_document(
    ...     load_registry(root / 'scenario_registry.json')
    ... )
    >>> document['operations']
    ['server.list_sessions']
    """
    return {
        "operations": sorted(registry.operations),
        "registry_sha256": registry.digest,
    }


def _list_sessions(server: object, tag: str) -> dict[str, object]:
    """Observe public ``Server.sessions`` without starting a server.

    Parameters
    ----------
    server : object
        Public materialized ``libtmux.Server`` instance.
    tag : str
        Registered operation tag.

    Returns
    -------
    dict[str, object]
        Closed list-sessions observation.

    Examples
    --------
    >>> class Example:
    ...     sessions = []
    >>> _list_sessions(Example(), 'server.list_sessions')['sessions']
    []
    """
    sessions = t.cast(t.Any, server).sessions
    rows = [
        {
            "id": session.session_id,
            "name": session.session_name,
        }
        for session in sessions
    ]
    return {"tag": tag, "sessions": rows, "stderr": [], "warnings": []}


def execute_reference(
    *,
    tmux_binary: pathlib.Path,
    endpoint: SocketEndpoint,
    scenario_path: pathlib.Path,
    repository: pathlib.Path,
    observation_path: pathlib.Path,
    input_manifest_path: pathlib.Path,
    semantic_contract_sha256: str,
) -> ScenarioRecord:
    """Execute one scenario through public pinned ``libtmux``.

    Parameters
    ----------
    tmux_binary : pathlib.Path
        Explicit executable resolved and hashed for this record.
    endpoint : SocketEndpoint
        Exactly one fixture-owned socket selector.
    scenario_path : pathlib.Path
        Closed scenario JSON.
    repository : pathlib.Path
        Git repository containing the recorded objects.
    observation_path : pathlib.Path
        Recorded development observation.
    input_manifest_path : pathlib.Path
        Authoritative input manifest.
    semantic_contract_sha256 : str
        Recomputed semantic contract digest supplied by the supervisor.

    Returns
    -------
    ScenarioRecord
        Canonical eight-field reference record.

    Raises
    ------
    ValueError
        Raised for malformed identity, import escape, or unregistered dispatch.

    Examples
    --------
    >>> callable(execute_reference)
    True
    """
    if _DIGEST.fullmatch(semantic_contract_sha256) is None:
        msg = "semantic contract digest is malformed"
        raise ValueError(msg)
    binary = resolve_tmux_binary(tmux_binary)
    root = scenario_path.parent.parent
    registry = load_registry(root / "scenario_registry.json")
    scenario = load_scenario(scenario_path, root / "scenario.schema.json", registry)
    observation = _load_json_object(observation_path)
    input_manifest = _load_json_object(input_manifest_path)

    with tempfile.TemporaryDirectory(prefix="libtmux-python-reference-") as directory:
        materialized = materialize_reference(
            repository,
            observation,
            input_manifest,
            pathlib.Path(directory) / "source",
        )
        previous = {
            name: module
            for name, module in tuple(sys.modules.items())
            if name == "libtmux" or name.startswith("libtmux.")
        }
        for name in previous:
            del sys.modules[name]
        sys.path.insert(0, str(materialized.import_root))
        try:
            module = importlib.import_module("libtmux")
            _verify_materialized_imports(materialized.import_root)
            server_class = module.Server
            server = server_class(
                tmux_bin=str(binary.path),
                **{f"socket_{endpoint.mode}": endpoint.value},
            )
            observations: list[dict[str, object]] = []
            handlers: dict[str, t.Callable[[object, str], dict[str, object]]] = {
                "list_sessions": _list_sessions
            }
            for operation in scenario.operations:
                registered = registry.operations[operation.tag]
                handler = handlers.get(registered.python_handler)
                if handler is None:
                    msg = f"unregistered Python handler {registered.python_handler!r}"
                    raise ValueError(msg)
                result = handler(server, operation.tag)
                observations.append(result)
            _verify_materialized_imports(materialized.import_root)
        finally:
            sys.path.remove(str(materialized.import_root))
            for name in tuple(sys.modules):
                if name == "libtmux" or name.startswith("libtmux."):
                    del sys.modules[name]
            sys.modules.update(previous)

        raw_record: dict[str, object] = {
            "scenario_id": scenario.scenario_id,
            "tmux_version": binary.version,
            "tmux_binary_sha256": binary.sha256,
            "python_source_commit": materialized.source_commit,
            "python_input_manifest_sha256": materialized.input_manifest_sha256,
            "semantic_contract_sha256": semantic_contract_sha256,
            "operations": [
                {"tag": operation.tag, "request": copy.deepcopy(operation.request)}
                for operation in scenario.operations
            ],
            "observations": observations,
        }
        canonical = canonicalize(
            raw_record,
            scenario.canonicalization,
            entity_fields=scenario.entity_fields,
        )
        if not isinstance(canonical, dict):
            msg = "canonical scenario record is not an object"
            raise TypeError(msg)
        record = scenario_record_from_document(canonical)
        for operation, result in zip(
            scenario.operations, record.observations, strict=True
        ):
            validate_observation(registry.operations[operation.tag], result)
        return record


def main(argv: t.Sequence[str] | None = None) -> int:
    """Run the reference adapter or list its registered operations.

    Parameters
    ----------
    argv : Sequence[str] | None, optional
        Arguments excluding the program name.

    Returns
    -------
    int
        Zero after writing the requested canonical artifact.

    Examples
    --------
    >>> callable(main)
    True
    """
    arguments = list(argv) if argv is not None else sys.argv[1:]
    if "--list-operations" in arguments:
        parser = argparse.ArgumentParser()
        parser.add_argument("--list-operations", action="store_true", required=True)
        parser.add_argument("--registry", type=pathlib.Path, required=True)
        namespace = parser.parse_args(arguments)
        sys.stdout.buffer.write(
            canonical_json_bytes(
                list_operations_document(load_registry(namespace.registry))
            )
        )
        return 0

    parser = argparse.ArgumentParser()
    parser.add_argument("--tmux-bin", type=pathlib.Path, required=True)
    socket = parser.add_mutually_exclusive_group(required=True)
    socket.add_argument("--socket-name")
    socket.add_argument("--socket-path")
    parser.add_argument("--scenario", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--repository", type=pathlib.Path, required=True)
    parser.add_argument("--observation", type=pathlib.Path, required=True)
    parser.add_argument("--input-manifest", type=pathlib.Path, required=True)
    parser.add_argument("--semantic-contract-sha256", required=True)
    namespace = parser.parse_args(arguments)
    endpoint = (
        SocketEndpoint("name", namespace.socket_name)
        if namespace.socket_name is not None
        else SocketEndpoint("path", namespace.socket_path)
    )
    record = execute_reference(
        tmux_binary=namespace.tmux_bin,
        endpoint=endpoint,
        scenario_path=namespace.scenario,
        repository=namespace.repository,
        observation_path=namespace.observation,
        input_manifest_path=namespace.input_manifest,
        semantic_contract_sha256=namespace.semantic_contract_sha256,
    )
    namespace.output.write_bytes(
        canonical_json_bytes(
            {
                "scenario_id": record.scenario_id,
                "tmux_version": record.tmux_version,
                "tmux_binary_sha256": record.tmux_binary_sha256,
                "python_source_commit": record.python_source_commit,
                "python_input_manifest_sha256": record.python_input_manifest_sha256,
                "semantic_contract_sha256": record.semantic_contract_sha256,
                "operations": list(record.operations),
                "observations": list(record.observations),
            }
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
