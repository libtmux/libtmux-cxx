"""Check that the docs and the workflows follow the CI manifest.

``tmux_matrix.json`` is the one list of tmux versions. This fails when a page
states a range the manifest does not, when a workflow carries a version list of
its own, and when the workflow structure the plan relies on has been undone: a
dispatch input spliced into a script, a tmate step that is not pinned or not
limited to the actor, an aggregate job that forgot a job, a release that no
longer runs the whole table.

Run from the repository root::

    python3 -m tools.ci.check
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

from tools.ci.plan import Manifest, PlanError, load_manifest

PAGES = ("README.md", "docs/compatibility.md", ".github/CONTRIBUTING.md")
REFUSAL_PAGES = ("README.md", ".github/CONTRIBUTING.md")
WORKFLOWS = pathlib.PurePosixPath(".github/workflows")

TMATE = re.compile(r"uses:\s*mxschmitt/action-tmate@(\S+)")
SPLICED = re.compile(
    r"\$\{\{[^}]*\b("
    r"inputs\.|github\.event\.inputs|needs\.plan\.outputs"
    r"|matrix\.(?:tmux|slug|compiler|family|version|preset|cmake_args|tmate)"
    r"|steps\.[\w-]+\.outputs"
    r")"
)
RANGE_CLAIM = re.compile(r"(\d+\.\d+[a-z]?) through (\d+\.\d+[a-z]?), plus `")
RUN_KEY = re.compile(r"^(\s*)(?:- )?run:\s*(.*)$")


def _squash(text: str) -> str:
    return re.sub(r"\s+", " ", text)


def _code(text: str) -> list[str]:
    """Return the lines of a workflow that are not comments."""
    return [line for line in text.splitlines() if not line.lstrip().startswith("#")]


def _run_bodies(text: str) -> list[tuple[int, str]]:
    """Return each line of every `run:` script with its line number."""
    lines = text.splitlines()
    bodies: list[tuple[int, str]] = []
    index = 0
    while index < len(lines):
        match = RUN_KEY.match(lines[index])
        index += 1
        if not match:
            continue
        indent, value = len(match[1]), match[2]
        if value and not value.startswith(("|", ">")):
            bodies.append((index, value))
            continue
        while index < len(lines):
            line = lines[index]
            if line.strip() and len(line) - len(line.lstrip()) <= indent:
                break
            bodies.append((index + 1, line))
            index += 1
    return bodies


def _jobs(text: str) -> dict[str, str]:
    """Return each top-level job of a workflow, by id, with its text."""
    section = text.split("\njobs:\n", 1)[-1]
    parts = re.split(r"(?m)^  ([A-Za-z0-9_-]+):\s*$", section)
    return dict(zip(parts[1::2], parts[2::2], strict=True))


def _steps(job: str) -> list[str]:
    return re.split(r"(?m)^      - ", job)[1:]


def check_docs(root: pathlib.Path, manifest: Manifest) -> list[str]:
    """Return where a page disagrees with the manifest."""
    problems = []
    range_claim = (
        f"{manifest.oldest} through {manifest.newest}, plus `{manifest.preview[0]}`"
    )
    edges_claim = f"{manifest.oldest} and {manifest.newest}"
    refusals = f"{', '.join(manifest.refused[:-1])} and {manifest.refused[-1]}"
    for page in PAGES:
        text = _squash((root / page).read_text())
        stated = RANGE_CLAIM.findall(text)
        if not stated:
            problems.append(f"{page} does not say CI covers {range_claim}")
        problems += [
            f"{page} says CI covers {first} through {last}, not {range_claim}"
            for first, last in stated
            if (first, last) != (manifest.oldest, manifest.newest)
        ]
        if edges_claim not in text:
            problems.append(f"{page} does not say a push runs the edges, {edges_claim}")
    for page in REFUSAL_PAGES:
        if refusals not in _squash((root / page).read_text()):
            problems.append(f"{page} does not list the refusal cells {refusals}")
    contributing = (root / ".github/CONTRIBUTING.md").read_text()
    if "gh workflow run ci.yml" not in contributing:
        problems.append(
            ".github/CONTRIBUTING.md has no `gh workflow run ci.yml` command"
        )
    return problems


def check_ci(text: str, manifest: Manifest) -> list[str]:
    """Return what is wrong with ci.yml."""
    problems = []
    code = "\n".join(_code(text))
    problems += [
        f"ci.yml names tmux {version}; the manifest owns that list"
        for version in (*manifest.released, *manifest.refused)
        if re.search(rf"(?<![\w.]){re.escape(version)}(?![\w.])", code)
    ]
    problems += [
        f"ci.yml:{number} splices an input into a script; use env:"
        for number, body in _run_bodies(text)
        if SPLICED.search(body)
    ]

    jobs = _jobs(text)
    for name, output in (("compatibility", "compat"), ("rejection", "rejection")):
        wanted = f"matrix: ${{{{ fromJSON(needs.plan.outputs.{output}) }}}}"
        if wanted not in jobs.get(name, ""):
            problems.append(f"the {name} job does not take its matrix from the plan")

    aggregate = jobs.get("all-checks-passed", "")
    listed = set(re.findall(r"(?m)^      - ([\w-]+)$", aggregate.split("steps:")[0]))
    wanted_jobs = set(jobs) - {"all-checks-passed"}
    problems += [
        f"all-checks-passed does not need {job}" for job in sorted(wanted_jobs - listed)
    ]
    problems += [
        f"all-checks-passed needs {job}, which is not a job"
        for job in sorted(listed - wanted_jobs)
    ]

    group = re.search(r"(?m)^  group: (.*)$", text.split("concurrency:", 1)[-1])
    if (
        not group
        or "github.run_id" not in group[1]
        or "workflow_dispatch" not in group[1]
    ):
        problems.append("the concurrency group does not separate dispatch runs")

    tmate = [
        step for step in _steps(jobs.get("compatibility", "")) if TMATE.search(step)
    ]
    if len(tmate) != 2:
        problems.append(
            "the compatibility job needs a before-tests and an on-failure tmate step"
        )
    problems.extend(_check_tmate_step(step) for step in tmate)
    if len(TMATE.findall(code)) != len(tmate):
        problems.append("action-tmate appears outside the compatibility job")
    return [problem for problem in problems if problem]


def _check_tmate_step(step: str) -> str:
    pin = TMATE.search(step)[1]
    if re.fullmatch(r"[0-9a-f]{40}", pin) is None:
        return f"action-tmate is pinned to {pin}, not a full commit"
    if "limit-access-to-actor: true" not in step:
        return "an action-tmate step does not limit access to the actor"
    if "timeout-minutes:" not in step:
        return "an action-tmate step has no timeout-minutes"
    if "github.event_name == 'workflow_dispatch'" not in step:
        return "an action-tmate step can run outside a workflow_dispatch"
    return ""


def check_release(text: str) -> list[str]:
    """Return what is wrong with release.yml."""
    problems = []
    jobs = _jobs(text)
    ci = jobs.get("ci", "")
    if "uses: ./.github/workflows/ci.yml" not in ci or "scope: full" not in ci:
        problems.append("release.yml does not run ci.yml at scope full")
    if not re.search(r"needs:\s*\[ci\]", jobs.get("source", "")):
        problems.append("the source job does not wait for ci")
    if "workflows/ci.yml/runs" in text:
        problems.append("release.yml trusts an earlier ci run instead of running one")
    return problems


def check(root: pathlib.Path) -> list[str]:
    """Return every disagreement between the manifest, the docs and the workflows."""
    manifest = load_manifest(root / "tools/ci/tmux_matrix.json")
    problems = check_docs(root, manifest)
    workflows = root / WORKFLOWS
    problems += check_ci((workflows / "ci.yml").read_text(), manifest)
    problems += check_release((workflows / "release.yml").read_text())
    for path in sorted(workflows.glob("*.yml")):
        if path.name != "ci.yml" and TMATE.search(path.read_text()):
            problems.append(f"{path.name} runs action-tmate; only ci.yml may")
    return problems


def main(argv: list[str] | None = None) -> int:
    """Print every disagreement and exit nonzero if there is one."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path())
    args = parser.parse_args(argv)
    try:
        problems = check(args.root.resolve())
    except PlanError as error:
        problems = [str(error)]
    for problem in problems:
        print(problem, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
