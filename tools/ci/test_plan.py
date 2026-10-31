"""Tests for the CI plan.

Each rule is checked twice: once against the real plan, and once against a
plan with that rule removed, which must fail. A check that passes either way
proves nothing about the rule it is named for.
"""

from __future__ import annotations

import json
import pathlib
import tempfile
import unittest
from typing import ClassVar
from unittest import mock

from tools.ci import plan

FIXTURE = plan.parse_manifest(
    {
        "released": ["1.0", "1.1", "1.1a", "2.0"],
        "preview": ["main"],
        "refused": ["0.9", "0.9a"],
        "compiler": "clang-18",
    }
)
SHA = "a" * 40


def make(**kwargs: str) -> plan.Plan:
    """Plan an event against the fixture."""
    kwargs.setdefault("event", "push")
    return plan.make_plan(FIXTURE, **kwargs)


def tmux_of(result: plan.Plan) -> list[str]:
    """Return the tmux refs of the compatibility legs."""
    return [leg["tmux"] for leg in result.compat]


def check_push_runs_edges() -> None:
    """Check that a push runs the oldest and newest release and nothing else."""
    result = make(event="push", ref_type="branch")
    assert tmux_of(result) == ["1.0", "2.0"]
    assert not result.rejection_planned
    assert result.tmux_jobs == 2


def check_pull_request_runs_edges() -> None:
    """Check that a pull request runs the same edges as a push."""
    result = make(event="pull_request")
    assert tmux_of(result) == ["1.0", "2.0"]
    assert not result.rejection_planned


def check_tag_runs_everything() -> None:
    """Check that a tag push proves the whole table, the preview and the refusals."""
    result = make(event="push", ref_type="tag")
    assert tmux_of(result) == ["1.0", "1.1", "1.1a", "2.0", "main"]
    assert result.rejection_planned
    assert result.tmux_jobs == 5 + 2


def check_schedule_runs_everything() -> None:
    """Check that a schedule keeps full scope."""
    result = make(event="schedule")
    assert result.scope == "full"
    assert result.rejection_planned


def check_dispatch_defaults_to_edges() -> None:
    """Check that a dispatch that asks for nothing runs the edges."""
    result = make(event="workflow_dispatch", scope="edges")
    assert tmux_of(result) == ["1.0", "2.0"]
    assert not result.rejection_planned


def check_dispatch_full() -> None:
    """Check that a dispatch with scope full runs everything, preview and refusals."""
    result = make(event="workflow_dispatch", scope="full")
    assert tmux_of(result) == ["1.0", "1.1", "1.1a", "2.0", "main"]
    assert result.rejection_planned


def check_workflow_call_full() -> None:
    """Check that a release calls the workflow with full scope on a tag push."""
    result = make(event="push", ref_type="tag", scope="full")
    assert result.scope == "full"
    called = make(event="push", ref_type="branch", scope="full")
    assert called.scope == "full"
    assert called.rejection_planned


def check_dispatch_custom() -> None:
    """Check that custom runs the refs it names, in order, once each."""
    result = make(
        event="workflow_dispatch",
        scope="custom",
        tmux_ref=f" 1.1a, main,{SHA},1.1a,feature/x ",
    )
    assert tmux_of(result) == ["1.1a", "main", SHA, "feature/x"]
    assert result.compat[2]["slug"] == "a" * 12
    assert result.compat[3]["slug"] == "feature-x"
    assert not result.rejection_planned


def check_edges_are_first_and_last_release() -> None:
    """Check that the edges are the ends of the released list, never a preview ref."""
    result = make(event="push")
    assert result.compat[0]["tmux"] == FIXTURE.released[0]
    assert result.compat[-1]["tmux"] == FIXTURE.released[-1]
    assert "main" not in tmux_of(result)


def check_long_path_follows_newest_release() -> None:
    """Check that the long-path step belongs to the newest release only."""
    result = make(event="workflow_dispatch", scope="full")
    flagged = [leg["tmux"] for leg in result.compat if leg["long_path"]]
    assert flagged == ["2.0"]


def check_tmate_takes_the_one_leg() -> None:
    """Check that a tmate session lands on the single leg of a custom dispatch."""
    result = make(
        event="workflow_dispatch",
        scope="custom",
        tmux_ref="1.1",
        tmate="on-failure",
    )
    assert [leg["tmate"] for leg in result.compat] == ["on-failure"]


def check_tmate_refuses_many_legs() -> None:
    """Check that edges are two legs, so tmate on them is an error."""
    try:
        make(event="workflow_dispatch", scope="edges", tmate="before-tests")
    except plan.PlanError as error:
        assert "exactly one" in str(error)
    else:
        message = "tmate on two legs was accepted"
        raise AssertionError(message)


def check_tmate_needs_a_dispatch() -> None:
    """Check that only a workflow_dispatch may open a session."""
    try:
        make(event="push", scope="custom", tmux_ref="1.1", tmate="on-failure")
    except plan.PlanError as error:
        assert "workflow_dispatch" in str(error)
    else:
        message = "tmate on a push was accepted"
        raise AssertionError(message)


def check_bad_ref_is_refused() -> None:
    """Check that a ref outside the allowed characters never reaches a step."""
    for bad in ("1.0;rm", "$(id)", "a b", "x" * 65, "-oops", "a,b|c"):
        try:
            make(event="workflow_dispatch", scope="custom", tmux_ref=bad)
        except plan.PlanError:
            continue
        message = f"{bad!r} was accepted"
        raise AssertionError(message)


def check_too_many_legs_is_refused() -> None:
    """Check that a dispatch cannot fan out past the limit."""
    refs = ",".join(f"9.{n}" for n in range(40))
    try:
        make(event="workflow_dispatch", scope="custom", tmux_ref=refs)
    except plan.PlanError:
        return
    message = "an oversized matrix was accepted"
    raise AssertionError(message)


def check_matrix_is_never_empty() -> None:
    """Check that every scope plans at least one leg and describes the refusal cells."""
    for event, scope, ref_type in (
        ("push", "", "branch"),
        ("push", "", "tag"),
        ("pull_request", "", ""),
        ("schedule", "", ""),
        ("workflow_dispatch", "edges", ""),
        ("workflow_dispatch", "full", ""),
    ):
        result = make(event=event, scope=scope, ref_type=ref_type)
        assert result.compat
        assert result.rejection


def check_aggregate_accepts_success() -> None:
    """Check that an all-success run passes with the refusals skipped."""
    ok = {"plan": {"result": "success"}, "compatibility": {"result": "success"}}
    assert plan.verify_needs(ok, rejection_planned=False) == []
    skipped = {**ok, "rejection": {"result": "skipped"}}
    assert plan.verify_needs(skipped, rejection_planned=False) == []
    ran = {**ok, "rejection": {"result": "success"}}
    assert plan.verify_needs(ran, rejection_planned=True) == []


def check_aggregate_refuses_anything_else() -> None:
    """Check that a failed, cancelled or skipped job fails the aggregate."""
    for result in ("failure", "cancelled", "skipped"):
        needs = {"compatibility": {"result": result}}
        assert plan.verify_needs(needs, rejection_planned=False)
    planned = {
        "compatibility": {"result": "success"},
        "rejection": {"result": "skipped"},
    }
    assert plan.verify_needs(planned, rejection_planned=True)
    unplanned = {
        "compatibility": {"result": "success"},
        "rejection": {"result": "success"},
    }
    assert plan.verify_needs(unplanned, rejection_planned=False)


def always(value: object):
    """Return a stand-in rule that ignores its arguments."""
    return lambda *_args, **_kwargs: value


def without_oldest_edge(self: plan.Manifest) -> str:
    """Stand in for an edge rule that takes the second release as the floor."""
    return self.released[1]


# Each entry pairs a check with a rule removed from the plan. The check must
# pass on the real plan and fail on the broken one.
MUTANTS = {
    check_push_runs_edges: ("scope_for", always("full")),
    check_pull_request_runs_edges: ("scope_for", always("full")),
    check_tag_runs_everything: ("scope_for", always("edges")),
    check_schedule_runs_everything: ("scope_for", always("edges")),
    check_dispatch_defaults_to_edges: ("scope_for", always("full")),
    check_dispatch_full: ("chosen_refs", lambda m, s, r: [m.oldest, m.newest]),
    check_workflow_call_full: ("scope_for", always("edges")),
    check_dispatch_custom: ("chosen_refs", lambda m, s, r: [m.oldest, m.newest]),
    check_tmate_takes_the_one_leg: ("assign_tmate", always(None)),
    check_tmate_refuses_many_legs: ("assign_tmate", always(None)),
    check_tmate_needs_a_dispatch: ("assign_tmate", always(None)),
    check_bad_ref_is_refused: ("_refs", lambda text, m: plan._split(text)),
    check_too_many_legs_is_refused: ("MAX_LEGS", 10_000),
    check_aggregate_accepts_success: ("expected_result", always("success")),
    check_aggregate_refuses_anything_else: ("expected_result", always("skipped")),
    check_matrix_is_never_empty: ("chosen_refs", always([])),
    check_edges_are_first_and_last_release: (
        "chosen_refs",
        lambda m, s, r: [m.released[1], m.preview[0]],
    ),
    check_long_path_follows_newest_release: ("_leg", None),
}


class PlanRulesTest(unittest.TestCase):
    """Run every check against the real plan."""

    def test_checks_pass_on_the_real_plan(self) -> None:
        """Every rule holds."""
        checks = [
            value
            for name, value in sorted(globals().items())
            if name.startswith("check_") and callable(value)
        ]
        self.assertGreater(len(checks), 10)
        for check in checks:
            with self.subTest(check=check.__name__):
                check()

    def test_every_check_has_a_mutant(self) -> None:
        """No rule is left without a negative test."""
        checks = {
            value
            for name, value in globals().items()
            if name.startswith("check_") and callable(value)
        }
        self.assertEqual(checks - set(MUTANTS), set())


class NegativeTest(unittest.TestCase):
    """Remove each rule and watch its check fail."""

    def test_removing_a_rule_fails_its_check(self) -> None:
        """A check that survives its rule's removal is not testing it."""
        for check, (name, broken) in MUTANTS.items():
            if broken is None:
                continue
            with self.subTest(check=check.__name__, removed=name):
                with (
                    mock.patch.object(plan, name, broken),
                    self.assertRaises((AssertionError, plan.PlanError)),
                ):
                    check()
                check()

    def test_the_edge_rule_fails_without_the_first_release(self) -> None:
        """Taking the second release for the floor breaks the edge check."""
        with (
            mock.patch.object(plan.Manifest, "oldest", property(without_oldest_edge)),
            self.assertRaises(AssertionError),
        ):
            check_push_runs_edges()
        check_push_runs_edges()

    def test_the_long_path_rule_fails_on_the_wrong_leg(self) -> None:
        """Marking the oldest release instead of the newest breaks the check."""
        real = plan._leg

        def wrong(
            ref: str, compiler: str, manifest: plan.Manifest
        ) -> dict[str, object]:
            leg = real(ref, compiler, manifest)
            leg["long_path"] = ref == manifest.oldest
            return leg

        with (
            mock.patch.object(plan, "_leg", wrong),
            self.assertRaises(AssertionError),
        ):
            check_long_path_follows_newest_release()


class ScopeAndInputTest(unittest.TestCase):
    """Cases that are errors or defaults rather than rules."""

    def test_an_unknown_scope_is_refused(self) -> None:
        """Only edges, full and custom exist."""
        with self.assertRaises(plan.PlanError):
            make(event="workflow_dispatch", scope="everything")

    def test_tmux_ref_without_custom_is_refused(self) -> None:
        """A forgotten scope must not silently run the edges."""
        with self.assertRaises(plan.PlanError):
            make(event="workflow_dispatch", scope="edges", tmux_ref="1.1")

    def test_custom_needs_something_to_customize(self) -> None:
        """An empty custom dispatch is a mistake."""
        with self.assertRaises(plan.PlanError):
            make(event="workflow_dispatch", scope="custom")

    def test_a_refusal_cell_cannot_be_a_compatibility_leg(self) -> None:
        """A release below the floor is run by the refusal job."""
        with self.assertRaises(plan.PlanError):
            make(event="workflow_dispatch", scope="custom", tmux_ref="0.9")

    def test_custom_compiler_alone_uses_the_edges(self) -> None:
        """A compiler without refs runs the edges under that compiler."""
        result = make(
            event="workflow_dispatch", scope="custom", cxx_compiler="gcc-14,clang-19"
        )
        self.assertEqual(
            [(leg["tmux"], leg["compiler"]) for leg in result.compat],
            [
                ("1.0", "gcc-14"),
                ("1.0", "clang-19"),
                ("2.0", "gcc-14"),
                ("2.0", "clang-19"),
            ],
        )
        gcc = result.compat[0]
        self.assertEqual(gcc["preset"], "cxx-gcc")
        self.assertEqual(
            gcc["cmake_args"], "-DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14"
        )
        clang = result.compat[1]
        self.assertEqual(clang["preset"], "cxx-dev")
        self.assertIn("clang++-19", clang["cmake_args"])

    def test_the_default_compiler_passes_no_arguments(self) -> None:
        """The normal stack is the preset as written."""
        for leg in make(event="push").compat:
            self.assertEqual(leg["cmake_args"], "")
            self.assertEqual(leg["compiler"], "clang-18")

    def test_a_bad_compiler_is_refused(self) -> None:
        """Compilers are `family-version`, nothing else."""
        for bad in ("clang", "icc-2", "gcc-14;id", "clang-123"):
            with self.subTest(bad=bad), self.assertRaises(plan.PlanError):
                make(event="workflow_dispatch", scope="custom", cxx_compiler=bad)

    def test_outputs_are_single_line_json(self) -> None:
        """GITHUB_OUTPUT takes one line per value."""
        outputs = make(event="push", ref_type="tag").outputs()
        for value in outputs.values():
            self.assertNotIn("\n", value)
        self.assertEqual(json.loads(outputs["compat"])["include"][0]["tmux"], "1.0")
        self.assertEqual(outputs["rejection_planned"], "true")
        self.assertEqual(outputs["tmux_jobs"], "7")
        self.assertEqual(
            json.loads(outputs["rejection"])["include"][0],
            {"rejected-tmux": "0.9", "client-tmux": "1.0"},
        )

    def test_the_refusal_matrix_is_described_even_when_skipped(self) -> None:
        """The matrix is never empty, so no evaluator sees `[]`."""
        outputs = make(event="push").outputs()
        self.assertEqual(outputs["rejection_planned"], "false")
        self.assertEqual(len(json.loads(outputs["rejection"])["include"]), 2)
        self.assertEqual(outputs["tmux_jobs"], "2")


class ManifestTest(unittest.TestCase):
    """The manifest refuses what would make the edges wrong."""

    base: ClassVar[dict[str, object]] = {
        "released": ["1.0", "2.0"],
        "preview": ["main"],
        "refused": ["0.9"],
        "compiler": "clang-18",
    }

    def parse(self, **changes: object) -> plan.Manifest:
        """Parse the base manifest with some keys replaced."""
        return plan.parse_manifest({**self.base, **changes})

    def test_the_shipped_manifest_loads(self) -> None:
        """The real manifest is valid."""
        manifest = plan.load_manifest()
        self.assertEqual(manifest.oldest, manifest.released[0])
        self.assertEqual(manifest.newest, manifest.released[-1])

    def test_released_must_be_oldest_first(self) -> None:
        """An out-of-order list would make the last entry the wrong edge."""
        with self.assertRaises(plan.PlanError):
            self.parse(released=["2.0", "1.0"])

    def test_released_must_be_unique(self) -> None:
        """A duplicate hides a mistake."""
        with self.assertRaises(plan.PlanError):
            self.parse(released=["1.0", "1.0"])

    def test_suffix_letters_order_after_the_release(self) -> None:
        """3.7 < 3.7a < 3.7b, as the library orders them."""
        self.parse(released=["1.0", "1.0a", "1.0b", "1.1"])
        with self.assertRaises(plan.PlanError):
            self.parse(released=["1.0a", "1.0"])

    def test_a_refusal_cell_must_sit_below_the_floor(self) -> None:
        """A refused release at or above the floor contradicts the table."""
        with self.assertRaises(plan.PlanError):
            self.parse(refused=["1.0"])
        with self.assertRaises(plan.PlanError):
            self.parse(refused=["1.5"])

    def test_the_preview_cannot_repeat_a_release(self) -> None:
        """A ref is either a release or a preview."""
        with self.assertRaises(plan.PlanError):
            self.parse(preview=["2.0"])

    def test_missing_keys_are_refused(self) -> None:
        """Every key is required."""
        for key in ("released", "preview", "refused", "compiler"):
            raw = {k: v for k, v in self.base.items() if k != key}
            with self.subTest(key=key), self.assertRaises(plan.PlanError):
                plan.parse_manifest(raw)

    def test_empty_lists_are_refused(self) -> None:
        """No released version means no edges, and no refusal cell no floor lane."""
        with self.assertRaises(plan.PlanError):
            self.parse(released=[])
        with self.assertRaises(plan.PlanError):
            self.parse(refused=[])


class CommandLineTest(unittest.TestCase):
    """The step the workflow runs."""

    def test_matrix_writes_github_output(self) -> None:
        """Each output lands as `name=value`."""
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "out"
            status = plan.main(
                ["matrix", "--event", "push", "--github-output", str(output)]
            )
            self.assertEqual(status, 0)
            lines = dict(line.split("=", 1) for line in output.read_text().splitlines())
        self.assertEqual(lines["scope"], "edges")
        self.assertEqual(lines["rejection_planned"], "false")
        self.assertEqual(len(json.loads(lines["compat"])["include"]), 2)

    def test_a_refused_plan_exits_nonzero(self) -> None:
        """Exit nonzero when tmate has two legs to attach to."""
        status = plan.main(
            [
                "matrix",
                "--event",
                "workflow_dispatch",
                "--scope",
                "edges",
                "--tmate",
                "on-failure",
            ]
        )
        self.assertEqual(status, 1)

    def test_verify_reports_a_failed_job(self) -> None:
        """The aggregate step exits nonzero on any non-success."""
        good = json.dumps({"lint": {"result": "success"}})
        bad = json.dumps({"lint": {"result": "failure"}})
        self.assertEqual(plan.main(["verify", "--needs", good]), 0)
        self.assertEqual(plan.main(["verify", "--needs", bad]), 1)


if __name__ == "__main__":
    unittest.main()
