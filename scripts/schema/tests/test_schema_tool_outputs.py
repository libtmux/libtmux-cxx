"""The tool-output validator checks each answer against the schema it shipped with."""

from __future__ import annotations

import contextlib
import io
import json
import pathlib
import typing as t

import pytest
import schema_tool_outputs

TIGHT = {
    "type": "object",
    "required": ["pane_id"],
    "properties": {"pane_id": {"type": "string", "pattern": "^%[0-9]+$"}},
    "additionalProperties": False,
}
LOOSE = {"type": "object"}
REFUSED = {"pane_id": ""}


def entry(tool: str, schema: dict[str, t.Any], document: dict[str, t.Any]) -> str:
    """Return one corpus line."""
    return json.dumps({"tool": tool, "schema": schema, "document": document})


def run(lines: list[str], tmp_path: pathlib.Path) -> tuple[int, str, str]:
    """Validate `lines` as a corpus; return status, stdout and stderr."""
    corpus = tmp_path / "answers.jsonl"
    corpus.write_text("\n".join(lines))
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        status = schema_tool_outputs.main(["--corpus", str(corpus)])
    return status, out.getvalue(), err.getvalue()


@pytest.fixture(autouse=True)
def one_malformed_answer(monkeypatch: pytest.MonkeyPatch) -> None:
    """Shrink the table of malformed answers to one that the tight schema refuses."""
    monkeypatch.setattr(
        schema_tool_outputs,
        "INVALID",
        [("capture_pane", "a pane ID names a pane", REFUSED)],
    )


def test_a_valid_answer_and_a_refused_malformed_one_pass(
    tmp_path: pathlib.Path,
) -> None:
    """The answer validates and the malformed table is refused."""
    status, out, _ = run([entry("capture_pane", TIGHT, {"pane_id": "%1"})], tmp_path)
    assert status == 0
    assert "1 emitted answers validate across 1 tools" in out


def test_an_answer_that_breaks_its_schema_fails(tmp_path: pathlib.Path) -> None:
    """The answer is read against the schema printed beside it."""
    status, _, err = run([entry("capture_pane", TIGHT, {"pane_id": "@1"})], tmp_path)
    assert status == 1
    assert "answer 1 from capture_pane" in err


def test_an_invalid_published_schema_is_reported(tmp_path: pathlib.Path) -> None:
    """A schema that is not itself valid is a failure before any answer is read."""
    broken = {"type": "no-such-type"}
    status, _, err = run([entry("capture_pane", broken, {})], tmp_path)
    assert status == 1
    assert "capture_pane: published schema is invalid" in err


def test_a_loose_schema_accepts_what_it_should_refuse(tmp_path: pathlib.Path) -> None:
    """The malformed table is what fails a schema that accepts everything."""
    status, _, err = run([entry("capture_pane", LOOSE, {"pane_id": "%1"})], tmp_path)
    assert status == 1
    assert "capture_pane accepted what it should refuse: a pane ID names a pane" in err


def test_a_tool_that_emitted_nothing_leaves_its_schema_unchecked(
    tmp_path: pathlib.Path,
) -> None:
    """A malformed-table entry with no answer behind it is a failure."""
    status, _, err = run([entry("list_panes", TIGHT, {"pane_id": "%1"})], tmp_path)
    assert status == 1
    assert "capture_pane: emitted no answer, so its schema went unchecked" in err


def test_an_empty_corpus_is_not_a_pass(tmp_path: pathlib.Path) -> None:
    """A corpus nobody wrote proves nothing."""
    status, _, err = run(["", " "], tmp_path)
    assert status == 1
    assert "holds no answers" in err
