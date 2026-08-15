"""Materialize the approved engine-ops sources from verified Git objects."""

from __future__ import annotations

import argparse
import ctypes
import dataclasses
import errno
import hashlib
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import tempfile
import typing as t
import urllib.parse

MANIFEST_NAME = ".materialization.json"
_OID = re.compile(r"[0-9a-f]{40}\Z")
_PINNED_REPOSITORY_URI = "https://github.com/tmux-python/libtmux.git"
_GIT_TIMEOUT_SECONDS = 30


@dataclasses.dataclass(frozen=True, slots=True)
class SourceFile:
    """One exact regular blob in the approved source lock.

    Attributes
    ----------
    path : str
        Safe repository-relative POSIX path.
    mode : str
        Exact Git tree mode.
    blob : str
        Exact SHA-1 blob object identity.
    """

    path: str
    mode: str
    blob: str


@dataclasses.dataclass(frozen=True, slots=True)
class SourceLock:
    """Closed provenance lock for the engine-ops inspection sources.

    Attributes
    ----------
    repository_uri : str
        Exact configured source repository URI.
    commit : str
        Exact commit object identity.
    tree : str
        Exact root tree object identity.
    files : tuple[SourceFile, ...]
        Ordered inspected regular blobs.
    """

    repository_uri: str
    commit: str
    tree: str
    files: tuple[SourceFile, ...]


PINNED_LOCK = SourceLock(
    repository_uri=_PINNED_REPOSITORY_URI,
    commit="5b2c88e57e6e15422a8e845ef5d55fe7a606c315",
    tree="6cf797dc43d0d5b0f20e8dda3ba0383557cf124c",
    files=(
        SourceFile(
            path="src/libtmux/experimental/engines/base.py",
            mode="100644",
            blob="efd157d725ed4b80482a83cc662576cdb3ce142b",
        ),
        SourceFile(
            path="src/libtmux/experimental/ops/_chain.py",
            mode="100644",
            blob="ec29d58dda80031e16c2ecea79535e647c0674b5",
        ),
        SourceFile(
            path="src/libtmux/experimental/ops/_types.py",
            mode="100644",
            blob="c563bd83364126d0821ef79f87acbbe486aa4135",
        ),
        SourceFile(
            path="src/libtmux/experimental/ops/execute.py",
            mode="100644",
            blob="96fe9b45396727f8d8916592c34a45dd34464ae6",
        ),
        SourceFile(
            path="src/libtmux/experimental/ops/operation.py",
            mode="100644",
            blob="96da64f43bb95d295f31ece3a2c3b72f7c352f0f",
        ),
        SourceFile(
            path="src/libtmux/experimental/ops/plan.py",
            mode="100644",
            blob="af3f5f85dd8a5e646d043163428f7a5c51b29738",
        ),
        SourceFile(
            path="src/libtmux/experimental/ops/planner.py",
            mode="100644",
            blob="41330f787f64171bcc8e2f82d38fd61642d8799d",
        ),
        SourceFile(
            path="src/libtmux/experimental/ops/results.py",
            mode="100644",
            blob="f5f4383eb206e67cccd971be198093da1312b1d3",
        ),
    ),
)


def canonical_json_bytes(value: object) -> bytes:
    r"""Encode deterministic UTF-8 JSON with one terminal newline.

    Examples
    --------
    >>> canonical_json_bytes({'b': 2, 'a': 1})
    b'{"a":1,"b":2}\n'
    """
    return (
        json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode()


def _duplicate_free_object(
    pairs: list[tuple[str, object]],
) -> dict[str, object]:
    """Build one JSON object while rejecting duplicate member names.

    Examples
    --------
    >>> _duplicate_free_object([('a', 1)])
    {'a': 1}
    """
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            msg = f"duplicate JSON key {key!r}"
            raise ValueError(msg)
        result[key] = value
    return result


def _reject_json_constant(value: str) -> t.NoReturn:
    """Reject non-finite JSON constants.

    Examples
    --------
    >>> _reject_json_constant('NaN')
    Traceback (most recent call last):
    ...
    ValueError: non-finite JSON value 'NaN'
    """
    msg = f"non-finite JSON value {value!r}"
    raise ValueError(msg)


def _safe_source_path(raw: object) -> str:
    """Validate one portable, repository-relative POSIX source path.

    Examples
    --------
    >>> _safe_source_path('src/libtmux/file.py')
    'src/libtmux/file.py'
    >>> _safe_source_path('../escape')
    Traceback (most recent call last):
    ...
    ValueError: unsafe source path '../escape'
    """
    if (
        not isinstance(raw, str)
        or not raw
        or "\0" in raw
        or "\\" in raw
        or any(part in {"", ".", ".."} for part in raw.split("/"))
        or (len(raw) >= 2 and raw[1] == ":")
    ):
        msg = f"unsafe source path {raw!r}"
        raise ValueError(msg)
    path = pathlib.PurePosixPath(raw)
    if path.is_absolute():
        msg = f"unsafe source path {raw!r}"
        raise ValueError(msg)
    return str(path)


def _validated_oid(raw: object, label: str) -> str:
    """Require one lowercase full SHA-1 object identity.

    Examples
    --------
    >>> _validated_oid('a' * 40, 'commit') == 'a' * 40
    True
    """
    if not isinstance(raw, str) or _OID.fullmatch(raw) is None:
        msg = f"source lock {label} must be a lowercase 40-hex object ID"
        raise ValueError(msg)
    return raw


def _lock_document(lock: SourceLock) -> dict[str, object]:
    """Project a source lock into its canonical closed JSON shape.

    Examples
    --------
    >>> _lock_document(PINNED_LOCK)['schema_version']
    1
    """
    return {
        "commit": lock.commit,
        "files": [dataclasses.asdict(entry) for entry in lock.files],
        "repository_uri": lock.repository_uri,
        "schema_version": 1,
        "tree": lock.tree,
    }


def _load_source_lock(path: pathlib.Path, expected: SourceLock) -> SourceLock:
    """Load a strict JSON lock and require the separately approved identity.

    Examples
    --------
    The focused tests exercise valid, duplicated, and mutated lock documents.
    """
    try:
        raw_document = json.loads(
            path.read_text(),
            object_pairs_hook=_duplicate_free_object,
            parse_constant=_reject_json_constant,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        msg = "source lock is unreadable or invalid JSON"
        raise ValueError(msg) from error
    if not isinstance(raw_document, dict) or set(raw_document) != {
        "commit",
        "files",
        "repository_uri",
        "schema_version",
        "tree",
    }:
        msg = "closed source lock has unexpected fields"
        raise ValueError(msg)
    if (
        type(raw_document["schema_version"]) is not int
        or raw_document["schema_version"] != 1
    ):
        msg = "source lock schema_version must be integer one"
        raise ValueError(msg)
    repository_uri = raw_document["repository_uri"]
    if not isinstance(repository_uri, str) or not repository_uri:
        msg = "source lock repository URI is invalid"
        raise ValueError(msg)
    raw_files = raw_document["files"]
    if not isinstance(raw_files, list):
        msg = "source lock files must be an array"
        raise TypeError(msg)
    files: list[SourceFile] = []
    seen: set[str] = set()
    for raw_file in raw_files:
        if not isinstance(raw_file, dict) or set(raw_file) != {
            "blob",
            "mode",
            "path",
        }:
            msg = "source lock file row is not closed"
            raise ValueError(msg)
        source_path = _safe_source_path(raw_file["path"])
        if source_path in seen:
            msg = f"source lock has duplicate path {source_path!r}"
            raise ValueError(msg)
        seen.add(source_path)
        mode = raw_file["mode"]
        if not isinstance(mode, str) or re.fullmatch(r"[0-7]{6}", mode) is None:
            msg = f"source lock mode for {source_path!r} is invalid"
            raise ValueError(msg)
        files.append(
            SourceFile(
                path=source_path,
                mode=mode,
                blob=_validated_oid(raw_file["blob"], "blob"),
            )
        )
    parsed = SourceLock(
        repository_uri=repository_uri,
        commit=_validated_oid(raw_document["commit"], "commit"),
        tree=_validated_oid(raw_document["tree"], "tree"),
        files=tuple(files),
    )
    if parsed != expected:
        msg = "source lock differs from the separately approved identity"
        raise ValueError(msg)
    for entry in parsed.files:
        if entry.mode == "120000":
            msg = f"source lock path {entry.path!r} is a symlink"
            raise ValueError(msg)
        if entry.mode == "160000":
            msg = f"source lock path {entry.path!r} is a submodule"
            raise ValueError(msg)
        if entry.mode != "100644":
            msg = f"source lock path {entry.path!r} is not a regular file"
            raise ValueError(msg)
    if tuple(entry.path for entry in parsed.files) != tuple(
        sorted(entry.path for entry in parsed.files)
    ):
        msg = "source lock paths must be sorted"
        raise ValueError(msg)
    return parsed


def _validate_repository_uri(uri: str, *, allow_file_uri: bool) -> None:
    """Reject remote helpers and accept only the configured URI class.

    Examples
    --------
    >>> _validate_repository_uri(_PINNED_REPOSITORY_URI, allow_file_uri=False)
    >>> _validate_repository_uri('ext::false', allow_file_uri=True)
    Traceback (most recent call last):
    ...
    ValueError: repository URI is not an allowed exact transport
    """
    parsed = urllib.parse.urlsplit(uri)
    if (
        "\0" in uri
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
    ):
        msg = "repository URI is not an allowed exact transport"
        raise ValueError(msg)
    if allow_file_uri:
        allowed = (
            parsed.scheme == "file"
            and parsed.netloc in {"", "localhost"}
            and pathlib.PurePosixPath(parsed.path).is_absolute()
        )
    else:
        allowed = uri == _PINNED_REPOSITORY_URI
    if not allowed:
        msg = "repository URI is not an allowed exact transport"
        raise ValueError(msg)


def _git_environment() -> dict[str, str]:
    """Return a child-only environment stripped of ambient Git channels.

    Examples
    --------
    >>> env = _git_environment()
    >>> env['GIT_CONFIG_GLOBAL']
    '/dev/null'
    """
    environment = {
        key: value for key, value in os.environ.items() if not key.startswith("GIT_")
    }
    environment.update(
        {
            "GIT_CONFIG_GLOBAL": "/dev/null",
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_SYSTEM": "/dev/null",
            "GIT_TERMINAL_PROMPT": "0",
            "LANG": "C",
            "LC_ALL": "C",
        }
    )
    return environment


def _run_git(
    repository: pathlib.Path,
    arguments: t.Sequence[str],
    *,
    context: str,
    allow_file_uri: bool,
) -> bytes:
    """Run one non-shell Git command in the owned repository.

    Examples
    --------
    The disposable-remote tests exercise every Git plumbing invocation.
    """
    configuration = [
        "-c",
        "core.hooksPath=/dev/null",
        "-c",
        "credential.helper=",
        "-c",
        "protocol.ext.allow=never",
        "-c",
        f"protocol.file.allow={'always' if allow_file_uri else 'never'}",
    ]
    try:
        completed = subprocess.run(
            ["git", *configuration, *arguments],
            cwd=repository,
            check=False,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            close_fds=True,
            env=_git_environment(),
            timeout=_GIT_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as error:
        msg = f"{context}: Git command timed out"
        raise ValueError(msg) from error
    except OSError as error:
        msg = f"{context}: Git could not start"
        raise ValueError(msg) from error
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", "backslashreplace").strip()
        msg = f"{context}: {detail or 'Git command failed'}"
        raise ValueError(msg)
    return completed.stdout


def _assert_no_symlink_ancestors(path: pathlib.Path) -> pathlib.Path:
    """Return an absolute lexical path whose existing ancestors are not links.

    Examples
    --------
    Existing normal temporary-directory ancestors pass this check in the tests.
    """
    absolute = path.absolute()
    current = pathlib.Path(absolute.anchor)
    for part in absolute.parts[1:]:
        current /= part
        try:
            metadata = current.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(metadata.st_mode):
            msg = "materialization path has a symlink ancestor"
            raise ValueError(msg)
    return absolute


def _prepare_directory(path: pathlib.Path, label: str) -> pathlib.Path:
    """Create or validate one non-symlink directory owned by this task.

    Examples
    --------
    The cache and output-parent tests cover creation and rejection paths.
    """
    absolute = _assert_no_symlink_ancestors(path)
    try:
        absolute.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        msg = f"{label} could not be created"
        raise ValueError(msg) from error
    _assert_no_symlink_ancestors(absolute)
    try:
        metadata = absolute.stat()
    except OSError as error:
        msg = f"{label} is unavailable"
        raise ValueError(msg) from error
    if not stat.S_ISDIR(metadata.st_mode):
        msg = f"{label} is not a directory"
        raise ValueError(msg)
    return absolute


def _git_tree_entry(
    repository: pathlib.Path,
    commit: str,
    expected: SourceFile,
    *,
    allow_file_uri: bool,
) -> tuple[str, str, str, str]:
    """Read exactly one NUL-delimited tree entry for a locked path.

    Examples
    --------
    The exact-entry and non-regular-entry tests exercise this parser.
    """
    output = _run_git(
        repository,
        ["ls-tree", "-z", "--full-tree", commit, "--", expected.path],
        context=f"inspect locked path {expected.path!r}",
        allow_file_uri=allow_file_uri,
    )
    records = [record for record in output.split(b"\0") if record]
    if len(records) != 1:
        msg = f"locked path {expected.path!r} is missing or ambiguous"
        raise ValueError(msg)
    try:
        metadata, raw_path = records[0].split(b"\t", 1)
        mode, kind, object_id = metadata.decode("ascii").split(" ", 2)
        actual_path = raw_path.decode("utf-8")
    except (UnicodeError, ValueError) as error:
        msg = f"locked path {expected.path!r} has malformed tree metadata"
        raise ValueError(msg) from error
    return mode, kind, object_id, actual_path


def _fetch_verified_payloads_in_directory(
    lock: SourceLock,
    fetch_directory: pathlib.Path,
    *,
    allow_file_uri: bool,
) -> dict[str, bytes]:
    """Fetch and verify objects inside one already-owned bare directory.

    Examples
    --------
    Disposable bare remotes cover success, unavailable remotes, and bad blobs.
    """
    _run_git(
        fetch_directory,
        ["init", "--bare", "--quiet", "."],
        context="initialize task-owned cache",
        allow_file_uri=allow_file_uri,
    )
    _run_git(
        fetch_directory,
        ["remote", "add", "origin", lock.repository_uri],
        context="configure exact repository URI",
        allow_file_uri=allow_file_uri,
    )
    configured = _run_git(
        fetch_directory,
        ["remote", "get-url", "--all", "origin"],
        context="verify exact repository URI",
        allow_file_uri=allow_file_uri,
    ).decode()
    if configured != lock.repository_uri + "\n":
        msg = "configured repository URI changed"
        raise ValueError(msg)
    _run_git(
        fetch_directory,
        [
            "fetch",
            "--depth=1",
            "--no-tags",
            "--no-recurse-submodules",
            "--force",
            "origin",
            f"{lock.commit}:refs/task/source",
        ],
        context="fetch pinned commit",
        allow_file_uri=allow_file_uri,
    )
    resolved_commit = (
        _run_git(
            fetch_directory,
            ["rev-parse", "--verify", "refs/task/source^{commit}"],
            context="resolve fetched commit",
            allow_file_uri=allow_file_uri,
        )
        .decode()
        .strip()
    )
    if resolved_commit != lock.commit:
        msg = "fetched commit identity differs from source lock"
        raise ValueError(msg)
    resolved_tree = (
        _run_git(
            fetch_directory,
            ["rev-parse", "--verify", f"{lock.commit}^{{tree}}"],
            context="resolve fetched commit tree",
            allow_file_uri=allow_file_uri,
        )
        .decode()
        .strip()
    )
    if resolved_tree != lock.tree:
        msg = "fetched commit tree identity differs from source lock"
        raise ValueError(msg)

    payloads: dict[str, bytes] = {}
    for expected in lock.files:
        mode, kind, object_id, actual_path = _git_tree_entry(
            fetch_directory,
            lock.commit,
            expected,
            allow_file_uri=allow_file_uri,
        )
        if mode == "120000":
            msg = f"locked path {expected.path!r} resolved to a symlink"
            raise ValueError(msg)
        if mode == "160000" or kind == "commit":
            msg = f"locked path {expected.path!r} resolved to a submodule"
            raise ValueError(msg)
        if (
            mode != expected.mode
            or kind != "blob"
            or object_id != expected.blob
            or actual_path != expected.path
        ):
            msg = f"locked path {expected.path!r} has changed blob identity"
            raise ValueError(msg)
        payload = _run_git(
            fetch_directory,
            ["cat-file", "blob", expected.blob],
            context=f"read locked blob {expected.path!r}",
            allow_file_uri=allow_file_uri,
        )
        git_identity = hashlib.sha1(
            f"blob {len(payload)}\0".encode() + payload, usedforsecurity=False
        ).hexdigest()
        if git_identity != expected.blob:
            msg = f"locked path {expected.path!r} payload has changed blob identity"
            raise ValueError(msg)
        payloads[expected.path] = payload
    return payloads


def _fetch_verified_payloads(
    lock: SourceLock,
    cache_root: pathlib.Path,
    *,
    allow_file_uri: bool,
) -> dict[str, bytes]:
    """Use one transient bare fetch repository and remove it on every exit.

    Examples
    --------
    Focused tests require cleanup after success, Git failure, and timeout.
    """
    fetch_directory = pathlib.Path(
        tempfile.mkdtemp(prefix="engine-ops-fetch-", dir=cache_root)
    )
    try:
        return _fetch_verified_payloads_in_directory(
            lock, fetch_directory, allow_file_uri=allow_file_uri
        )
    finally:
        _remove_owned_tree(fetch_directory)


def _write_verified_file(path: pathlib.Path, payload: bytes) -> None:
    """Create, flush, and make one staging file read-only.

    Examples
    --------
    Materialization tests verify exact contents and final mode ``0444``.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())
    path.chmod(0o444)


def _make_tree_writable(root: pathlib.Path) -> None:
    """Restore owner permissions solely for cleanup of owned staging.

    Examples
    --------
    Failure cleanup tests verify unrelated siblings remain untouched.
    """
    if not root.exists():
        return
    for path in sorted(root.rglob("*"), key=lambda item: len(item.parts), reverse=True):
        if not path.is_symlink():
            path.chmod(path.stat().st_mode | stat.S_IWUSR | stat.S_IXUSR)
    root.chmod(root.stat().st_mode | stat.S_IWUSR | stat.S_IXUSR)


def _remove_owned_tree(root: pathlib.Path) -> None:
    """Remove one known task-created tree after restoring permissions.

    Examples
    --------
    The post-fetch failure test asserts that only staging is removed.
    """
    if not root.exists():
        return
    _make_tree_writable(root)
    shutil.rmtree(root)


def _make_tree_read_only(root: pathlib.Path) -> None:
    """Remove write bits from every materialized file and directory.

    Examples
    --------
    The focused success test checks ``0444`` files and ``0555`` directories.
    """
    directories = [root, *(path for path in root.rglob("*") if path.is_dir())]
    for directory in sorted(
        directories, key=lambda item: len(item.parts), reverse=True
    ):
        directory.chmod(0o555)


def _rename_noreplace(parent_fd: int, staging: str, destination: str) -> None:
    """Atomically publish a staging directory without replacing any entry.

    Examples
    --------
    Publication is exercised by every successful materializer test.
    """
    try:
        renameat2 = ctypes.CDLL(None, use_errno=True).renameat2
    except AttributeError as error:
        msg = "renameat2 is unavailable for immutable materialization"
        raise ValueError(msg) from error
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
            parent_fd,
            staging.encode(),
            parent_fd,
            destination.encode(),
            1,
        )
        == 0
    ):
        return
    error_number = ctypes.get_errno()
    if error_number == errno.EEXIST:
        msg = "materialization destination already exists"
        raise ValueError(msg)
    msg = f"materialization publication failed: {os.strerror(error_number)}"
    raise ValueError(msg)


def _manifest(lock: SourceLock, payloads: t.Mapping[str, bytes]) -> dict[str, object]:
    """Build the closed normalized materialization manifest.

    Examples
    --------
    Manifest tests verify every key, digest, size, identity, and ordering.
    """
    return {
        "commit": lock.commit,
        "files": [
            {
                "blob": entry.blob,
                "mode": entry.mode,
                "path": entry.path,
                "payload_sha256": "sha256:"
                + hashlib.sha256(payloads[entry.path]).hexdigest(),
                "size": len(payloads[entry.path]),
            }
            for entry in lock.files
        ],
        "repository_uri": lock.repository_uri,
        "schema_version": 1,
        "source_lock_sha256": "sha256:"
        + hashlib.sha256(canonical_json_bytes(_lock_document(lock))).hexdigest(),
        "tree": lock.tree,
    }


def _materialize(
    *,
    spec_path: pathlib.Path,
    output: pathlib.Path,
    cache_root: pathlib.Path,
    expected_lock: SourceLock,
    allow_file_uri: bool,
) -> pathlib.Path:
    """Materialize one separately approved lock through an explicit test seam.

    Parameters
    ----------
    spec_path : pathlib.Path
        Source-lock JSON to validate.
    output : pathlib.Path
        New immutable destination; it must not already exist.
    cache_root : pathlib.Path
        Task-owned parent for an isolated bare fetch repository.
    expected_lock : SourceLock
        Separately trusted identity, never derived from ``spec_path``.
    allow_file_uri : bool
        Permit a local disposable Git remote for tests only.

    Returns
    -------
    pathlib.Path
        Absolute immutable materialization root.
    """
    lock = _load_source_lock(spec_path, expected_lock)
    _validate_repository_uri(lock.repository_uri, allow_file_uri=allow_file_uri)
    destination = output.absolute()
    if os.path.lexists(destination):
        msg = "materialization destination already exists"
        raise ValueError(msg)
    _assert_no_symlink_ancestors(destination.parent)
    cache = _prepare_directory(cache_root, "task-owned cache")
    payloads = _fetch_verified_payloads(lock, cache, allow_file_uri=allow_file_uri)
    parent = _prepare_directory(destination.parent, "materialization parent")
    if os.path.lexists(destination):
        msg = "materialization destination already exists"
        raise ValueError(msg)
    if destination.name in {"", ".", ".."}:
        msg = "materialization destination name is unsafe"
        raise ValueError(msg)

    staging = pathlib.Path(
        tempfile.mkdtemp(prefix=f".{destination.name}.stage-", dir=parent)
    )
    published = False
    try:
        for entry in lock.files:
            _write_verified_file(staging / entry.path, payloads[entry.path])
        _write_verified_file(
            staging / MANIFEST_NAME, canonical_json_bytes(_manifest(lock, payloads))
        )
        _make_tree_read_only(staging)
        flags = (
            os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
        )
        parent_fd = os.open(parent, flags)
        try:
            _rename_noreplace(parent_fd, staging.name, destination.name)
            published = True
            os.fsync(parent_fd)
        finally:
            os.close(parent_fd)
    finally:
        if not published:
            _remove_owned_tree(staging)
    return destination


def materialize_engine_ops(
    *,
    spec_path: pathlib.Path,
    output: pathlib.Path,
    cache_root: pathlib.Path | None = None,
) -> pathlib.Path:
    """Materialize only the compiled approved engine-ops source lock.

    Parameters
    ----------
    spec_path : pathlib.Path
        Checked-in source-lock document.
    output : pathlib.Path
        New immutable output directory.
    cache_root : pathlib.Path | None
        Optional task-owned cache; defaults beside ``output``.

    Returns
    -------
    pathlib.Path
        Absolute immutable output directory.

    Examples
    --------
    The CLI and focused tests exercise the production pinned-lock entry point.
    """
    selected_cache = (
        cache_root
        if cache_root is not None
        else output.parent / ".engine-ops-materializer-cache"
    )
    return _materialize(
        spec_path=spec_path,
        output=output,
        cache_root=selected_cache,
        expected_lock=PINNED_LOCK,
        allow_file_uri=False,
    )


def _parser() -> argparse.ArgumentParser:
    """Build the materializer command-line parser.

    Examples
    --------
    >>> arguments = _parser().parse_args(
    ...     ["--spec", "source.json", "--output", "materialized"]
    ... )
    >>> (arguments.spec.name, arguments.output.name, arguments.cache_root)
    ('source.json', 'materialized', None)
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spec", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--cache-root", type=pathlib.Path)
    return parser


def main(argv: t.Sequence[str] | None = None) -> int:
    """Run the pinned materializer command.

    Examples
    --------
    >>> callable(main)
    True
    """
    arguments = _parser().parse_args(argv)
    materialize_engine_ops(
        spec_path=arguments.spec,
        output=arguments.output,
        cache_root=arguments.cache_root,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
