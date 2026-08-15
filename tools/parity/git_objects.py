"""Small Git-plumbing wrappers used by the parity extractor."""

from __future__ import annotations

import dataclasses
import pathlib
import subprocess
import typing as t


@dataclasses.dataclass(frozen=True, slots=True)
class TreeEntry:
    """One entry returned by ``git ls-tree``.

    Attributes
    ----------
    mode : str
        Git file mode.
    kind : str
        Git object kind.
    object_id : str
        Git object ID.
    path : str
        Repository-relative path.
    """

    mode: str
    kind: str
    object_id: str
    path: str


def _git(repo: pathlib.Path, args: t.Sequence[str]) -> bytes:
    """Run a Git plumbing command and return its standard output.

    Parameters
    ----------
    repo : pathlib.Path
        Repository whose Git metadata is queried.
    args : Sequence[str]
        Arguments following the ``git`` executable.

    Returns
    -------
    bytes
        Captured standard output from Git.

    Raises
    ------
    subprocess.CalledProcessError
        Raised when Git rejects the command.

    Examples
    --------
    >>> _git(pathlib.Path.cwd(), ("rev-parse", "--verify", "HEAD"))[:7] != b""
    True
    """
    result = subprocess.run(
        ["git", *args],
        cwd=repo,
        check=True,
        capture_output=True,
    )
    return result.stdout


def rev_parse(repo: pathlib.Path, revision: str) -> str:
    """Resolve a revision to an object ID without checking it out.

    Parameters
    ----------
    repo : pathlib.Path
        Repository containing *revision*.
    revision : str
        Git revision expression to resolve.

    Returns
    -------
    str
        Full object ID for the resolved revision.

    Raises
    ------
    subprocess.CalledProcessError
        Raised when *revision* cannot be resolved.

    Examples
    --------
    >>> len(rev_parse(pathlib.Path.cwd(), "HEAD")) >= 40
    True
    """
    return _git(repo, ("rev-parse", "--verify", revision)).decode().strip()


def show(repo: pathlib.Path, revision: str, path: str) -> bytes:
    """Read a file from a revision without importing or checking it out.

    Parameters
    ----------
    repo : pathlib.Path
        Repository containing the requested object.
    revision : str
        Git revision expression.
    path : str
        Repository-relative file path.

    Returns
    -------
    bytes
        File contents stored in the requested Git revision.

    Raises
    ------
    subprocess.CalledProcessError
        Raised when the revision or path is unavailable.

    Examples
    --------
    >>> bool(show(pathlib.Path.cwd(), "HEAD", "pyproject.toml"))
    True
    """
    return _git(repo, ("show", f"{revision}:{path}"))


def diff(repo: pathlib.Path, revision: str, paths: t.Sequence[str]) -> str:
    """Return name-status differences from a revision for selected paths.

    Parameters
    ----------
    repo : pathlib.Path
        Repository whose working tree is compared.
    revision : str
        Git revision used as the comparison baseline.
    paths : Sequence[str]
        Repository-relative paths included in the comparison.

    Returns
    -------
    str
        Git name-status output for the selected paths.

    Raises
    ------
    subprocess.CalledProcessError
        Raised when Git cannot perform the comparison.

    Examples
    --------
    >>> isinstance(diff(pathlib.Path.cwd(), "HEAD", ("pyproject.toml",)), str)
    True
    """
    return _git(repo, ("diff", "--name-status", revision, "--", *paths)).decode()


def raw_diff(repo: pathlib.Path, revision: str, paths: t.Sequence[str]) -> str:
    """Return raw Git differences, including file modes, for selected paths.

    Parameters
    ----------
    repo : pathlib.Path
        Repository whose working tree is compared.
    revision : str
        Git revision used as the comparison baseline.
    paths : Sequence[str]
        Repository-relative paths included in the comparison.

    Returns
    -------
    str
        Raw Git diff output with rename detection disabled.

    Raises
    ------
    subprocess.CalledProcessError
        Raised when Git cannot perform the comparison.

    Examples
    --------
    >>> isinstance(raw_diff(pathlib.Path.cwd(), "HEAD", ("pyproject.toml",)), str)
    True
    """
    return _git(
        repo,
        ("diff", "--raw", "--no-renames", revision, "--", *paths),
    ).decode()


def ls_tree(
    repo: pathlib.Path,
    revision: str,
    paths: t.Sequence[str],
    *,
    recursive: bool = False,
) -> tuple[TreeEntry, ...]:
    """List selected Git tree entries without materializing a revision.

    Parameters
    ----------
    repo : pathlib.Path
        Repository containing the requested tree.
    revision : str
        Git revision expression.
    paths : Sequence[str]
        Repository-relative paths to list.
    recursive : bool, default=False
        Whether to descend into selected subtrees.

    Returns
    -------
    tuple[TreeEntry, ...]
        Entries in Git's stable tree order.

    Raises
    ------
    subprocess.CalledProcessError
        Raised when Git cannot list the requested tree.

    Examples
    --------
    >>> bool(ls_tree(pathlib.Path.cwd(), "HEAD", ("pyproject.toml",)))
    True
    """
    recursive_args = ("-r",) if recursive else ()
    output = _git(repo, ("ls-tree", *recursive_args, revision, "--", *paths))
    entries: list[TreeEntry] = []
    for line in output.decode().splitlines():
        metadata, path = line.split("\t", maxsplit=1)
        mode, kind, object_id = metadata.split()
        entries.append(TreeEntry(mode, kind, object_id, path))
    return tuple(entries)


def untracked_paths(repo: pathlib.Path, paths: t.Sequence[str]) -> tuple[str, ...]:
    """Return untracked paths under a selected boundary.

    Parameters
    ----------
    repo : pathlib.Path
        Repository whose working tree is inspected.
    paths : Sequence[str]
        Repository-relative paths that limit the search.

    Returns
    -------
    tuple[str, ...]
        Untracked repository-relative paths in Git order.

    Raises
    ------
    subprocess.CalledProcessError
        Raised when Git cannot list untracked paths.

    Examples
    --------
    >>> isinstance(untracked_paths(pathlib.Path.cwd(), ("cxx",)), tuple)
    True
    """
    output = _git(repo, ("ls-files", "--others", "--exclude-standard", "--", *paths))
    return tuple(path for path in output.decode().splitlines() if path)


def is_submodule(repo: pathlib.Path, path: str) -> bool:
    """Return whether a working-tree directory belongs to a superproject.

    Parameters
    ----------
    repo : pathlib.Path
        Superproject repository path.
    path : str
        Repository-relative directory to inspect.

    Returns
    -------
    bool
        ``True`` when the directory is a checked-out submodule.

    Raises
    ------
    subprocess.CalledProcessError
        Raised when *path* cannot be inspected by Git.

    Examples
    --------
    >>> is_submodule(pathlib.Path.cwd(), ".")
    False
    """
    result = subprocess.run(
        [
            "git",
            "-C",
            str(repo / path),
            "rev-parse",
            "--show-superproject-working-tree",
        ],
        cwd=repo,
        check=True,
        capture_output=True,
    )
    return bool(result.stdout.strip())
