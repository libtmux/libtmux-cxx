"""Evaluate the frozen lookup inventory against pinned Python sources.

The oracle is the authority for C++ lookup parity. It never reads the
surrounding worktree: the CLI resolves ``libtmux._internal.query_list`` from a
materialized source root so a later edit to the checkout cannot silently move
the goalposts.
"""

from __future__ import annotations

import argparse
import dataclasses
import importlib.util
import json
import pathlib
import sys
import typing as t

Json = dict[str, t.Any]

_MODULE = "libtmux/_internal/query_list.py"


@dataclasses.dataclass(frozen=True, slots=True)
class LookupCase:
    """One frozen lookup observation.

    Attributes
    ----------
    case_id : str
        Stable identifier the C++ runner joins on.
    operator : str
        Lookup name from the Python operator mapping.
    value : object
        Left-hand datum, tagged when it cannot be spelled in JSON.
    operand : object
        Right-hand operand, tagged when it cannot be spelled in JSON.
    expected_kind : str
        Recorded outcome: ``match``, ``no_match``, or ``error``.
    """

    case_id: str
    operator: str
    value: object
    operand: object
    expected_kind: str


def decode(value: object) -> object:
    r"""Return a tagged JSON value as the Python object it stands for.

    >>> decode({"kind": "bytes", "hex": "ff"})
    b'\xff'
    >>> decode({"kind": "missing"}) is None
    True
    >>> decode(["a", "b"])
    ['a', 'b']
    """
    if isinstance(value, dict) and "kind" in value:
        kind = value["kind"]
        if kind == "bytes":
            return bytes.fromhex(t.cast(str, value["hex"]))
        if kind == "missing":
            return None
        if kind == "mapping":
            return dict(t.cast(Json, value["items"]))
    return value


def _lookups() -> t.Mapping[str, t.Any]:
    from libtmux._internal.query_list import LOOKUP_NAME_MAP

    return LOOKUP_NAME_MAP


def evaluate_case(case: LookupCase) -> dict[str, object]:
    """Classify one lookup without leaking an exception message.

    >>> evaluate_case(
    ...     LookupCase("x", "exact", "a", "a", "match")
    ... ) == {"case_id": "x", "error_category": "", "outcome": "match"}
    True
    """
    lookups = _lookups()
    operator = lookups.get(case.operator)
    if operator is None:
        return {
            "case_id": case.case_id,
            "error_category": "unknown_operator",
            "outcome": "error",
        }
    try:
        matched = operator(decode(case.value), decode(case.operand))
    except Exception as error:  # noqa: BLE001 - the category is the evidence
        return {
            "case_id": case.case_id,
            "error_category": type(error).__name__,
            "outcome": "error",
        }
    return {
        "case_id": case.case_id,
        "error_category": "",
        "outcome": "match" if matched else "no_match",
    }


def _load_pinned(source_root: pathlib.Path) -> None:
    """Import the query mapping from the materialized source, not the worktree."""
    module_path = source_root / _MODULE
    if not module_path.is_file():
        message = f"materialized source is missing {_MODULE}"
        raise SystemExit(message)
    specification = importlib.util.spec_from_file_location(
        "libtmux._internal.query_list", module_path
    )
    if specification is None or specification.loader is None:
        message = "materialized query mapping is not importable"
        raise SystemExit(message)
    module = importlib.util.module_from_spec(specification)
    sys.modules["libtmux._internal.query_list"] = module
    specification.loader.exec_module(module)
    resolved = pathlib.Path(t.cast(str, module.__file__)).resolve()
    if not resolved.is_relative_to(source_root.resolve()):
        message = "resolved query mapping escaped the materialized source"
        raise SystemExit(message)


def canonical_json_bytes(value: object) -> bytes:
    r"""Serialize compact canonical JSON with one terminal LF.

    >>> canonical_json_bytes({"b": 2, "a": 1})
    b'{"a":1,"b":2}\n'
    """
    return (
        json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode()


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="query_oracle.py")
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--input-manifest", type=pathlib.Path, required=True)
    parser.add_argument("--expected-observation", type=pathlib.Path, required=True)
    parser.add_argument("--cases", type=pathlib.Path, required=True)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--output", type=pathlib.Path)
    group.add_argument("--check", type=pathlib.Path)
    return parser


def evaluate_inventory(cases: list[Json]) -> Json:
    """Return one result row per frozen case, ordered by identifier."""
    rows = [
        evaluate_case(
            LookupCase(
                case_id=t.cast(str, row["case_id"]),
                operator=t.cast(str, row["operator"]),
                value=row["value"],
                operand=row["operand"],
                expected_kind=t.cast(str, row["expected_kind"]),
            )
        )
        for row in cases
    ]
    return {
        "results": sorted(rows, key=lambda row: t.cast(str, row["case_id"])),
        "schema_version": 1,
    }


def main(argv: t.Sequence[str] | None = None) -> int:
    """Generate or re-verify the frozen Python lookup golden.

    >>> _parser().prog
    'query_oracle.py'
    """
    namespace = _parser().parse_args(argv)
    _load_pinned(namespace.source_root)
    cases = t.cast(list[Json], json.loads(namespace.cases.read_bytes())["cases"])
    document = evaluate_inventory(cases)
    payload = canonical_json_bytes(document)
    if namespace.check is not None:
        if namespace.check.read_bytes() != payload:
            print("python lookup golden does not reproduce", file=sys.stderr)
            return 1
        return 0
    namespace.output.parent.mkdir(parents=True, exist_ok=True)
    namespace.output.write_bytes(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
