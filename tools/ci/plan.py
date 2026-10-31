r"""Plan the tmux axis of the CI matrix from one manifest and the triggering event.

``tmux_matrix.json`` is the only list of tmux versions CI uses. A push or pull
request runs its first and last released entries; a release, a ``schedule`` run
and a dispatch with ``scope: full`` run all of it, the refusal cells and the
preview refs; a dispatch with ``scope: custom`` runs the refs it names. Nothing
here touches the network, so the same plan can be reproduced locally.

Run from the repository root::

    python3 -m tools.ci.plan matrix --event push
    python3 -m tools.ci.plan matrix --event workflow_dispatch --scope custom \\
        --tmux-ref 3.6b,master --tmate on-failure
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import re
import sys

MANIFEST_PATH = pathlib.Path(__file__).with_name("tmux_matrix.json")

REF_PATTERN = re.compile(r"[A-Za-z0-9._/-]{1,64}")
SHA_PATTERN = re.compile(r"[0-9a-f]{40}")
VERSION_PATTERN = re.compile(r"(\d+)\.(\d+)([a-z]?)")
COMPILER_PATTERN = re.compile(r"(clang|gcc)-(\d{1,2})")

SCOPES = ("edges", "full", "custom")
TMATE_MODES = ("off", "before-tests", "on-failure")

# GitHub rejects a matrix over 256 jobs; a dispatch that asks for more than
# this is a typo or a loop, and it is cheaper to refuse it here.
MAX_LEGS = 16


class PlanError(Exception):
    """The inputs describe a run CI will not make."""


@dataclasses.dataclass(frozen=True)
class Manifest:
    """The tmux versions CI knows about, oldest release first."""

    released: tuple[str, ...]
    preview: tuple[str, ...]
    refused: tuple[str, ...]
    compiler: str

    @property
    def oldest(self) -> str:
        """Return the oldest released version, the support floor."""
        return self.released[0]

    @property
    def newest(self) -> str:
        """Return the newest released version."""
        return self.released[-1]


@dataclasses.dataclass(frozen=True)
class Plan:
    """What one run builds on the tmux axis."""

    scope: str
    compat: tuple[dict[str, object], ...]
    rejection: tuple[dict[str, str], ...]
    rejection_planned: bool

    @property
    def tmux_jobs(self) -> int:
        """Return how many jobs the tmux axis expands to."""
        planned = len(self.rejection) if self.rejection_planned else 0
        return len(self.compat) + planned

    def outputs(self) -> dict[str, str]:
        """Return the step outputs, each on one line."""
        return {
            "scope": self.scope,
            "compat": _json({"include": list(self.compat)}),
            "rejection": _json({"include": list(self.rejection)}),
            "rejection_planned": str(self.rejection_planned).lower(),
            "tmux_jobs": str(self.tmux_jobs),
        }


def _json(value: object) -> str:
    return json.dumps(value, separators=(",", ":"))


def _version_key(version: str) -> tuple[int, int, str]:
    match = VERSION_PATTERN.fullmatch(version)
    if match is None:
        message = f"{version!r} is not a tmux release such as 3.6 or 3.6b"
        raise PlanError(message)
    return int(match[1]), int(match[2]), match[3]


def _strings(raw: dict[str, object], key: str) -> tuple[str, ...]:
    value = raw.get(key)
    if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
        message = f"the manifest needs {key!r} as a list of strings"
        raise PlanError(message)
    return tuple(value)


def parse_manifest(raw: dict[str, object]) -> Manifest:
    """Validate a decoded manifest and return it."""
    released = _strings(raw, "released")
    preview = _strings(raw, "preview")
    refused = _strings(raw, "refused")
    compiler = raw.get("compiler")
    if not released:
        message = "the manifest lists no released tmux"
        raise PlanError(message)
    if not refused:
        message = "the manifest lists no refusal cell"
        raise PlanError(message)
    keys = [_version_key(version) for version in released]
    if keys != sorted(set(keys)):
        message = "released tmux versions must be unique and oldest first"
        raise PlanError(message)
    floor = keys[0]
    for version in refused:
        if _version_key(version) >= floor:
            message = f"refusal cell {version} is not below the floor {released[0]}"
            raise PlanError(message)
    for ref in preview:
        if REF_PATTERN.fullmatch(ref) is None or ref in released:
            message = f"preview ref {ref!r} is malformed or already released"
            raise PlanError(message)
    if not isinstance(compiler, str) or COMPILER_PATTERN.fullmatch(compiler) is None:
        message = "the manifest needs 'compiler' such as clang-18"
        raise PlanError(message)
    return Manifest(released, preview, refused, compiler)


def load_manifest(path: pathlib.Path = MANIFEST_PATH) -> Manifest:
    """Read and validate the manifest at ``path``."""
    return parse_manifest(json.loads(path.read_text()))


def scope_for(event: str, ref_type: str, requested: str) -> str:
    """Return the scope an event runs at.

    An explicit request wins. Otherwise a tag and a ``schedule`` run the whole
    table, because a release must prove all of it, and everything else runs the
    edges.
    """
    if requested:
        if requested not in SCOPES:
            message = f"scope must be one of {', '.join(SCOPES)}, not {requested!r}"
            raise PlanError(message)
        return requested
    if event == "schedule" or (event == "push" and ref_type == "tag"):
        return "full"
    return "edges"


def _split(text: str) -> list[str]:
    items: list[str] = []
    for item in text.split(","):
        item = item.strip()
        if item and item not in items:
            items.append(item)
    return items


def _refs(text: str, manifest: Manifest) -> list[str]:
    refs = _split(text)
    for ref in refs:
        if REF_PATTERN.fullmatch(ref) is None or ref.startswith("-"):
            message = f"tmux-ref {ref!r} must match [A-Za-z0-9._/-]{{1,64}}"
            raise PlanError(message)
        if ref in manifest.refused:
            message = (
                f"tmux {ref} is below the floor; its refusal cell runs with scope full"
            )
            raise PlanError(message)
    return refs


def _compilers(text: str, manifest: Manifest) -> list[str]:
    compilers = _split(text)
    for compiler in compilers:
        if COMPILER_PATTERN.fullmatch(compiler) is None:
            message = f"cxx-compiler {compiler!r} must look like clang-19 or gcc-14"
            raise PlanError(message)
    return compilers or [manifest.compiler]


def _slug(ref: str) -> str:
    if SHA_PATTERN.fullmatch(ref):
        return ref[:12]
    return re.sub(r"[^A-Za-z0-9._-]", "-", ref)


def _leg(ref: str, compiler: str, manifest: Manifest) -> dict[str, object]:
    family, version = COMPILER_PATTERN.fullmatch(compiler).groups()
    args = ""
    if compiler != manifest.compiler:
        cc, cxx = ("clang", "clang++") if family == "clang" else ("gcc", "g++")
        args = f"-DCMAKE_C_COMPILER={cc}-{version} -DCMAKE_CXX_COMPILER={cxx}-{version}"
    return {
        "tmux": ref,
        "slug": _slug(ref),
        "compiler": compiler,
        "family": family,
        "version": version,
        "preset": "cxx-dev" if family == "clang" else "cxx-gcc",
        "cmake_args": args,
        "long_path": ref == manifest.newest,
        "tmate": "off",
    }


def chosen_refs(manifest: Manifest, scope: str, refs: list[str]) -> list[str]:
    """Return the tmux refs the compatibility job builds for a scope."""
    if scope == "full":
        return [*manifest.released, *manifest.preview]
    if scope == "custom" and refs:
        return refs
    return [manifest.oldest, manifest.newest]


def rejection_in_scope(scope: str) -> bool:
    """Return whether the refusal cells run: only when the whole table does."""
    return scope == "full"


def assign_tmate(legs: list[dict[str, object]], tmate: str, event: str) -> None:
    """Give the one compatibility leg a tmate session, or refuse the run."""
    if tmate in ("", "off"):
        return
    if tmate not in TMATE_MODES:
        message = f"tmate must be one of {', '.join(TMATE_MODES)}, not {tmate!r}"
        raise PlanError(message)
    if event != "workflow_dispatch":
        message = "tmate only runs from a workflow_dispatch"
        raise PlanError(message)
    if len(legs) != 1:
        message = (
            f"tmate needs exactly one compatibility leg and this plan has "
            f"{len(legs)}: use scope custom with one tmux-ref and at most one "
            f"cxx-compiler"
        )
        raise PlanError(message)
    legs[0]["tmate"] = tmate


def make_plan(
    manifest: Manifest,
    *,
    event: str,
    ref_type: str = "",
    scope: str = "",
    tmux_ref: str = "",
    cxx_compiler: str = "",
    tmate: str = "",
) -> Plan:
    """Return the plan for one event and its dispatch inputs."""
    effective = scope_for(event, ref_type, scope)
    refs = _refs(tmux_ref, manifest)
    compilers = _compilers(cxx_compiler, manifest)
    if refs and effective != "custom":
        message = "tmux-ref only applies with scope custom"
        raise PlanError(message)
    if effective == "custom" and not refs and not cxx_compiler.strip():
        message = "scope custom needs a tmux-ref or a cxx-compiler"
        raise PlanError(message)

    legs = [
        _leg(ref, compiler, manifest)
        for ref in chosen_refs(manifest, effective, refs)
        for compiler in compilers
    ]
    if len(legs) > MAX_LEGS:
        message = f"{len(legs)} compatibility legs is over the limit of {MAX_LEGS}"
        raise PlanError(message)
    assign_tmate(legs, tmate, event)
    if not legs:
        message = "the plan has no compatibility leg"
        raise PlanError(message)

    # The refusal cells are always described, so the matrix is never empty
    # even when the job is skipped; `rejection_planned` says whether they run.
    rejection = tuple(
        {"rejected-tmux": version, "client-tmux": manifest.oldest}
        for version in manifest.refused
    )
    return Plan(
        effective,
        tuple(legs),
        rejection,
        rejection_planned=rejection_in_scope(effective),
    )


def expected_result(job: str, *, rejection_planned: bool) -> str:
    """Return the result a job must report for the aggregate to pass.

    Every job must have succeeded. The refusal cells are the one exception:
    they are skipped when the plan put them out of scope.
    """
    if job == "rejection" and not rejection_planned:
        return "skipped"
    return "success"


def verify_needs(
    needs: dict[str, dict[str, object]], *, rejection_planned: bool
) -> list[str]:
    """Return what is wrong with the results the aggregate job saw."""
    problems = []
    for name, info in sorted(needs.items()):
        expected = expected_result(name, rejection_planned=rejection_planned)
        result = info.get("result")
        if result != expected:
            problems.append(f"{name} reported {result}, expected {expected}")
    return problems


def _summary(plan: Plan) -> str:
    legs = ", ".join(f"{leg['tmux']} ({leg['compiler']})" for leg in plan.compat)
    refusals = (
        ", ".join(leg["rejected-tmux"] for leg in plan.rejection)
        if plan.rejection_planned
        else "out of scope"
    )
    return "\n".join(
        [
            f"scope: {plan.scope}",
            f"compatibility: {legs}",
            f"rejection: {refusals}",
            f"tmux jobs: {plan.tmux_jobs}",
        ]
    )


def _run_matrix(args: argparse.Namespace) -> int:
    plan = make_plan(
        load_manifest(),
        event=args.event,
        ref_type=args.ref_type,
        scope=args.scope,
        tmux_ref=args.tmux_ref,
        cxx_compiler=args.cxx_compiler,
        tmate=args.tmate,
    )
    print(_summary(plan))
    if args.github_output:
        with pathlib.Path(args.github_output).open("a") as output:
            output.writelines(f"{k}={v}\n" for k, v in plan.outputs().items())
    return 0


def _run_verify(args: argparse.Namespace) -> int:
    needs = json.loads(args.needs)
    problems = verify_needs(needs, rejection_planned=args.rejection_planned == "true")
    for problem in problems:
        print(problem, file=sys.stderr)
    return 1 if problems else 0


def main(argv: list[str] | None = None) -> int:
    """Plan the matrix, or check the results of the jobs it ran."""
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    matrix = subparsers.add_parser("matrix", help="print the plan for an event")
    matrix.add_argument("--event", required=True)
    matrix.add_argument("--ref-type", default="")
    matrix.add_argument("--scope", default="")
    matrix.add_argument("--tmux-ref", default="")
    matrix.add_argument("--cxx-compiler", default="")
    matrix.add_argument("--tmate", default="")
    matrix.add_argument("--github-output", default="")
    matrix.set_defaults(handler=_run_matrix)

    verify = subparsers.add_parser("verify", help="check the aggregate job's needs")
    verify.add_argument("--needs", required=True, help="the JSON of `needs`")
    verify.add_argument("--rejection-planned", default="false")
    verify.set_defaults(handler=_run_verify)

    args = parser.parse_args(argv)
    try:
        return args.handler(args)
    except PlanError as error:
        print(f"::error::{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
