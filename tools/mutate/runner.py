"""Break the code on purpose and find out whether anything notices.

Every finding worth having in the recent work came from this rather than
from reading: a test that asserted an empty key was refused stayed green
once the guard was deleted, because tmux refuses it anyway, and a pause the
builder never took went unnoticed because one threshold covered two sleeps.

Run by hand, it has a failure mode that is worse than not running it. A
pattern that no longer matches, or a mutation that does not compile, prints
nothing and reads exactly like a test suite holding firm. So the two
outcomes are named here and both fail the run:

``killed``
    The mutation applied, built, and a test failed. What is wanted.
``survived``
    It applied, built, and every test passed. Something is untested.
``not a result``
    It did not apply, did not build, or did not reach the binary that was
    then tested. Nothing was learned, and saying so is the whole point:
    a verdict nobody earned is worse than no verdict.

The tree is edited in place and each file put back as the run moves on, so
nothing else may read it meanwhile. Committing during a run captures a file
mid-mutation, which is how a `load-buffer` lost the flag naming its buffer
and reached a commit looking like ordinary work.
"""

from __future__ import annotations

import contextlib
import dataclasses
import hashlib
import json
import os
import pathlib
import subprocess
import typing as t


@dataclasses.dataclass(frozen=True)
class Mutation:
    """One deliberate break, and where it should be noticed.

    Attributes
    ----------
    mutation_id : str
        Short name, used to select one and to report it.
    path : str
        Repository-relative source to edit.
    find : str
        Text to replace.  Must appear exactly once, so an edit elsewhere
        that duplicates it is reported rather than silently applied twice.
    replace : str
        What to put there.
    target : str
        CMake target to build and run.
    guards : str
        What this mutation is asking about, in one line, so a survivor
        says what is untested rather than only which text changed.
    executable : str | None, optional
        Executable file name when it differs from the CMake target name.
    test_regex : str | None, optional
        Explicit CTest regular expression. When omitted, the legacy target-name
        convention is used.
    presets : tuple[str, ...], optional
        Build presets where this mutation has a target and test. Empty means
        every preset.
    """

    mutation_id: str
    path: str
    find: str
    replace: str
    target: str
    guards: str
    executable: str | None = None
    test_regex: str | None = None
    presets: tuple[str, ...] = ()

    def applies_to(self, preset: str) -> bool:
        """Return whether this mutation belongs to the selected build."""
        return not self.presets or preset in self.presets


@dataclasses.dataclass(frozen=True)
class Outcome:
    """What running one mutation established.

    Attributes
    ----------
    mutation : Mutation
        The mutation that was run.
    verdict : str
        ``killed``, ``survived``, or ``not a result``.
    detail : str
        Why, for the verdicts that need one.
    """

    mutation: Mutation
    verdict: str
    detail: str = ""


def _touch_forward(source: pathlib.Path, seconds: int = 2) -> None:
    """Give a file an mtime no build output can already be newer than.

    Parameters
    ----------
    source : pathlib.Path
        File just written.
    seconds : int, optional
        How far ahead to put it.

    Returns
    -------
    None
        The file's timestamps are updated in place.

    Examples
    --------
    >>> import tempfile
    >>> with tempfile.TemporaryDirectory() as directory:
    ...     path = pathlib.Path(directory) / "a"
    ...     _ = path.write_text("x")
    ...     was = path.stat().st_mtime
    ...     _touch_forward(path)
    ...     path.stat().st_mtime > was
    True
    """
    ahead = source.stat().st_mtime + seconds
    os.utime(source, (ahead, ahead))


@contextlib.contextmanager
def _mutated(source: pathlib.Path, find: str, replace: str) -> t.Iterator[bool]:
    r"""Apply one edit for the duration of the block, then put it back.

    Parameters
    ----------
    source : pathlib.Path
        File to edit.
    find : str
        Text to replace, which must appear exactly once.
    replace : str
        Replacement text.

    Yields
    ------
    bool
        Whether the edit was applied.

    Examples
    --------
    >>> import tempfile
    >>> with tempfile.TemporaryDirectory() as directory:
    ...     path = pathlib.Path(directory) / "a.cpp"
    ...     _ = path.write_text("int x = 1;\n")
    ...     with _mutated(path, "1", "2") as applied:
    ...         (applied, path.read_text().strip())
    ...     path.read_text().strip()
    (True, 'int x = 2;')
    'int x = 1;'
    """
    original = source.read_text(encoding="utf-8")
    if original.count(find) != 1:
        yield False
        return
    source.write_text(original.replace(find, replace, 1), encoding="utf-8")
    # Two mutations in one run can edit the same header, and a build system
    # that decides by timestamp will skip the second when the first build's
    # output is no older than the rewrite. Pushing the mtime clearly forward
    # makes the rebuild unambiguous rather than a race the report inherits.
    _touch_forward(source)
    try:
        yield True
    finally:
        source.write_text(original, encoding="utf-8")
        _touch_forward(source)


def _fingerprint(build_root: pathlib.Path, preset: str, target: str) -> str | None:
    """Return a digest of the executable a target builds, if it is there.

    Two mutations in one run can edit the same header and build the same
    target, and whether the second one is compiled at all then depends on
    file timestamps. A verdict read off a binary that does not hold the
    mutation is not a verdict, so the digest is compared across the build.

    Parameters
    ----------
    build_root : pathlib.Path
        Directory holding the build trees.
    preset : str
        Which tree to look in. Every preset builds the same target names, so
        searching all of them found whichever sorted first — a tree nothing
        in this run rebuilt, whose digest therefore never moved and made
        every mutation look like it had not reached the binary.
    target : str
        CMake target name, which is also the executable's file name.

    Returns
    -------
    str | None
        Hex digest, or None when no such executable exists yet.

    Examples
    --------
    >>> _fingerprint(pathlib.Path("."), "cxx-dev", "no_such_target") is None
    True
    """
    candidates: list[pathlib.Path] = []
    for executable in (target, f"{target}.exe"):
        candidates.extend(build_root.glob(f"build/{preset}/**/{executable}"))
    for candidate in sorted(candidates):
        if candidate.is_file():
            return hashlib.sha256(candidate.read_bytes()).hexdigest()
    return None


def run(
    mutation: Mutation,
    repository: pathlib.Path,
    preset: str,
    runner: t.Callable[[list[str]], subprocess.CompletedProcess[bytes]] | None = None,
    build_root: pathlib.Path | None = None,
) -> Outcome:
    """Apply one mutation, build, run its target, and put the file back.

    Parameters
    ----------
    mutation : Mutation
        What to break.
    repository : pathlib.Path
        Repository root.
    preset : str
        CMake preset to build with.
    runner : Callable | None, optional
        Subprocess runner, for tests that must not invoke a compiler.
    build_root : pathlib.Path | None, optional
        Where the presets live, defaulting to the repository itself.
        Every source path stays repository-relative, so the two cannot be
        confused for one another.

    Returns
    -------
    Outcome
        Verdict and, where it matters, why.

    Examples
    --------
    >>> mutation = Mutation("id", "no/such.cpp", "a", "b", "t", "nothing")
    >>> run(mutation, pathlib.Path("."), "cxx-dev").verdict
    'not a result'
    """
    where = build_root if build_root is not None else repository

    def dispatch(argv: list[str]) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(argv, capture_output=True, check=False, cwd=where)

    execute = runner or dispatch
    source = repository / mutation.path
    if not source.is_file():
        return Outcome(mutation, "not a result", f"no such file: {mutation.path}")
    # From a clean build, because the previous mutation left its own binary
    # in the tree: comparing against that would call a real change no change
    # whenever two runs mutate the same place. Ninja makes this a no-op when
    # the tree is already clean.
    restored = execute(
        ["cmake", "--build", "--preset", preset, "--target", mutation.target]
    )
    if restored.returncode != 0:
        return Outcome(mutation, "not a result", "the target does not build unmutated")
    executable = mutation.executable or mutation.target
    before = _fingerprint(where, preset, executable)
    if before is None:
        return Outcome(mutation, "not a result", "the target executable was not found")
    test_regex = mutation.test_regex or mutation.target.replace(
        "libtmux_", "libtmux."
    ).replace("_test", "")
    selected = execute(
        [
            "ctest",
            "--preset",
            preset,
            "--show-only=json-v1",
            "--tests-regex",
            test_regex,
        ]
    )
    if selected.returncode != 0:
        return Outcome(mutation, "not a result", "the test selector could not be read")
    try:
        selection = json.loads(selected.stdout.decode("utf-8-sig"))
    except (AttributeError, UnicodeDecodeError, json.JSONDecodeError):
        return Outcome(
            mutation, "not a result", "CTest returned an unreadable test list"
        )
    if not isinstance(selection, dict) or not selection.get("tests"):
        return Outcome(mutation, "not a result", "the test selector matched no tests")
    test_command = [
        "ctest",
        "--preset",
        preset,
        "--no-tests=error",
        "--tests-regex",
        test_regex,
    ]
    baseline = execute(test_command)
    if baseline.returncode != 0:
        return Outcome(mutation, "not a result", "the selected tests already fail")
    with _mutated(source, mutation.find, mutation.replace) as applied:
        if not applied:
            return Outcome(
                mutation, "not a result", "the text to replace is absent or repeated"
            )
        build = execute(
            [
                "cmake",
                "--build",
                "--preset",
                preset,
                "--target",
                mutation.target,
            ]
        )
        if build.returncode != 0:
            return Outcome(mutation, "not a result", "the mutation did not build")
        after = _fingerprint(where, preset, executable)
        if after is None or after == before:
            return Outcome(
                mutation,
                "not a result",
                "the mutation did not reach the binary that was tested",
            )
        tested = execute(test_command)
    if tested.returncode == 0:
        return Outcome(mutation, "survived", mutation.guards)
    restored_after_mutation = execute(
        ["cmake", "--build", "--preset", preset, "--target", mutation.target]
    )
    if restored_after_mutation.returncode != 0:
        return Outcome(mutation, "not a result", "the restored target did not build")
    restored_fingerprint = _fingerprint(where, preset, executable)
    if restored_fingerprint is None or restored_fingerprint == after:
        return Outcome(
            mutation,
            "not a result",
            "the restoration did not reach the binary that was retested",
        )
    if execute(test_command).returncode != 0:
        return Outcome(mutation, "not a result", "the selected tests did not recover")
    return Outcome(mutation, "killed")


def report(outcomes: t.Sequence[Outcome]) -> str:
    """Render one run, survivors and non-results first.

    Parameters
    ----------
    outcomes : Sequence[Outcome]
        What each mutation established.

    Returns
    -------
    str
        One line per mutation, then a count.

    Examples
    --------
    >>> mutation = Mutation("guard", "a.cpp", "x", "y", "t", "the guard")
    >>> print(report([Outcome(mutation, "killed")]))
    killed       guard
    <BLANKLINE>
    1 killed, 0 survived, 0 not a result
    """
    order = {"survived": 0, "not a result": 1, "killed": 2}
    lines = []
    for outcome in sorted(outcomes, key=lambda one: order[one.verdict]):
        detail = f"  ({outcome.detail})" if outcome.detail else ""
        lines.append(f"{outcome.verdict:<12} {outcome.mutation.mutation_id}{detail}")
    counts = dict.fromkeys(order, 0)
    for outcome in outcomes:
        counts[outcome.verdict] += 1
    lines.append("")
    lines.append(
        f"{counts['killed']} killed, {counts['survived']} survived, "
        f"{counts['not a result']} not a result"
    )
    return "\n".join(lines)


def failed(outcomes: t.Sequence[Outcome]) -> bool:
    """Return whether a run should be treated as a failure.

    A survivor means something is untested.  A non-result means the
    catalogue is stale, which is worse: it looks like a pass.

    Parameters
    ----------
    outcomes : Sequence[Outcome]
        What each mutation established.

    Returns
    -------
    bool
        True when anything other than a kill happened.

    Examples
    --------
    >>> mutation = Mutation("id", "a.cpp", "x", "y", "t", "a guard")
    >>> failed([Outcome(mutation, "killed")])
    False
    >>> failed([Outcome(mutation, "not a result", "did not build")])
    True
    """
    return any(outcome.verdict != "killed" for outcome in outcomes)
