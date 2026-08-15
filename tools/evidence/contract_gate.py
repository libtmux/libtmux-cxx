"""Run the aggregate Task 6 contract evidence gate."""

from __future__ import annotations

import argparse
import contextlib
import copy
import ctypes
import fcntl
import hashlib
import itertools
import json
import os
import pathlib
import re
import secrets
import shutil
import stat
import subprocess
import sys
import typing as t

_DIGEST = re.compile(r"^sha256:[0-9a-f]{64}$")
_OBJECT_ID = re.compile(r"^[0-9a-f]{40}$")
_DRIVE_PATH = re.compile(r"(?:^|[^A-Za-z0-9_.-])[A-Za-z]:[\\/]")
_UNIX_PATH = re.compile(r"(?:^|[^A-Za-z0-9_.-])/(?:[^\x00\s]+)")
_EMAIL = re.compile(r"\b[^\s@]+@[^\s@]+\.[^\s@]+\b")
_FIXTURE_BINDING = {
    "name": "scoped_tmux_server_ScopedTmuxServer.StartsByNameAndExposesResolvedPath",
    "path": "scoped_tmux_server_ScopedTmuxServer.StartsByExactPath",
}

_CTEST_GATES = (
    ("cxx-dev", "--match", "^.*$", "contract-dev"),
    ("cxx-sanitize", "--label", "real-tmux", "contract-real-tmux"),
    (
        "cxx-tsan",
        "--label",
        "real-tmux",
        "contract-real-tmux-tsan",
    ),
)
_INSTRUCTION_LINKS = {
    "CLAUDE.md": "AGENTS.md",
    "docs/CLAUDE.md": "AGENTS.md",
}
_PRODUCTION_PATHS = ("include", "src", "spikes")
_CORE_EXCLUDED_FIELDS = [
    "ctest_gates.*.gate_sha256",
    "ctest_gates.*.tmux.sha256",
    "evidence_core_sha256",
    "final_evidence_sha256",
    "gate_source.source_snapshot",
    "review",
    "toolchain.artifacts",
]
_REQUIRED_TOOLS = {"clang", "cmake", "just", "ninja", "python", "uv"}
_COMMAND_NAMES = (
    "tool.just",
    "tool.uv",
    "tool.python",
    "tool.cmake",
    "tool.ninja",
    "tool.tmux",
    "toolchain.ctest",
    "configure.cxx-dev",
    "toolchain.ninja.cxx-dev",
    "tool.clang",
    "build.cxx-dev",
    "toolchain.libcxx_config",
    "toolchain.libcxx",
    "toolchain.libcxxabi",
    "ctest.contract-dev",
    "configure.cxx-sanitize",
    "toolchain.ninja.cxx-sanitize",
    "build.cxx-sanitize",
    "ctest.contract-real-tmux",
    "configure.cxx-tsan",
    "toolchain.ninja.cxx-tsan",
    "build.cxx-tsan",
    "ctest.contract-real-tmux-tsan",
    "parity.generate",
    "parity.drift",
    "parity.verify",
    "ruff.format",
    "pytest.initial",
    "ruff.check",
    "mypy",
    "doctest",
    "docs",
    "pytest.final",
)
_VERSIONED_COMMANDS = {
    "tool.just",
    "tool.uv",
    "tool.python",
    "tool.cmake",
    "tool.ninja",
    "tool.tmux",
    "toolchain.ctest",
    "toolchain.ninja.cxx-dev",
    "tool.clang",
    "toolchain.ninja.cxx-sanitize",
    "toolchain.ninja.cxx-tsan",
}
_OUTPUT_PATH = pathlib.Path("cxx/docs/evidence/contract-and-harness.json")
_PROVED_CLAIMS = [
    "current Linux harness",
    "pinned parity-source reproducibility",
    "test-only differential framework",
    "both fixture modes on one selected tmux binary",
    "ASan/UBSan and TSan fixture behavior",
    "source immutability",
    "absence of production C++ code",
]
_UNPROVED_CLAIMS = [
    "complete parity",
    "live Python-versus-C++ comparison",
    "a selected transport/query design",
    "production API behavior",
    "tmux version-matrix support",
    "package ABI",
    "relocation",
    "final reproducibility",
]


class GateError(ValueError):
    """Raised when aggregate evidence is invalid or cannot be published."""


def _abort(detail: str) -> t.NoReturn:
    """Raise a stable aggregate-gate error.

    Examples
    --------
    >>> try:
    ...     _abort("example")
    ... except GateError as error:
    ...     str(error)
    'example'
    """
    raise GateError(detail)


def build_parser() -> argparse.ArgumentParser:
    """Build the aggregate evidence command-line parser.

    Examples
    --------
    >>> build_parser().parse_args(["--output", "evidence.json"]).review is None
    True
    """
    parser = argparse.ArgumentParser(prog="python -m cxx.tools.evidence.contract_gate")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--review", type=pathlib.Path)
    return parser


def _review_path(
    output: pathlib.Path,
    supplied: pathlib.Path | None,
) -> pathlib.Path:
    """Return the only review path that a candidate can exclude in advance.

    Examples
    --------
    >>> _review_path(pathlib.Path("evidence.json").absolute(), None).name
    'evidence-review.md'
    """
    expected = output.with_name(f"{output.stem}-review.md")
    if supplied is not None and supplied.absolute() != expected:
        _abort("review path must match output-derived sibling")
    return expected


def _canonical_bytes(value: object) -> bytes:
    r"""Serialize one public evidence value deterministically.

    Examples
    --------
    >>> _canonical_bytes({"b": 2, "a": 1})
    b'{\n  "a": 1,\n  "b": 2\n}\n'
    """
    try:
        serialized = json.dumps(
            value,
            indent=2,
            sort_keys=True,
            ensure_ascii=False,
            allow_nan=False,
        )
    except ValueError:
        _abort("nonfinite evidence value")
    return (serialized + "\n").encode()


def _sha256(contents: bytes) -> str:
    """Return the project digest spelling for bytes.

    Examples
    --------
    >>> _sha256(b"")[:7]
    'sha256:'
    """
    return f"sha256:{hashlib.sha256(contents).hexdigest()}"


def _git_environment() -> dict[str, str]:
    """Return a Git child environment without inherited repository routing.

    Examples
    --------
    >>> _git_environment()["GIT_CONFIG_NOSYSTEM"]
    '1'
    """
    environment = {
        key: value for key, value in os.environ.items() if not key.startswith("GIT_")
    }
    environment.update(
        {
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_SYSTEM": os.devnull,
        }
    )
    return environment


def _git_bytes(
    root: pathlib.Path,
    argv: list[str],
    label: str,
    *,
    executable: pathlib.Path | None = None,
    environment: dict[str, str] | None = None,
) -> bytes:
    """Return bytes from one Git query that is required for source evidence.

    Examples
    --------
    >>> root = pathlib.Path(__file__).parents[3]
    >>> _git_bytes(root, ["rev-parse", "--is-inside-work-tree"], "worktree").strip()
    b'true'
    """
    command = "git" if executable is None else os.fspath(executable)
    child_environment = _git_environment() if environment is None else environment
    result = subprocess.run(
        [command, *argv],
        cwd=root,
        env=child_environment,
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        _abort(f"Git {label} failed")
    return result.stdout


def _safe_source_path(raw: bytes, label: str) -> str:
    """Decode one NUL-delimited Git path without accepting path traversal.

    Examples
    --------
    >>> _safe_source_path(b"cxx/input.txt", "example")
    'cxx/input.txt'
    """
    try:
        value = raw.decode("utf-8")
    except UnicodeDecodeError:
        _abort(f"{label} is not UTF-8")
    path = pathlib.PurePosixPath(value)
    if (
        not value
        or path.is_absolute()
        or "\\" in value
        or any(part in {"", ".", ".."} for part in path.parts)
        or _DRIVE_PATH.search(value)
    ):
        _abort(f"unsafe {label}")
    return value


def _source_snapshot(
    root: pathlib.Path,
    *,
    excluded: pathlib.Path,
    additionally_excluded: tuple[pathlib.Path, ...] = (),
    git: pathlib.Path | None = None,
    git_environment: dict[str, str] | None = None,
) -> dict[str, object]:
    """Hash the index inventory and nonignored untracked source bytes.

    Only the two tracked instruction links may be represented as symlinks.

    Examples
    --------
    >>> import tempfile
    >>> with tempfile.TemporaryDirectory() as raw:
    ...     root = pathlib.Path(raw)
    ...     (root / "cxx").mkdir()
    ...     source = root / "input.txt"
    ...     _ = source.write_bytes(b"source")
    ...     _ = subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    ...     _ = subprocess.run(["git", "add", "input.txt"], cwd=root, check=True)
    ...     snapshot = _source_snapshot(root, excluded=root / "evidence.json")
    >>> t.cast(str, snapshot["sha256"]).startswith("sha256:")
    True
    """
    root = root.absolute()
    child_environment = (
        _git_environment() if git_environment is None else git_environment
    )
    excluded_paths = (excluded, *additionally_excluded)
    excluded_names: set[str] = set()
    for excluded_path in excluded_paths:
        absolute = excluded_path.absolute()
        if not absolute.is_relative_to(root):
            _abort("source exclusion escapes repository")
        excluded_names.add(absolute.relative_to(root).as_posix())
    _verify_no_production_paths(root)
    tracked: list[tuple[str, str]] = []
    tracked_inventory = _git_bytes(
        root,
        ["ls-files", "-s", "-z"],
        "index",
        executable=git,
        environment=child_environment,
    )
    for item in tracked_inventory.split(b"\0"):
        if not item:
            continue
        index_metadata, separator, raw_name = item.partition(b"\t")
        fields = index_metadata.split()
        if separator != b"\t" or len(fields) != 3 or fields[2] != b"0":
            _abort("invalid cached source inventory")
        try:
            mode = fields[0].decode("ascii")
        except UnicodeDecodeError:
            _abort("invalid cached source inventory")
        if mode not in {"100644", "100755", "120000"}:
            _abort("invalid cached source mode")
        name = _safe_source_path(raw_name, "cached source path")
        if name not in excluded_names:
            tracked.append((name, mode))
    tracked_names = {name for name, _ in tracked}
    entries: list[dict[str, str]] = []
    for name, mode in tracked:
        path = root / name
        if mode == "120000":
            if name not in _INSTRUCTION_LINKS:
                _abort("unexpected source symlink")
            target = _read_source_symlink(path, "cached source")
            if target != _INSTRUCTION_LINKS[name]:
                _abort("unexpected instruction symlink target")
            target_path = pathlib.PurePosixPath(target)
            if target_path.is_absolute() or any(
                part in {"", ".", ".."} for part in target_path.parts
            ):
                _abort("unsafe instruction symlink target")
            contents = os.fsencode(target)
        else:
            _reject_source_symlinked_ancestors(root, path)
            try:
                metadata = path.lstat()
            except OSError:
                _abort("missing cached source path")
            if not stat.S_ISREG(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
                _abort("cached source is not regular")
            actual_mode = "100755" if metadata.st_mode & 0o111 else "100644"
            if actual_mode != mode:
                _abort("cached source mode differs")
            contents = _read_source_regular(path, "cached source")
        entries.append(
            {
                "mode": mode,
                "path": name,
                "sha256": _sha256(contents),
                "source": "tracked",
            }
        )
    untracked_inventory = _git_bytes(
        root,
        ["ls-files", "--others", "--exclude-standard", "-z"],
        "untracked",
        executable=git,
        environment=child_environment,
    )
    for raw_name in untracked_inventory.split(b"\0"):
        if not raw_name:
            continue
        name = _safe_source_path(raw_name, "untracked source path")
        if name in excluded_names:
            continue
        if name in tracked_names:
            _abort("source inventory overlaps tracked path")
        path = root / name
        _reject_source_symlinked_ancestors(root, path)
        try:
            metadata = path.lstat()
        except OSError:
            _abort("missing untracked source path")
        if not stat.S_ISREG(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
            _abort("untracked source is not regular")
        entries.append(
            {
                "mode": "100755" if metadata.st_mode & 0o111 else "100644",
                "path": name,
                "sha256": _sha256(_read_source_regular(path, "untracked source")),
                "source": "untracked",
            }
        )
    if (
        _git_bytes(
            root,
            ["ls-files", "-s", "-z"],
            "index",
            executable=git,
            environment=child_environment,
        )
        != tracked_inventory
        or _git_bytes(
            root,
            ["ls-files", "--others", "--exclude-standard", "-z"],
            "untracked",
            executable=git,
            environment=child_environment,
        )
        != untracked_inventory
    ):
        _abort("source inventory changed during capture")
    _verify_no_production_paths(root)
    entries.sort(key=lambda entry: entry["path"])
    projection = {"entries": entries}
    return {"sha256": _sha256(_canonical_bytes(projection)), **projection}


def _verify_no_production_paths(root: pathlib.Path) -> None:
    """Reject filesystem entries at every reserved production C++ path.

    Examples
    --------
    >>> import tempfile
    >>> with tempfile.TemporaryDirectory() as raw:
    ...     root = pathlib.Path(raw)
    ...     (root / "cxx").mkdir()
    ...     _verify_no_production_paths(root)
    """
    changed = "production C++ path changed while checking"
    with _anchored_parent(
        root / "cxx" / _PRODUCTION_PATHS[0],
        changed="production C++ path has a symlinked ancestor",
    ) as binding:
        _verify_directory_binding(binding, changed)
        for leaf in _PRODUCTION_PATHS:
            try:
                os.stat(
                    leaf,
                    dir_fd=binding.parent_fd,
                    follow_symlinks=False,
                )
            except FileNotFoundError:
                continue
            except OSError:
                _abort(changed)
            _abort("production C++ path exists")
        _verify_directory_binding(binding, changed)


def _read_source_regular(path: pathlib.Path, label: str) -> bytes:
    """Read one source file while binding its pathname and opened inode.

    Examples
    --------
    >>> b"aggregate Task 6" in _read_source_regular(pathlib.Path(__file__), "module")
    True
    """
    return _read_anchored_regular(
        path,
        missing=f"missing {label}",
        not_regular=f"{label} is not regular",
        ancestor_changed=f"{label} path has a symlinked ancestor",
        identity_changed="source changed while reading",
        bytes_changed="source bytes changed during reading",
        require_single_link=False,
    )


def _read_source_symlink(path: pathlib.Path, label: str) -> str:
    """Read one permitted source symlink through a retained parent descriptor.

    Examples
    --------
    >>> root = pathlib.Path(__file__).parents[3]
    >>> _read_source_symlink(root / "CLAUDE.md", "instruction link")
    'AGENTS.md'
    """
    changed = f"{label} symlink changed while reading"
    with _anchored_parent(
        path,
        changed=f"{label} path has a symlinked ancestor",
    ) as binding:
        try:
            before = os.stat(
                binding.leaf,
                dir_fd=binding.parent_fd,
                follow_symlinks=False,
            )
            target = os.readlink(binding.leaf, dir_fd=binding.parent_fd)
            after = os.stat(
                binding.leaf,
                dir_fd=binding.parent_fd,
                follow_symlinks=False,
            )
        except OSError:
            _abort(f"missing {label}")
        if not stat.S_ISLNK(before.st_mode) or not os.path.samestat(before, after):
            _abort(changed)
        _verify_directory_binding(binding, changed)
        try:
            confirmation = os.readlink(binding.leaf, dir_fd=binding.parent_fd)
            final = os.stat(
                binding.leaf,
                dir_fd=binding.parent_fd,
                follow_symlinks=False,
            )
        except OSError:
            _abort(changed)
        _verify_directory_binding(binding, changed)
        if not os.path.samestat(after, final) or confirmation != target:
            _abort(changed)
        return target


class _DirectoryBinding(t.NamedTuple):
    """Retained directory descriptors for one absolute leaf path.

    Attributes
    ----------
    leaf : str
        Leaf name opened relative to ``parent_fd``.
    parent_fd : int
        Retained descriptor for the immediate parent directory.
    components : tuple[str, ...]
        Absolute directory components below the filesystem root.
    metadata : tuple[os.stat_result, ...]
        Inode identities for the root and every retained component.
    descriptors : tuple[int, ...]
        Open descriptors retained for the binding lifetime.
    """

    leaf: str
    parent_fd: int
    components: tuple[str, ...]
    metadata: tuple[os.stat_result, ...]
    descriptors: tuple[int, ...]


class _SymlinkBinding(t.NamedTuple):
    """Retained identity for one permitted leaf symlink.

    Attributes
    ----------
    directory : _DirectoryBinding
        Anchored parent containing the symlink.
    metadata : os.stat_result
        Symlink inode identity captured without following it.
    target : str
        Exact link payload used to select the next path.
    """

    directory: _DirectoryBinding
    metadata: os.stat_result
    target: str


class _BoundRegular(t.NamedTuple):
    """One captured regular file and every pathname identity leading to it.

    Attributes
    ----------
    path : pathlib.Path
        Lexically resolved final regular path.
    handle : typing.BinaryIO
        Retained open handle for the regular file.
    metadata : os.stat_result
        Opened regular-file identity.
    contents : bytes
        Bytes captured from the retained handle.
    directory : _DirectoryBinding
        Anchored parent containing the final file.
    symlinks : tuple[_SymlinkBinding, ...]
        Permitted leaf symlinks traversed before the final file.
    """

    path: pathlib.Path
    handle: t.BinaryIO
    metadata: os.stat_result
    contents: bytes
    directory: _DirectoryBinding
    symlinks: tuple[_SymlinkBinding, ...]


class _BoundDirectory(t.NamedTuple):
    """One retained directory leaf and its anchored parent pathname.

    Attributes
    ----------
    path : pathlib.Path
        Absolute directory path.
    descriptor : int
        Retained directory descriptor.
    metadata : os.stat_result
        Opened directory identity.
    parent : _DirectoryBinding
        Anchored parent containing this directory.
    """

    path: pathlib.Path
    descriptor: int
    metadata: os.stat_result
    parent: _DirectoryBinding


class _RegularCheckpoint(t.NamedTuple):
    """One retained regular-file identity required by aggregate checkpoints.

    Attributes
    ----------
    bound : _BoundRegular
        Retained file, path chain, and captured bytes.
    changed : str
        Stable failure detail when the identity no longer matches.
    require_executable : bool
        Whether the retained file must remain executable.
    require_single_link : bool
        Whether the final regular inode must keep one hard link.
    """

    bound: _BoundRegular
    changed: str
    require_executable: bool
    require_single_link: bool


class _CTestGateFiles(t.NamedTuple):
    """Captured named and immutable-leaf bytes for one CTest projection.

    Attributes
    ----------
    gate_id : str
        Stable logical CTest gate identifier.
    digest : str
        Content-addressed gate digest selecting the immutable leaf.
    files : tuple[bytes, bytes, bytes, bytes]
        Named record, leaf record, registry, and JUnit bytes.
    """

    gate_id: str
    digest: str
    files: tuple[bytes, bytes, bytes, bytes]


def _open_directory_chain(
    components: tuple[str, ...],
) -> tuple[tuple[int, ...], tuple[os.stat_result, ...]]:
    """Open one absolute directory chain without following symlinks.

    Examples
    --------
    >>> descriptors, metadata = _open_directory_chain(())
    >>> len(descriptors) == len(metadata) == 1
    True
    >>> os.close(descriptors[0])
    """
    flags = os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0)
    descriptors: list[int] = []
    metadata: list[os.stat_result] = []
    try:
        descriptors.append(os.open(os.path.sep, flags))
        metadata.append(os.fstat(descriptors[-1]))
        for component in components:
            descriptors.append(os.open(component, flags, dir_fd=descriptors[-1]))
            metadata.append(os.fstat(descriptors[-1]))
    except OSError:
        for descriptor in reversed(descriptors):
            os.close(descriptor)
        raise
    return tuple(descriptors), tuple(metadata)


@contextlib.contextmanager
def _anchored_parent(
    path: pathlib.Path,
    *,
    changed: str,
) -> t.Iterator[_DirectoryBinding]:
    """Retain an absolute, nonsymlinked parent chain for one leaf.

    Examples
    --------
    >>> with _anchored_parent(pathlib.Path(__file__), changed="changed") as binding:
    ...     bool(binding.leaf) and binding.parent_fd >= 0
    True
    """
    absolute = path.absolute()
    parts = absolute.parts
    if (
        not absolute.is_absolute()
        or len(parts) < 2
        or parts[0] != os.path.sep
        or any(component in {"", ".", ".."} for component in parts[1:])
    ):
        _abort(changed)
    components = parts[1:-1]
    try:
        descriptors, metadata = _open_directory_chain(components)
    except OSError:
        _abort(changed)
    binding = _DirectoryBinding(
        leaf=parts[-1],
        parent_fd=descriptors[-1],
        components=components,
        metadata=metadata,
        descriptors=descriptors,
    )
    try:
        yield binding
    finally:
        for descriptor in reversed(descriptors):
            os.close(descriptor)


def _verify_directory_binding(binding: _DirectoryBinding, changed: str) -> None:
    """Verify that every retained directory still owns its absolute pathname.

    Examples
    --------
    >>> with _anchored_parent(pathlib.Path(__file__), changed="changed") as binding:
    ...     _verify_directory_binding(binding, "changed")
    """
    try:
        descriptors, metadata = _open_directory_chain(binding.components)
    except OSError:
        _abort(changed)
    try:
        if len(metadata) != len(binding.metadata) or any(
            not os.path.samestat(before, after)
            for before, after in zip(binding.metadata, metadata, strict=True)
        ):
            _abort(changed)
    finally:
        for descriptor in reversed(descriptors):
            os.close(descriptor)


def _verify_bound_directory(bound: _BoundDirectory, changed: str) -> None:
    """Require a retained directory to remain at the same absolute pathname.

    Examples
    --------
    >>> with _bound_directory(pathlib.Path(__file__).parent, label="module") as bound:
    ...     _verify_bound_directory(bound, "changed")
    """
    _verify_directory_binding(bound.parent, changed)
    try:
        current = os.stat(
            bound.parent.leaf,
            dir_fd=bound.parent.parent_fd,
            follow_symlinks=False,
        )
        opened = os.fstat(bound.descriptor)
    except OSError:
        _abort(changed)
    if (
        not stat.S_ISDIR(current.st_mode)
        or not os.path.samestat(bound.metadata, current)
        or not os.path.samestat(bound.metadata, opened)
    ):
        _abort(changed)


@contextlib.contextmanager
def _bound_directory(path: pathlib.Path, *, label: str) -> t.Iterator[_BoundDirectory]:
    """Open and retain one nonsymlinked directory at an anchored pathname.

    Examples
    --------
    >>> with _bound_directory(pathlib.Path(__file__).parent, label="module") as bound:
    ...     bound.descriptor >= 0
    True
    """
    missing = f"missing {label}"
    changed = f"{label} changed while reading"
    with _anchored_parent(
        path,
        changed=f"{label} path has a symlinked ancestor",
    ) as parent:
        try:
            before = os.stat(
                parent.leaf,
                dir_fd=parent.parent_fd,
                follow_symlinks=False,
            )
            descriptor = os.open(
                parent.leaf,
                os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0),
                dir_fd=parent.parent_fd,
            )
        except OSError:
            _abort(missing)
        try:
            opened = os.fstat(descriptor)
            if not stat.S_ISDIR(before.st_mode) or not os.path.samestat(before, opened):
                _abort(changed)
            bound = _BoundDirectory(path.absolute(), descriptor, opened, parent)
            _verify_bound_directory(bound, changed)
            try:
                yield bound
            finally:
                _verify_bound_directory(bound, changed)
        finally:
            os.close(descriptor)


def _ensure_directory(path: pathlib.Path, *, label: str) -> None:
    """Create one final directory component through an anchored parent.

    Examples
    --------
    >>> _ensure_directory(pathlib.Path(__file__).parent, label="module directory")
    """
    with _anchored_parent(
        path,
        changed=f"{label} path has a symlinked ancestor",
    ) as parent:
        try:
            metadata = os.stat(
                parent.leaf,
                dir_fd=parent.parent_fd,
                follow_symlinks=False,
            )
        except FileNotFoundError:
            try:
                os.mkdir(parent.leaf, mode=0o755, dir_fd=parent.parent_fd)
                metadata = os.stat(
                    parent.leaf,
                    dir_fd=parent.parent_fd,
                    follow_symlinks=False,
                )
            except OSError:
                _abort(f"missing {label}")
        except OSError:
            _abort(f"missing {label}")
        if not stat.S_ISDIR(metadata.st_mode):
            _abort(f"{label} is not a directory")
        _verify_directory_binding(parent, f"{label} changed while creating")
    with _bound_directory(path, label=label):
        pass


def _symlink_target_path(
    path: pathlib.Path,
    target: str,
    *,
    changed: str,
) -> pathlib.Path:
    """Return one lexical absolute symlink target without resolving another link.

    Examples
    --------
    >>> _symlink_target_path(pathlib.Path("/a/link"), "../b", changed="bad")
    PosixPath('/b')
    """
    if not target or "\x00" in target:
        _abort(changed)
    candidate = pathlib.Path(target)
    if not candidate.is_absolute():
        candidate = path.parent / candidate
    normalized = pathlib.Path(os.path.normpath(candidate))
    if not normalized.is_absolute():
        _abort(changed)
    return normalized


def _verify_bound_regular(
    bound: _BoundRegular,
    *,
    changed: str,
    require_executable: bool,
    require_single_link: bool,
) -> None:
    """Revalidate a captured regular file, its symlinks, and directory paths.

    Examples
    --------
    >>> with _bound_regular(
    ...     pathlib.Path(__file__), label="module", allow_leaf_symlinks=False
    ... ) as bound:
    ...     _verify_bound_regular(
    ...         bound,
    ...         changed="changed",
    ...         require_executable=False,
    ...         require_single_link=False,
    ...     )
    """
    for link in bound.symlinks:
        _verify_directory_binding(link.directory, changed)
        try:
            current = os.stat(
                link.directory.leaf,
                dir_fd=link.directory.parent_fd,
                follow_symlinks=False,
            )
            target = os.readlink(
                link.directory.leaf,
                dir_fd=link.directory.parent_fd,
            )
        except OSError:
            _abort(changed)
        if (
            not stat.S_ISLNK(current.st_mode)
            or not os.path.samestat(link.metadata, current)
            or target != link.target
        ):
            _abort(changed)
    _verify_directory_binding(bound.directory, changed)
    try:
        current = os.stat(
            bound.directory.leaf,
            dir_fd=bound.directory.parent_fd,
            follow_symlinks=False,
        )
        descriptor_state = os.fstat(bound.handle.fileno())
        bound.handle.seek(0)
        confirmation = bound.handle.read()
    except OSError:
        _abort(changed)
    if (
        not stat.S_ISREG(current.st_mode)
        or not os.path.samestat(bound.metadata, current)
        or not os.path.samestat(bound.metadata, descriptor_state)
        or (require_executable and current.st_mode & 0o111 == 0)
        or (require_single_link and current.st_nlink != 1)
        or confirmation != bound.contents
    ):
        _abort(changed)


def _verify_regular_checkpoints(
    checkpoints: t.Iterable[_RegularCheckpoint],
) -> None:
    """Revalidate every retained external regular-file identity.

    Examples
    --------
    >>> _verify_regular_checkpoints(())
    """
    for checkpoint in checkpoints:
        _verify_bound_regular(
            checkpoint.bound,
            changed=checkpoint.changed,
            require_executable=checkpoint.require_executable,
            require_single_link=checkpoint.require_single_link,
        )


@contextlib.contextmanager
def _bound_regular(
    path: pathlib.Path,
    *,
    label: str,
    allow_leaf_symlinks: bool,
    require_executable: bool = False,
    require_single_link: bool = False,
) -> t.Iterator[_BoundRegular]:
    """Capture one regular file through retained nonsymlinked directories.

    Examples
    --------
    >>> with _bound_regular(
    ...     pathlib.Path(__file__),
    ...     label="module",
    ...     allow_leaf_symlinks=False,
    ... ) as bound:
    ...     bool(bound.contents)
    True
    """
    missing = f"missing {label}"
    not_regular = f"{label} is not regular"
    changed = f"{label} changed while reading"
    symlinks: list[_SymlinkBinding] = []
    seen: set[pathlib.Path] = set()
    current = path.absolute()
    with contextlib.ExitStack() as stack:
        while True:
            if current in seen or len(seen) >= 40:
                _abort(changed)
            seen.add(current)
            directory = stack.enter_context(
                _anchored_parent(
                    current,
                    changed=f"{label} path has a symlinked ancestor",
                )
            )
            try:
                metadata = os.stat(
                    directory.leaf,
                    dir_fd=directory.parent_fd,
                    follow_symlinks=False,
                )
            except OSError:
                _abort(missing)
            if stat.S_ISLNK(metadata.st_mode):
                if not allow_leaf_symlinks:
                    _abort(not_regular)
                try:
                    target = os.readlink(
                        directory.leaf,
                        dir_fd=directory.parent_fd,
                    )
                except OSError:
                    _abort(changed)
                symlinks.append(_SymlinkBinding(directory, metadata, target))
                current = _symlink_target_path(current, target, changed=changed)
                continue
            if not stat.S_ISREG(metadata.st_mode):
                _abort(not_regular)
            if require_executable and metadata.st_mode & 0o111 == 0:
                _abort(f"{label} is not executable")
            if require_single_link and metadata.st_nlink != 1:
                _abort(not_regular)
            try:
                descriptor = os.open(
                    directory.leaf,
                    os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0),
                    dir_fd=directory.parent_fd,
                )
            except OSError:
                _abort(missing)
            handle = stack.enter_context(os.fdopen(descriptor, "rb"))
            opened = os.fstat(handle.fileno())
            if not os.path.samestat(metadata, opened):
                _abort(changed)
            contents = handle.read()
            bound = _BoundRegular(
                path=current,
                handle=handle,
                metadata=opened,
                contents=contents,
                directory=directory,
                symlinks=tuple(symlinks),
            )
            _verify_bound_regular(
                bound,
                changed=changed,
                require_executable=require_executable,
                require_single_link=require_single_link,
            )
            try:
                yield bound
            finally:
                _verify_bound_regular(
                    bound,
                    changed=changed,
                    require_executable=require_executable,
                    require_single_link=require_single_link,
                )
            return


def _first_symlink(path: pathlib.Path, *, label: str) -> pathlib.Path | None:
    """Find the first symlink component through no-follow directory descriptors.

    Examples
    --------
    >>> _first_symlink(pathlib.Path(__file__), label="module") is None
    True
    """
    absolute = path.absolute()
    components = absolute.parts[1:]
    flags = os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0)
    descriptors: list[int] = []
    prefix = pathlib.Path(os.path.sep)
    try:
        descriptors.append(os.open(os.path.sep, flags))
        for index, component in enumerate(components):
            try:
                metadata = os.stat(
                    component,
                    dir_fd=descriptors[-1],
                    follow_symlinks=False,
                )
            except OSError:
                _abort(f"missing {label}")
            candidate = prefix / component
            if stat.S_ISLNK(metadata.st_mode):
                return candidate
            if index != len(components) - 1:
                if not stat.S_ISDIR(metadata.st_mode):
                    _abort(f"{label} path has a non-directory ancestor")
                try:
                    descriptors.append(
                        os.open(component, flags, dir_fd=descriptors[-1])
                    )
                except OSError:
                    _abort(f"{label} path changed while resolving")
                prefix = candidate
        return None
    finally:
        for descriptor in reversed(descriptors):
            os.close(descriptor)


@contextlib.contextmanager
def _bound_installed_regular(
    path: pathlib.Path,
    *,
    label: str,
) -> t.Iterator[_BoundRegular]:
    """Bind an installed file while permitting verified system symlink aliases.

    Examples
    --------
    >>> with _bound_installed_regular(pathlib.Path(__file__), label="module") as bound:
    ...     b"aggregate Task 6" in bound.contents
    True
    """
    current = pathlib.Path(os.path.normpath(path))
    if not current.is_absolute():
        _abort(f"invalid {label} path")
    seen: set[pathlib.Path] = set()
    links: list[_SymlinkBinding] = []
    with contextlib.ExitStack() as stack:
        while True:
            if current in seen or len(seen) >= 40:
                _abort(f"{label} changed while reading")
            seen.add(current)
            link_path = _first_symlink(current, label=label)
            if link_path is None:
                break
            directory = stack.enter_context(
                _anchored_parent(
                    link_path,
                    changed=f"{label} path changed while resolving",
                )
            )
            try:
                metadata = os.stat(
                    directory.leaf,
                    dir_fd=directory.parent_fd,
                    follow_symlinks=False,
                )
                target = os.readlink(
                    directory.leaf,
                    dir_fd=directory.parent_fd,
                )
            except OSError:
                _abort(f"{label} changed while resolving")
            if not stat.S_ISLNK(metadata.st_mode):
                _abort(f"{label} changed while resolving")
            links.append(_SymlinkBinding(directory, metadata, target))
            relative = current.relative_to(link_path.parent)
            remaining = relative.parts[1:]
            target_path = _symlink_target_path(
                link_path,
                target,
                changed=f"{label} changed while resolving",
            )
            current = pathlib.Path(os.path.normpath(target_path.joinpath(*remaining)))
        with _bound_regular(
            current,
            label=label,
            allow_leaf_symlinks=False,
        ) as final:
            bound = final._replace(symlinks=tuple(links))
            _verify_bound_regular(
                bound,
                changed=f"{label} changed while reading",
                require_executable=False,
                require_single_link=False,
            )
            try:
                yield bound
            finally:
                _verify_bound_regular(
                    bound,
                    changed=f"{label} changed while reading",
                    require_executable=False,
                    require_single_link=False,
                )


def _read_anchored_regular(
    path: pathlib.Path,
    *,
    missing: str,
    not_regular: str,
    ancestor_changed: str,
    identity_changed: str,
    bytes_changed: str,
    require_single_link: bool,
) -> bytes:
    """Read one regular leaf twice through a retained directory binding.

    Examples
    --------
    >>> contents = _read_anchored_regular(
    ...     pathlib.Path(__file__),
    ...     missing="missing",
    ...     not_regular="not regular",
    ...     ancestor_changed="ancestor changed",
    ...     identity_changed="identity changed",
    ...     bytes_changed="bytes changed",
    ...     require_single_link=False,
    ... )
    >>> b"aggregate Task 6" in contents
    True
    """
    with _anchored_parent(path, changed=ancestor_changed) as binding:
        try:
            before = os.stat(
                binding.leaf,
                dir_fd=binding.parent_fd,
                follow_symlinks=False,
            )
        except OSError:
            _abort(missing)
        if not stat.S_ISREG(before.st_mode) or (
            require_single_link and before.st_nlink != 1
        ):
            _abort(not_regular)
        flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
        try:
            descriptor = os.open(binding.leaf, flags, dir_fd=binding.parent_fd)
        except OSError:
            _abort(missing)
        with os.fdopen(descriptor, "rb") as handle:
            opened = os.fstat(handle.fileno())
            if not os.path.samestat(before, opened) or not stat.S_ISREG(opened.st_mode):
                _abort(identity_changed)
            contents = handle.read()
            after = os.fstat(handle.fileno())
        try:
            final = os.stat(
                binding.leaf,
                dir_fd=binding.parent_fd,
                follow_symlinks=False,
            )
        except OSError:
            _abort(identity_changed)
        if (
            not os.path.samestat(opened, after)
            or not os.path.samestat(after, final)
            or (require_single_link and final.st_nlink != 1)
        ):
            _abort(identity_changed)
        _verify_directory_binding(binding, ancestor_changed)
        try:
            confirmation_descriptor = os.open(
                binding.leaf,
                flags,
                dir_fd=binding.parent_fd,
            )
        except OSError:
            _abort(identity_changed)
        with os.fdopen(confirmation_descriptor, "rb") as confirmation_handle:
            confirmation_opened = os.fstat(confirmation_handle.fileno())
            confirmation = confirmation_handle.read()
            confirmation_after = os.fstat(confirmation_handle.fileno())
        try:
            confirmed_path = os.stat(
                binding.leaf,
                dir_fd=binding.parent_fd,
                follow_symlinks=False,
            )
        except OSError:
            _abort(identity_changed)
        _verify_directory_binding(binding, ancestor_changed)
        if (
            not os.path.samestat(opened, confirmation_opened)
            or not os.path.samestat(confirmation_opened, confirmation_after)
            or not os.path.samestat(confirmation_after, confirmed_path)
            or (require_single_link and confirmed_path.st_nlink != 1)
            or confirmation != contents
        ):
            _abort(bytes_changed)
        return contents


def _reject_source_symlinked_ancestors(root: pathlib.Path, path: pathlib.Path) -> None:
    """Reject a source leaf whose repository-relative parent is a symlink.

    Examples
    --------
    >>> root = pathlib.Path(__file__).parents[3]
    >>> _reject_source_symlinked_ancestors(root, pathlib.Path(__file__))
    """
    try:
        relative = path.relative_to(root)
    except ValueError:
        _abort("source path escapes repository")
    parent = root
    for component in relative.parts[:-1]:
        parent /= component
        if parent.is_symlink():
            _abort("source path has a symlinked ancestor")


def _strict_json(contents: bytes, label: str) -> object:
    """Decode one JSON document while rejecting duplicate object keys.

    Examples
    --------
    >>> _strict_json(b'{"value": 1}', "example")
    {'value': 1}
    """

    def pairs(items: list[tuple[str, object]]) -> dict[str, object]:
        value: dict[str, object] = {}
        for key, item in items:
            if key in value:
                _abort(f"duplicate JSON key in {label}")
            value[key] = item
        return value

    def constant(value: str) -> t.NoReturn:
        _abort(f"nonfinite JSON value in {label}: {value}")

    try:
        return json.loads(
            contents,
            object_pairs_hook=pairs,
            parse_constant=constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError):
        _abort(f"invalid {label}")


def _parity_document(root: pathlib.Path, name: str) -> tuple[object, str]:
    """Load one regular parity document and retain its raw-byte digest.

    Examples
    --------
    >>> root = pathlib.Path(__file__).parents[3]
    >>> _, digest = _parity_document(root, "manifest.json")
    >>> digest.startswith("sha256:")
    True
    """
    path = root / "cxx" / "parity" / name
    try:
        contents = _read_source_regular(path, f"parity document: {name}")
    except GateError:
        _abort(f"parity document changed while reading: {name}")
    return _strict_json(contents, f"parity document: {name}"), _sha256(contents)


def _observation_identity(
    value: object,
    *,
    digest: str,
    label: str,
) -> dict[str, str]:
    """Project one pinned parity observation without exposing source paths.

    Examples
    --------
    >>> observation = {
    ...     "observation_id": "example",
    ...     "source": {"revision": "HEAD", "commit": "a" * 40, "tree": "b" * 40},
    ... }
    >>> identity = _observation_identity(
    ...     observation, digest=_sha256(b""), label="example"
    ... )
    >>> identity["revision"]
    'HEAD'
    """
    if not isinstance(value, dict):
        _abort(f"invalid {label} parity observation")
    source = value.get("source")
    if not isinstance(source, dict):
        _abort(f"invalid {label} parity observation")
    observation_id = value.get("observation_id")
    revision = source.get("revision")
    commit = source.get("commit")
    tree = source.get("tree")
    if not (
        isinstance(observation_id, str)
        and isinstance(revision, str)
        and isinstance(commit, str)
        and _OBJECT_ID.fullmatch(commit)
        and isinstance(tree, str)
        and _OBJECT_ID.fullmatch(tree)
    ):
        _abort(f"invalid {label} parity observation")
    return {
        "observation_id": observation_id,
        "revision": revision,
        "commit": commit,
        "tree": tree,
        "sidecar_sha256": digest,
    }


def _python_contract(root: pathlib.Path) -> dict[str, object]:
    """Bind the six pinned parity sidecars recorded by the aggregate gate.

    Examples
    --------
    >>> root = pathlib.Path(__file__).parents[3]
    >>> _python_contract(root)["semantic_contract_sha256"][:7]
    'sha256:'
    """
    manifest, manifest_sha256 = _parity_document(root, "manifest.json")
    inputs, inputs_sha256 = _parity_document(root, "inputs.json")
    if not isinstance(manifest, dict):
        _abort("invalid parity manifest")
    documents: dict[str, tuple[object, str]] = {}
    for key, filename in (
        ("release", "release-v0.62.0.json"),
        ("development", "development.json"),
        ("mapping", "mapping.json"),
        ("approvals", "approvals.json"),
        ("evidence", "evidence.json"),
        ("shards", "shards.json"),
    ):
        documents[key] = _parity_document(root, filename)
    bindings = manifest.get("bindings")
    if not isinstance(bindings, dict):
        _abort("missing parity bindings")
    from cxx.tools.parity.generate import canonical_sha256

    for key, (document, _) in documents.items():
        binding = bindings.get(f"{key}_sha256")
        if binding != canonical_sha256(document):
            _abort("parity sidecar digest differs from manifest binding")
        if manifest.get(key) != document:
            _abort("parity sidecar differs from manifest")
    for key in ("release", "development"):
        observation = documents[key][0]
        if (
            not isinstance(observation, dict)
            or observation.get("input_manifest") != inputs
        ):
            _abort("parity inputs differ from manifest")
    semantic = manifest.get("semantic_contract_sha256")
    if not isinstance(semantic, str) or not _DIGEST.fullmatch(semantic):
        _abort("invalid parity semantic digest")
    from cxx.tools.parity.sync import semantic_contract_sha256

    if semantic_contract_sha256(manifest) != semantic:
        _abort("parity semantic digest differs from manifest")
    return {
        "manifest_sha256": manifest_sha256,
        "inputs_sha256": inputs_sha256,
        "semantic_contract_sha256": semantic,
        "release": _observation_identity(
            documents["release"][0],
            digest=documents["release"][1],
            label="release",
        ),
        "development": _observation_identity(
            documents["development"][0],
            digest=documents["development"][1],
            label="development",
        ),
        "sidecar_sha256": {
            key: documents[key][1]
            for key in ("approvals", "evidence", "mapping", "shards")
        },
    }


def _tool_pins(root: pathlib.Path) -> dict[str, list[str]]:
    """Read finite version pins required by the aggregate tool boundary.

    Examples
    --------
    >>> root = pathlib.Path(__file__).parents[3]
    >>> bool(_tool_pins(root)["clang"])
    True
    """
    path = root / ".tool-versions"
    if path.is_symlink() or not path.is_file():
        _abort("missing .tool-versions")
    try:
        contents = _read_source_regular(path, "tool pins").decode("utf-8")
    except GateError:
        _abort("tool pins changed while reading")
    except UnicodeDecodeError:
        _abort("unsafe tool pin")
    pins: dict[str, list[str]] = {}
    for line in contents.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        name, *versions = stripped.split()
        if not versions or any(
            not re.fullmatch(r"[0-9]+(?:\.[0-9]+)*", item) for item in versions
        ):
            _abort("unsafe tool pin")
        if name in pins:
            _abort(f"duplicate tool pin: {name}")
        pins[name] = versions
    if not set(pins) >= _REQUIRED_TOOLS:
        _abort("missing required tool pin")
    return {name: pins[name] for name in sorted(_REQUIRED_TOOLS)}


def _normalize_tool_version(
    name: str,
    stdout: bytes,
    pins: list[str],
) -> str:
    r"""Normalize one runtime identity and require a declared version pin.

    Examples
    --------
    >>> _normalize_tool_version(
    ...     "uv", b"uv 0.12.1 (x86_64-unknown-linux-musl)\n", ["0.12.1"]
    ... )
    'uv 0.12.1'
    """
    try:
        text = stdout.decode("utf-8")
    except UnicodeDecodeError:
        _abort(f"invalid {name} version")
    patterns = {
        "just": (r"^just ([0-9]+(?:\.[0-9]+)*)$", "just {version}"),
        "uv": (
            (
                r"uv ([0-9]+(?:\.[0-9]+)*)"
                r"(?: \([A-Za-z0-9_.]+(?:-[A-Za-z0-9_.]+){2,3}\))?"
                r"(?:\r\n|\n)?"
            ),
            "uv {version}",
        ),
        "python": (r"^Python ([0-9]+(?:\.[0-9]+)*)$", "Python {version}"),
        "cmake": (
            r"^cmake version ([0-9]+(?:\.[0-9]+)*)$",
            "cmake version {version}",
        ),
        "ninja": (r"^([0-9]+(?:\.[0-9]+)*)$", "{version}"),
        "clang": (
            r"^(?:[^\r\n]* )?clang version ([0-9]+(?:\.[0-9]+)*)",
            "clang version {version}",
        ),
    }
    rule = patterns.get(name)
    if rule is None:
        _abort(f"invalid {name} version")
    match = (
        re.fullmatch(rule[0], text)
        if name == "uv"
        else re.search(rule[0], text, flags=re.MULTILINE)
    )
    if match is None:
        _abort(f"invalid {name} version")
    version = match.group(1)
    if name == "python":
        components = version.split(".")
        accepted = ".".join(components[:2]) in pins
    else:
        accepted = version in pins
    if not accepted:
        _abort(f"unexpected {name} version")
    return rule[1].format(version=version)


@contextlib.contextmanager
def _configured_compiler_identity(
    root: pathlib.Path,
    *,
    preset: str,
    cmake_version: str,
    clang_pins: list[str],
    language: t.Literal["C", "CXX"] = "CXX",
) -> t.Iterator[tuple[_BoundRegular, dict[str, str]]]:
    """Read one freshly configured CMake C or C++ compiler identity.

    Examples
    --------
    >>> root = pathlib.Path(__file__).parents[3]
    >>> try:
    ...     with _configured_compiler_identity(
    ...         root,
    ...         preset="missing",
    ...         cmake_version="0",
    ...         clang_pins=["0"],
    ...     ):
    ...         pass
    ... except GateError as error:
    ...     str(error).startswith("configured C++ compiler metadata for missing")
    True
    """
    human_language = "C" if language == "C" else "C++"
    metadata = (
        root
        / "cxx"
        / "build"
        / preset
        / "CMakeFiles"
        / cmake_version
        / f"CMake{language}Compiler.cmake"
    )
    try:
        contents = _read_source_regular(
            metadata,
            f"configured {human_language} compiler metadata for {preset}",
        ).decode("utf-8")
    except UnicodeDecodeError:
        _abort(f"invalid configured {human_language} compiler metadata for {preset}")

    def one(variable: str) -> str:
        matches: list[str] = re.findall(
            rf'^set\({re.escape(variable)} "([^"]+)"\)$',
            contents,
            flags=re.MULTILINE,
        )
        if len(matches) != 1:
            _abort(
                f"missing configured {human_language} compiler identity for {preset}"
            )
        return matches[0]

    compiler = pathlib.Path(one(f"CMAKE_{language}_COMPILER"))
    compiler_id = one(f"CMAKE_{language}_COMPILER_ID")
    compiler_version = one(f"CMAKE_{language}_COMPILER_VERSION")
    if not compiler.is_absolute():
        _abort(f"invalid configured {human_language} compiler for {preset}")
    if compiler_id != "Clang" or compiler_version not in clang_pins:
        _abort(f"unexpected configured {human_language} compiler identity for {preset}")
    with _bound_regular(
        compiler,
        label=f"configured {human_language} compiler for {preset}",
        allow_leaf_symlinks=True,
        require_executable=True,
        require_single_link=True,
    ) as bound:
        yield bound, {"id": compiler_id, "version": compiler_version}


def _configured_build_tool_paths(
    contents: bytes, *, preset: str
) -> dict[str, pathlib.Path]:
    r"""Parse exact absolute CMake, CTest, and build-backend cache paths.

    Examples
    --------
    >>> paths = _configured_build_tool_paths(
    ...     b"CMAKE_COMMAND:INTERNAL=/usr/bin/cmake\n"
    ...     b"CMAKE_CTEST_COMMAND:INTERNAL=/usr/bin/ctest\n"
    ...     b"CMAKE_MAKE_PROGRAM:FILEPATH=/usr/bin/ninja\n",
    ...     preset="cxx-dev",
    ... )
    >>> sorted(paths)
    ['cmake', 'ctest', 'ninja']
    """
    try:
        text = contents.decode("utf-8")
    except UnicodeDecodeError:
        _abort(f"invalid configured build tools for {preset}")
    fields = {
        "cmake": ("CMAKE_COMMAND", "INTERNAL"),
        "ctest": ("CMAKE_CTEST_COMMAND", "INTERNAL"),
        "ninja": ("CMAKE_MAKE_PROGRAM", "FILEPATH"),
    }
    paths: dict[str, pathlib.Path] = {}
    for logical_name, (variable, cache_type) in fields.items():
        matches = re.findall(
            rf"^{variable}:{cache_type}=([^\r\n]+)$",
            text,
            flags=re.MULTILINE,
        )
        if len(matches) != 1:
            _abort(f"invalid configured build tools for {preset}")
        path = pathlib.Path(matches[0])
        if not path.is_absolute() or pathlib.Path(os.path.normpath(path)) != path:
            _abort(f"invalid configured build tools for {preset}")
        paths[logical_name] = path
    return paths


def _run_command(
    argv: list[str],
    *,
    cwd: pathlib.Path,
    environment: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[bytes]:
    """Run one evidence command through the argv-only process boundary.

    Examples
    --------
    >>> result = _run_command(
    ...     [sys.executable, "-c", "print('ok')"], cwd=pathlib.Path.cwd()
    ... )
    >>> result.stdout.strip()
    b'ok'
    """
    return subprocess.run(
        argv,
        cwd=cwd,
        env=environment,
        check=False,
        capture_output=True,
    )


def _required_command(
    argv: list[str],
    *,
    name: str,
    cwd: pathlib.Path,
    environment: dict[str, str] | None = None,
    record_version: bool = False,
) -> dict[str, object]:
    """Run one passing command and retain its stable public result.

    Examples
    --------
    >>> command = _required_command(
    ...     [sys.executable, "-c", "pass"],
    ...     name="example",
    ...     cwd=pathlib.Path.cwd(),
    ... )
    >>> command["exit_code"]
    0
    """
    result = _run_command(argv, cwd=cwd, environment=environment)
    if result.returncode != 0:
        _abort(f"command failed: {name}")
    record: dict[str, object] = {
        "name": name,
        "cwd": "." if cwd == pathlib.Path.cwd() else cwd.name,
        "exit_code": result.returncode,
    }
    if record_version:
        try:
            record["version"] = result.stdout.decode("utf-8").strip()
        except UnicodeDecodeError:
            _abort(f"invalid UTF-8 tool version: {name}")
    return record


def _git_identity(
    root: pathlib.Path,
    *,
    executable: pathlib.Path | None = None,
    environment: dict[str, str] | None = None,
) -> tuple[str, str]:
    """Read the committed source identity for the running aggregate gate.

    Examples
    --------
    >>> root = pathlib.Path(__file__).parents[3]
    >>> commit, tree = _git_identity(root)
    >>> len(commit) == len(tree) == 40
    True
    """
    command = "git" if executable is None else os.fspath(executable)
    child_environment = _git_environment() if environment is None else environment
    values: list[str] = []
    for reference in ("HEAD",):
        result = subprocess.run(
            [command, "rev-parse", reference],
            cwd=root,
            env=child_environment,
            check=False,
            capture_output=True,
        )
        if result.returncode != 0:
            _abort("missing gate source identity")
        values.append(result.stdout.decode("ascii").strip())
    commit = values[0]
    tree_result = subprocess.run(
        [command, "rev-parse", f"{commit}^{{tree}}"],
        cwd=root,
        env=child_environment,
        check=False,
        capture_output=True,
    )
    if tree_result.returncode != 0:
        _abort("missing gate source identity")
    tree = tree_result.stdout.decode("ascii").strip()
    _verify_git_head(
        root,
        commit,
        executable=executable,
        environment=child_environment,
    )
    return commit, tree


def _verify_git_head(
    root: pathlib.Path,
    commit: str,
    *,
    executable: pathlib.Path | None = None,
    environment: dict[str, str] | None = None,
) -> None:
    """Require the repository HEAD to remain at the captured commit.

    Examples
    --------
    >>> root = pathlib.Path(__file__).parents[3]
    >>> commit, _ = _git_identity(root)
    >>> _verify_git_head(root, commit)
    """
    command = "git" if executable is None else os.fspath(executable)
    child_environment = _git_environment() if environment is None else environment
    result = subprocess.run(
        [command, "rev-parse", "HEAD"],
        cwd=root,
        env=child_environment,
        check=False,
        capture_output=True,
    )
    if result.returncode != 0 or result.stdout.decode("ascii").strip() != commit:
        _abort("gate source identity changed during aggregate gate")


def _ctest_artifact_bytes(
    root: pathlib.Path,
    path: pathlib.Path,
    *,
    missing: str,
) -> bytes:
    """Read one CTest artifact while binding its repository path and inode.

    Examples
    --------
    >>> root = pathlib.Path(__file__).parents[3]
    >>> contents = _ctest_artifact_bytes(
    ...     root, pathlib.Path(__file__), missing="missing module"
    ... )
    >>> b"aggregate Task 6" in contents
    True
    """
    try:
        path.relative_to(root)
    except ValueError:
        _abort(missing)
    return _read_anchored_regular(
        path,
        missing=missing,
        not_regular=missing,
        ancestor_changed="CTest artifact path has a symlinked ancestor",
        identity_changed="CTest artifact changed while reading",
        bytes_changed="CTest artifact bytes changed during reading",
        require_single_link=True,
    )


def _validate_ctest_build_snapshot(value: object) -> None:
    """Validate the complete raw build-input binding from one CTest gate.

    Examples
    --------
    >>> digest = _sha256(b"")
    >>> snapshot = {
    ...     "cache_sha256": digest,
    ...     "compile_commands_sha256": digest,
    ...     "registration_files": [
    ...         {"path": "CTestTestfile.cmake", "sha256": digest}
    ...     ],
    ...     "executables": [{"path": "tests/example", "sha256": digest}],
    ... }
    >>> _validate_ctest_build_snapshot(snapshot)
    """
    if not isinstance(value, dict) or set(value) != {
        "cache_sha256",
        "compile_commands_sha256",
        "registration_files",
        "executables",
    }:
        _abort("invalid CTest build snapshot")
    cache = value.get("cache_sha256")
    compile_commands = value.get("compile_commands_sha256")
    registrations = value.get("registration_files")
    executables = value.get("executables")
    if (
        not isinstance(cache, str)
        or not _DIGEST.fullmatch(cache)
        or not isinstance(compile_commands, str)
        or not _DIGEST.fullmatch(compile_commands)
        or not isinstance(registrations, list)
        or not registrations
        or not isinstance(executables, list)
        or not executables
    ):
        _abort("invalid CTest build snapshot")

    def binding(item: object) -> tuple[str, str]:
        if not isinstance(item, dict) or set(item) != {"path", "sha256"}:
            _abort("invalid CTest build snapshot")
        path = item.get("path")
        digest = item.get("sha256")
        if not isinstance(path, str) or not isinstance(digest, str):
            _abort("invalid CTest build snapshot")
        try:
            safe_path = _safe_source_path(path.encode(), "CTest build path")
        except GateError:
            _abort("invalid CTest build snapshot")
        if safe_path != path or not _DIGEST.fullmatch(digest):
            _abort("invalid CTest build snapshot")
        return path, digest

    registration_bindings = [binding(item) for item in registrations]
    executable_bindings = [binding(item) for item in executables]
    registration_paths = [path for path, _ in registration_bindings]
    if (
        registration_paths != sorted(set(registration_paths))
        or "CTestTestfile.cmake" not in registration_paths
        or not executable_bindings
    ):
        _abort("invalid CTest build snapshot")
    executable_by_path: dict[str, str] = {}
    for path, digest in executable_bindings:
        previous = executable_by_path.setdefault(path, digest)
        if previous != digest:
            _abort("invalid CTest build snapshot")


def _validate_ctest_compiler(value: object) -> dict[str, str]:
    """Validate one path-free compiler identity from a CTest gate.

    Examples
    --------
    >>> digest = _sha256(b"")
    >>> value = {
    ...     "executable_sha256": digest,
    ...     "id": "Clang",
    ...     "metadata_sha256": digest,
    ...     "version": "18.1.3",
    ... }
    >>> _validate_ctest_compiler(value)["id"]
    'Clang'
    """
    if not isinstance(value, dict) or set(value) != {
        "executable_sha256",
        "id",
        "metadata_sha256",
        "version",
    }:
        _abort("invalid CTest gate record")
    executable = value.get("executable_sha256")
    metadata = value.get("metadata_sha256")
    compiler_id = value.get("id")
    version = value.get("version")
    if not (
        isinstance(executable, str)
        and _DIGEST.fullmatch(executable)
        and isinstance(metadata, str)
        and _DIGEST.fullmatch(metadata)
        and compiler_id == "Clang"
        and isinstance(version, str)
        and re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", version)
    ):
        _abort("invalid CTest gate record")
    return t.cast(dict[str, str], value)


def _ctest_projection(
    root: pathlib.Path,
    gate_id: str,
    *,
    preset: str,
    selector: dict[str, str],
) -> dict[str, object]:
    """Project one named CTest gate only after its immutable leaf matches.

    Examples
    --------
    >>> root = pathlib.Path(__file__).parents[3]
    >>> try:
    ...     _ctest_projection(
    ...         root,
    ...         "missing",
    ...         preset="cxx-dev",
    ...         selector={"match": "^$"},
    ...     )
    ... except GateError as error:
    ...     str(error)
    'missing named CTest record'
    """
    path = root / "cxx" / "build" / "evidence" / f"{gate_id}.json"
    named_bytes = _ctest_artifact_bytes(
        root,
        path,
        missing="missing named CTest record",
    )
    value = _strict_json(named_bytes, "named CTest record")
    expected_fields = {
        "artifacts",
        "compiler",
        "ctest_names",
        "executed_test_ids",
        "execution_sha256",
        "fixture_binding",
        "fixture_modes",
        "gate_id",
        "gate_sha256",
        "junit_sha256",
        "preset",
        "raw_bindings",
        "registered_test_ids",
        "registration_sha256",
        "schema_version",
        "selector",
        "status",
    }
    if (
        not isinstance(value, dict)
        or set(value) != expected_fields
        or value.get("gate_id") != gate_id
    ):
        _abort("invalid CTest gate record")
    digest = value.get("gate_sha256")
    if not isinstance(digest, str) or not _DIGEST.fullmatch(digest):
        _abort("invalid CTest gate record")
    unhashed = dict(value)
    del unhashed["gate_sha256"]
    if _sha256(_canonical_bytes(unhashed)) != digest:
        _abort("invalid CTest gate digest")
    leaf = root / "cxx" / "build" / "evidence" / "ctest" / digest[7:]
    with _bound_directory(
        leaf,
        label="immutable CTest gate leaf",
    ) as bound_leaf:
        return _ctest_projection_from_leaf(
            root,
            gate_id,
            preset=preset,
            selector=selector,
            named_bytes=named_bytes,
            value=value,
            digest=digest,
            leaf=leaf,
            bound_leaf=bound_leaf,
        )


def _ctest_projection_from_leaf(
    root: pathlib.Path,
    gate_id: str,
    *,
    preset: str,
    selector: dict[str, str],
    named_bytes: bytes,
    value: dict[object, object],
    digest: str,
    leaf: pathlib.Path,
    bound_leaf: _BoundDirectory,
) -> dict[str, object]:
    """Validate one CTest projection through its retained immutable leaf.

    Examples
    --------
    >>> root = pathlib.Path(__file__).parents[3]
    >>> module_directory = pathlib.Path(__file__).parent
    >>> with _bound_directory(module_directory, label="module") as bound:
    ...     try:
    ...         _ctest_projection_from_leaf(
    ...             root,
    ...             "missing",
    ...             preset="cxx-dev",
    ...             selector={"match": "^$"},
    ...             named_bytes=b"{}",
    ...             value={},
    ...             digest=_sha256(b""),
    ...             leaf=module_directory,
    ...             bound_leaf=bound,
    ...         )
    ...     except GateError as error:
    ...         str(error)
    'invalid immutable CTest gate leaf'
    """
    leaf_record = leaf / "gate.json"
    try:
        if set(os.listdir(bound_leaf.descriptor)) != {  # noqa: PTH208
            "gate.json",
            "registered-tests.json",
            "results.junit.xml",
        }:
            _abort("invalid immutable CTest gate leaf")
        if (
            _ctest_artifact_bytes(
                root,
                leaf_record,
                missing="missing immutable CTest gate leaf",
            )
            != named_bytes
        ):
            _abort("named CTest record differs from immutable leaf")
    except OSError:
        _abort("missing immutable CTest gate leaf")
    artifacts = value.get("artifacts")
    raw_bindings = value.get("raw_bindings")
    _validate_ctest_compiler(value.get("compiler"))
    if not isinstance(artifacts, dict) or not isinstance(raw_bindings, dict):
        _abort("invalid CTest gate record")
    if set(raw_bindings) != {
        "registry_sha256",
        "junit_sha256",
        "build_snapshot",
    }:
        _abort("invalid CTest build snapshot")
    _validate_ctest_build_snapshot(raw_bindings.get("build_snapshot"))
    registry = _ctest_artifact_bytes(
        root,
        leaf / "registered-tests.json",
        missing="missing immutable CTest gate leaf",
    )
    junit = _ctest_artifact_bytes(
        root,
        leaf / "results.junit.xml",
        missing="missing immutable CTest gate leaf",
    )
    try:
        _verify_bound_directory(bound_leaf, "CTest leaf changed while reading")
        final_names = set(os.listdir(bound_leaf.descriptor))  # noqa: PTH208
    except OSError:
        _abort("CTest leaf changed while reading")
    if final_names != {
        "gate.json",
        "registered-tests.json",
        "results.junit.xml",
    }:
        _abort("CTest leaf changed while reading")
    if (
        artifacts
        != {"registration": "registered-tests.json", "junit": "results.junit.xml"}
        or _sha256(registry) != value.get("registration_sha256")
        or _sha256(junit) != value.get("junit_sha256")
        or raw_bindings.get("registry_sha256") != _sha256(registry)
        or raw_bindings.get("junit_sha256") != _sha256(junit)
    ):
        _abort("immutable CTest leaf differs from its record")
    registered = value.get("registered_test_ids")
    executed = value.get("executed_test_ids")
    if not (
        type(value.get("schema_version")) is int
        and value.get("schema_version") == 2
        and value.get("status") == "passed"
        and value.get("preset") == preset
        and value.get("selector") == selector
        and isinstance(registered, list)
        and isinstance(executed, list)
        and all(isinstance(item, str) and item for item in registered)
        and all(isinstance(item, str) and item for item in executed)
        and registered == sorted(set(registered))
        and executed == registered
        and value.get("ctest_names") == registered
        and isinstance(value.get("fixture_modes"), list)
        and isinstance(value.get("fixture_binding"), dict)
        and isinstance(value.get("execution_sha256"), str)
        and _DIGEST.fullmatch(t.cast(str, value["execution_sha256"]))
    ):
        _abort("invalid CTest gate record")
    execution = {
        "preset": value["preset"],
        "selector": value["selector"],
        "registered_test_ids": registered,
        "executed_test_ids": executed,
        "fixture_modes": value["fixture_modes"],
        "fixture_binding": value["fixture_binding"],
    }
    if _sha256(_canonical_bytes(execution)) != value["execution_sha256"]:
        _abort("invalid CTest execution digest")
    if (
        _ctest_artifact_bytes(
            root,
            leaf_record,
            missing="missing immutable CTest gate leaf",
        )
        != named_bytes
        or _ctest_artifact_bytes(
            root,
            leaf / "registered-tests.json",
            missing="missing immutable CTest gate leaf",
        )
        != registry
        or _ctest_artifact_bytes(
            root,
            leaf / "results.junit.xml",
            missing="missing immutable CTest gate leaf",
        )
        != junit
    ):
        _abort("CTest leaf changed while reading")
    _verify_bound_directory(bound_leaf, "CTest leaf changed while reading")
    named_record = root / "cxx" / "build" / "evidence" / f"{gate_id}.json"
    if (
        _ctest_artifact_bytes(
            root,
            named_record,
            missing="missing named CTest record",
        )
        != named_bytes
    ):
        _abort("named CTest record changed while reading")
    return {
        field: value[field]
        for field in (
            "compiler",
            "execution_sha256",
            "executed_test_ids",
            "fixture_binding",
            "fixture_modes",
            "gate_id",
            "gate_sha256",
            "preset",
            "registered_test_ids",
            "selector",
            "status",
        )
    }


def _read_ctest_gate_files(
    root: pathlib.Path,
    gate_id: str,
    digest: str,
) -> tuple[bytes, bytes, bytes, bytes]:
    """Read one named CTest record and its exact immutable-leaf files.

    Examples
    --------
    >>> root = pathlib.Path(__file__).parents[3]
    >>> try:
    ...     _read_ctest_gate_files(root, "missing", _sha256(b""))
    ... except GateError as error:
    ...     str(error)
    'CTest gate files changed'
    """
    if not _DIGEST.fullmatch(digest):
        _abort("CTest gate files changed")
    named_path = root / "cxx" / "build" / "evidence" / f"{gate_id}.json"
    leaf = root / "cxx" / "build" / "evidence" / "ctest" / digest[7:]
    expected_names = {
        "gate.json",
        "registered-tests.json",
        "results.junit.xml",
    }
    named = _ctest_artifact_bytes(
        root,
        named_path,
        missing="CTest gate files changed",
    )
    with _bound_directory(leaf, label="immutable CTest gate leaf") as bound_leaf:
        try:
            if set(os.listdir(bound_leaf.descriptor)) != expected_names:  # noqa: PTH208
                _abort("CTest gate files changed")
        except OSError:
            _abort("CTest gate files changed")
        files = tuple(
            _ctest_artifact_bytes(
                root,
                leaf / name,
                missing="CTest gate files changed",
            )
            for name in (
                "gate.json",
                "registered-tests.json",
                "results.junit.xml",
            )
        )
        _verify_bound_directory(bound_leaf, "CTest gate files changed")
        try:
            if set(os.listdir(bound_leaf.descriptor)) != expected_names:  # noqa: PTH208
                _abort("CTest gate files changed")
        except OSError:
            _abort("CTest gate files changed")
        confirmation = tuple(
            _ctest_artifact_bytes(
                root,
                leaf / name,
                missing="CTest gate files changed",
            )
            for name in (
                "gate.json",
                "registered-tests.json",
                "results.junit.xml",
            )
        )
        _verify_bound_directory(bound_leaf, "CTest gate files changed")
    final_named = _ctest_artifact_bytes(
        root,
        named_path,
        missing="CTest gate files changed",
    )
    if files != confirmation or named != final_named or named != files[0]:
        _abort("CTest gate files changed")
    return named, files[0], files[1], files[2]


def _capture_ctest_gate_files(
    root: pathlib.Path,
    gate_id: str,
    digest: str,
) -> _CTestGateFiles:
    """Capture one CTest projection for later common checkpoints.

    Examples
    --------
    >>> root = pathlib.Path(__file__).parents[3]
    >>> try:
    ...     _capture_ctest_gate_files(root, "missing", _sha256(b""))
    ... except GateError as error:
    ...     str(error)
    'CTest gate files changed'
    """
    return _CTestGateFiles(
        gate_id=gate_id,
        digest=digest,
        files=_read_ctest_gate_files(root, gate_id, digest),
    )


def _verify_ctest_gate_files(root: pathlib.Path, binding: _CTestGateFiles) -> None:
    """Reprobe one CTest named record and immutable leaf exactly.

    Examples
    --------
    >>> root = pathlib.Path(__file__).parents[3]
    >>> binding = _CTestGateFiles("missing", _sha256(b""), (b"",) * 4)
    >>> try:
    ...     _verify_ctest_gate_files(root, binding)
    ... except GateError as error:
    ...     str(error)
    'CTest gate files changed'
    """
    if _read_ctest_gate_files(root, binding.gate_id, binding.digest) != binding.files:
        _abort("CTest gate files changed")


def _evidence_core(value: dict[str, object]) -> dict[str, object]:
    """Return the reviewable semantic projection of one full evidence record.

    Examples
    --------
    >>> _evidence_core({"review": {"status": "pending"}})
    {}
    """
    core = copy.deepcopy(value)
    for field in ("evidence_core_sha256", "final_evidence_sha256", "review"):
        core.pop(field, None)
    gate_source = core.get("gate_source")
    if isinstance(gate_source, dict):
        gate_source.pop("source_snapshot", None)
    toolchain = core.get("toolchain")
    if isinstance(toolchain, dict):
        toolchain.pop("artifacts", None)
    gates = core.get("ctest_gates")
    if isinstance(gates, list):
        for gate in gates:
            if isinstance(gate, dict):
                gate.pop("gate_sha256", None)
                tmux = gate.get("tmux")
                if isinstance(tmux, dict):
                    tmux.pop("sha256", None)
    return core


def _final_projection(value: dict[str, object]) -> dict[str, object]:
    """Return all evidence fields except the self-referential final digest.

    Examples
    --------
    >>> _final_projection({"value": 1, "final_evidence_sha256": "digest"})
    {'value': 1}
    """
    final = copy.deepcopy(value)
    final.pop("final_evidence_sha256", None)
    return final


def _atomic_output(
    path: pathlib.Path,
    contents: bytes,
    *,
    checkpoint: t.Callable[[pathlib.Path | None], None] | None = None,
    finalize: t.Callable[[], None] | None = None,
) -> None:
    """Atomically replace the public JSON output after all gate checks pass.

    Examples
    --------
    >>> import tempfile
    >>> with tempfile.TemporaryDirectory() as raw:
    ...     output = pathlib.Path(raw) / "evidence.json"
    ...     _atomic_output(output, b"evidence")
    ...     output.read_bytes()
    b'evidence'
    """
    if checkpoint is not None:
        checkpoint(None)
    _ensure_directory(path.parent, label="output parent")
    with _bound_directory(path.parent, label="output parent") as parent:
        _publish_output_at(
            parent,
            path.name,
            contents,
            checkpoint=checkpoint,
            finalize=finalize,
        )


def _read_output_at(directory_fd: int, name: str, label: str) -> bytes:
    """Read one single-link regular publication file through its parent fd.

    Examples
    --------
    >>> import tempfile
    >>> with tempfile.TemporaryDirectory() as raw:
    ...     output = pathlib.Path(raw) / "evidence.json"
    ...     _ = output.write_bytes(b"evidence")
    ...     descriptor = os.open(raw, os.O_RDONLY | os.O_DIRECTORY)
    ...     try:
    ...         contents = _read_output_at(descriptor, output.name, "evidence")
    ...     finally:
    ...         os.close(descriptor)
    >>> contents
    b'evidence'
    """
    try:
        before = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        descriptor = os.open(
            name,
            os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0),
            dir_fd=directory_fd,
        )
    except OSError:
        _abort(f"{label} is not a regular file")
    with os.fdopen(descriptor, "rb") as handle:
        opened = os.fstat(handle.fileno())
        contents = handle.read()
        after = os.fstat(handle.fileno())
    try:
        final = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
    except OSError:
        _abort(f"{label} changed while reading")
    if (
        not stat.S_ISREG(before.st_mode)
        or before.st_nlink != 1
        or not os.path.samestat(before, opened)
        or not os.path.samestat(opened, after)
        or not os.path.samestat(after, final)
        or final.st_nlink != 1
    ):
        _abort(f"{label} changed while reading")
    return contents


def _rename_exchange(directory_fd: int, first: str, second: str) -> None:
    """Atomically exchange two output-parent entries for exact rollback.

    Examples
    --------
    >>> import tempfile
    >>> with tempfile.TemporaryDirectory() as raw:
    ...     directory = pathlib.Path(raw)
    ...     _ = (directory / "first").write_bytes(b"first")
    ...     _ = (directory / "second").write_bytes(b"second")
    ...     descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    ...     try:
    ...         _rename_exchange(descriptor, "first", "second")
    ...     finally:
    ...         os.close(descriptor)
    ...     (directory / "first").read_bytes()
    b'second'
    """
    try:
        renameat2 = ctypes.CDLL(None, use_errno=True).renameat2
    except AttributeError:
        _abort("output exchange is unavailable")
    renameat2.argtypes = [
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_uint,
    ]
    renameat2.restype = ctypes.c_int
    if (
        renameat2(
            directory_fd,
            first.encode(),
            directory_fd,
            second.encode(),
            2,
        )
        != 0
    ):
        _abort("output exchange failed")


def _publish_output_at(
    parent: _BoundDirectory,
    name: str,
    contents: bytes,
    *,
    checkpoint: t.Callable[[pathlib.Path | None], None] | None,
    finalize: t.Callable[[], None] | None,
) -> None:
    """Publish through one locked parent and roll back failed post-validation.

    Examples
    --------
    >>> import tempfile
    >>> with tempfile.TemporaryDirectory() as raw:
    ...     directory = pathlib.Path(raw)
    ...     with _bound_directory(directory, label="output parent") as parent:
    ...         _publish_output_at(
    ...             parent,
    ...             "evidence.json",
    ...             b"evidence",
    ...             checkpoint=None,
    ...             finalize=None,
    ...         )
    ...     (directory / "evidence.json").read_bytes()
    b'evidence'
    """
    if name in {"", ".", ".."} or pathlib.PurePath(name).name != name:
        _abort("unsafe output name")
    temporary = f".contract-gate-{secrets.token_hex(16)}"
    temporary_path = parent.path / temporary
    temporary_created = False
    committed = False
    lock_acquired = False
    destination: os.stat_result | None = None
    prior_contents: bytes | None = None
    fcntl.flock(parent.descriptor, fcntl.LOCK_EX)
    lock_acquired = True
    try:
        try:
            destination = os.stat(
                name,
                dir_fd=parent.descriptor,
                follow_symlinks=False,
            )
        except FileNotFoundError:
            destination = None
        if destination is not None:
            if not stat.S_ISREG(destination.st_mode) or destination.st_nlink != 1:
                _abort("output path is not a single-link regular file")
            prior_contents = _read_output_at(parent.descriptor, name, "output")
        descriptor = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
            0o600,
            dir_fd=parent.descriptor,
        )
        temporary_created = True
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(contents)
            handle.flush()
            os.fsync(handle.fileno())
        if (
            _read_output_at(parent.descriptor, temporary, "output temporary")
            != contents
        ):
            _abort("output temporary bytes differ")
        if checkpoint is not None:
            checkpoint(temporary_path)
        _verify_bound_directory(parent, "output parent changed during publication")
        try:
            current = os.stat(
                name,
                dir_fd=parent.descriptor,
                follow_symlinks=False,
            )
        except FileNotFoundError:
            current = None
        if (destination is None) != (current is None) or (
            destination is not None
            and current is not None
            and not os.path.samestat(destination, current)
        ):
            _abort("output changed during publication")
        if destination is not None and _read_output_at(
            parent.descriptor, name, "output"
        ) != t.cast(bytes, prior_contents):
            _abort("output bytes changed during publication")
        if destination is None:
            try:
                os.link(
                    temporary,
                    name,
                    src_dir_fd=parent.descriptor,
                    dst_dir_fd=parent.descriptor,
                    follow_symlinks=False,
                )
            except FileExistsError:
                _abort("output changed during publication")
        else:
            _rename_exchange(parent.descriptor, temporary, name)
        try:
            if destination is None:
                os.unlink(temporary, dir_fd=parent.descriptor)
                temporary_created = False
            published = os.stat(
                name,
                dir_fd=parent.descriptor,
                follow_symlinks=False,
            )
            if not stat.S_ISREG(published.st_mode):
                _abort("published output is not regular")
            if destination is not None and _read_output_at(
                parent.descriptor, temporary, "output rollback candidate"
            ) != t.cast(bytes, prior_contents):
                _abort("output rollback candidate differs")
            if checkpoint is not None:
                checkpoint(temporary_path if destination is not None else None)
            _verify_bound_directory(parent, "output parent changed during publication")
            if _read_output_at(parent.descriptor, name, "published output") != contents:
                _abort("published output bytes differ")
            if finalize is not None:
                finalize()
            os.fsync(parent.descriptor)
            if destination is not None:
                os.unlink(temporary, dir_fd=parent.descriptor)
                temporary_created = False
            committed = True
        except BaseException:
            try:
                if destination is None:
                    os.unlink(name, dir_fd=parent.descriptor)
                else:
                    _rename_exchange(parent.descriptor, temporary, name)
                if destination is not None and _read_output_at(
                    parent.descriptor, name, "restored output"
                ) != t.cast(bytes, prior_contents):
                    _abort("output rollback did not restore prior bytes")
                os.fsync(parent.descriptor)
            except BaseException as rollback_error:
                message = "output rollback failed"
                raise GateError(message) from rollback_error
            raise
    finally:
        if temporary_created:
            if committed:
                with contextlib.suppress(OSError):
                    os.unlink(temporary, dir_fd=parent.descriptor)
            else:
                with contextlib.suppress(FileNotFoundError):
                    os.unlink(temporary, dir_fd=parent.descriptor)
        if lock_acquired:
            if committed:
                with contextlib.suppress(OSError):
                    fcntl.flock(parent.descriptor, fcntl.LOCK_UN)
            else:
                fcntl.flock(parent.descriptor, fcntl.LOCK_UN)


@contextlib.contextmanager
def _bound_artifact(
    path: pathlib.Path,
    *,
    label: str,
    allow_soname_link: bool,
) -> t.Iterator[_BoundRegular]:
    """Bind one compiler-selected artifact through its permitted path shape.

    Examples
    --------
    >>> with _bound_artifact(
    ...     pathlib.Path(__file__), label="module", allow_soname_link=False
    ... ) as bound:
    ...     b"aggregate Task 6" in bound.contents
    True
    """
    system_roots = tuple(
        pathlib.Path(value) for value in ("/lib", "/lib64", "/usr/lib", "/usr/lib64")
    )
    installed_alias = allow_soname_link and any(
        path.is_relative_to(root) for root in system_roots
    )
    if installed_alias:
        binding = _bound_installed_regular(path, label=label)
    else:
        binding = _bound_regular(
            path,
            label=label,
            allow_leaf_symlinks=allow_soname_link,
        )
    with binding as bound:
        yield bound


@contextlib.contextmanager
def _selected_executable(
    path: pathlib.Path,
    *,
    label: str,
) -> t.Iterator[_BoundRegular]:
    """Retain one executable while permitting verified installed aliases.

    Examples
    --------
    >>> with _selected_executable(
    ...     pathlib.Path(sys.executable), label="python executable"
    ... ) as bound:
    ...     bool(bound.contents)
    True
    """
    with _bound_installed_regular(path, label=label) as bound:
        _verify_bound_regular(
            bound,
            changed=f"{label} changed while running",
            require_executable=True,
            require_single_link=True,
        )
        try:
            yield bound
        finally:
            _verify_bound_regular(
                bound,
                changed=f"{label} changed while running",
                require_executable=True,
                require_single_link=True,
            )


def _selected_tool_path(name: str) -> pathlib.Path:
    """Resolve one logical PATH tool before binding it.

    Examples
    --------
    >>> _selected_tool_path("python").is_absolute()
    True
    """
    selected = shutil.which(name)
    if selected is None:
        _abort(f"missing {name} executable")
    return pathlib.Path(selected).absolute()


def _path_resolves_selected_tools(
    value: str,
    aliases: dict[str, pathlib.Path],
) -> bool:
    """Return whether PATH resolves every logical name to its retained alias.

    Examples
    --------
    >>> selected = pathlib.Path(t.cast(str, shutil.which("python"))).absolute()
    >>> _path_resolves_selected_tools(os.environ["PATH"], {"python": selected})
    True
    """
    for name, alias in aliases.items():
        selected = shutil.which(name, path=value)
        if (
            selected is None
            or pathlib.Path(os.path.normpath(selected)).absolute() != alias
        ):
            return False
    return True


def _controlled_tool_path(
    aliases: dict[str, pathlib.Path],
    inherited: str,
) -> str:
    """Build a PATH whose logical names resolve to exact retained aliases.

    Examples
    --------
    >>> selected = pathlib.Path(t.cast(str, shutil.which("python"))).absolute()
    >>> controlled = _controlled_tool_path({"python": selected}, os.environ["PATH"])
    >>> _path_resolves_selected_tools(controlled, {"python": selected})
    True
    """
    parents = tuple(dict.fromkeys(alias.parent for alias in aliases.values()))
    for ordering in itertools.permutations(parents):
        entries = [os.fspath(parent) for parent in ordering]
        if inherited:
            entries.append(inherited)
        candidate = os.pathsep.join(entries)
        if _path_resolves_selected_tools(candidate, aliases):
            return candidate
    _abort("selected tool aliases cannot share one controlled PATH")


@contextlib.contextmanager
def _selected_tmux() -> t.Iterator[_BoundRegular]:
    """Open and retain the selected regular tmux executable for one gate run.

    Examples
    --------
    >>> with _selected_tmux() as bound:
    ...     bound.path.name
    'tmux'
    """
    selected = shutil.which("tmux")
    if selected is None:
        _abort("missing tmux executable")
    path = pathlib.Path(selected).absolute()
    with _bound_regular(
        path,
        label="tmux executable",
        allow_leaf_symlinks=False,
        require_executable=True,
        require_single_link=True,
    ) as bound:
        yield bound


def _verify_selected_tmux(
    bound: _BoundRegular,
) -> None:
    """Require the selected tmux pathname and open bytes to remain identical.

    Examples
    --------
    >>> with _selected_tmux() as bound:
    ...     _verify_selected_tmux(bound)
    """
    _verify_bound_regular(
        bound,
        changed="selected tmux executable changed",
        require_executable=True,
        require_single_link=True,
    )


def _normalize_tmux_version(stdout: bytes) -> str:
    r"""Validate the raw single-line tmux version identity.

    Examples
    --------
    >>> _normalize_tmux_version(b"tmux 3.7b\n")
    'tmux 3.7b'
    """
    try:
        version = stdout.decode("utf-8").strip()
    except UnicodeDecodeError:
        _abort("invalid tmux version")
    if re.fullmatch(r"tmux [^\r\n]+", version) is None:
        _abort("invalid tmux version")
    return version


def _normalize_ctest_version(stdout: bytes, cmake_version: str) -> str:
    r"""Require CTest to come from the selected pinned CMake suite.

    Examples
    --------
    >>> raw = (
    ...     b"ctest version 3.28.3\n\n"
    ...     b"CMake suite maintained and supported by Kitware (kitware.com/cmake).\n"
    ... )
    >>> _normalize_ctest_version(raw, "3.28.3")
    'ctest version 3.28.3'
    """
    try:
        text = stdout.decode("utf-8")
    except UnicodeDecodeError:
        _abort("invalid ctest version")
    match = re.fullmatch(
        (
            r"ctest version ([0-9]+(?:\.[0-9]+)*)"
            r"(?:"
            r"(?P<line_ending>\r\n|\n)"
            r"(?:"
            r"(?P=line_ending)"
            r"CMake suite maintained and supported by Kitware "
            r"\(kitware\.com/cmake\)\."
            r"(?P=line_ending)?"
            r")?"
            r")?"
        ),
        text,
    )
    if match is None:
        _abort("invalid ctest version")
    version = match.group(1)
    if version != cmake_version:
        _abort("unexpected ctest version")
    return f"ctest version {version}"


def _review_binding(
    root: pathlib.Path,
    path: pathlib.Path,
    *,
    source_sha256: str,
    core_sha256: str,
) -> tuple[dict[str, str], bytes]:
    r"""Validate an independent review against one source and evidence core.

    Examples
    --------
    >>> digest = _sha256(b"")
    >>> body = "\n".join(
    ...     (
    ...         "Verdict: Ready",
    ...         "Unresolved findings: 0",
    ...         f"Reviewed source: {digest}",
    ...         f"Evidence core: {digest}",
    ...     )
    ... )
    >>> import tempfile
    >>> with tempfile.TemporaryDirectory() as raw:
    ...     root = pathlib.Path(raw)
    ...     review = root / "evidence-review.md"
    ...     _ = review.write_text(body, encoding="utf-8")
    ...     result, _ = _review_binding(
    ...         root, review, source_sha256=digest, core_sha256=digest
    ...     )
    >>> result["status"]
    'passed'
    """
    if not path.is_relative_to(root):
        _abort("review path escapes repository")
    _reject_source_symlinked_ancestors(root, path)
    try:
        contents = _read_source_regular(path, "review document")
        text = contents.decode("utf-8")
    except UnicodeDecodeError:
        _abort("invalid review document")
    required = (
        "Verdict: Ready",
        "Unresolved findings: 0",
        f"Reviewed source: {source_sha256}",
        f"Evidence core: {core_sha256}",
    )
    lines = text.splitlines()
    machine_lines = {
        prefix: [line for line in lines if line.startswith(prefix)]
        for prefix in (
            "Verdict:",
            "Unresolved findings:",
            "Reviewed source:",
            "Evidence core:",
        )
    }
    open_marker = re.compile(
        r"^\s*(?:[-*]\s*)?(?:still open|status|disposition|finding status):"
        r"\s*(?:open|unresolved|blocked|not ready)\s*$",
        flags=re.IGNORECASE,
    )
    if (
        any(lines.count(line) != 1 for line in required)
        or any(
            machine_lines[line.split(":", 1)[0] + ":"] != [line] for line in required
        )
        or any(open_marker.fullmatch(line) for line in lines)
    ):
        _abort("review binding differs from candidate evidence")
    _public_values(text)
    return {"status": "passed", "sha256": _sha256(contents)}, contents


def run(namespace: argparse.Namespace) -> dict[str, object]:
    """Run the ordered aggregate evidence commands and publish their projection.

    Examples
    --------
    >>> namespace = argparse.Namespace(
    ...     output=pathlib.Path("not-contract-evidence.json"), review=None
    ... )
    >>> try:
    ...     run(namespace)
    ... except GateError as error:
    ...     str(error)
    'output path must be the canonical contract evidence destination'
    """
    if namespace.output != _OUTPUT_PATH:
        _abort("output path must be the canonical contract evidence destination")
    root = pathlib.Path.cwd()
    git_environment = _git_environment()
    with contextlib.ExitStack() as binding_stack:
        selected_git = binding_stack.enter_context(
            _selected_executable(
                _selected_tool_path("git"),
                label="git executable",
            )
        )
        selected_tmux = binding_stack.enter_context(_selected_tmux())
        return _run_aggregate_with_compilers(
            namespace,
            root=root,
            git_environment=git_environment,
            selected_git=selected_git,
            selected_tmux=selected_tmux,
            compiler_stack=binding_stack,
        )


def _run_aggregate_with_compilers(
    namespace: argparse.Namespace,
    *,
    root: pathlib.Path,
    git_environment: dict[str, str],
    selected_git: _BoundRegular,
    selected_tmux: _BoundRegular,
    compiler_stack: contextlib.ExitStack,
) -> dict[str, object]:
    """Execute an aggregate while retaining every configured compiler binding.

    Examples
    --------
    >>> namespace = argparse.Namespace(
    ...     output=pathlib.Path("not-contract-evidence.json"), review=None
    ... )
    >>> missing = t.cast(_BoundRegular, None)
    >>> try:
    ...     _run_aggregate_with_compilers(
    ...         namespace,
    ...         root=pathlib.Path.cwd(),
    ...         git_environment={},
    ...         selected_git=missing,
    ...         selected_tmux=missing,
    ...         compiler_stack=contextlib.ExitStack(),
    ...     )
    ... except GateError as error:
    ...     str(error)
    'output path must be the canonical contract evidence destination'
    """
    if namespace.output != _OUTPUT_PATH:
        _abort("output path must be the canonical contract evidence destination")
    tmux_path = selected_tmux.path
    tmux_contents = selected_tmux.contents
    tmux_sha256 = _sha256(tmux_contents)
    output = (root / _OUTPUT_PATH).absolute()
    review_path = _review_path(output, namespace.review)
    git_path = selected_git.path
    commit, tree = _git_identity(
        root,
        executable=git_path,
        environment=git_environment,
    )
    source_snapshot = _source_snapshot(
        root,
        excluded=output,
        git=git_path,
        git_environment=git_environment,
    )
    reviewable_snapshot = _source_snapshot(
        root,
        excluded=output,
        additionally_excluded=(review_path,),
        git=git_path,
        git_environment=git_environment,
    )
    python_contract = _python_contract(root)
    tool_pins = _tool_pins(root)
    regular_checkpoints = [
        _RegularCheckpoint(
            selected_git,
            "git executable changed while running",
            True,
            True,
        ),
        _RegularCheckpoint(
            selected_tmux,
            "selected tmux executable changed",
            True,
            True,
        ),
    ]
    ctest_file_bindings: list[_CTestGateFiles] = []
    selected_tools: dict[str, _BoundRegular] = {}
    selected_tool_aliases: dict[str, pathlib.Path] = {}
    for tool_name in ("just", "uv", "cmake", "ninja"):
        selected_alias = _selected_tool_path(tool_name)
        selected = compiler_stack.enter_context(
            _selected_executable(
                selected_alias,
                label=f"{tool_name} executable",
            )
        )
        selected_tool_aliases[tool_name] = selected_alias
        selected_tools[tool_name] = selected
        regular_checkpoints.append(
            _RegularCheckpoint(
                selected,
                f"{tool_name} executable changed while running",
                True,
                True,
            )
        )
    clangxx_alias = _selected_tool_path("clang++")
    clang_alias = clangxx_alias.with_name("clang")
    selected_compilers: dict[str, _BoundRegular] = {}
    for name, alias in (("clang++", clangxx_alias), ("clang", clang_alias)):
        selected = compiler_stack.enter_context(
            _selected_executable(alias, label=f"{name} executable")
        )
        selected_compilers[name] = selected
        regular_checkpoints.append(
            _RegularCheckpoint(
                selected,
                f"{name} executable changed while running",
                True,
                True,
            )
        )
    if (
        not os.path.samestat(
            selected_compilers["clang++"].metadata,
            selected_compilers["clang"].metadata,
        )
        or selected_compilers["clang++"].contents
        != selected_compilers["clang"].contents
    ):
        _abort("selected C and C++ compiler identities differ")
    selected_python = compiler_stack.enter_context(
        _selected_executable(
            pathlib.Path(sys.executable).absolute(),
            label="python executable",
        )
    )
    regular_checkpoints.append(
        _RegularCheckpoint(
            selected_python,
            "python executable changed while running",
            True,
            True,
        )
    )
    selected_ctest = compiler_stack.enter_context(
        _selected_executable(
            selected_tools["cmake"].path.with_name("ctest"),
            label="ctest executable",
        )
    )
    regular_checkpoints.append(
        _RegularCheckpoint(
            selected_ctest,
            "ctest executable changed while running",
            True,
            True,
        )
    )
    command_paths = {
        name: os.fspath(bound.path) for name, bound in selected_tools.items()
    }
    command_paths[sys.executable] = os.fspath(selected_python.path)
    inherited_path = os.environ.get("PATH", "")
    configure_path = _controlled_tool_path(
        {"clang++": clangxx_alias, "clang": clang_alias},
        inherited_path,
    )
    docs_path = _controlled_tool_path(
        {
            "just": selected_tool_aliases["just"],
            "uv": selected_tool_aliases["uv"],
        },
        inherited_path,
    )

    def verify_common_bindings() -> None:
        _verify_regular_checkpoints(regular_checkpoints)
        for binding in ctest_file_bindings:
            _verify_ctest_gate_files(root, binding)

    def verify_command_checkpoint() -> None:
        _verify_git_head(
            root,
            commit,
            executable=git_path,
            environment=git_environment,
        )
        if (
            _source_snapshot(
                root,
                excluded=output,
                git=git_path,
                git_environment=git_environment,
            )
            != source_snapshot
        ):
            _abort("source changed during aggregate gate")
        verify_common_bindings()

    def checkpointed_command(
        argv: list[str],
        *,
        name: str,
        cwd: pathlib.Path,
        environment: dict[str, str] | None = None,
        record_version: bool = False,
    ) -> dict[str, object]:
        verify_command_checkpoint()
        invoked_argv = list(argv)
        invoked_argv[0] = command_paths.get(invoked_argv[0], invoked_argv[0])
        command = _required_command(
            invoked_argv,
            name=name,
            cwd=cwd,
            environment=environment,
            record_version=record_version,
        )
        verify_command_checkpoint()
        return command

    commands: list[dict[str, object]] = []
    tool_versions: dict[str, str] = {}
    for program, argv in (
        ("tool.just", ["just", "--version"]),
        ("tool.uv", ["uv", "--version"]),
        ("tool.python", [sys.executable, "--version"]),
        ("tool.cmake", ["cmake", "--version"]),
        ("tool.ninja", ["ninja", "--version"]),
        ("tool.tmux", [str(tmux_path), "-V"]),
    ):
        command = checkpointed_command(
            argv,
            name=program,
            cwd=root,
            record_version=True,
        )
        tool = program.removeprefix("tool.")
        if tool == "tmux":
            command["version"] = _normalize_tmux_version(
                t.cast(str, command["version"]).encode()
            )
            _verify_selected_tmux(selected_tmux)
        else:
            command["version"] = _normalize_tool_version(
                tool,
                t.cast(str, command["version"]).encode(),
                tool_pins[tool],
            )
        tool_versions[tool] = t.cast(str, command["version"])
        commands.append(command)
    cmake_pins = tool_pins["cmake"]
    if len(cmake_pins) != 1:
        _abort("cmake requires exactly one version pin")
    ctest_command = checkpointed_command(
        [os.fspath(selected_ctest.path), "--version"],
        name="toolchain.ctest",
        cwd=root,
        record_version=True,
    )
    ctest_command["version"] = _normalize_ctest_version(
        t.cast(str, ctest_command["version"]).encode(),
        cmake_pins[0],
    )
    commands.append(ctest_command)

    ctest_gates: list[dict[str, object]] = []
    compiler: _BoundRegular | None = None
    compiler_identity: dict[str, str] | None = None
    configured_ninja: _BoundRegular | None = None
    artifacts: list[dict[str, str]] = []
    libcxx_version: int | None = None
    for preset, selector, selector_value, gate_id in _CTEST_GATES:
        cxx = root / "cxx"
        configure_environment = dict(os.environ)
        configure_environment["PATH"] = configure_path
        if not _path_resolves_selected_tools(
            configure_path,
            {"clang++": clangxx_alias, "clang": clang_alias},
        ):
            _abort("configured compiler aliases changed before configure")
        commands.append(
            checkpointed_command(
                [
                    "cmake",
                    "--preset",
                    preset,
                    f"-DCMAKE_CXX_COMPILER:FILEPATH={clangxx_alias}",
                    f"-DCMAKE_C_COMPILER:FILEPATH={clang_alias}",
                ],
                name=f"configure.{preset}",
                cwd=cxx,
                environment=configure_environment,
            )
        )
        if not _path_resolves_selected_tools(
            configure_path,
            {"clang++": clangxx_alias, "clang": clang_alias},
        ):
            _abort("configured compiler aliases changed during configure")
        cache_binding = compiler_stack.enter_context(
            _bound_regular(
                root / "cxx" / "build" / preset / "CMakeCache.txt",
                label=f"configured CMake cache for {preset}",
                allow_leaf_symlinks=False,
                require_single_link=True,
            )
        )
        regular_checkpoints.append(
            _RegularCheckpoint(
                cache_binding,
                f"configured CMake cache for {preset} changed while reading",
                False,
                True,
            )
        )
        configured_paths = _configured_build_tool_paths(
            cache_binding.contents,
            preset=preset,
        )
        configured_cmake = compiler_stack.enter_context(
            _selected_executable(
                configured_paths["cmake"],
                label=f"configured CMake for {preset}",
            )
        )
        configured_ctest = compiler_stack.enter_context(
            _selected_executable(
                configured_paths["ctest"],
                label=f"configured CTest for {preset}",
            )
        )
        for label, bound in (
            (f"configured CMake for {preset}", configured_cmake),
            (f"configured CTest for {preset}", configured_ctest),
        ):
            regular_checkpoints.append(
                _RegularCheckpoint(
                    bound,
                    f"{label} changed while running",
                    True,
                    True,
                )
            )
        if (
            not os.path.samestat(
                configured_cmake.metadata,
                selected_tools["cmake"].metadata,
            )
            or configured_cmake.contents != selected_tools["cmake"].contents
            or not os.path.samestat(
                configured_ctest.metadata,
                selected_ctest.metadata,
            )
            or configured_ctest.contents != selected_ctest.contents
        ):
            _abort(f"configured CMake suite differs for {preset}")
        preset_ninja = compiler_stack.enter_context(
            _selected_executable(
                configured_paths["ninja"],
                label=f"configured Ninja for {preset}",
            )
        )
        regular_checkpoints.append(
            _RegularCheckpoint(
                preset_ninja,
                f"configured Ninja for {preset} changed while running",
                True,
                True,
            )
        )
        if configured_ninja is None:
            configured_ninja = preset_ninja
        elif (
            not os.path.samestat(configured_ninja.metadata, preset_ninja.metadata)
            or configured_ninja.contents != preset_ninja.contents
        ):
            _abort(f"configured Ninja differs for {preset}")
        ninja_command = checkpointed_command(
            [os.fspath(preset_ninja.path), "--version"],
            name=f"toolchain.ninja.{preset}",
            cwd=root,
            record_version=True,
        )
        ninja_version = _normalize_tool_version(
            "ninja",
            t.cast(str, ninja_command["version"]).encode(),
            tool_pins["ninja"],
        )
        if ninja_version != tool_versions["ninja"]:
            _abort(f"configured Ninja version differs for {preset}")
        ninja_command["version"] = ninja_version
        commands.append(ninja_command)
        configured_compiler, configured_identity = compiler_stack.enter_context(
            _configured_compiler_identity(
                root,
                preset=preset,
                cmake_version=cmake_pins[0],
                clang_pins=tool_pins["clang"],
            )
        )
        regular_checkpoints.append(
            _RegularCheckpoint(
                configured_compiler,
                f"configured C++ compiler for {preset} changed while reading",
                True,
                True,
            )
        )
        configured_c_compiler, configured_c_identity = compiler_stack.enter_context(
            _configured_compiler_identity(
                root,
                preset=preset,
                cmake_version=cmake_pins[0],
                clang_pins=tool_pins["clang"],
                language="C",
            )
        )
        regular_checkpoints.append(
            _RegularCheckpoint(
                configured_c_compiler,
                f"configured C compiler for {preset} changed while reading",
                True,
                True,
            )
        )
        if (
            not os.path.samestat(
                configured_compiler.metadata,
                selected_compilers["clang++"].metadata,
            )
            or configured_compiler.contents != selected_compilers["clang++"].contents
        ):
            _abort(f"configured C++ compiler differs for {preset}")
        if (
            not os.path.samestat(
                configured_c_compiler.metadata,
                selected_compilers["clang"].metadata,
            )
            or configured_c_compiler.contents != selected_compilers["clang"].contents
        ):
            _abort(f"configured C compiler differs for {preset}")
        first_compiler = compiler is None
        if first_compiler:
            compiler = configured_compiler
            compiler_identity = configured_identity
            artifacts.append({"name": "clang++", "sha256": _sha256(compiler.contents)})
            _verify_bound_regular(
                compiler,
                changed="configured C++ compiler changed while running",
                require_executable=True,
                require_single_link=True,
            )
            clang_command = checkpointed_command(
                [os.fspath(clangxx_alias), "--version"],
                name="tool.clang",
                cwd=root,
                record_version=True,
            )
            _verify_bound_regular(
                compiler,
                changed="configured C++ compiler changed while running",
                require_executable=True,
                require_single_link=True,
            )
            clang_version = _normalize_tool_version(
                "clang",
                t.cast(str, clang_command["version"]).encode(),
                tool_pins["clang"],
            )
            clang_command["version"] = clang_version
            if clang_version != f"clang version {configured_identity['version']}":
                _abort("configured C++ compiler runtime identity differs")
            tool_versions["clang"] = clang_version
            commands.append(clang_command)
            if clang_version != f"clang version {configured_c_identity['version']}":
                _abort("configured C compiler runtime identity differs")
        else:
            if compiler is None:
                _abort("missing configured C++ compiler identity")
            if (
                not os.path.samestat(configured_compiler.metadata, compiler.metadata)
                or configured_compiler.contents != compiler.contents
                or configured_identity != compiler_identity
            ):
                _abort(f"configured C++ compiler differs for {preset}")
        commands.append(
            checkpointed_command(
                ["cmake", "--build", "--preset", preset],
                name=f"build.{preset}",
                cwd=cxx,
            )
        )
        if first_compiler:
            if compiler is None:
                _abort("missing configured C++ compiler identity")
            for command_name, logical_name in (
                ("libcxx_config", "include/c++/v1/__config"),
                ("libcxx", "libc++.so.1"),
                ("libcxxabi", "libc++abi.so.1"),
            ):
                _verify_bound_regular(
                    compiler,
                    changed="configured C++ compiler changed while running",
                    require_executable=True,
                    require_single_link=True,
                )
                artifact_command = checkpointed_command(
                    [os.fspath(clangxx_alias), f"-print-file-name={logical_name}"],
                    name=f"toolchain.{command_name}",
                    cwd=root,
                    record_version=True,
                )
                _verify_bound_regular(
                    compiler,
                    changed="configured C++ compiler changed while running",
                    require_executable=True,
                    require_single_link=True,
                )
                artifact_path = pathlib.Path(t.cast(str, artifact_command["version"]))
                label = "libc++abi" if command_name == "libcxxabi" else "libc++"
                if (
                    artifact_path == pathlib.Path(logical_name)
                    or not artifact_path.is_absolute()
                ):
                    _abort(f"invalid {label} artifact path")
                artifact_path = pathlib.Path(os.path.normpath(artifact_path))
                artifact_label = (
                    "libc++ configuration header"
                    if command_name == "libcxx_config"
                    else f"{label} soname"
                )
                artifact_binding = compiler_stack.enter_context(
                    _bound_artifact(
                        artifact_path,
                        label=artifact_label,
                        allow_soname_link=command_name != "libcxx_config",
                    )
                )
                regular_checkpoints.append(
                    _RegularCheckpoint(
                        artifact_binding,
                        f"{artifact_label} changed while reading",
                        False,
                        False,
                    )
                )
                contents = artifact_binding.contents
                artifacts.append(
                    {
                        "name": (
                            "libcxx.__config"
                            if command_name == "libcxx_config"
                            else logical_name
                        ),
                        "sha256": _sha256(contents),
                    }
                )
                if command_name == "libcxx_config":
                    version_matches = re.findall(
                        rb"^\s*#\s*define\s+_LIBCPP_VERSION\s+([0-9]+)\s*$",
                        contents,
                        flags=re.MULTILINE,
                    )
                    if len(version_matches) != 1:
                        _abort("unexpected libc++ configuration version")
                    libcxx_version = int(version_matches[0])
                    if libcxx_version != 180100:
                        _abort("unexpected libc++ configuration version")
                artifact_command.pop("version")
                commands.append(artifact_command)
        ctest_environment = dict(os.environ)
        ctest_environment["PATH"] = os.pathsep.join(
            (
                os.fspath(tmux_path.parent),
                os.fspath(selected_ctest.path.parent),
                ctest_environment.get("PATH", ""),
            )
        )
        ctest_environment["LIBTMUX_CTEST_GATE_CTEST"] = os.fspath(selected_ctest.path)
        commands.append(
            checkpointed_command(
                [
                    sys.executable,
                    "-m",
                    "cxx.tools.evidence.ctest_gate",
                    "--source-dir",
                    "cxx",
                    "--preset",
                    preset,
                    selector,
                    selector_value,
                    "--gate-id",
                    gate_id,
                    "--output-root",
                    "cxx/build/evidence/ctest",
                    "--record",
                    f"cxx/build/evidence/{gate_id}.json",
                ],
                name=f"ctest.{gate_id}",
                cwd=root,
                environment=ctest_environment,
            )
        )
        _verify_selected_tmux(selected_tmux)
        ctest_gate = _ctest_projection(
            root,
            gate_id,
            preset=preset,
            selector={selector.removeprefix("--"): selector_value},
        )
        ctest_file_bindings.append(
            _capture_ctest_gate_files(
                root,
                gate_id,
                t.cast(str, ctest_gate["gate_sha256"]),
            )
        )
        verify_command_checkpoint()
        ctest_gate["tmux"] = {
            "sha256": tmux_sha256,
            "version": tool_versions["tmux"],
        }
        ctest_gates.append(ctest_gate)

    for name, argv in (
        (
            "parity.generate",
            [
                "uv",
                "run",
                "python",
                "-m",
                "cxx.tools.parity",
                "generate",
                "--check",
                "cxx/parity",
            ],
        ),
        (
            "parity.drift",
            [
                "uv",
                "run",
                "python",
                "-m",
                "cxx.tools.parity",
                "drift",
                "--manifest",
                "cxx/parity/manifest.json",
                "--worktree",
                ".",
            ],
        ),
        (
            "parity.verify",
            [
                "uv",
                "run",
                "python",
                "-m",
                "cxx.tools.parity",
                "verify",
                "--manifest",
                "cxx/parity/manifest.json",
                "--mode",
                "structural",
                "--allow-pending",
            ],
        ),
        ("ruff.format", ["uv", "run", "ruff", "format", "--check", "."]),
    ):
        commands.append(checkpointed_command(argv, name=name, cwd=root))
    pytest_environment = dict(os.environ)
    pytest_environment.pop("__MISE_ZSH_ACTIVATE_PATH", None)
    pytest_environment.pop("__MISE_ORIG_PATH", None)
    commands.append(
        checkpointed_command(
            ["uv", "run", "pytest"],
            name="pytest.initial",
            cwd=root,
            environment=pytest_environment,
        )
    )
    for name, argv in (
        ("ruff.check", ["uv", "run", "ruff", "check", "."]),
        ("mypy", ["uv", "run", "mypy"]),
        (
            "doctest",
            [
                "uv",
                "run",
                "pytest",
                "--doctest-modules",
                "cxx/tools",
                "cxx/tests/differential",
            ],
        ),
    ):
        commands.append(checkpointed_command(argv, name=name, cwd=root))
    docs_environment = dict(os.environ)
    docs_environment["PATH"] = docs_path
    docs_aliases = {
        "just": selected_tool_aliases["just"],
        "uv": selected_tool_aliases["uv"],
    }
    if not _path_resolves_selected_tools(docs_path, docs_aliases):
        _abort("docs tool aliases changed before documentation build")
    commands.append(
        checkpointed_command(
            ["just", "build-docs"],
            name="docs",
            cwd=root,
            environment=docs_environment,
        )
    )
    if not _path_resolves_selected_tools(docs_path, docs_aliases):
        _abort("docs tool aliases changed during documentation build")
    commands.append(
        checkpointed_command(
            ["uv", "run", "pytest"],
            name="pytest.final",
            cwd=root,
            environment=pytest_environment,
        )
    )

    if (
        _source_snapshot(
            root,
            excluded=output,
            git=git_path,
            git_environment=git_environment,
        )
        != source_snapshot
    ):
        _abort("source changed during aggregate gate")
    _verify_git_head(
        root,
        commit,
        executable=git_path,
        environment=git_environment,
    )
    if libcxx_version is None:
        _abort("unexpected libc++ configuration version")
    if compiler_identity is None:
        _abort("missing configured C++ compiler identity")
    artifacts.append({"name": "tmux", "sha256": tmux_sha256})
    source_digest = t.cast(str, reviewable_snapshot["sha256"])
    record: dict[str, object] = {
        "schema_version": 1,
        "status": "passed",
        "gate_source": {
            "commit": commit,
            "tree": tree,
            "source_snapshot": source_snapshot,
        },
        "reviewable_source_sha256": source_digest,
        "python_contract": python_contract,
        "tools": {
            "pins": tool_pins,
            "versions": tool_versions,
        },
        "toolchain": {
            "compiler": compiler_identity,
            "libcxx_version": libcxx_version,
            "artifacts": artifacts,
        },
        "ctest_gates": ctest_gates,
        "commands": commands,
        "claims": {
            "proved": _PROVED_CLAIMS,
            "not_proved": _UNPROVED_CLAIMS,
        },
        "core_excluded_fields": _CORE_EXCLUDED_FIELDS,
        "review": {"status": "pending"},
    }
    record["evidence_core_sha256"] = _sha256(_canonical_bytes(_evidence_core(record)))
    review_contents: bytes | None = None
    if namespace.review is not None:
        review, review_contents = _review_binding(
            root,
            review_path,
            source_sha256=source_digest,
            core_sha256=t.cast(str, record["evidence_core_sha256"]),
        )
        record["review"] = review
    record["final_evidence_sha256"] = _sha256(
        _canonical_bytes(_final_projection(record))
    )
    validate_evidence(record)

    def publication_checkpoint(temporary: pathlib.Path | None) -> None:
        _verify_git_head(
            root,
            commit,
            executable=git_path,
            environment=git_environment,
        )
        transient = () if temporary is None else (temporary,)
        if (
            _source_snapshot(
                root,
                excluded=output,
                additionally_excluded=transient,
                git=git_path,
                git_environment=git_environment,
            )
            != source_snapshot
            or _source_snapshot(
                root,
                excluded=output,
                additionally_excluded=(review_path, *transient),
                git=git_path,
                git_environment=git_environment,
            )
            != reviewable_snapshot
        ):
            _abort("source changed during aggregate gate")
        if review_contents is not None:
            _, final_review_contents = _review_binding(
                root,
                review_path,
                source_sha256=source_digest,
                core_sha256=t.cast(str, record["evidence_core_sha256"]),
            )
            if final_review_contents != review_contents:
                _abort("review changed during aggregate gate")
        verify_common_bindings()

    _atomic_output(
        output,
        _canonical_bytes(record),
        checkpoint=publication_checkpoint,
        finalize=compiler_stack.close,
    )
    return record


def _public_values(value: object) -> None:
    """Reject physical paths and personal identifiers in public evidence.

    Examples
    --------
    >>> _public_values({"name": "tool.just"})
    """
    if isinstance(value, str):
        if (
            "file://" in value
            or "/home/" in value
            or "/Users/" in value
            or _DRIVE_PATH.search(value)
            or _UNIX_PATH.search(value)
            or _EMAIL.search(value)
        ):
            _abort("evidence contains a private or physical path")
        return
    if isinstance(value, dict):
        for key, item in value.items():
            if not isinstance(key, str):
                _abort("evidence has a non-string key")
            _public_values(item)
        return
    if isinstance(value, list):
        for item in value:
            _public_values(item)
        return
    if value is None or type(value) in {bool, int, float}:
        return
    _abort("evidence has an unsupported value")


def _validate_python_contract(value: object) -> None:
    """Validate the pinned public Python parity-source identity.

    Examples
    --------
    >>> try:
    ...     _validate_python_contract({})
    ... except GateError as error:
    ...     str(error)
    'invalid Python contract'
    """
    if not isinstance(value, dict) or set(value) != {
        "manifest_sha256",
        "inputs_sha256",
        "semantic_contract_sha256",
        "release",
        "development",
        "sidecar_sha256",
    }:
        _abort("invalid Python contract")
    for field in ("manifest_sha256", "inputs_sha256", "semantic_contract_sha256"):
        digest = value.get(field)
        if not isinstance(digest, str) or not _DIGEST.fullmatch(digest):
            _abort("invalid Python contract")

    def observation(identity: object, *, observation_id: str, revision: str) -> None:
        if not isinstance(identity, dict) or set(identity) != {
            "observation_id",
            "revision",
            "commit",
            "tree",
            "sidecar_sha256",
        }:
            _abort("invalid Python contract")
        commit = identity.get("commit")
        tree = identity.get("tree")
        digest = identity.get("sidecar_sha256")
        if not (
            identity.get("observation_id") == observation_id
            and identity.get("revision") == revision
            and isinstance(commit, str)
            and _OBJECT_ID.fullmatch(commit)
            and isinstance(tree, str)
            and _OBJECT_ID.fullmatch(tree)
            and isinstance(digest, str)
            and _DIGEST.fullmatch(digest)
        ):
            _abort("invalid Python contract")

    observation(
        value.get("release"),
        observation_id="release-v0.62.0",
        revision="v0.62.0",
    )
    observation(
        value.get("development"),
        observation_id="development",
        revision="HEAD",
    )
    sidecars = value.get("sidecar_sha256")
    if not isinstance(sidecars, dict) or set(sidecars) != {
        "approvals",
        "evidence",
        "mapping",
        "shards",
    }:
        _abort("invalid Python contract")
    if any(
        not isinstance(digest, str) or not _DIGEST.fullmatch(digest)
        for digest in sidecars.values()
    ):
        _abort("invalid Python contract")


def _validate_ctest_gate_matrix(value: object) -> list[dict[str, object]]:
    """Validate the exact passing CTest matrix and semantic projections.

    Examples
    --------
    >>> try:
    ...     _validate_ctest_gate_matrix([])
    ... except GateError as error:
    ...     str(error)
    'invalid CTest gate matrix'
    """
    if not isinstance(value, list) or len(value) != len(_CTEST_GATES):
        _abort("invalid CTest gate matrix")
    gates: list[dict[str, object]] = []
    expected_fields = {
        "compiler",
        "execution_sha256",
        "executed_test_ids",
        "fixture_binding",
        "fixture_modes",
        "gate_id",
        "gate_sha256",
        "preset",
        "registered_test_ids",
        "selector",
        "status",
        "tmux",
    }
    for gate, (preset, selector_flag, selector_value, gate_id) in zip(
        value,
        _CTEST_GATES,
        strict=True,
    ):
        if not isinstance(gate, dict) or set(gate) != expected_fields:
            _abort("invalid CTest gate matrix")
        selector = {selector_flag.removeprefix("--"): selector_value}
        registered = gate.get("registered_test_ids")
        executed = gate.get("executed_test_ids")
        gate_digest = gate.get("gate_sha256")
        execution_digest = gate.get("execution_sha256")
        compiler = gate.get("compiler")
        tmux = gate.get("tmux")
        if not (
            gate.get("gate_id") == gate_id
            and gate.get("status") == "passed"
            and gate.get("preset") == preset
            and gate.get("selector") == selector
            and isinstance(registered, list)
            and bool(registered)
            and all(isinstance(item, str) and item for item in registered)
            and registered == sorted(set(registered))
            and executed == registered
            and isinstance(gate_digest, str)
            and _DIGEST.fullmatch(gate_digest)
            and isinstance(execution_digest, str)
            and _DIGEST.fullmatch(execution_digest)
            and isinstance(compiler, dict)
            and isinstance(tmux, dict)
            and set(tmux) == {"sha256", "version"}
            and isinstance(tmux.get("sha256"), str)
            and _DIGEST.fullmatch(t.cast(str, tmux["sha256"]))
            and isinstance(tmux.get("version"), str)
        ):
            _abort("invalid CTest gate matrix")
        try:
            _validate_ctest_compiler(compiler)
        except GateError:
            _abort("invalid CTest gate matrix")
        if gate_id == "contract-dev":
            if gate.get("fixture_modes") != [] or gate.get("fixture_binding") != {}:
                _abort("invalid CTest gate matrix")
        elif (
            gate.get("fixture_modes") != ["name", "path"]
            or gate.get("fixture_binding") != _FIXTURE_BINDING
            or not set(_FIXTURE_BINDING.values()).issubset(registered)
        ):
            _abort("invalid CTest gate matrix")
        execution = {
            field: gate[field]
            for field in (
                "preset",
                "selector",
                "registered_test_ids",
                "executed_test_ids",
                "fixture_modes",
                "fixture_binding",
            )
        }
        if _sha256(_canonical_bytes(execution)) != execution_digest:
            _abort("invalid CTest gate matrix")
        gates.append(gate)
    return gates


def _validate_command_matrix(value: object) -> list[dict[str, object]]:
    """Validate the complete ordered public aggregate command projection.

    Examples
    --------
    >>> try:
    ...     _validate_command_matrix([])
    ... except GateError as error:
    ...     str(error)
    'invalid command matrix'
    """
    if not isinstance(value, list) or len(value) != len(_COMMAND_NAMES):
        _abort("invalid command matrix")
    commands: list[dict[str, object]] = []
    for command, expected_name in zip(value, _COMMAND_NAMES, strict=True):
        if (
            isinstance(command, dict)
            and type(command.get("exit_code")) is int
            and command.get("exit_code") != 0
        ):
            _abort("command did not pass")
        expected_fields = {"name", "cwd", "exit_code"}
        if expected_name in _VERSIONED_COMMANDS:
            expected_fields.add("version")
        expected_cwd = (
            "cxx" if expected_name.startswith(("configure.cxx-", "build.cxx-")) else "."
        )
        if not (
            isinstance(command, dict)
            and set(command) == expected_fields
            and command.get("name") == expected_name
            and command.get("cwd") == expected_cwd
            and type(command.get("exit_code")) is int
            and command.get("exit_code") == 0
            and (
                expected_name not in _VERSIONED_COMMANDS
                or isinstance(command.get("version"), str)
            )
        ):
            _abort("invalid command matrix")
        commands.append(command)
    return commands


def _validate_tool_identity(
    value: dict[str, object],
    gates: t.Sequence[object],
    commands: t.Sequence[object],
) -> None:
    """Validate pinned runtime, compiler, artifact, and CTest tmux bindings.

    Examples
    --------
    >>> try:
    ...     _validate_tool_identity({"tools": {}}, [], [])
    ... except GateError as error:
    ...     str(error)
    'invalid tool identity'
    """
    tools = value.get("tools")
    if not isinstance(tools, dict) or set(tools) != {"pins", "versions"}:
        _abort("invalid tool identity")
    pins = tools.get("pins")
    versions = tools.get("versions")
    if (
        not isinstance(pins, dict)
        or set(pins) != _REQUIRED_TOOLS
        or not isinstance(versions, dict)
        or set(versions) != _REQUIRED_TOOLS | {"tmux"}
    ):
        _abort("invalid tool identity")
    normalized: dict[str, str] = {}
    for name in sorted(_REQUIRED_TOOLS):
        declared = pins.get(name)
        version = versions.get(name)
        if not (
            isinstance(declared, list)
            and declared
            and all(
                isinstance(item, str) and re.fullmatch(r"[0-9]+(?:\.[0-9]+)*", item)
                for item in declared
            )
            and isinstance(version, str)
        ):
            _abort("invalid tool identity")
        try:
            normalized[name] = _normalize_tool_version(
                name,
                version.encode(),
                t.cast(list[str], declared),
            )
        except GateError:
            _abort("invalid tool identity")
        if normalized[name] != version:
            _abort("invalid tool identity")
    tmux_version = versions.get("tmux")
    if not isinstance(tmux_version, str):
        _abort("invalid tool identity")
    try:
        normalized_tmux = _normalize_tmux_version(tmux_version.encode())
    except GateError:
        _abort("invalid tool identity")
    if normalized_tmux != tmux_version:
        _abort("invalid tool identity")

    toolchain = value.get("toolchain")
    if not isinstance(toolchain, dict) or set(toolchain) != {
        "compiler",
        "libcxx_version",
        "artifacts",
    }:
        _abort("invalid tool identity")
    compiler = toolchain.get("compiler")
    if not isinstance(compiler, dict) or set(compiler) != {"id", "version"}:
        _abort("invalid tool identity")
    compiler_version = compiler.get("version")
    if (
        compiler.get("id") != "Clang"
        or not isinstance(compiler_version, str)
        or compiler_version not in t.cast(list[str], pins["clang"])
        or versions["clang"] != f"clang version {compiler_version}"
    ):
        _abort("invalid tool identity")
    if toolchain.get("libcxx_version") != 180100:
        _abort("invalid tool identity")
    artifacts = toolchain.get("artifacts")
    if not isinstance(artifacts, list) or len(artifacts) != 5:
        _abort("invalid tool identity")
    artifact_by_name: dict[str, str] = {}
    for artifact in artifacts:
        if not isinstance(artifact, dict) or set(artifact) != {"name", "sha256"}:
            _abort("invalid tool identity")
        artifact_name = artifact.get("name")
        digest = artifact.get("sha256")
        if (
            not isinstance(artifact_name, str)
            or artifact_name in artifact_by_name
            or not isinstance(digest, str)
            or not _DIGEST.fullmatch(digest)
        ):
            _abort("invalid tool identity")
        artifact_by_name[artifact_name] = digest
    if set(artifact_by_name) != {
        "clang++",
        "libcxx.__config",
        "libc++.so.1",
        "libc++abi.so.1",
        "tmux",
    }:
        _abort("invalid tool identity")
    tmux_binding = {
        "sha256": artifact_by_name["tmux"],
        "version": tmux_version,
    }
    if any(
        not isinstance(gate, dict) or gate.get("tmux") != tmux_binding for gate in gates
    ):
        _abort("invalid tool identity")
    gate_metadata: set[str] = set()
    for gate in gates:
        if not isinstance(gate, dict):
            _abort("invalid tool identity")
        gate_compiler = gate.get("compiler")
        if not isinstance(gate_compiler, dict):
            _abort("invalid tool identity")
        if (
            gate_compiler.get("id") != compiler.get("id")
            or gate_compiler.get("version") != compiler_version
            or gate_compiler.get("executable_sha256") != artifact_by_name["clang++"]
        ):
            _abort("invalid tool identity")
        metadata_digest = gate_compiler.get("metadata_sha256")
        if not isinstance(metadata_digest, str):
            _abort("invalid tool identity")
        gate_metadata.add(metadata_digest)
    if len(gate_metadata) != 1:
        _abort("invalid tool identity")

    command_versions = {
        command.get("name", "").removeprefix("tool."): command.get("version")
        for command in commands
        if isinstance(command, dict)
        and isinstance(command.get("name"), str)
        and t.cast(str, command["name"]).startswith("tool.")
    }
    if command_versions != versions:
        _abort("invalid tool identity")
    command_by_name = {
        t.cast(str, command["name"]): command
        for command in commands
        if isinstance(command, dict) and isinstance(command.get("name"), str)
    }
    if (
        len(t.cast(list[str], pins["cmake"])) != 1
        or command_by_name["toolchain.ctest"].get("version")
        != f"ctest version {t.cast(list[str], pins['cmake'])[0]}"
    ):
        _abort("invalid tool identity")
    if any(
        command_by_name[f"toolchain.ninja.{preset}"].get("version") != versions["ninja"]
        for preset in ("cxx-dev", "cxx-sanitize", "cxx-tsan")
    ):
        _abort("invalid tool identity")


def _validate_source_snapshot(value: object) -> str:
    """Validate one canonical public source inventory and return its digest.

    Examples
    --------
    >>> try:
    ...     _validate_source_snapshot({})
    ... except GateError as error:
    ...     str(error)
    'invalid source snapshot'
    """
    if not isinstance(value, dict) or set(value) != {"sha256", "entries"}:
        _abort("invalid source snapshot")
    digest = value.get("sha256")
    entries = value.get("entries")
    if (
        not isinstance(digest, str)
        or not _DIGEST.fullmatch(digest)
        or not isinstance(entries, list)
        or not entries
    ):
        _abort("invalid source snapshot")
    names: list[str] = []
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {
            "mode",
            "path",
            "sha256",
            "source",
        }:
            _abort("invalid source snapshot")
        mode = entry.get("mode")
        name = entry.get("path")
        entry_digest = entry.get("sha256")
        source = entry.get("source")
        if not (
            isinstance(name, str)
            and _safe_source_path(name.encode(), "source snapshot path") == name
            and mode in {"100644", "100755", "120000"}
            and isinstance(entry_digest, str)
            and _DIGEST.fullmatch(entry_digest)
            and source in {"tracked", "untracked"}
            and (mode != "120000" or source == "tracked")
        ):
            _abort("invalid source snapshot")
        if mode == "120000" and (
            name not in _INSTRUCTION_LINKS
            or entry_digest != _sha256(os.fsencode(_INSTRUCTION_LINKS[name]))
        ):
            _abort("invalid source snapshot")
        names.append(name)
    if names != sorted(set(names)):
        _abort("invalid source snapshot")
    if any(
        name == prefix or name.startswith(f"{prefix}/")
        for name in names
        for prefix in ("cxx/include", "cxx/src", "cxx/spikes")
    ):
        _abort("invalid source snapshot")
    if _sha256(_canonical_bytes({"entries": entries})) != digest:
        _abort("invalid source snapshot")
    return digest


def validate_evidence(value: object) -> None:
    """Validate the public claim and path boundary of one evidence record.

    Parameters
    ----------
    value : object
        Candidate decoded JSON value.

    Examples
    --------
    >>> try:
    ...     validate_evidence({})
    ... except GateError as error:
    ...     str(error)
    'missing gate source identity'
    """
    if not isinstance(value, dict):
        _abort("invalid evidence schema")
    gate_source = value.get("gate_source")
    if not isinstance(gate_source, dict) or not (
        isinstance(gate_source.get("commit"), str)
        and _OBJECT_ID.fullmatch(t.cast(str, gate_source["commit"]))
        and isinstance(gate_source.get("tree"), str)
        and _OBJECT_ID.fullmatch(t.cast(str, gate_source["tree"]))
    ):
        _abort("missing gate source identity")
    if "python_contract" not in value:
        _abort("invalid Python contract")
    if set(value) != {
        "schema_version",
        "status",
        "gate_source",
        "reviewable_source_sha256",
        "python_contract",
        "tools",
        "toolchain",
        "ctest_gates",
        "commands",
        "claims",
        "core_excluded_fields",
        "review",
        "evidence_core_sha256",
        "final_evidence_sha256",
    }:
        _abort("invalid evidence schema")
    if set(gate_source) != {"commit", "tree", "source_snapshot"}:
        _abort("invalid gate source identity")
    if (
        type(value.get("schema_version")) is not int
        or value.get("schema_version") != 1
        or value.get("status") != "passed"
    ):
        _abort("invalid evidence schema")
    if value.get("claims") != {
        "proved": _PROVED_CLAIMS,
        "not_proved": _UNPROVED_CLAIMS,
    }:
        _abort("invalid claim boundary")
    _validate_python_contract(value.get("python_contract"))
    _public_values(value)
    source_snapshot_digest = _validate_source_snapshot(
        gate_source.get("source_snapshot")
    )
    commands = _validate_command_matrix(value.get("commands"))
    gates = _validate_ctest_gate_matrix(value.get("ctest_gates"))
    _validate_tool_identity(value, gates, commands)
    for field in (
        "reviewable_source_sha256",
        "evidence_core_sha256",
        "final_evidence_sha256",
    ):
        field_value = value.get(field)
        if not isinstance(field_value, str) or not _DIGEST.fullmatch(field_value):
            _abort("invalid evidence digest")
    review = value.get("review")
    if review != {"status": "pending"} and not (
        isinstance(review, dict)
        and set(review) == {"status", "sha256"}
        and review.get("status") == "passed"
        and isinstance(review.get("sha256"), str)
        and _DIGEST.fullmatch(t.cast(str, review["sha256"]))
    ):
        _abort("invalid review binding")
    source_snapshot = t.cast(dict[str, t.Any], gate_source["source_snapshot"])
    source_entries = t.cast(list[dict[str, str]], source_snapshot["entries"])
    review_path = _OUTPUT_PATH.with_name(f"{_OUTPUT_PATH.stem}-review.md").as_posix()
    review_entries = [
        entry for entry in source_entries if entry.get("path") == review_path
    ]
    if review == {"status": "pending"}:
        if review_entries:
            _abort("invalid review binding")
        if value.get("reviewable_source_sha256") != source_snapshot_digest:
            _abort("invalid source snapshot")
    else:
        if (
            len(review_entries) != 1
            or review_entries[0].get("mode") != "100644"
            or t.cast(dict[str, str], review).get("sha256")
            != review_entries[0].get("sha256")
        ):
            _abort("invalid review binding")
        reviewable_entries = [
            entry for entry in source_entries if entry.get("path") != review_path
        ]
        if value.get("reviewable_source_sha256") != _sha256(
            _canonical_bytes({"entries": reviewable_entries})
        ):
            _abort("invalid review binding")
    if value.get("core_excluded_fields") != _CORE_EXCLUDED_FIELDS:
        _abort("invalid evidence digest")
    if value["evidence_core_sha256"] != _sha256(
        _canonical_bytes(_evidence_core(value))
    ) or value["final_evidence_sha256"] != _sha256(
        _canonical_bytes(_final_projection(value))
    ):
        _abort("invalid evidence digest")


def main(argv: t.Sequence[str] | None = None) -> int:
    r"""Run the aggregate contract gate CLI and return a process status.

    Examples
    --------
    >>> import io
    >>> errors = io.StringIO()
    >>> with contextlib.redirect_stderr(errors):
    ...     status = main(["--output", "not-contract-evidence.json"])
    >>> status
    2
    >>> errors.getvalue()
    'output path must be the canonical contract evidence destination\n'
    """
    try:
        run(build_parser().parse_args(argv))
    except (GateError, OSError, TypeError, subprocess.SubprocessError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
