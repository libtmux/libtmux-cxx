"""Capture one immutable, path-scrubbed CTest gate record."""

from __future__ import annotations

import argparse
import contextlib
import ctypes
import errno
import fcntl
import hashlib
import json
import math
import os
import pathlib
import re
import secrets
import selectors
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import typing as t
import xml.etree.ElementTree as etree

_DRIVE_PATH = re.compile(r"^[A-Za-z]:")
_EMBEDDED_DRIVE_PATH = re.compile(r"[A-Za-z]:[\\\\/]")
_EMBEDDED_UNIX_PATH = re.compile(r"(?:^|[^A-Za-z0-9_.-])/(?:[^\x00\s]+)")
_FIXTURE_TESTS = {
    "name": "scoped_tmux_server_ScopedTmuxServer.StartsByNameAndExposesResolvedPath",
    "path": "scoped_tmux_server_ScopedTmuxServer.StartsByExactPath",
}
_CTEST_OVERRIDE = "LIBTMUX_CTEST_GATE_CTEST"
_COMPILER_OUTPUT_LIMIT = 64 * 1024
_COMPILER_QUERY_TIMEOUT = 5.0
_COMPILER_REAP_TIMEOUT = 1.0


class GateError(ValueError):
    """Raised when CTest evidence cannot be trusted or published."""


class _ExecutableBinding(t.NamedTuple):
    """Retained identity for the selected CTest executable.

    Attributes
    ----------
    path : pathlib.Path
        Canonical absolute executable path used for both CTest calls.
    handle : t.BinaryIO
        Open handle retaining the selected executable inode.
    metadata : os.stat_result
        Metadata captured from the opened executable.
    contents : bytes
        Executable bytes captured before discovery.
    parent_fd : int
        Descriptor retaining the canonical executable parent directory.
    """

    path: pathlib.Path
    handle: t.BinaryIO
    metadata: os.stat_result
    contents: bytes
    parent_fd: int


class _CompilerAliasBinding(t.NamedTuple):
    """One retained symlink in the configured compiler alias chain.

    Attributes
    ----------
    path : pathlib.Path
        Absolute alias pathname.
    parent_fd : int
        Retained descriptor for the alias parent directory.
    metadata : os.stat_result
        Symlink inode captured without following it.
    target : str
        Exact symlink payload selecting the next chain entry.
    """

    path: pathlib.Path
    parent_fd: int
    metadata: os.stat_result
    target: str


class _CompilerBinding(t.NamedTuple):
    """Retained configured compiler, metadata, and alias-chain identity.

    Attributes
    ----------
    configured_path : pathlib.Path
        Absolute compiler alias recorded by CMake.
    aliases : tuple[_CompilerAliasBinding, ...]
        Retained leaf symlinks traversed to the executable.
    executable : _ExecutableBinding
        Retained final compiler executable and bytes.
    metadata_path : pathlib.Path
        Cache-version-selected CMake compiler metadata file.
    metadata : _ExecutableBinding
        Retained metadata inode and bytes; executable mode is not required.
    cache : _ExecutableBinding
        Retained CMake cache inode and bytes selecting the metadata/compiler.
    compile_commands : _ExecutableBinding
        Retained compile-command inode and bytes binding translation-unit flags.
    compiler_id : str
        Normalized configured compiler implementation ID.
    compiler_version : str
        Normalized configured compiler version.
    """

    configured_path: pathlib.Path
    aliases: tuple[_CompilerAliasBinding, ...]
    executable: _ExecutableBinding
    metadata_path: pathlib.Path
    metadata: _ExecutableBinding
    cache: _ExecutableBinding
    compile_commands: _ExecutableBinding
    compiler_id: str
    compiler_version: str


def _abort(detail: str) -> t.NoReturn:
    """Terminate the current gate with a stable diagnostic.

    Examples
    --------
    >>> try:
    ...     _abort("example")
    ... except GateError as error:
    ...     str(error)
    'example'
    """
    error = GateError(detail)
    raise error


def _canonical_bytes(value: object) -> bytes:
    r"""Serialize a value as deterministic UTF-8 JSON.

    Examples
    --------
    >>> _canonical_bytes({"b": 2, "a": 1})
    b'{\n  "a": 1,\n  "b": 2\n}\n'
    """
    try:
        return (
            json.dumps(
                value,
                indent=2,
                sort_keys=True,
                ensure_ascii=False,
                allow_nan=False,
            )
            + "\n"
        ).encode()
    except ValueError:
        _abort("canonical JSON contains a nonfinite float")


def _strict_json(data: bytes, label: str) -> object:
    """Load JSON while rejecting duplicate keys and nonfinite constants.

    Examples
    --------
    >>> _strict_json(b'{"a": 1}', "example")
    {'a': 1}
    """

    def pairs(values: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in values:
            if key in result:
                _abort(f"duplicate JSON key in {label}")
            result[key] = value
        return result

    def constant(value: str) -> t.NoReturn:
        _abort(f"nonfinite JSON constant in {label}: {value}")

    try:
        return json.loads(data, object_pairs_hook=pairs, parse_constant=constant)
    except (UnicodeDecodeError, json.JSONDecodeError):
        _abort(f"invalid {label}")


def _ctest_property_value(value: object) -> bool:
    """Return whether one JSON-v1 CTest property value has a finite shape.

    Examples
    --------
    >>> _ctest_property_value(["label", 30.0])
    True
    """
    if value is None or type(value) in {bool, int, str}:
        return True
    if isinstance(value, float):
        return math.isfinite(value)
    return isinstance(value, list) and all(
        _ctest_property_value(item) for item in value
    )


def _sha256(value: bytes) -> str:
    """Return the project digest notation for bytes.

    Examples
    --------
    >>> _sha256(b"")[:7]
    'sha256:'
    """
    return f"sha256:{hashlib.sha256(value).hexdigest()}"


def _reject_windows_path(value: str, label: str) -> None:
    """Reject a Windows-drive path at a portable command boundary.

    Examples
    --------
    >>> _reject_windows_path("relative", "value")
    """
    if _DRIVE_PATH.match(value):
        _abort(f"{label} must not use a Windows drive path")


def _safe_existing_path(
    path: pathlib.Path, label: str, *, directory: bool
) -> pathlib.Path:
    """Return a real existing path with no symlinked ancestor.

    Examples
    --------
    >>> _safe_existing_path(pathlib.Path(__file__), "module", directory=False).name
    'ctest_gate.py'
    """
    _reject_windows_path(str(path), label)
    absolute = path.absolute()
    for ancestor in (absolute, *absolute.parents):
        if ancestor.is_symlink():
            _abort(f"{label} contains a symlink")
    if not absolute.exists() or (
        not absolute.is_dir() if directory else not absolute.is_file()
    ):
        _abort(f"missing {label}")
    if not directory and not absolute.is_file():
        _abort(f"{label} is not a regular file")
    return absolute.resolve(strict=True)


def _open_directory_tree(
    path: pathlib.Path,
    label: str,
    *,
    create: bool,
    forbidden_fd: int | None = None,
) -> int:
    """Walk an absolute directory tree from ``/`` without following links.

    Examples
    --------
    >>> descriptor = _open_directory_tree(pathlib.Path.cwd(), "cwd", create=False)
    >>> os.close(descriptor)
    """
    _reject_windows_path(str(path), label)
    absolute = path.absolute()
    if not absolute.is_absolute() or any(
        part in {"", ".", ".."} for part in absolute.parts
    ):
        _abort(f"unsafe {label}")
    try:
        descriptor = os.open(
            "/", os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0)
        )
    except OSError:
        _abort(f"{label} has no trusted filesystem root")
    try:
        forbidden = os.fstat(forbidden_fd) if forbidden_fd is not None else None
        if forbidden is not None and os.path.samestat(os.fstat(descriptor), forbidden):
            _abort(f"{label} aliases a forbidden directory")
        for component in absolute.parts[1:]:
            try:
                child = os.open(
                    component,
                    os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0),
                    dir_fd=descriptor,
                )
            except FileNotFoundError:
                if not create:
                    _abort(f"missing {label}")
                try:
                    os.mkdir(component, mode=0o700, dir_fd=descriptor)
                except FileExistsError:
                    pass
                except OSError:
                    _abort(f"{label} contains a symlink or non-directory ancestor")
                try:
                    child = os.open(
                        component,
                        os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0),
                        dir_fd=descriptor,
                    )
                except OSError:
                    _abort(f"{label} contains a symlink or non-directory ancestor")
            except OSError:
                _abort(f"{label} contains a symlink or non-directory ancestor")
            if forbidden is not None and os.path.samestat(os.fstat(child), forbidden):
                os.close(child)
                _abort(f"{label} aliases a forbidden directory")
            os.close(descriptor)
            descriptor = child
    except BaseException:
        os.close(descriptor)
        raise
    return descriptor


def _verify_directory_anchor(path: pathlib.Path, descriptor: int, label: str) -> None:
    """Require a pathname to retain the directory held by ``descriptor``.

    Examples
    --------
    >>> descriptor = _open_directory_tree(pathlib.Path.cwd(), "cwd", create=False)
    >>> _verify_directory_anchor(pathlib.Path.cwd(), descriptor, "cwd")
    >>> os.close(descriptor)
    """
    candidate = _open_directory_tree(path, f"{label} anchor", create=False)
    try:
        if not os.path.samestat(os.fstat(candidate), os.fstat(descriptor)):
            _abort(f"{label} changed during publication")
    finally:
        os.close(candidate)


def _verify_selected_ctest(binding: _ExecutableBinding) -> None:
    """Require the selected CTest pathname, inode, and bytes to remain fixed.

    Examples
    --------
    >>> with _selected_ctest() as selected:
    ...     _verify_selected_ctest(selected)
    """
    changed = "selected CTest executable changed during gate"
    _verify_directory_anchor(
        binding.path.parent,
        binding.parent_fd,
        "selected CTest executable parent",
    )
    try:
        current = os.stat(
            binding.path.name,
            dir_fd=binding.parent_fd,
            follow_symlinks=False,
        )
        retained = os.fstat(binding.handle.fileno())
        binding.handle.seek(0)
        contents = binding.handle.read()
    except OSError:
        _abort(changed)
    if (
        not stat.S_ISREG(current.st_mode)
        or current.st_mode & 0o111 == 0
        or current.st_nlink != 1
        or not os.path.samestat(binding.metadata, current)
        or not os.path.samestat(binding.metadata, retained)
        or contents != binding.contents
    ):
        _abort(changed)


@contextlib.contextmanager
def _selected_ctest() -> t.Iterator[_ExecutableBinding]:
    """Select and retain one canonical CTest executable for a complete gate.

    Examples
    --------
    >>> with _selected_ctest() as selected:
    ...     selected.path.name
    'ctest'
    """
    override = os.environ.get(_CTEST_OVERRIDE)
    if override is None:
        selected = shutil.which("ctest")
        if selected is None:
            _abort("missing CTest executable")
    else:
        if not override:
            _abort("invalid CTest executable override")
        selected = override
    _reject_windows_path(selected, "CTest executable")
    unresolved = pathlib.Path(selected)
    if override is not None and not unresolved.is_absolute():
        _abort("invalid CTest executable override")
    try:
        path = unresolved.resolve(strict=True)
    except (OSError, RuntimeError):
        _abort("missing CTest executable")
    with contextlib.ExitStack() as stack:
        parent_fd = _open_directory_tree(
            path.parent,
            "selected CTest executable parent",
            create=False,
        )

        def close_parent() -> None:
            """Close the retained parent without masking the committed result."""
            with contextlib.suppress(OSError):
                os.close(parent_fd)

        stack.callback(close_parent)
        try:
            metadata = os.stat(path.name, dir_fd=parent_fd, follow_symlinks=False)
        except OSError:
            _abort("missing CTest executable")
        if (
            not stat.S_ISREG(metadata.st_mode)
            or metadata.st_mode & 0o111 == 0
            or metadata.st_nlink != 1
        ):
            _abort("CTest executable is not a single-link regular executable")
        try:
            descriptor = os.open(
                path.name,
                os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0),
                dir_fd=parent_fd,
            )
        except OSError:
            _abort("missing CTest executable")
        handle = stack.enter_context(os.fdopen(descriptor, "rb"))
        opened = os.fstat(handle.fileno())
        if not os.path.samestat(metadata, opened):
            _abort("selected CTest executable changed during gate")
        contents = handle.read()
        binding = _ExecutableBinding(path, handle, opened, contents, parent_fd)
        _verify_selected_ctest(binding)
        yield binding


def _cmake_cache_entries(cache: pathlib.Path) -> dict[str, str]:
    r"""Return unique CMake cache values needed to bind the compiler.

    Examples
    --------
    >>> import tempfile
    >>> with tempfile.TemporaryDirectory() as raw:
    ...     cache = pathlib.Path(raw) / "CMakeCache.txt"
    ...     _ = cache.write_text("KEY:STRING=value\n", encoding="utf-8")
    ...     _cmake_cache_entries(cache)["KEY"]
    'value'
    """
    return _cmake_cache_values(_regular_file(cache, "CMake cache").read_bytes())


def _cmake_cache_values(contents: bytes) -> dict[str, str]:
    r"""Parse unique CMake cache entries from retained bytes.

    Examples
    --------
    >>> _cmake_cache_values(b"KEY:STRING=value\n")["KEY"]
    'value'
    """
    try:
        lines = contents.decode("utf-8").splitlines()
    except UnicodeDecodeError:
        _abort("invalid CMake cache")
    entries: dict[str, str] = {}
    for line in lines:
        if not line or line.startswith(("#", "//")):
            continue
        key_and_type, separator, value = line.partition("=")
        key, type_separator, _type = key_and_type.partition(":")
        if separator != "=" or type_separator != ":" or not key or key in entries:
            _abort("invalid CMake cache")
        entries[key] = value
    return entries


def _compiler_metadata_values(contents: bytes) -> dict[str, str]:
    r"""Parse the closed CMake-generated C++ compiler identity fields.

    Examples
    --------
    >>> data = (
    ...     b'set(CMAKE_CXX_COMPILER "/usr/bin/clang++")\n'
    ...     b'set(CMAKE_CXX_COMPILER_ARG1 "")\n'
    ...     b'set(CMAKE_CXX_COMPILER_ID "Clang")\n'
    ...     b'set(CMAKE_CXX_COMPILER_VERSION "18.1.3")\n'
    ...     b'set(CMAKE_CXX_COMPILER_WRAPPER "")\n'
    ... )
    >>> _compiler_metadata_values(data)["CMAKE_CXX_COMPILER_ID"]
    'Clang'
    """
    try:
        text = contents.decode("utf-8")
    except UnicodeDecodeError:
        _abort("invalid C++ compiler metadata")
    pattern = re.compile(r'^set\((CMAKE_CXX_[A-Z0-9_]+) "([^"\n]*)"\)$')
    wanted = {
        "CMAKE_CXX_COMPILER",
        "CMAKE_CXX_COMPILER_ARG1",
        "CMAKE_CXX_COMPILER_ID",
        "CMAKE_CXX_COMPILER_VERSION",
        "CMAKE_CXX_COMPILER_WRAPPER",
    }
    values: dict[str, str] = {}
    for line in text.splitlines():
        match = pattern.fullmatch(line)
        if match is None or match.group(1) not in wanted:
            continue
        key, value = match.groups()
        if key in values:
            _abort("invalid C++ compiler metadata")
        values[key] = value
    if set(values) != wanted or any(
        len(re.findall(rf"\b{re.escape(key)}\b", text)) != 1 for key in wanted
    ):
        _abort("invalid C++ compiler metadata")
    return values


def _open_retained_regular(
    stack: contextlib.ExitStack,
    path: pathlib.Path,
    *,
    label: str,
    executable: bool,
) -> _ExecutableBinding:
    """Open one single-link regular file through a retained parent."""
    parent_fd = _open_directory_tree(path.parent, f"{label} parent", create=False)
    stack.callback(_close_descriptor, parent_fd)
    try:
        metadata = os.stat(path.name, dir_fd=parent_fd, follow_symlinks=False)
    except OSError:
        _abort(f"missing {label}")
    if (
        not stat.S_ISREG(metadata.st_mode)
        or metadata.st_nlink != 1
        or (executable and metadata.st_mode & 0o111 == 0)
    ):
        _abort(f"{label} is not a single-link regular file")
    try:
        descriptor = os.open(
            path.name,
            os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0),
            dir_fd=parent_fd,
        )
    except OSError:
        _abort(f"missing {label}")
    handle = stack.enter_context(os.fdopen(descriptor, "rb"))
    opened = os.fstat(handle.fileno())
    if not os.path.samestat(metadata, opened):
        _abort(f"{label} changed during gate")
    contents = handle.read()
    return _ExecutableBinding(path, handle, opened, contents, parent_fd)


def _close_descriptor(descriptor: int) -> None:
    """Close a retained descriptor without changing a committed gate result.

    Examples
    --------
    >>> descriptor = os.open(os.devnull, os.O_RDONLY)
    >>> _close_descriptor(descriptor)
    """
    with contextlib.suppress(OSError):
        os.close(descriptor)


def _verify_retained_regular(
    binding: _ExecutableBinding,
    *,
    label: str,
    executable: bool,
) -> None:
    """Require one retained pathname, inode, and byte sequence to match."""
    changed = f"{label} changed during gate"
    _verify_directory_anchor(binding.path.parent, binding.parent_fd, f"{label} parent")
    try:
        current = os.stat(
            binding.path.name,
            dir_fd=binding.parent_fd,
            follow_symlinks=False,
        )
        retained = os.fstat(binding.handle.fileno())
        binding.handle.seek(0)
        contents = binding.handle.read()
    except OSError:
        _abort(changed)
    if (
        not stat.S_ISREG(current.st_mode)
        or current.st_nlink != 1
        or (executable and current.st_mode & 0o111 == 0)
        or not os.path.samestat(binding.metadata, current)
        or not os.path.samestat(binding.metadata, retained)
        or contents != binding.contents
    ):
        _abort(changed)


def _compiler_public_identity(binding: _CompilerBinding) -> dict[str, str]:
    """Return the path-free public compiler identity.

    Examples
    --------
    >>> set(_compiler_public_identity(t.cast(_CompilerBinding, None)))
    Traceback (most recent call last):
    ...
    AttributeError: 'NoneType' object has no attribute 'executable'
    """
    return {
        "executable_sha256": _sha256(binding.executable.contents),
        "id": binding.compiler_id,
        "metadata_sha256": _sha256(binding.metadata.contents),
        "version": binding.compiler_version,
    }


def _verify_selected_compiler(binding: _CompilerBinding) -> None:
    """Revalidate the configured alias chain, metadata, and compiler bytes."""
    changed = "configured compiler changed during gate"
    for alias in binding.aliases:
        _verify_directory_anchor(alias.path.parent, alias.parent_fd, changed)
        try:
            current = os.stat(
                alias.path.name,
                dir_fd=alias.parent_fd,
                follow_symlinks=False,
            )
            target = os.readlink(alias.path.name, dir_fd=alias.parent_fd)
        except OSError:
            _abort(changed)
        if (
            not stat.S_ISLNK(current.st_mode)
            or not os.path.samestat(alias.metadata, current)
            or target != alias.target
        ):
            _abort(changed)
    _verify_retained_regular(
        binding.executable,
        label="configured compiler",
        executable=True,
    )
    _verify_retained_regular(
        binding.metadata,
        label="C++ compiler metadata",
        executable=False,
    )
    _verify_retained_regular(
        binding.cache,
        label="CMake cache",
        executable=False,
    )
    _verify_retained_regular(
        binding.compile_commands,
        label="compile commands",
        executable=False,
    )


def _normalized_compiler_version(
    compiler_id: str,
    configured_version: str,
    stdout: bytes,
) -> str:
    r"""Validate runtime compiler identity against CMake metadata.

    Examples
    --------
    >>> _normalized_compiler_version("Clang", "18.1.3", b"clang version 18.1.3\n")
    '18.1.3'
    """
    if compiler_id != "Clang":
        _abort("configured compiler identity is not Clang")
    try:
        first_line = stdout.decode("utf-8").splitlines()[0]
    except (UnicodeDecodeError, IndexError):
        _abort("invalid configured compiler version output")
    match = re.search(
        r"(?:^|\s)clang version ([0-9]+(?:\.[0-9]+)+)(?:\s|$)", first_line
    )
    if match is None or match.group(1) != configured_version:
        _abort("configured compiler version does not match metadata")
    return configured_version


@contextlib.contextmanager
def _selected_compiler(build: pathlib.Path) -> t.Iterator[_CompilerBinding]:
    """Capture the cache-selected C++ compiler for the complete gate.

    Examples
    --------
    >>> try:
    ...     with _selected_compiler(pathlib.Path("__missing_build__")):
    ...         pass
    ... except GateError as error:
    ...     str(error)
    'missing CMake cache parent'
    """
    cache = build / "CMakeCache.txt"
    with contextlib.ExitStack() as stack:
        cache_binding = _open_retained_regular(
            stack,
            cache,
            label="CMake cache",
            executable=False,
        )
        entries = _cmake_cache_values(cache_binding.contents)
        version_keys = (
            "CMAKE_CACHE_MAJOR_VERSION",
            "CMAKE_CACHE_MINOR_VERSION",
            "CMAKE_CACHE_PATCH_VERSION",
        )
        if any(
            key not in entries or not entries[key].isdecimal() for key in version_keys
        ):
            _abort("invalid CMake compiler metadata version")
        cmake_version = ".".join(entries[key] for key in version_keys)
        metadata_path = build / "CMakeFiles" / cmake_version / "CMakeCXXCompiler.cmake"
        metadata = _open_retained_regular(
            stack,
            metadata_path,
            label="C++ compiler metadata",
            executable=False,
        )
        values = _compiler_metadata_values(metadata.contents)
        configured_value = values["CMAKE_CXX_COMPILER"]
        configured = pathlib.Path(configured_value)
        if not configured.is_absolute() or _DRIVE_PATH.match(configured_value):
            _abort("invalid configured compiler path")
        cache_compiler = entries.get("CMAKE_CXX_COMPILER")
        if not cache_compiler:
            _abort("missing CMake compiler selection")
        cache_path = pathlib.Path(cache_compiler)
        if cache_path.is_absolute():
            try:
                cache_matches = cache_path.resolve(strict=True) == configured.resolve(
                    strict=True
                )
            except (OSError, RuntimeError):
                cache_matches = False
        else:
            cache_matches = (
                "/" not in cache_compiler
                and "\\" not in cache_compiler
                and configured.name == cache_compiler
            )
        if not cache_matches:
            _abort("CMake compiler selection does not match metadata")
        if values["CMAKE_CXX_COMPILER_ARG1"]:
            _abort("configured compiler argument is unsupported")
        if values["CMAKE_CXX_COMPILER_WRAPPER"]:
            _abort("configured compiler wrapper is unsupported")
        if entries.get("CMAKE_CXX_COMPILER_LAUNCHER", ""):
            _abort("configured compiler launcher is unsupported")
        compiler_id = values["CMAKE_CXX_COMPILER_ID"]
        compiler_version = values["CMAKE_CXX_COMPILER_VERSION"]
        if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", compiler_version):
            _abort("invalid configured compiler version")
        aliases: list[_CompilerAliasBinding] = []
        current = configured
        seen: set[pathlib.Path] = set()
        while True:
            if current in seen or len(seen) >= 40:
                _abort("configured compiler alias cycle")
            seen.add(current)
            parent_fd = _open_directory_tree(
                current.parent,
                "configured compiler parent",
                create=False,
            )
            stack.callback(_close_descriptor, parent_fd)
            try:
                state = os.stat(
                    current.name,
                    dir_fd=parent_fd,
                    follow_symlinks=False,
                )
            except OSError:
                _abort("missing configured compiler")
            if not stat.S_ISLNK(state.st_mode):
                break
            try:
                target = os.readlink(current.name, dir_fd=parent_fd)
            except OSError:
                _abort("configured compiler changed during gate")
            aliases.append(_CompilerAliasBinding(current, parent_fd, state, target))
            candidate = pathlib.Path(target)
            if not candidate.is_absolute():
                candidate = current.parent / candidate
            current = pathlib.Path(os.path.normpath(candidate))
            if not current.is_absolute():
                _abort("invalid configured compiler alias")
        executable = _open_retained_regular(
            stack,
            current,
            label="configured compiler",
            executable=True,
        )
        compile_commands = _open_retained_regular(
            stack,
            build / "compile_commands.json",
            label="compile commands",
            executable=False,
        )
        stdout_chunks: list[bytes] = []
        stderr_chunks: list[bytes] = []
        captured = 0
        deadline = time.monotonic() + _COMPILER_QUERY_TIMEOUT
        failure: str | None = None
        unexpected: BaseException | None = None
        with contextlib.closing(selectors.DefaultSelector()) as selector:
            process = subprocess.Popen(
                [os.fspath(configured), "--version"],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                close_fds=True,
                start_new_session=True,
            )
            try:
                if process.stdout is None or process.stderr is None:
                    _abort("configured compiler version query failed")
                for pipe, chunks in (
                    (process.stdout, stdout_chunks),
                    (process.stderr, stderr_chunks),
                ):
                    os.set_blocking(pipe.fileno(), False)
                    selector.register(pipe, selectors.EVENT_READ, chunks)
                while selector.get_map():
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        failure = "configured compiler version query timed out"
                        break
                    events = selector.select(remaining)
                    if not events:
                        failure = "configured compiler version query timed out"
                        break
                    for key, _mask in events:
                        pipe = t.cast(t.BinaryIO, key.fileobj)
                        maximum = min(
                            16 * 1024,
                            _COMPILER_OUTPUT_LIMIT - captured + 1,
                        )
                        chunk = os.read(pipe.fileno(), maximum)
                        if not chunk:
                            selector.unregister(pipe)
                            pipe.close()
                            continue
                        t.cast(list[bytes], key.data).append(chunk)
                        captured += len(chunk)
                        if captured > _COMPILER_OUTPUT_LIMIT:
                            failure = (
                                "configured compiler version output exceeded limit"
                            )
                            break
                    if failure is not None:
                        break
                if failure is None:
                    remaining = max(0.0, deadline - time.monotonic())
                    try:
                        process.wait(timeout=remaining)
                    except subprocess.TimeoutExpired:
                        failure = "configured compiler version query timed out"
            except GateError as error:
                unexpected = error
            except (OSError, subprocess.SubprocessError):
                failure = "configured compiler version query failed"
            except BaseException as error:  # noqa: BLE001
                unexpected = error
            if failure is not None or unexpected is not None:
                with contextlib.suppress(OSError):
                    os.killpg(process.pid, signal.SIGKILL)
                for owned_pipe in (process.stdout, process.stderr):
                    if owned_pipe is not None:
                        with contextlib.suppress(OSError):
                            owned_pipe.close()
                try:
                    process.wait(timeout=_COMPILER_REAP_TIMEOUT)
                except subprocess.TimeoutExpired:
                    _abort("configured compiler did not reap after cancellation")
                if unexpected is not None:
                    raise unexpected
                _abort(t.cast(str, failure))
            for owned_pipe in (process.stdout, process.stderr):
                if owned_pipe is not None:
                    with contextlib.suppress(OSError):
                        owned_pipe.close()
            stdout = b"".join(stdout_chunks)
            if process.returncode != 0:
                _abort("configured compiler version query failed")
            _normalized_compiler_version(compiler_id, compiler_version, stdout)
        binding = _CompilerBinding(
            configured,
            tuple(aliases),
            executable,
            metadata_path,
            metadata,
            cache_binding,
            compile_commands,
            compiler_id,
            compiler_version,
        )
        _verify_selected_compiler(binding)
        try:
            yield binding
        finally:
            with contextlib.suppress(GateError, OSError):
                _verify_selected_compiler(binding)


def _regular_file(path: pathlib.Path, label: str) -> pathlib.Path:
    """Require a nonsymlinked regular file.

    Examples
    --------
    >>> _regular_file(pathlib.Path(__file__), "module").name
    'ctest_gate.py'
    """
    if path.is_symlink() or not path.is_file():
        _abort(f"{label} is not a regular file")
    return path


def _file_digest(path: pathlib.Path, label: str) -> str:
    """Hash a required regular file.

    Examples
    --------
    >>> _file_digest(pathlib.Path(__file__), "module").startswith("sha256:")
    True
    """
    return _sha256(_regular_file(path, label).read_bytes())


def _preset_build_dir(source: pathlib.Path, preset_name: str) -> pathlib.Path:
    """Resolve one configure preset's binary directory without invoking CMake.

    Examples
    --------
    >>> try:
    ...     _preset_build_dir(pathlib.Path("__missing_source__"), "cxx-dev")
    ... except GateError as error:
    ...     str(error)
    'missing CMakePresets.json'
    """
    presets_path = _safe_existing_path(
        source / "CMakePresets.json", "CMakePresets.json", directory=False
    )
    try:
        document = _strict_json(presets_path.read_bytes(), "CMakePresets.json")
    except OSError:
        _abort("invalid CMakePresets.json")
    raw = document.get("configurePresets") if isinstance(document, dict) else None
    if not isinstance(raw, list):
        _abort("invalid CMakePresets.json")
    by_name = {
        entry.get("name"): entry
        for entry in raw
        if isinstance(entry, dict) and isinstance(entry.get("name"), str)
    }

    def resolved(name: str, seen: set[str]) -> dict[str, object]:
        if name in seen or name not in by_name:
            _abort("invalid CMake preset inheritance")
        value = dict(t.cast(dict[str, object], by_name[name]))
        inherited = value.get("inherits")
        parents = [inherited] if isinstance(inherited, str) else inherited
        if parents is not None:
            if not isinstance(parents, list) or not all(
                isinstance(parent, str) for parent in parents
            ):
                _abort("invalid CMake preset inheritance")
            merged: dict[str, object] = {}
            for parent in parents:
                merged.update(resolved(parent, seen | {name}))
            merged.update(value)
            value = merged
        return value

    preset = resolved(preset_name, set())
    binary_dir = preset.get("binaryDir")
    if not isinstance(binary_dir, str) or not binary_dir:
        _abort("CMake preset lacks binaryDir")
    expanded = binary_dir.replace("${sourceDir}", str(source)).replace(
        "${presetName}", preset_name
    )
    _reject_windows_path(expanded, "CMake preset binaryDir")
    candidate = pathlib.Path(expanded)
    if not candidate.is_absolute():
        candidate = source / candidate
    result = _safe_existing_path(candidate, "CMake preset tree", directory=True)
    if not result.is_relative_to(source):
        _abort("CMake preset tree escapes source directory")
    return result


def _registration_files(build: pathlib.Path) -> list[pathlib.Path]:
    """Collect CTest registration files and their effective include files.

    Examples
    --------
    >>> try:
    ...     _registration_files(pathlib.Path("__missing_build__"))
    ... except GateError as error:
    ...     str(error)
    'missing CTest registration files'
    """
    root = build / "CTestTestfile.cmake"
    if not root.exists() or root.is_symlink():
        _abort("missing CTest registration files")
    included: set[pathlib.Path] = set()
    pending = [root]
    argument = (
        r'(?:"(?P<quoted>[^\"]+)"|\[(?P<equals>=*)\[(?P<bracket>.*?)\](?P=equals)\])'
    )
    command_pattern = re.compile(
        rf"\b(?P<command>include|subdirs)\s*\(\s*{argument}\s*\)",
        re.IGNORECASE | re.DOTALL,
    )
    property_pattern = re.compile(
        rf"\bset_property\s*\(\s*DIRECTORY\s+APPEND\s+PROPERTY\s+"
        rf"TEST_INCLUDE_FILES\s+{argument}\s*\)",
        re.IGNORECASE | re.DOTALL,
    )
    while pending:
        path = pending.pop()
        _regular_file(path, "CTest registration file")
        if path in included:
            continue
        included.add(path)
        try:
            content = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            _abort("invalid CTest registration file")
        commands = list(command_pattern.finditer(content))
        properties = list(property_pattern.finditer(content))
        accepted_commands = {command.start() for command in commands}
        for directive in ("include", "subdirs"):
            for opening in re.finditer(rf"\b{directive}\s*\(", content, re.IGNORECASE):
                if opening.start() not in accepted_commands:
                    _abort("unsupported CTest registration directive")
        for mention in re.finditer(r"\bTEST_INCLUDE_FILES\b", content, re.IGNORECASE):
            if not any(
                property_match.start() <= mention.start() < property_match.end()
                for property_match in properties
            ):
                _abort("unsupported CTest registration property")
        for command in [*commands, *properties]:
            command_path = command.group("quoted") or command.group("bracket")
            if not command_path:
                _abort("unsupported CTest registration directive")
            candidate = pathlib.Path(command_path)
            if not candidate.is_absolute():
                candidate = path.parent / candidate
            if command.groupdict().get("command", "").lower() == "subdirs":
                candidate /= "CTestTestfile.cmake"
            if not candidate.exists() or candidate.is_symlink():
                _abort("unresolved or symlinked CTest registration input")
            candidate = candidate.resolve(strict=True)
            if not candidate.is_relative_to(build):
                _abort("CTest registration input escapes preset tree")
            pending.append(candidate)
    return sorted(included)


def _snapshot(
    build: pathlib.Path, executables: t.Sequence[pathlib.Path]
) -> dict[str, object]:
    """Capture every mutable build artifact that binds an execution.

    Examples
    --------
    >>> try:
    ...     _snapshot(pathlib.Path("__missing_build__"), [])
    ... except GateError as error:
    ...     str(error)
    'missing CTest registration files'
    """
    cache = build / "CMakeCache.txt"
    compile_commands = build / "compile_commands.json"
    registrations = _registration_files(build)
    return {
        "cache_sha256": _file_digest(cache, "CMake cache"),
        "compile_commands_sha256": _file_digest(
            compile_commands,
            "compile commands",
        ),
        "registration_files": [
            {
                "path": path.relative_to(build).as_posix(),
                "sha256": _file_digest(path, "CTest registration file"),
            }
            for path in registrations
        ],
        "executables": [
            {
                "path": path.relative_to(build).as_posix(),
                "sha256": _file_digest(path, "selected executable"),
            }
            for path in executables
        ],
    }


def _selected_tests(
    registry_bytes: bytes, build: pathlib.Path
) -> tuple[dict[str, object], list[str], list[pathlib.Path]]:
    """Validate JSON-v1 and return selected IDs with contained executables.

    Examples
    --------
    >>> try:
    ...     _selected_tests(b"{}", pathlib.Path.cwd())
    ... except GateError as error:
    ...     str(error)
    'invalid CTest JSON-v1 registry'
    """
    registry = _strict_json(registry_bytes, "CTest JSON-v1 registry")
    if not isinstance(registry, dict) or registry.get("kind") != "ctestInfo":
        _abort("invalid CTest JSON-v1 registry")
    version = registry.get("version")
    tests = registry.get("tests")
    if (
        not isinstance(version, dict)
        or type(version.get("major")) is not int
        or type(version.get("minor")) is not int
        or version.get("major") != 1
        or not isinstance(tests, list)
    ):
        _abort("invalid CTest JSON-v1 registry")
    names: list[str] = []
    commands: set[tuple[str, ...]] = set()
    executables: list[pathlib.Path] = []
    for test in tests:
        if not isinstance(test, dict):
            _abort("invalid CTest JSON-v1 registry")
        name = test.get("name")
        command = test.get("command")
        properties = test.get("properties", [])
        if (
            not isinstance(name, str)
            or not name
            or not isinstance(command, list)
            or not command
            or not all(isinstance(item, str) for item in command)
            or not isinstance(properties, list)
            or not all(
                isinstance(property_, dict)
                and isinstance(property_.get("name"), str)
                and _ctest_property_value(property_.get("value"))
                for property_ in properties
            )
        ):
            _abort("invalid CTest JSON-v1 registry")
        command_tuple = tuple(t.cast(list[str], command))
        if name in names or command_tuple in commands:
            _abort("CTest selection contains duplicate IDs or commands")
        executable_value = command_tuple[0]
        _reject_windows_path(executable_value, "selected executable")
        executable = pathlib.Path(executable_value)
        if not executable.is_absolute():
            executable = build / executable
        executable = _safe_existing_path(
            executable, "selected executable", directory=False
        )
        if not executable.is_relative_to(build):
            _abort("selected executable escapes preset tree")
        names.append(name)
        commands.add(command_tuple)
        executables.append(executable)
    if not names:
        _abort("CTest selection is empty")
    return registry, names, executables


def _junit_names(junit_bytes: bytes, selected: t.Sequence[str]) -> None:
    """Require exactly one passing JUnit case for every selected CTest ID.

    Examples
    --------
    >>> _junit_names(
    ...     b'<testsuite tests="0" failures="0" skipped="0"/>',
    ...     [],
    ... )
    """
    try:
        root = etree.fromstring(junit_bytes)
    except etree.ParseError:
        _abort("invalid CTest JUnit")

    def tag(element: etree.Element) -> str:
        return str(element.tag).rsplit("}", maxsplit=1)[-1]

    if tag(root) not in {"testsuite", "testsuites"}:
        _abort("invalid CTest JUnit")
    suites = [root] if tag(root) == "testsuite" else list(root)
    if not suites or any(tag(suite) != "testsuite" for suite in suites):
        _abort("invalid CTest JUnit")
    cases: list[etree.Element] = []
    totals = {"tests": 0, "failures": 0, "errors": 0, "skipped": 0, "disabled": 0}
    for suite in suites:
        direct = list(suite)
        if any(
            tag(child) not in {"testcase", "properties", "system-out", "system-err"}
            for child in direct
        ):
            _abort("invalid CTest JUnit")
        suite_cases = [child for child in direct if tag(child) == "testcase"]
        suite_totals = {
            "tests": len(suite_cases),
            "failures": 0,
            "errors": 0,
            "skipped": 0,
            "disabled": 0,
        }
        for case in suite_cases:
            if case.get("status") != "run":
                _abort("CTest JUnit testcase did not run")
            statuses = [tag(child) for child in case]
            if any(
                status
                not in {
                    "failure",
                    "error",
                    "skipped",
                    "properties",
                    "system-out",
                    "system-err",
                }
                for status in statuses
            ) or any(
                tag(property_child) != "property" or list(property_child)
                for properties in case
                if tag(properties) == "properties"
                for property_child in properties
            ):
                _abort("invalid CTest JUnit")
            for field, status in (
                ("failures", "failure"),
                ("errors", "error"),
                ("skipped", "skipped"),
            ):
                suite_totals[field] += int(status in statuses)
        for field, actual in suite_totals.items():
            declared = suite.get(field)
            if field in {"errors", "disabled"} and declared is None and actual == 0:
                totals[field] += actual
                continue
            if declared is None or not declared.isdecimal() or int(declared) != actual:
                _abort("invalid CTest JUnit totals")
            totals[field] += actual
        cases.extend(suite_cases)
    if tag(root) == "testsuites":
        for field, actual in totals.items():
            declared = root.get(field)
            if field in {"errors", "disabled"} and declared is None and actual == 0:
                continue
            if declared is None or not declared.isdecimal() or int(declared) != actual:
                _abort("invalid CTest JUnit totals")
    names = [case.get("name") for case in cases]
    if (
        any(not isinstance(name, str) or not name for name in names)
        or len(names) != len(set(names))
        or set(names) != set(selected)
    ):
        _abort("CTest JUnit does not exactly match selected tests")
    if any(totals[field] for field in ("failures", "errors", "skipped", "disabled")):
        _abort("CTest JUnit contains non-passing cases")


def _fixture_binding(
    selected: t.Sequence[str], selector: dict[str, str]
) -> tuple[list[str], dict[str, str]]:
    """Bind each closed real-tmux fixture test to its supported socket mode.

    Examples
    --------
    >>> names = [_FIXTURE_TESTS[mode] for mode in ("name", "path")]
    >>> _fixture_binding(names, {"label": "real-tmux"})[0]
    ['name', 'path']
    """
    if selector != {"label": "real-tmux"}:
        return [], {}
    if any(selected.count(name) != 1 for name in _FIXTURE_TESTS.values()):
        _abort("real-tmux selection lacks the closed fixture binding")
    return ["name", "path"], dict(_FIXTURE_TESTS)


def _atomic_file(
    path: pathlib.Path,
    contents: bytes,
    *,
    parent_fd: int | None = None,
    validator: t.Callable[[], None] | None = None,
) -> None:
    r"""Atomically replace one named record after its gate is complete.

    Examples
    --------
    >>> with tempfile.TemporaryDirectory() as raw:
    ...     record = pathlib.Path(raw) / "record.json"
    ...     _atomic_file(record, b"{}\n")
    ...     record.read_bytes()
    b'{}\n'
    """
    owned_parent = parent_fd is None
    committed = False
    if parent_fd is None:
        parent_fd = _open_directory_tree(path.parent, "record parent", create=True)
    if path.name in {"", ".", ".."} or pathlib.PurePath(path.name).name != path.name:
        if owned_parent:
            os.close(parent_fd)
        _abort("unsafe record name")
    temporary = f".ctest-gate-{secrets.token_hex(16)}"
    temporary_created = False
    lock_acquired = False
    prior_contents: bytes | None = None
    try:
        fcntl.flock(parent_fd, fcntl.LOCK_EX)
        lock_acquired = True
        try:
            destination = os.lstat(path.name, dir_fd=parent_fd)
        except FileNotFoundError:
            destination = None
        if destination is not None and not stat.S_ISREG(destination.st_mode):
            _abort("record is not a regular file")
        if destination is not None and destination.st_nlink != 1:
            _abort("record has filesystem aliases")
        if destination is not None:
            prior_contents = _read_regular_at(parent_fd, path.name, "record")
        descriptor = os.open(
            temporary,
            os.O_RDWR | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
            0o600,
            dir_fd=parent_fd,
        )
        temporary_created = True
        _write_verified_file(descriptor, contents, "record temporary")
        try:
            current = os.lstat(path.name, dir_fd=parent_fd)
        except FileNotFoundError:
            current = None
        if (destination is None) != (current is None) or (
            destination is not None
            and current is not None
            and not os.path.samestat(destination, current)
        ):
            _abort("record changed during publication")
        if current is not None and current.st_nlink != 1:
            _abort("record has filesystem aliases")
        if destination is not None and _read_regular_at(
            parent_fd, path.name, "record"
        ) != t.cast(bytes, prior_contents):
            _abort("record bytes changed during publication")
        if validator is not None:
            validator()
        if _read_regular_at(parent_fd, temporary, "record temporary") != contents:
            _abort("record temporary bytes differ")
        if destination is None:
            try:
                _rename_noreplace(parent_fd, temporary, path.name)
            except GateError as error:
                if str(error) == "immutable gate leaf already exists":
                    _abort("record changed during publication")
                raise
            temporary_created = False
        else:
            _rename_exchange(parent_fd, temporary, path.name)
        committed = True
        try:
            if _read_regular_at(parent_fd, path.name, "record") != contents:
                _abort("published record bytes differ")
            if destination is not None and _read_regular_at(
                parent_fd, temporary, "record rollback candidate"
            ) != t.cast(bytes, prior_contents):
                _abort("record rollback candidate differs")
            if validator is not None:
                validator()
            os.fsync(parent_fd)
        except BaseException:
            try:
                if destination is None:
                    os.unlink(path.name, dir_fd=parent_fd)
                else:
                    _rename_exchange(parent_fd, temporary, path.name)
                committed = False
                if destination is None:
                    try:
                        os.lstat(path.name, dir_fd=parent_fd)
                    except FileNotFoundError:
                        pass
                    else:
                        _abort("record rollback did not restore absence")
                elif _read_regular_at(
                    parent_fd, path.name, "restored record"
                ) != t.cast(bytes, prior_contents):
                    _abort("record rollback did not restore prior bytes")
                os.fsync(parent_fd)
            except BaseException as rollback_error:
                detail = "record rollback failed"
                raise GateError(detail) from rollback_error
            raise
    finally:
        if temporary_created:
            if committed:
                with contextlib.suppress(OSError):
                    os.unlink(temporary, dir_fd=parent_fd)
            else:
                with contextlib.suppress(FileNotFoundError):
                    os.unlink(temporary, dir_fd=parent_fd)
        if lock_acquired:
            if committed:
                with contextlib.suppress(OSError):
                    fcntl.flock(parent_fd, fcntl.LOCK_UN)
            else:
                fcntl.flock(parent_fd, fcntl.LOCK_UN)
        if owned_parent:
            if committed:
                with contextlib.suppress(OSError):
                    os.close(parent_fd)
            else:
                os.close(parent_fd)


def _write_verified_file(descriptor: int, contents: bytes, label: str) -> None:
    """Write, sync, and verify one new single-link regular file.

    Examples
    --------
    >>> with tempfile.TemporaryDirectory() as raw:
    ...     path = pathlib.Path(raw) / "artifact"
    ...     descriptor = os.open(path, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
    ...     _write_verified_file(descriptor, b"evidence", "artifact")
    ...     path.read_bytes()
    b'evidence'
    """
    with os.fdopen(descriptor, "w+b") as handle:
        handle.write(contents)
        handle.flush()
        os.fsync(handle.fileno())
        metadata = os.fstat(handle.fileno())
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
            _abort(f"{label} is not a single-link regular file")
        handle.seek(0)
        if handle.read() != contents:
            _abort(f"{label} bytes changed before publication")
        after = os.fstat(handle.fileno())
        if not os.path.samestat(metadata, after) or after.st_nlink != 1:
            _abort(f"{label} changed before publication")


def _read_regular_at(directory_fd: int, name: str, label: str) -> bytes:
    """Read one regular, nonsymlinked file beneath an anchored directory.

    Examples
    --------
    >>> with tempfile.TemporaryDirectory() as raw:
    ...     root = pathlib.Path(raw)
    ...     _ = (root / "artifact").write_bytes(b"evidence")
    ...     descriptor = _open_directory_tree(root, "root", create=False)
    ...     try:
    ...         contents = _read_regular_at(descriptor, "artifact", "artifact")
    ...     finally:
    ...         os.close(descriptor)
    ...     contents
    b'evidence'
    """
    try:
        metadata = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
    except FileNotFoundError:
        _abort(f"missing {label}")
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
        _abort(f"{label} is not a single-link regular file")
    try:
        descriptor = os.open(
            name,
            os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0),
            dir_fd=directory_fd,
        )
    except OSError:
        _abort(f"{label} is not a regular file")
    with os.fdopen(descriptor, "rb") as handle:
        opened = os.fstat(handle.fileno())
        if (
            not os.path.samestat(metadata, opened)
            or not stat.S_ISREG(opened.st_mode)
            or opened.st_nlink != 1
        ):
            _abort(f"{label} changed while opening")
        contents = handle.read()
        after = os.fstat(handle.fileno())
        if not os.path.samestat(opened, after) or after.st_nlink != 1:
            _abort(f"{label} changed while reading")
        try:
            final_path = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        except FileNotFoundError:
            _abort(f"{label} changed while reading")
        if (
            not os.path.samestat(after, final_path)
            or not stat.S_ISREG(final_path.st_mode)
            or final_path.st_nlink != 1
        ):
            _abort(f"{label} changed while reading")
        return contents


def _verify_leaf_at(root_fd: int, name: str, artifacts: dict[str, bytes]) -> None:
    r"""Verify an existing immutable leaf using a trusted parent descriptor.

    Examples
    --------
    >>> with tempfile.TemporaryDirectory() as raw:
    ...     root = pathlib.Path(raw)
    ...     (root / "leaf").mkdir()
    ...     _ = (root / "leaf" / "gate.json").write_bytes(b"{}\n")
    ...     descriptor = _open_directory_tree(root, "root", create=False)
    ...     try:
    ...         _verify_leaf_at(descriptor, "leaf", {"gate.json": b"{}\n"})
    ...     finally:
    ...         os.close(descriptor)
    """
    try:
        metadata = os.stat(name, dir_fd=root_fd, follow_symlinks=False)
    except FileNotFoundError:
        _abort("missing immutable gate leaf")
    if not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
        _abort("immutable gate leaf is not a directory")
    try:
        leaf_fd = os.open(
            name,
            os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0),
            dir_fd=root_fd,
        )
    except OSError:
        _abort("immutable gate leaf is not a trusted directory")
    try:
        opened = os.fstat(leaf_fd)
        if not os.path.samestat(metadata, opened) or not stat.S_ISDIR(opened.st_mode):
            _abort("immutable gate leaf changed while opening")
        expected_entries = set(artifacts)
        if set(os.listdir(leaf_fd)) != expected_entries:  # noqa: PTH208
            _abort("immutable gate leaf has unexpected entries")
        for artifact_name, contents in artifacts.items():
            if (
                _read_regular_at(leaf_fd, artifact_name, "immutable gate artifact")
                != contents
            ):
                _abort("immutable gate leaf differs from new evidence")
        after = os.fstat(leaf_fd)
        try:
            final_path = os.stat(name, dir_fd=root_fd, follow_symlinks=False)
        except FileNotFoundError:
            _abort("immutable gate leaf changed during validation")
        if (
            not os.path.samestat(opened, after)
            or not os.path.samestat(after, final_path)
            or not stat.S_ISDIR(final_path.st_mode)
        ):
            _abort("immutable gate leaf changed during validation")
        if set(os.listdir(leaf_fd)) != expected_entries:  # noqa: PTH208
            _abort("immutable gate leaf has unexpected entries")
    finally:
        os.close(leaf_fd)


def _rename_noreplace(root_fd: int, staging: str, leaf: str) -> None:
    """Atomically install one private staging directory without replacement.

    Examples
    --------
    >>> with tempfile.TemporaryDirectory() as raw:
    ...     root = pathlib.Path(raw)
    ...     (root / "staging").mkdir()
    ...     descriptor = _open_directory_tree(root, "root", create=False)
    ...     _rename_noreplace(descriptor, "staging", "leaf")
    ...     os.close(descriptor)
    ...     (root / "leaf").is_dir()
    True
    """
    try:
        renameat2 = ctypes.CDLL(None, use_errno=True).renameat2
    except AttributeError:
        _abort("renameat2 is unavailable for immutable publication")
    renameat2.argtypes = [
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_uint,
    ]
    renameat2.restype = ctypes.c_int
    if renameat2(root_fd, staging.encode(), root_fd, leaf.encode(), 1) != 0:
        error = ctypes.get_errno()
        if error == errno.EEXIST:
            _abort("immutable gate leaf already exists")
        _abort(f"immutable gate publication failed: {os.strerror(error)}")


def _rename_exchange(directory_fd: int, first: str, second: str) -> None:
    """Atomically exchange two entries for exact named-record rollback.

    Examples
    --------
    >>> with tempfile.TemporaryDirectory() as raw:
    ...     root = pathlib.Path(raw)
    ...     _ = (root / "first").write_text("first")
    ...     _ = (root / "second").write_text("second")
    ...     descriptor = _open_directory_tree(root, "root", create=False)
    ...     _rename_exchange(descriptor, "first", "second")
    ...     os.close(descriptor)
    ...     [(root / name).read_text() for name in ("first", "second")]
    ['second', 'first']
    """
    try:
        renameat2 = ctypes.CDLL(None, use_errno=True).renameat2
    except AttributeError:
        _abort("renameat2 is unavailable for record publication")
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
        error = ctypes.get_errno()
        _abort(f"record exchange failed: {os.strerror(error)}")


def _remove_staging_at(root_fd: int, name: str) -> None:
    """Remove a private staging directory after a failed install.

    Examples
    --------
    >>> with tempfile.TemporaryDirectory() as raw:
    ...     root = pathlib.Path(raw)
    ...     (root / "staging" / "nested").mkdir(parents=True)
    ...     _ = (root / "staging" / "nested" / "artifact").write_bytes(b"data")
    ...     descriptor = _open_directory_tree(root, "root", create=False)
    ...     _remove_staging_at(descriptor, "staging")
    ...     os.close(descriptor)
    ...     (root / "staging").exists()
    False
    """
    try:
        staging_fd = os.open(
            name,
            os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0),
            dir_fd=root_fd,
        )
    except FileNotFoundError:
        return

    def clear(directory_fd: int) -> None:
        for entry in os.listdir(directory_fd):
            metadata = os.stat(entry, dir_fd=directory_fd, follow_symlinks=False)
            if stat.S_ISDIR(metadata.st_mode) and not stat.S_ISLNK(metadata.st_mode):
                child_fd = os.open(
                    entry,
                    os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0),
                    dir_fd=directory_fd,
                )
                try:
                    clear(child_fd)
                finally:
                    os.close(child_fd)
                os.rmdir(entry, dir_fd=directory_fd)
            else:
                os.unlink(entry, dir_fd=directory_fd)

    try:
        clear(staging_fd)
    finally:
        os.close(staging_fd)
    os.rmdir(name, dir_fd=root_fd)


def _install_leaf_at(root_fd: int, digest: str, artifacts: dict[str, bytes]) -> None:
    r"""Install a leaf through an already anchored output-root descriptor.

    Examples
    --------
    >>> with tempfile.TemporaryDirectory() as raw:
    ...     root = pathlib.Path(raw)
    ...     descriptor = _open_directory_tree(root, "root", create=False)
    ...     try:
    ...         _install_leaf_at(
    ...             descriptor, "sha256:" + "a" * 64, {"gate.json": b"{}\n"}
    ...         )
    ...     finally:
    ...         os.close(descriptor)
    ...     (root / ("a" * 64) / "gate.json").read_bytes()
    b'{}\n'
    """
    leaf = digest.removeprefix("sha256:")
    staging = f".ctest-gate-{secrets.token_hex(16)}"
    staging_created = False
    try:
        try:
            os.lstat(leaf, dir_fd=root_fd)
        except FileNotFoundError:
            pass
        else:
            _verify_leaf_at(root_fd, leaf, artifacts)
            return
        os.mkdir(staging, mode=0o700, dir_fd=root_fd)
        staging_created = True
        staging_fd = os.open(
            staging,
            os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0),
            dir_fd=root_fd,
        )
        try:
            for name, contents in artifacts.items():
                descriptor = os.open(
                    name,
                    os.O_RDWR | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
                    0o600,
                    dir_fd=staging_fd,
                )
                _write_verified_file(descriptor, contents, "staged gate artifact")
            os.fsync(staging_fd)
        finally:
            os.close(staging_fd)
        try:
            _rename_noreplace(root_fd, staging, leaf)
        except GateError as error:
            if str(error) != "immutable gate leaf already exists":
                raise
        _verify_leaf_at(root_fd, leaf, artifacts)
        os.fsync(root_fd)
    finally:
        if staging_created:
            _remove_staging_at(root_fd, staging)


def _validate_public_record(value: object, forbidden: t.Collection[str]) -> None:
    """Reject path-bearing or private strings from the normalized record.

    Examples
    --------
    >>> _validate_public_record({"id": "gate"}, {"/tmp/source"})
    """
    if isinstance(value, str):
        if (
            _DRIVE_PATH.match(value)
            or _EMBEDDED_DRIVE_PATH.search(value)
            or _EMBEDDED_UNIX_PATH.search(value)
            or "file://" in value
            or any(pattern and pattern in value for pattern in forbidden)
        ):
            _abort("gate record contains a private or physical path")
        return
    if isinstance(value, dict):
        for key, item in value.items():
            if not isinstance(key, str):
                _abort("gate record has a non-string key")
            _validate_public_record(item, forbidden)
        return
    if isinstance(value, list):
        for item in value:
            _validate_public_record(item, forbidden)
        return
    if type(value) is float:
        if not math.isfinite(value):
            _abort("gate record has a nonfinite numeric value")
        return
    if value is None or type(value) in {bool, int}:
        return
    _abort("gate record has an unsupported value")


def build_parser() -> argparse.ArgumentParser:
    """Build the CTest gate command-line parser.

    Examples
    --------
    >>> args = ["--source-dir", "cxx", "--preset", "cxx-dev", "--match", "."]
    >>> args += ["--gate-id", "gate", "--output-root", "out", "--record", "record.json"]
    >>> build_parser().parse_args(args).preset
    'cxx-dev'
    """
    parser = argparse.ArgumentParser(prog="python -m tools.evidence.ctest_gate")
    parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    parser.add_argument("--preset", required=True)
    selector = parser.add_mutually_exclusive_group(required=True)
    selector.add_argument("--label")
    selector.add_argument("--match")
    parser.add_argument("--gate-id", required=True)
    parser.add_argument("--output-root", type=pathlib.Path, required=True)
    parser.add_argument("--record", type=pathlib.Path, required=True)
    return parser


def run(namespace: argparse.Namespace) -> dict[str, object]:
    """Capture and publish one CTest selection after immutable validation.

    Examples
    --------
    >>> with monkeypatch.context() as patch:
    ...     patch.setenv(_CTEST_OVERRIDE, "/__missing_ctest__/ctest")
    ...     try:
    ...         run(argparse.Namespace())
    ...     except GateError as error:
    ...         str(error)
    'missing CTest executable'
    """
    with _selected_ctest() as ctest:
        source = _safe_existing_path(
            namespace.source_dir,
            "source directory",
            directory=True,
        )
        preset = namespace.preset
        if (
            not isinstance(preset, str)
            or not preset
            or "/" in preset
            or "\\" in preset
            or _DRIVE_PATH.match(preset)
        ):
            _abort("unsafe preset")
        build = _preset_build_dir(source, namespace.preset)
        with _selected_compiler(build) as compiler:
            return _run_with_ctest(namespace, ctest=ctest, compiler=compiler)


def _run_with_ctest(
    namespace: argparse.Namespace,
    *,
    ctest: _ExecutableBinding,
    compiler: _CompilerBinding | None = None,
) -> dict[str, object]:
    """Capture one gate with a retained CTest executable identity.

    Examples
    --------
    >>> namespace = argparse.Namespace(source_dir=pathlib.Path("__missing_source__"))
    >>> try:
    ...     _run_with_ctest(namespace, ctest=t.cast(_ExecutableBinding, None))
    ... except GateError as error:
    ...     str(error)
    'missing source directory'
    """
    source = _safe_existing_path(
        namespace.source_dir, "source directory", directory=True
    )
    for value, label in ((namespace.preset, "preset"), (namespace.gate_id, "gate ID")):
        if (
            not isinstance(value, str)
            or not value
            or "/" in value
            or "\\" in value
            or _DRIVE_PATH.match(value)
        ):
            _abort(f"unsafe {label}")
    selector = (
        {"label": namespace.label}
        if namespace.label is not None
        else {"match": namespace.match}
    )
    selector_value = next(iter(selector.values()))
    if (
        not isinstance(selector_value, str)
        or not selector_value
        or _DRIVE_PATH.match(selector_value)
        or pathlib.PurePath(selector_value).is_absolute()
    ):
        _abort("unsafe CTest selector")
    output_root = namespace.output_root.absolute()
    record = namespace.record.absolute()
    for path, label in ((output_root, "output root"), (record, "record")):
        _reject_windows_path(str(path), label)
        if not path.is_absolute() or any(
            part in {"", ".", ".."} for part in path.parts
        ):
            _abort(f"unsafe {label}")
    if record.is_relative_to(output_root) or output_root.is_relative_to(record):
        _abort("record must be outside the output root")
    build = _preset_build_dir(source, namespace.preset)
    if compiler is None:
        _abort("missing configured compiler binding")
    selector_argv = (
        ["-L", selector_value] if "label" in selector else ["-R", selector_value]
    )
    _verify_selected_ctest(ctest)
    _verify_selected_compiler(compiler)
    before_discovery = _snapshot(build, [])
    retained_cache_sha256 = _sha256(compiler.cache.contents)
    retained_compile_commands_sha256 = _sha256(compiler.compile_commands.contents)
    if before_discovery["cache_sha256"] != retained_cache_sha256:
        _abort("CMake cache changed after compiler selection")
    if before_discovery["compile_commands_sha256"] != retained_compile_commands_sha256:
        _abort("compile commands changed after compiler selection")
    discovery_environment = dict(os.environ)
    discovery_environment.pop(_CTEST_OVERRIDE, None)
    discovery = subprocess.run(
        [
            os.fspath(ctest.path),
            "--preset",
            namespace.preset,
            "--show-only=json-v1",
            *selector_argv,
        ],
        cwd=source,
        env=discovery_environment,
        check=False,
        capture_output=True,
        close_fds=True,
    )
    _verify_selected_ctest(ctest)
    _verify_selected_compiler(compiler)
    if discovery.returncode != 0:
        _abort("CTest discovery failed")
    registry_bytes = discovery.stdout
    _, names, executables = _selected_tests(registry_bytes, build)
    before_execution = _snapshot(build, executables)
    if before_execution["cache_sha256"] != retained_cache_sha256:
        _abort("CMake cache changed after compiler selection")
    if before_execution["compile_commands_sha256"] != retained_compile_commands_sha256:
        _abort("compile commands changed after compiler selection")
    if (
        before_discovery["cache_sha256"] != before_execution["cache_sha256"]
        or before_discovery["registration_files"]
        != before_execution["registration_files"]
    ):
        _abort("preset tree changed during CTest discovery")
    if (
        before_discovery["compile_commands_sha256"]
        != before_execution["compile_commands_sha256"]
    ):
        _abort("compile commands changed during CTest discovery")
    output_fd = _open_directory_tree(output_root, "output root", create=True)
    try:
        record_parent_fd = _open_directory_tree(
            record.parent,
            "record parent",
            create=True,
            forbidden_fd=output_fd,
        )
        try:
            _verify_directory_anchor(output_root, output_fd, "output root")
            _verify_directory_anchor(record.parent, record_parent_fd, "record parent")
            junit_root = pathlib.Path(tempfile.gettempdir()).absolute()
            junit_parent_fd = _open_directory_tree(
                junit_root,
                "JUnit temporary root",
                create=False,
            )
            junit_staging = f".libtmux-ctest-junit-{secrets.token_hex(16)}"
            junit_staging_created = False
            junit_dir_fd: int | None = None
            try:
                os.mkdir(junit_staging, mode=0o700, dir_fd=junit_parent_fd)
                junit_staging_created = True
                junit_dir_fd = os.open(
                    junit_staging,
                    os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0),
                    dir_fd=junit_parent_fd,
                )
                junit_directory = junit_root / junit_staging
                _verify_directory_anchor(
                    junit_directory,
                    junit_dir_fd,
                    "JUnit staging directory",
                )
                junit_name = "results.junit.xml"
                descriptor = os.open(
                    junit_name,
                    os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
                    0o600,
                    dir_fd=junit_dir_fd,
                )
                os.close(descriptor)
                _verify_selected_ctest(ctest)
                _verify_selected_compiler(compiler)
                execution_environment = dict(os.environ)
                execution_environment.pop(_CTEST_OVERRIDE, None)
                execution = subprocess.run(
                    [
                        os.fspath(ctest.path),
                        "--preset",
                        namespace.preset,
                        "--no-tests=error",
                        "--output-junit",
                        str(junit_directory / junit_name),
                        *selector_argv,
                    ],
                    cwd=source,
                    env=execution_environment,
                    check=False,
                    capture_output=True,
                    close_fds=True,
                )
                _verify_selected_ctest(ctest)
                _verify_selected_compiler(compiler)
                if execution.returncode != 0:
                    _abort("CTest execution failed")
                _verify_directory_anchor(
                    junit_directory,
                    junit_dir_fd,
                    "JUnit staging directory",
                )
                if set(os.listdir(junit_dir_fd)) != {junit_name}:  # noqa: PTH208
                    _abort("CTest JUnit staging has unexpected entries")
                junit_bytes = _read_regular_at(junit_dir_fd, junit_name, "CTest JUnit")
                _junit_names(junit_bytes, names)
                after_execution = _snapshot(build, executables)
                if after_execution["cache_sha256"] != retained_cache_sha256:
                    _abort("CMake cache changed after compiler selection")
                if (
                    after_execution["compile_commands_sha256"]
                    != retained_compile_commands_sha256
                ):
                    _abort("compile commands changed after compiler selection")
                if before_execution != after_execution:
                    if (
                        before_execution["compile_commands_sha256"]
                        != after_execution["compile_commands_sha256"]
                    ):
                        _abort("compile commands changed during CTest execution")
                    _abort("preset tree changed during CTest execution")
                _verify_selected_ctest(ctest)
                _verify_selected_compiler(compiler)
            finally:
                if junit_dir_fd is not None:
                    os.close(junit_dir_fd)
                try:
                    if junit_staging_created:
                        _remove_staging_at(junit_parent_fd, junit_staging)
                finally:
                    os.close(junit_parent_fd)
            fixture_modes, fixture_binding = _fixture_binding(names, selector)
            registered_test_ids = sorted(names)
            executed_test_ids = sorted(names)
            if registered_test_ids != executed_test_ids:
                _abort("registered and executed test IDs differ")
            execution_projection = {
                "preset": namespace.preset,
                "selector": selector,
                "registered_test_ids": registered_test_ids,
                "executed_test_ids": executed_test_ids,
                "fixture_modes": fixture_modes,
                "fixture_binding": fixture_binding,
            }
            execution_sha256 = _sha256(_canonical_bytes(execution_projection))
            gate = {
                "schema_version": 2,
                "gate_id": namespace.gate_id,
                "status": "passed",
                "preset": namespace.preset,
                "selector": selector,
                "ctest_names": registered_test_ids,
                "registered_test_ids": registered_test_ids,
                "executed_test_ids": executed_test_ids,
                "fixture_modes": fixture_modes,
                "fixture_binding": fixture_binding,
                "registration_sha256": _sha256(registry_bytes),
                "junit_sha256": _sha256(junit_bytes),
                "execution_sha256": execution_sha256,
                "compiler": _compiler_public_identity(compiler),
                "artifacts": {
                    "registration": "registered-tests.json",
                    "junit": "results.junit.xml",
                },
                "raw_bindings": {
                    "registry_sha256": _sha256(registry_bytes),
                    "junit_sha256": _sha256(junit_bytes),
                    "build_snapshot": before_execution,
                },
            }
            _validate_public_record(
                gate,
                {str(source), str(output_root), "/home/", "/Users/"},
            )
            gate_sha256 = _sha256(_canonical_bytes(gate))
            gate["gate_sha256"] = gate_sha256
            gate_bytes = _canonical_bytes(gate)
            artifacts = {
                "registered-tests.json": registry_bytes,
                "results.junit.xml": junit_bytes,
                "gate.json": gate_bytes,
            }
            _verify_selected_ctest(ctest)
            _verify_selected_compiler(compiler)
            _install_leaf_at(
                output_fd,
                gate_sha256,
                artifacts,
            )
            _verify_selected_ctest(ctest)
            _verify_selected_compiler(compiler)

            def validate_publication() -> None:
                """Validate every anchored object bound by named publication."""
                _verify_selected_ctest(ctest)
                _verify_selected_compiler(compiler)
                _verify_directory_anchor(output_root, output_fd, "output root")
                _verify_directory_anchor(
                    record.parent, record_parent_fd, "record parent"
                )
                _verify_leaf_at(
                    output_fd,
                    gate_sha256.removeprefix("sha256:"),
                    artifacts,
                )

            _atomic_file(
                record,
                gate_bytes,
                parent_fd=record_parent_fd,
                validator=validate_publication,
            )
        finally:
            with contextlib.suppress(OSError):
                os.close(record_parent_fd)
    finally:
        with contextlib.suppress(OSError):
            os.close(output_fd)
    return gate


def main(argv: t.Sequence[str] | None = None) -> int:
    """Run the CTest gate CLI and return a process-compatible status.

    Examples
    --------
    >>> arguments = [
    ...     "--source-dir", "source", "--preset", "cxx-dev", "--match", ".",
    ...     "--gate-id", "gate", "--output-root", "out", "--record", "record.json",
    ... ]
    >>> with monkeypatch.context() as patch:
    ...     patch.setenv(_CTEST_OVERRIDE, "/__missing_ctest__/ctest")
    ...     with tempfile.TemporaryFile(mode="w+") as errors:
    ...         with contextlib.redirect_stderr(errors):
    ...             main(arguments)
    2
    """
    try:
        run(build_parser().parse_args(argv))
    except (GateError, OSError, TypeError, subprocess.SubprocessError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
