"""Check emitted filter-expression documents against the published schema.

The C++ side writes a corpus of real lowered expressions; this reads the schema
that consumers in other languages would read and validates every one of them
against it. It then requires a table of malformed documents to be rejected,
because a schema that accepts everything passes the first half perfectly.

That combination is what was missing. The schema required only ``kind`` on a
node, so ``{"kind": "string_test"}`` was a valid document with nothing in it,
and the test named ``SchemaConformance`` never loaded the schema at all.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import typing as t

import jsonschema

# Documents that must not validate. Each names the rule it is here to exercise,
# so a schema loosened by accident reports which guarantee it dropped.
INVALID: t.Final[list[tuple[str, dict[str, t.Any]]]] = [
    ("a node needs more than its kind", {"kind": "string_test"}),
    (
        "a string_test needs an operand",
        {"kind": "string_test", "name": "pane_current_command", "op": "eq"},
    ),
    (
        "a string_test has no ordering operators",
        {
            "kind": "string_test",
            "name": "pane_current_command",
            "op": "lt",
            "operand": "a",
        },
    ),
    (
        "a number_test compares a number, not its text",
        {"kind": "number_test", "name": "pane_index", "op": "eq", "operand": "1"},
    ),
    (
        "a number_test has no string operators",
        {"kind": "number_test", "name": "pane_index", "op": "contains", "number": 1},
    ),
    ("a bool_test needs what it expects", {"kind": "bool_test", "name": "pane_active"}),
    ("a begin_group needs its conjunction", {"kind": "begin_group"}),
    (
        "a begin_relation needs a quantifier in range",
        {"kind": "begin_relation", "name": "panes", "quantifier": 9},
    ),
    (
        "a bracket carries nothing else",
        {"kind": "end_group", "name": "panes"},
    ),
    (
        "fields of another kind are not permitted",
        {
            "kind": "bool_test",
            "name": "pane_active",
            "expected": True,
            "quantifier": 0,
        },
    ),
    ("an unknown kind is not a node", {"kind": "no_such_kind"}),
]


def _document(nodes: list[dict[str, t.Any]]) -> dict[str, t.Any]:
    return {"version": 1, "nodes": nodes}


def main(argv: t.Sequence[str] | None = None) -> int:
    """Validate the emitted corpus and reject the malformed table.

    Parameters
    ----------
    argv : Sequence[str] | None, optional
        Command line, defaulting to :data:`sys.argv`.

    Returns
    -------
    int
        Zero when every emitted document validates and every malformed one is
        refused.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--schema", type=pathlib.Path, required=True)
    parser.add_argument(
        "--corpus",
        type=pathlib.Path,
        required=True,
        help="one emitted document per line, written by the C++ test",
    )
    arguments = parser.parse_args(argv)

    schema = json.loads(arguments.schema.read_text())
    validator_for = jsonschema.validators.validator_for(schema)
    validator_for.check_schema(schema)
    validator = validator_for(schema)

    lines = [
        line for line in arguments.corpus.read_text().splitlines() if line.strip()
    ]
    if not lines:
        sys.stderr.write(f"{arguments.corpus} holds no documents\n")
        return 1

    failures = 0
    for number, line in enumerate(lines, start=1):
        document = json.loads(line)
        for error in validator.iter_errors(document):
            failures += 1
            sys.stderr.write(f"document {number}: {error.message}\n")

    for reason, node in INVALID:
        if validator.is_valid(_document([node])):
            failures += 1
            sys.stderr.write(f"schema accepted what it should refuse: {reason}\n")

    # And the envelope itself, which the node table cannot reach.
    for reason, document in [
        ("a document needs a version", {"nodes": []}),
        ("a document needs nodes", {"version": 1}),
        ("an unknown version is not this one", {"version": 2, "nodes": []}),
        ("nothing else belongs at the top", {"version": 1, "nodes": [], "extra": 1}),
    ]:
        if validator.is_valid(document):
            failures += 1
            sys.stderr.write(f"schema accepted what it should refuse: {reason}\n")

    if failures:
        return 1
    sys.stdout.write(
        f"{len(lines)} emitted documents validate; "
        f"{len(INVALID) + 4} malformed ones are refused\n",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
