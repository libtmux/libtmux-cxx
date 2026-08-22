"""Check emitted MCP tool answers against the output schemas they ship with.

The C++ side calls every tool against a real tmux and writes each answer beside
the ``outputSchema`` a client reads from ``tools/list``. This validates the
schemas themselves, then every answer against its own, then requires a table of
malformed answers to be rejected — because a schema that accepts everything
passes the first two halves perfectly.

The table is what caught ``wait_for_text`` reporting ``pane_id: ""`` when its
deadline expired before the target resolved: the schema required a pane ID
matching ``^%[0-9]+$``, so the empty string was never a legal answer.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import typing as t

import jsonschema

# Answers that must not validate, each naming the rule it exercises. A schema
# loosened by accident reports which guarantee it dropped.
INVALID: t.Final[list[tuple[str, str, dict[str, t.Any]]]] = [
    (
        "wait_for_text",
        "a pane ID names a pane or is absent",
        {
            "elapsed_ms": 1,
            "matched": False,
            "mode": "pane-lookup",
            "pane_id": "",
            "text": "",
            "timed_out": True,
        },
    ),
    (
        "wait_for_text",
        "a pane ID is not a window ID",
        {
            "elapsed_ms": 1,
            "matched": False,
            "mode": "capture-at-entry",
            "pane_id": "@1",
            "text": "",
            "timed_out": True,
        },
    ),
    (
        "wait_for_text",
        "an answer states whether it timed out",
        {"elapsed_ms": 1, "matched": False, "mode": "pane-lookup", "text": ""},
    ),
    (
        "wait_for_text",
        "elapsed time does not run backwards",
        {
            "elapsed_ms": -1,
            "matched": False,
            "mode": "pane-lookup",
            "text": "",
            "timed_out": True,
        },
    ),
    (
        "capture_pane",
        "captured text names the pane it came from",
        {"text": "hello"},
    ),
    (
        "list_sessions",
        "a session carries its own ID",
        {"sessions": [{"attached": False, "name": "a", "window_count": 1}]},
    ),
    (
        "list_sessions",
        "nothing else belongs beside the sessions",
        {"sessions": [], "extra": 1},
    ),
    (
        "create_session",
        "a session ID is not a pane ID",
        {"name": "a", "session_id": "%1"},
    ),
]


def main(argv: t.Sequence[str] | None = None) -> int:
    """Validate the emitted answers and reject the malformed table.

    Parameters
    ----------
    argv : Sequence[str] | None, optional
        Command line, defaulting to :data:`sys.argv`.

    Returns
    -------
    int
        Zero when every emitted answer validates, every published schema is
        itself valid, and every malformed answer is refused.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--corpus",
        type=pathlib.Path,
        required=True,
        help="one {tool, schema, document} object per line, written by the C++ test",
    )
    arguments = parser.parse_args(argv)

    lines = [line for line in arguments.corpus.read_text().splitlines() if line.strip()]
    if not lines:
        sys.stderr.write(f"{arguments.corpus} holds no answers\n")
        return 1

    failures = 0
    schemas: dict[str, dict[str, t.Any]] = {}
    for number, line in enumerate(lines, start=1):
        entry = json.loads(line)
        tool, schema, document = entry["tool"], entry["schema"], entry["document"]
        schemas.setdefault(tool, schema)

        validator_for = jsonschema.validators.validator_for(schema)
        try:
            validator_for.check_schema(schema)
        except jsonschema.exceptions.SchemaError as error:
            failures += 1
            sys.stderr.write(f"{tool}: published schema is invalid: {error.message}\n")
            continue

        for error in validator_for(schema).iter_errors(document):
            failures += 1
            sys.stderr.write(f"answer {number} from {tool}: {error.message}\n")

    for tool, reason, document in INVALID:
        schema = schemas.get(tool)
        if schema is None:
            failures += 1
            sys.stderr.write(
                f"{tool}: emitted no answer, so its schema went unchecked\n"
            )
            continue
        if jsonschema.validators.validator_for(schema)(schema).is_valid(document):
            failures += 1
            sys.stderr.write(f"{tool} accepted what it should refuse: {reason}\n")

    if failures:
        return 1
    sys.stdout.write(
        f"{len(lines)} emitted answers validate across {len(schemas)} tools; "
        f"{len(INVALID)} malformed ones are refused\n",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
