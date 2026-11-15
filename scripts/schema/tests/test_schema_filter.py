"""The filter-expression validator passes a good corpus and catches a loose schema."""

from __future__ import annotations

import contextlib
import io
import json
import pathlib
import typing as t

import schema_filter

SCHEMA = (
    pathlib.Path(__file__).resolve().parents[3]
    / "schema/filter-expression-v1.schema.json"
)
GOOD = {
    "version": 1,
    "nodes": [
        {"kind": "begin_group", "conjunction": True},
        {"kind": "bool_test", "name": "pane_active", "expected": True},
        {"kind": "number_test", "name": "pane_index", "op": "eq", "number": 1},
        {"kind": "end_group"},
    ],
}


def run(
    schema: pathlib.Path, lines: list[str], tmp_path: pathlib.Path
) -> tuple[int, str]:
    """Validate `lines` as a corpus against `schema`; return status and stderr."""
    corpus = tmp_path / "corpus.jsonl"
    corpus.write_text("\n".join(lines))
    err = io.StringIO()
    with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
        status = schema_filter.main(["--schema", str(schema), "--corpus", str(corpus)])
    return status, err.getvalue()


def test_the_published_schema_accepts_a_good_corpus(tmp_path: pathlib.Path) -> None:
    """Every emitted document validates and every malformed one is refused."""
    assert run(SCHEMA, [json.dumps(GOOD)], tmp_path) == (0, "")


def test_an_emitted_document_the_schema_refuses_fails(tmp_path: pathlib.Path) -> None:
    """A document of the wrong shape is reported by its position."""
    bad = {"version": 1, "nodes": [{"kind": "string_test"}]}
    status, err = run(SCHEMA, [json.dumps(GOOD), json.dumps(bad)], tmp_path)
    assert status == 1
    assert err.startswith("document 2:")


def test_an_empty_corpus_is_not_a_pass(tmp_path: pathlib.Path) -> None:
    """A corpus nobody wrote proves nothing about the schema."""
    status, err = run(SCHEMA, ["", "  "], tmp_path)
    assert status == 1
    assert "holds no documents" in err


def test_a_schema_that_accepts_everything_is_caught(tmp_path: pathlib.Path) -> None:
    """The malformed table is what fails a loosened schema."""
    loose = tmp_path / "loose.schema.json"
    loose.write_text(
        json.dumps({"$schema": "https://json-schema.org/draft/2020-12/schema"})
    )
    status, err = run(loose, [json.dumps(GOOD)], tmp_path)
    assert status == 1
    assert (
        "schema accepted what it should refuse: a node needs more than its kind" in err
    )
    assert "schema accepted what it should refuse: a document needs a version" in err


def test_every_malformed_document_names_a_distinct_rule() -> None:
    """Two entries with one reason would hide a dropped guarantee."""
    reasons: list[str] = [reason for reason, _ in schema_filter.INVALID]
    assert len(reasons) == len(set(reasons))


def test_the_table_of_malformed_documents_is_json(tmp_path: t.Any) -> None:
    """Each malformed node survives a round trip through JSON."""
    for _, node in schema_filter.INVALID:
        assert json.loads(json.dumps(node)) == node
