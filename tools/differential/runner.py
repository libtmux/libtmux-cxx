"""Shell-free supervision and immutable differential execution receipts."""

from __future__ import annotations

import base64
import dataclasses
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import typing as t

from cxx.tools.parity.generate import canonical_sha256
from cxx.tools.parity.sync import semantic_contract_sha256

from .compare import structural_diff
from .model import (
    AdapterOutcome,
    AdapterSpec,
    ExecutionReceipt,
    RegistryOperation,
    ScenarioRecord,
    ScenarioRegistry,
    ScenarioSpec,
    SocketEndpoint,
    TmuxBinaryIdentity,
    load_registry,
    load_scenario,
    scenario_record_document,
    scenario_record_from_document,
    validate_observation,
)


def _observation_validation_error(
    operation: RegistryOperation, observation: t.Mapping[str, object]
) -> str | None:
    """Return a response-schema error without escaping adapter supervision.

    Parameters
    ----------
    operation : RegistryOperation
        Registry row owning the observation.
    observation : Mapping[str, object]
        Canonical observation to validate.

    Returns
    -------
    str | None
        Validation message, or ``None`` for a conforming observation.

    Examples
    --------
    >>> callable(_observation_validation_error)
    True
    """
    try:
        validate_observation(operation, observation)
    except ValueError as error:
        return str(error)
    return None


def _load_json_object(path: pathlib.Path) -> dict[str, object]:
    """Load one UTF-8 JSON object for execution validation.

    Parameters
    ----------
    path : pathlib.Path
        Artifact path.

    Returns
    -------
    dict[str, object]
        Decoded object.

    Raises
    ------
    ValueError
        Raised when JSON is malformed or not an object.

    Examples
    --------
    >>> callable(_load_json_object)
    True
    """
    try:
        value: object = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        msg = f"malformed JSON artifact {path}: {error}"
        raise ValueError(msg) from error
    if not isinstance(value, dict):
        msg = f"malformed JSON artifact {path}: root is not an object"
        raise TypeError(msg)
    return t.cast(dict[str, object], value)


def _file_sha256(path: pathlib.Path) -> str:
    """Hash one regular file with a SHA-256 prefix.

    Parameters
    ----------
    path : pathlib.Path
        File to hash.

    Returns
    -------
    str
        Prefixed lowercase digest.

    Examples
    --------
    >>> _file_sha256(pathlib.Path('pyproject.toml')).startswith('sha256:')
    True
    """
    return f"sha256:{hashlib.sha256(path.read_bytes()).hexdigest()}"


@dataclasses.dataclass(frozen=True, slots=True)
class _BoundArtifact:
    """One file identity retained across adapter execution.

    Attributes
    ----------
    locator : pathlib.Path
        Absolute original path whose target mapping must remain stable.
    target : pathlib.Path
        Resolved regular file used by the execution plan.
    sha256 : str
        Digest of the bound target bytes.
    device : int
        Device identity of the bound target.
    inode : int
        Inode identity of the bound target.
    mode : int
        Permission and file-type mode of the bound target.
    """

    locator: pathlib.Path
    target: pathlib.Path
    sha256: str
    device: int
    inode: int
    mode: int


@dataclasses.dataclass(frozen=True, slots=True)
class _AdapterExecutionPlan:
    """Immutable adapter argv and artifacts used for one child execution.

    Attributes
    ----------
    argv : tuple[str, ...]
        Exact child argv with resolved file entry points or a bound module finder.
    cwd : pathlib.Path
        Frozen child resolution and execution directory.
    digest : str
        Canonical adapter identity digest.
    artifacts : tuple[_BoundArtifact, ...]
        File and target identities revalidated after child completion.
    stdin : bytes | None
        Bootstrap source supplied on standard input for module execution.
    """

    argv: tuple[str, ...]
    cwd: pathlib.Path
    digest: str
    artifacts: tuple[_BoundArtifact, ...]
    stdin: bytes | None = None


@dataclasses.dataclass(frozen=True, slots=True)
class _PythonModuleBinding:
    """One exact module spec retained for module-mode execution.

    Attributes
    ----------
    fullname : str
        Import name intercepted by the execution finder.
    artifact : _BoundArtifact | None
        Bound source artifact, or ``None`` for a namespace package.
    source : bytes | None
        Exact source bytes compiled by the execution loader.
    search_locations : tuple[pathlib.Path, ...] | None
        Resolved package search locations, or ``None`` for a plain module.
    """

    fullname: str
    artifact: _BoundArtifact | None
    source: bytes | None
    search_locations: tuple[pathlib.Path, ...] | None


_PYTHON_MODULE_RESOLVER = (
    "import importlib.machinery\n"
    "import json\n"
    "import pathlib\n"
    "import sys\n"
    "import sysconfig\n"
    "name = sys.argv[1]\n"
    "search_path = json.loads(sys.argv[2])\n"
    "include_site = sys.argv[3] == '1'\n"
    "def add_path(value):\n"
    "    if isinstance(value, str) and value not in search_path:\n"
    "        search_path.append(value)\n"
    "for value in sys.path:\n"
    "    add_path(value)\n"
    "if include_site:\n"
    "    for key in ('purelib', 'platlib'):\n"
    "        value = sysconfig.get_path(key)\n"
    "        candidate = pathlib.Path(value)\n"
    "        if candidate.is_dir() and not tuple(candidate.glob('*.pth')):\n"
    "            add_path(value)\n"
    "def startup_customized(paths):\n"
    "    for value in paths:\n"
    "        root = pathlib.Path(value)\n"
    "        for name in ('sitecustomize', 'usercustomize'):\n"
    "            if (root / name).is_dir():\n"
    "                return True\n"
    "            for suffix in ('.py', '.pyc'):\n"
    "                if (root / (name + suffix)).is_file():\n"
    "                    return True\n"
    "    return False\n"
    "if startup_customized(search_path):\n"
    "    raise SystemExit('unsupported Python startup customization')\n"
    "records = []\n"
    "def record(fullname, path):\n"
    "    spec = importlib.machinery.PathFinder.find_spec(fullname, path)\n"
    "    if spec is None:\n"
    "        raise SystemExit('module not found')\n"
    "    locations = spec.submodule_search_locations\n"
    "    if locations is not None:\n"
    "        locations = list(locations)\n"
    "        if not all(isinstance(item, str) for item in locations):\n"
    "            raise SystemExit('module search location is invalid')\n"
    "    loader = spec.loader\n"
    "    if spec.origin is None:\n"
    "        if loader is not None or locations is None:\n"
    "            raise SystemExit('module loader is unsupported')\n"
    "    else:\n"
    "        if (\n"
    "            not isinstance(spec.origin, str)\n"
    "            or type(loader) is not importlib.machinery.SourceFileLoader\n"
    "            or not any(\n"
    "                spec.origin.endswith(suffix)\n"
    "                for suffix in importlib.machinery.SOURCE_SUFFIXES\n"
    "            )\n"
    "            or vars(loader) != {'name': fullname, 'path': spec.origin}\n"
    "        ):\n"
    "            raise SystemExit('module loader is unsupported')\n"
    "    records.append({\n"
    "        'fullname': fullname, 'origin': spec.origin,\n"
    "        'search_locations': locations,\n"
    "    })\n"
    "    return locations\n"
    "parts = name.split('.')\n"
    "if not parts or any(not part for part in parts):\n"
    "    raise SystemExit('module name is invalid')\n"
    "path = search_path\n"
    "for index in range(1, len(parts) + 1):\n"
    "    fullname = '.'.join(parts[:index])\n"
    "    path = record(fullname, path)\n"
    "    if index < len(parts) and path is None:\n"
    "        raise SystemExit('module parent is not a package')\n"
    "if records[-1]['search_locations'] is not None:\n"
    "    main_locations = record(name + '.__main__', path)\n"
    "    if main_locations is not None:\n"
    "        raise SystemExit('package __main__ is not a module')\n"
    "sys.stdout.write(json.dumps({'records': records, 'search_path': search_path}))\n"
)


_PYTHON_MODULE_BOOTSTRAP = (
    "import base64\n"
    "import importlib\n"
    "import importlib.abc\n"
    "import importlib.machinery\n"
    "import importlib.util\n"
    "import json\n"
    "import os\n"
    "import runpy\n"
    "import sys\n"
    "requested = sys.argv[1]\n"
    "bindings = {item['fullname']: item for item in json.loads(sys.argv[2])}\n"
    "planned_path = json.loads(sys.argv[3])\n"
    "planned_cwd = sys.argv[4]\n"
    "pre_module_flags = json.loads(sys.argv[5])\n"
    "planned_pythonpath = json.loads(sys.argv[6])\n"
    "def startup_customized():\n"
    "    for root in planned_path:\n"
    "        for name in ('sitecustomize', 'usercustomize'):\n"
    "            if os.path.isdir(os.path.join(root, name)):\n"
    "                return True\n"
    "            for suffix in ('.py', '.pyc'):\n"
    "                if os.path.isfile(os.path.join(root, name + suffix)):\n"
    "                    return True\n"
    "    return False\n"
    "if startup_customized():\n"
    "    raise SystemExit('unsupported Python startup customization')\n"
    "if any(fullname in sys.modules for fullname in bindings):\n"
    "    raise SystemExit('bound Python module loaded before execution')\n"
    "executed_bindings = set()\n"
    "class _ExactSourceLoader(importlib.abc.Loader):\n"
    "    def __init__(self, binding):\n"
    "        self.binding = binding\n"
    "    def create_module(self, spec):\n"
    "        return None\n"
    "    def exec_module(self, module):\n"
    "        exec(self.get_code(module.__name__), module.__dict__)\n"
    "        executed_bindings.add(module.__name__)\n"
    "        if any(\n"
    "            fullname in sys.modules and fullname not in executed_bindings\n"
    "            for fullname in bindings\n"
    "        ):\n"
    "            raise ImportError('bound Python module loaded before execution')\n"
    "    def get_code(self, fullname):\n"
    "        if fullname != self.binding['fullname']:\n"
    "            raise ImportError(fullname)\n"
    "        source = base64.b64decode(self.binding['source'], validate=True)\n"
    "        return compile(\n"
    "            source, self.binding['origin'], 'exec',\n"
    "            dont_inherit=True, optimize=sys.flags.optimize,\n"
    "        )\n"
    "    def get_filename(self, fullname):\n"
    "        return self.binding['origin']\n"
    "    def is_package(self, fullname):\n"
    "        return self.binding['search_locations'] is not None\n"
    "class _ExactModuleFinder(importlib.abc.MetaPathFinder):\n"
    "    def find_spec(self, fullname, path=None, target=None):\n"
    "        binding = bindings.get(fullname)\n"
    "        if binding is None:\n"
    "            return None\n"
    "        if path is not None:\n"
    "            parent = fullname.rpartition('.')[0]\n"
    "            parent_binding = bindings.get(parent)\n"
    "            expected = (\n"
    "                None if parent_binding is None\n"
    "                else parent_binding['search_locations']\n"
    "            )\n"
    "            try:\n"
    "                actual = [os.path.realpath(item) for item in path]\n"
    "            except TypeError as error:\n"
    "                raise ImportError(\n"
    "                    'bound Python package search path changed'\n"
    "                ) from error\n"
    "            if expected is None or actual != expected:\n"
    "                raise ImportError('bound Python package search path changed')\n"
    "        origin = binding['origin']\n"
    "        locations = binding['search_locations']\n"
    "        if origin is None:\n"
    "            executed_bindings.add(fullname)\n"
    "            spec = importlib.machinery.ModuleSpec(\n"
    "                fullname, None, is_package=True\n"
    "            )\n"
    "            spec.submodule_search_locations = list(locations)\n"
    "            return spec\n"
    "        loader = _ExactSourceLoader(binding)\n"
    "        return importlib.util.spec_from_file_location(\n"
    "            fullname, origin, loader=loader,\n"
    "            submodule_search_locations=locations\n"
    "        )\n"
    "os.chdir(planned_cwd)\n"
    "if planned_pythonpath is None:\n"
    "    os.environ.pop('PYTHONPATH', None)\n"
    "else:\n"
    "    os.environ['PYTHONPATH'] = planned_pythonpath\n"
    "sys.path[:] = planned_path\n"
    "sys.meta_path.insert(0, _ExactModuleFinder())\n"
    "runtime_args = sys.argv[7:]\n"
    "sys.orig_argv[:] = [\n"
    "    sys.orig_argv[0], *pre_module_flags, '-m', requested, *runtime_args\n"
    "]\n"
    "sys.argv[:] = [requested, *runtime_args]\n"
    "runpy._run_module_as_main(requested, alter_argv=True)\n"
)


def _bind_artifact(locator: pathlib.Path, target: pathlib.Path) -> _BoundArtifact:
    """Capture one locator-to-target identity for later revalidation.

    Parameters
    ----------
    locator : pathlib.Path
        Absolute original path used during resolution.
    target : pathlib.Path
        Resolved regular file target.

    Returns
    -------
    _BoundArtifact
        Immutable path, content, and filesystem identity.

    Raises
    ------
    ValueError
        Raised when the target is not a regular file.

    Examples
    --------
    >>> artifact = _bind_artifact(
    ...     pathlib.Path('/bin/sh'), pathlib.Path('/bin/sh').resolve()
    ... )
    >>> artifact.target.is_file()
    True
    """
    if not target.is_file():
        msg = f"adapter identity target is not a regular file: {target}"
        raise ValueError(msg)
    metadata = target.stat()
    return _BoundArtifact(
        locator.absolute(),
        target,
        _file_sha256(target),
        metadata.st_dev,
        metadata.st_ino,
        metadata.st_mode,
    )


def _resolve_launcher(
    argument: str, cwd: pathlib.Path
) -> tuple[pathlib.Path, pathlib.Path]:
    """Resolve one child argv-zero spelling and its final executable target.

    Parameters
    ----------
    argument : str
        Exact argv-zero value used by the child.
    cwd : pathlib.Path
        Child working directory used for relative and ``PATH`` lookup.

    Returns
    -------
    tuple[pathlib.Path, pathlib.Path]
        Invocation path preserving symlink context and resolved file target.

    Raises
    ------
    ValueError
        Raised when argv zero cannot identify an executable regular file.

    Examples
    --------
    >>> invocation, target = _resolve_launcher('/bin/sh', pathlib.Path.cwd())
    >>> invocation.is_absolute() and target.is_file()
    True
    """
    candidate = pathlib.Path(argument).expanduser()
    if candidate.is_absolute() or candidate.parent != pathlib.Path():
        invocation = candidate if candidate.is_absolute() else cwd / candidate
    else:
        found = shutil.which(argument)
        if found is None:
            msg = f"adapter identity cannot resolve launcher {argument!r}"
            raise ValueError(msg)
        invocation = pathlib.Path(found)
        if not invocation.is_absolute():
            invocation = cwd / invocation
    invocation = invocation.absolute()
    try:
        target = invocation.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        msg = f"adapter identity cannot resolve launcher {argument!r}"
        raise ValueError(msg) from error
    if not target.is_file() or not os.access(target, os.X_OK):
        msg = f"adapter identity launcher is not executable: {argument!r}"
        raise ValueError(msg)
    return invocation, target


def _python_entrypoint(argv: tuple[str, ...]) -> tuple[str, int] | None:
    """Locate a Python command, module, or script entry point in one argv.

    Parameters
    ----------
    argv : tuple[str, ...]
        Python interpreter argv.

    Returns
    -------
    tuple[str, int] | None
        Entrypoint kind and argument index, or ``None`` for no source.

    Examples
    --------
    >>> _python_entrypoint(('python', '-I', '-m', 'example'))
    ('module', 3)
    >>> _python_entrypoint(('python', '-c', 'pass'))
    ('command', 2)
    """
    index = 1
    while index < len(argv):
        argument = argv[index]
        if argument == "-m":
            return ("module", index + 1) if index + 1 < len(argv) else None
        if argument == "-c":
            return ("command", index + 1) if index + 1 < len(argv) else None
        if argument == "--":
            return ("script", index + 1) if index + 1 < len(argv) else None
        if argument in {"-W", "-X", "--check-hash-based-pycs"}:
            index += 2
            continue
        if argument.startswith("-"):
            index += 1
            continue
        return ("script", index)
    return None


def _python_module_sources(
    launcher: pathlib.Path,
    argv: tuple[str, ...],
    module_index: int,
    cwd: pathlib.Path,
) -> tuple[tuple[_PythonModuleBinding, ...], tuple[pathlib.Path, ...]]:
    """Resolve module-mode source files with the child interpreter context.

    Parameters
    ----------
    launcher : pathlib.Path
        Invocation path used by the actual child interpreter.
    argv : tuple[str, ...]
        Adapter argv beginning with the interpreter.
    module_index : int
        Index containing the module name after ``-m``.
    cwd : pathlib.Path
        Exact child working directory.

    Returns
    -------
    tuple[tuple[_PythonModuleBinding, ...], tuple[pathlib.Path, ...]]
        Exact parent, target, and package-main bindings plus the isolated
        execution search path.

    Raises
    ------
    ValueError
        Raised when module source identity cannot be proven.

    Examples
    --------
    >>> callable(_python_module_sources)
    True
    """
    module = argv[module_index]
    if not module:
        msg = "adapter identity cannot resolve an empty Python module"
        raise ValueError(msg)
    module_flag = module_index - 1
    interpreter_flags = argv[1:module_flag]

    enabled_flags: set[str] = set()
    argument_index = 0
    while argument_index < len(interpreter_flags):
        argument = interpreter_flags[argument_index]
        if not argument.startswith("-") or argument.startswith("--"):
            argument_index += 2 if argument == "--check-hash-based-pycs" else 1
            continue
        short_options = argument[1:]
        separate_payload = False
        for flag_index, flag in enumerate(short_options):
            if flag in {"W", "X"}:
                separate_payload = flag_index == len(short_options) - 1
                break
            if flag in {"E", "I", "P", "S"}:
                enabled_flags.add(flag)
        argument_index += 2 if separate_payload else 1

    def flag_enabled(flag: str) -> bool:
        return flag in enabled_flags

    initial_path: list[str] = []

    def add_initial_path(path: pathlib.Path) -> None:
        normalized = str(path.expanduser().resolve(strict=False))
        if normalized not in initial_path:
            initial_path.append(normalized)

    ignores_environment = flag_enabled("E") or flag_enabled("I")
    safe_path = flag_enabled("P") or flag_enabled("I")
    if not ignores_environment:
        safe_path = safe_path or bool(os.environ.get("PYTHONSAFEPATH"))
    if not safe_path:
        add_initial_path(cwd)
    if not ignores_environment:
        python_path = os.environ.get("PYTHONPATH")
        if python_path is not None:
            for value in python_path.split(os.pathsep):
                candidate = pathlib.Path(value) if value else cwd
                if not candidate.is_absolute():
                    candidate = cwd / candidate
                add_initial_path(candidate)
    try:
        result = subprocess.run(
            [
                str(launcher),
                "-I",
                "-S",
                "-B",
                "-c",
                _PYTHON_MODULE_RESOLVER,
                module,
                json.dumps(initial_path, separators=(",", ":")),
                "0" if flag_enabled("S") else "1",
            ],
            cwd=cwd,
            check=False,
            capture_output=True,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        msg = f"adapter identity cannot resolve Python module {module!r}"
        raise ValueError(msg) from error
    if result.returncode != 0:
        msg = f"adapter identity cannot resolve Python module {module!r}"
        raise ValueError(msg)
    try:
        raw_result: object = json.loads(result.stdout)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        msg = f"adapter identity cannot resolve Python module {module!r}"
        raise ValueError(msg) from error
    if not isinstance(raw_result, dict) or set(raw_result) != {
        "records",
        "search_path",
    }:
        msg = f"adapter identity cannot resolve Python module {module!r}"
        raise ValueError(msg)
    raw_origins = raw_result["records"]
    raw_search_path = raw_result["search_path"]
    if (
        not isinstance(raw_origins, list)
        or not raw_origins
        or not isinstance(raw_search_path, list)
        or not all(isinstance(item, str) for item in raw_search_path)
    ):
        msg = f"adapter identity cannot resolve Python module {module!r}"
        raise ValueError(msg)
    search_path = tuple(
        pathlib.Path(item).resolve(strict=False)
        for item in t.cast(list[str], raw_search_path)
    )
    bindings: list[_PythonModuleBinding] = []
    for raw_binding in t.cast(list[object], raw_origins):
        if not isinstance(raw_binding, dict) or set(raw_binding) != {
            "fullname",
            "origin",
            "search_locations",
        }:
            msg = f"adapter identity cannot resolve Python module {module!r}"
            raise ValueError(msg)
        fullname = raw_binding["fullname"]
        raw_origin = raw_binding["origin"]
        raw_locations = raw_binding["search_locations"]
        if not isinstance(fullname, str) or (
            raw_origin is not None and not isinstance(raw_origin, str)
        ):
            msg = f"adapter identity cannot resolve Python module {module!r}"
            raise ValueError(msg)
        artifact: _BoundArtifact | None = None
        source: bytes | None = None
        if isinstance(raw_origin, str):
            origin_locator = pathlib.Path(raw_origin)
            if not origin_locator.is_absolute():
                origin_locator = cwd / origin_locator
            try:
                origin = origin_locator.resolve(strict=True)
            except (OSError, RuntimeError) as error:
                msg = f"adapter identity cannot resolve Python module {module!r}"
                raise ValueError(msg) from error
            if not origin.is_file():
                msg = f"adapter identity cannot resolve Python module {module!r}"
                raise ValueError(msg)
            try:
                with origin.open("rb") as stream:
                    metadata = os.fstat(stream.fileno())
                    source = stream.read()
                    final_metadata = os.fstat(stream.fileno())
            except OSError as error:
                msg = f"adapter identity cannot resolve Python module {module!r}"
                raise ValueError(msg) from error
            identity = (metadata.st_dev, metadata.st_ino, metadata.st_mode)
            final_identity = (
                final_metadata.st_dev,
                final_metadata.st_ino,
                final_metadata.st_mode,
            )
            if identity != final_identity:
                msg = f"adapter identity cannot resolve Python module {module!r}"
                raise ValueError(msg)
            artifact = _BoundArtifact(
                origin_locator.absolute(),
                origin,
                "sha256:" + hashlib.sha256(source).hexdigest(),
                metadata.st_dev,
                metadata.st_ino,
                metadata.st_mode,
            )
        search_locations: tuple[pathlib.Path, ...] | None = None
        if raw_locations is not None:
            if not isinstance(raw_locations, list) or not all(
                isinstance(item, str) for item in raw_locations
            ):
                msg = f"adapter identity cannot resolve Python module {module!r}"
                raise ValueError(msg)
            resolved_locations: list[pathlib.Path] = []
            for raw_location in t.cast(list[str], raw_locations):
                candidate = pathlib.Path(raw_location)
                if not candidate.is_absolute():
                    candidate = cwd / candidate
                try:
                    resolved = candidate.resolve(strict=True)
                except (OSError, RuntimeError) as error:
                    msg = f"adapter identity cannot resolve Python module {module!r}"
                    raise ValueError(msg) from error
                if not resolved.is_dir():
                    msg = f"adapter identity cannot resolve Python module {module!r}"
                    raise ValueError(msg)
                resolved_locations.append(resolved)
            search_locations = tuple(resolved_locations)
        if (artifact is None) != (source is None) or (
            artifact is None and search_locations is None
        ):
            msg = f"adapter identity cannot resolve Python module {module!r}"
            raise ValueError(msg)
        bindings.append(
            _PythonModuleBinding(
                fullname,
                artifact,
                source,
                search_locations,
            )
        )
    return tuple(bindings), search_path


def _build_execution_plan(
    spec: AdapterSpec, *, cwd: pathlib.Path | None = None
) -> _AdapterExecutionPlan:
    """Resolve one immutable adapter argv and its bound file identities.

    Parameters
    ----------
    spec : AdapterSpec
        Exact adapter name and base argv.
    cwd : pathlib.Path | None, optional
        Child resolution context. Defaults to the current working directory.

    Returns
    -------
    _AdapterExecutionPlan
        Resolved argv, child directory, digest, and bound artifacts.

    Raises
    ------
    ValueError
        Raised when the adapter or any executable source cannot be identified.

    Examples
    --------
    >>> plan = _build_execution_plan(AdapterSpec('shell', ('/bin/sh', '-c', 'true')))
    >>> plan.argv[0] == str(pathlib.Path('/bin/sh').resolve())
    True
    """
    if not spec.name or not spec.argv or any(not argument for argument in spec.argv):
        msg = "adapter specification requires a name and nonempty argv"
        raise ValueError(msg)
    child_cwd = (cwd or pathlib.Path.cwd()).resolve(strict=True)
    invocation, launcher = _resolve_launcher(spec.argv[0], child_cwd)
    canonical_argv = list(spec.argv)
    canonical_argv[0] = str(launcher)
    identity_argv = canonical_argv.copy()
    artifacts = [_bind_artifact(invocation, launcher)]
    files: list[dict[str, object]] = [
        {"index": 0, "path": str(launcher), "sha256": artifacts[0].sha256}
    ]
    module_specs: list[dict[str, object]] = []
    module_search_path: list[str] | None = None
    module_cwd: str | None = None
    module_stdin: bytes | None = None
    launcher_name = launcher.name.lower().removesuffix(".exe")
    python_suffix = launcher_name.removeprefix("python").removesuffix("t")
    is_python = launcher_name.startswith("python") and (
        not python_suffix
        or all(part.isdigit() for part in python_suffix.split(".") if part)
    )
    if is_python:
        entrypoint = _python_entrypoint(spec.argv)
        if entrypoint is None:
            msg = "adapter identity cannot identify Python entry point"
            raise ValueError(msg)
        kind, index = entrypoint
        if kind == "script":
            candidate = pathlib.Path(spec.argv[index]).expanduser()
            if not candidate.is_absolute():
                candidate = child_cwd / candidate
            try:
                source = candidate.resolve(strict=True)
            except (OSError, RuntimeError) as error:
                msg = (
                    "adapter identity cannot resolve Python script "
                    f"{spec.argv[index]!r}"
                )
                raise ValueError(msg) from error
            artifact = _bind_artifact(candidate, source)
            artifacts.append(artifact)
            canonical_argv[index] = str(source)
            identity_argv[index] = str(source)
            files.append(
                {
                    "index": index,
                    "path": str(source),
                    "sha256": artifact.sha256,
                }
            )
        if kind == "module":
            module_bindings, search_path = _python_module_sources(
                invocation, spec.argv, index, child_cwd
            )
            execution_specs: list[dict[str, object]] = []
            for binding in module_bindings:
                module_artifact = binding.artifact
                origin = (
                    None if module_artifact is None else str(module_artifact.target)
                )
                locations = (
                    None
                    if binding.search_locations is None
                    else [str(path) for path in binding.search_locations]
                )
                source_sha256 = (
                    None if module_artifact is None else module_artifact.sha256
                )
                module_specs.append(
                    {
                        "fullname": binding.fullname,
                        "origin": origin,
                        "search_locations": locations,
                        "sha256": source_sha256,
                    }
                )
                execution_specs.append(
                    {
                        "fullname": binding.fullname,
                        "origin": origin,
                        "search_locations": locations,
                        "source": (
                            None
                            if binding.source is None
                            else base64.b64encode(binding.source).decode("ascii")
                        ),
                    }
                )
                if module_artifact is not None:
                    artifacts.append(module_artifact)
                    files.append(
                        {
                            "module": binding.fullname,
                            "path": str(module_artifact.target),
                            "sha256": module_artifact.sha256,
                        }
                    )
            module_flag = index - 1
            module_cwd = str(child_cwd)
            module_search_path = [str(path) for path in search_path]
            canonical_argv[module_flag : index + 1] = [
                "-",
                spec.argv[index],
                json.dumps(
                    execution_specs,
                    sort_keys=True,
                    separators=(",", ":"),
                ),
                json.dumps(module_search_path, separators=(",", ":")),
                module_cwd,
                json.dumps(spec.argv[1:module_flag], separators=(",", ":")),
                json.dumps(os.environ.get("PYTHONPATH")),
            ]
            module_stdin = _PYTHON_MODULE_BOOTSTRAP.encode("utf-8")
    digest = canonical_sha256(
        {
            "name": spec.name,
            "argv": identity_argv,
            "files": files,
            "module_cwd": module_cwd,
            "module_search_path": module_search_path,
            "module_specs": module_specs,
        }
    )
    return _AdapterExecutionPlan(
        tuple(canonical_argv),
        child_cwd,
        digest,
        tuple(artifacts),
        module_stdin,
    )


def _adapter_sha256(spec: AdapterSpec, *, cwd: pathlib.Path | None = None) -> str:
    """Return the immutable execution plan digest for one adapter.

    Parameters
    ----------
    spec : AdapterSpec
        Exact adapter name and base argv.
    cwd : pathlib.Path | None, optional
        Child resolution context. Defaults to the current working directory.

    Returns
    -------
    str
        Canonical adapter identity digest.

    Raises
    ------
    ValueError
        Raised when the adapter or any executable source cannot be identified.

    Examples
    --------
    >>> import sys
    >>> spec = AdapterSpec('python', (sys.executable, '-c', 'pass'))
    >>> _adapter_sha256(spec).startswith('sha256:')
    True
    """
    return _build_execution_plan(spec, cwd=cwd).digest


def _execution_plan_changed(plan: _AdapterExecutionPlan) -> bool:
    """Rehash every bound artifact and report any path or byte change.

    Parameters
    ----------
    plan : _AdapterExecutionPlan
        Frozen pre-execution file identities.

    Returns
    -------
    bool
        Whether any locator, target, metadata, or content changed.

    Examples
    --------
    >>> plan = _build_execution_plan(AdapterSpec('shell', ('/bin/sh', '-c', 'true')))
    >>> _execution_plan_changed(plan)
    False
    """
    changed = False
    for artifact in plan.artifacts:
        try:
            current_target = artifact.locator.resolve(strict=True)
            metadata = artifact.target.stat()
            current_digest = _file_sha256(artifact.target)
        except (OSError, RuntimeError):
            changed = True
            continue
        changed = (
            current_target != artifact.target
            or metadata.st_dev != artifact.device
            or metadata.st_ino != artifact.inode
            or metadata.st_mode != artifact.mode
            or current_digest != artifact.sha256
            or changed
        )
    return changed


def _query_socket_identity(
    binary: TmuxBinaryIdentity, endpoint: SocketEndpoint
) -> SocketEndpoint:
    """Query and canonicalize the actual socket selected by one tmux server.

    Parameters
    ----------
    binary : TmuxBinaryIdentity
        Already verified executable used for the query.
    endpoint : SocketEndpoint
        Structured name or path selector supplied by the fixture.

    Returns
    -------
    SocketEndpoint
        Canonical absolute path identity reported by that tmux server.

    Raises
    ------
    ValueError
        Raised when the server cannot prove one absolute socket identity.

    Examples
    --------
    >>> callable(_query_socket_identity)
    True
    """
    selector = endpoint
    if endpoint.mode == "path":
        try:
            normalized = pathlib.Path(endpoint.value).expanduser().resolve(strict=False)
        except (OSError, RuntimeError) as error:
            msg = f"socket identity cannot normalize {endpoint.value!r}"
            raise ValueError(msg) from error
        selector = SocketEndpoint("path", str(normalized))
    try:
        result = subprocess.run(
            [
                str(binary.path),
                *selector.tmux_arguments(),
                "display-message",
                "-p",
                "#{socket_path}",
            ],
            check=False,
            capture_output=True,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        msg = f"socket identity query failed for {endpoint!r}"
        raise ValueError(msg) from error
    if result.returncode != 0 or not result.stdout:
        msg = f"socket identity query failed for {endpoint!r}: {result.stderr!r}"
        raise ValueError(msg)
    try:
        output = result.stdout.decode("utf-8")
    except UnicodeDecodeError as error:
        msg = f"socket identity query returned non-UTF-8 for {endpoint!r}"
        raise ValueError(msg) from error
    path_text = output.rstrip("\r\n")
    if (
        not path_text
        or "\x00" in path_text
        or output not in {path_text, f"{path_text}\n", f"{path_text}\r\n"}
    ):
        msg = f"socket identity query returned malformed output for {endpoint!r}"
        raise ValueError(msg)
    raw_path = pathlib.Path(path_text).expanduser()
    if not raw_path.is_absolute():
        msg = f"socket identity query returned a relative path for {endpoint!r}"
        raise ValueError(msg)
    try:
        actual_path = raw_path.resolve(strict=False)
    except (OSError, RuntimeError) as error:
        msg = f"socket identity cannot normalize {path_text!r}"
        raise ValueError(msg) from error
    return SocketEndpoint("path", str(actual_path))


def resolve_tmux_binary(executable: pathlib.Path) -> TmuxBinaryIdentity:
    """Resolve, hash, and query one immutable tmux executable.

    Parameters
    ----------
    executable : pathlib.Path
        Explicit executable path.

    Returns
    -------
    TmuxBinaryIdentity
        Canonical path, byte digest, and raw ``-V`` output.

    Raises
    ------
    ValueError
        Raised for a missing, non-executable, or failing binary.

    Examples
    --------
    >>> callable(resolve_tmux_binary)
    True
    """
    try:
        resolved = executable.expanduser().resolve(strict=True)
    except OSError as error:
        msg = f"tmux binary cannot be resolved: {executable}"
        raise ValueError(msg) from error
    if (
        not resolved.is_file()
        or resolved.is_symlink()
        or not os.access(resolved, os.X_OK)
    ):
        msg = f"tmux binary is not an executable regular file: {resolved}"
        raise ValueError(msg)
    result = subprocess.run(
        [str(resolved), "-V"],
        check=False,
        capture_output=True,
        timeout=5,
    )
    if result.returncode != 0 or not result.stdout:
        msg = f"tmux -V failed with {result.returncode}: {result.stderr!r}"
        raise ValueError(msg)
    try:
        version = result.stdout.decode("utf-8")
    except UnicodeDecodeError as error:
        msg = "tmux -V output is not UTF-8"
        raise ValueError(msg) from error
    if "\x00" in version or not version.rstrip("\r\n"):
        msg = "tmux -V output is malformed"
        raise ValueError(msg)
    return TmuxBinaryIdentity(resolved, _file_sha256(resolved), result.stdout)


def _semantic_identity(
    manifest: t.Mapping[str, object],
) -> str:
    """Verify and return the manifest's recomputed semantic contract digest.

    Parameters
    ----------
    manifest : Mapping[str, object]
        Explicit synchronized parity manifest.

    Returns
    -------
    str
        Current semantic projection digest.

    Raises
    ------
    ValueError
        Raised when the stored digest is absent or stale.

    Examples
    --------
    >>> callable(_semantic_identity)
    True
    """
    stored = manifest.get("semantic_contract_sha256")
    if not isinstance(stored, str):
        msg = "manifest lacks semantic_contract_sha256"
        raise TypeError(msg)
    recomputed = semantic_contract_sha256(manifest)
    if stored != recomputed:
        msg = "manifest has stale semantic_contract_sha256"
        raise ValueError(msg)
    return stored


def _expected_provenance(
    manifest: t.Mapping[str, object],
    observation: t.Mapping[str, object],
    input_manifest: t.Mapping[str, object],
) -> tuple[str, str]:
    """Validate explicit observation bindings and return source identities.

    Parameters
    ----------
    manifest : Mapping[str, object]
        Synchronized parity manifest.
    observation : Mapping[str, object]
        Explicit recorded development observation.
    input_manifest : Mapping[str, object]
        Explicit authoritative input manifest.

    Returns
    -------
    tuple[str, str]
        Recorded source commit and input-manifest digest.

    Raises
    ------
    ValueError
        Raised for stale or malformed provenance bindings.

    Examples
    --------
    >>> callable(_expected_provenance)
    True
    """
    if observation.get("input_manifest") != input_manifest:
        msg = "observation input manifest binding is stale"
        raise ValueError(msg)
    if manifest.get("development") != observation:
        msg = "parity manifest development observation is stale"
        raise ValueError(msg)
    source = observation.get("source")
    if not isinstance(source, dict) or not isinstance(source.get("commit"), str):
        msg = "development observation lacks source commit"
        raise TypeError(msg)
    return t.cast(str, source["commit"]), canonical_sha256(input_manifest)


def _validate_record(
    record: ScenarioRecord,
    *,
    scenario: ScenarioSpec,
    registry: ScenarioRegistry,
    binary: TmuxBinaryIdentity,
    source_commit: str,
    input_manifest_sha256: str,
    semantic_digest: str,
) -> str | None:
    """Return the first immutable identity mismatch in an adapter record.

    Parameters
    ----------
    record : ScenarioRecord
        Parsed adapter record.
    scenario : ScenarioSpec
        Executed scenario contract.
    registry : ScenarioRegistry
        Registry owning each canonical observation schema.
    binary : TmuxBinaryIdentity
        Runner-owned executable identity.
    source_commit : str
        Recorded Python source commit.
    input_manifest_sha256 : str
        Authoritative input-manifest digest.
    semantic_digest : str
        Recomputed semantic contract digest.

    Returns
    -------
    str | None
        Mismatch description, or ``None`` when all identities bind.

    Examples
    --------
    >>> callable(_validate_record)
    True
    """
    checks = (
        (record.scenario_id == scenario.scenario_id, "scenario ID mismatch"),
        (record.tmux_version == binary.version, "tmux version mismatch"),
        (record.tmux_binary_sha256 == binary.sha256, "tmux binary digest mismatch"),
        (record.python_source_commit == source_commit, "Python source commit mismatch"),
        (
            record.python_input_manifest_sha256 == input_manifest_sha256,
            "Python input manifest digest mismatch",
        ),
        (
            record.semantic_contract_sha256 == semantic_digest,
            "semantic contract digest mismatch",
        ),
        (
            record.operations
            == tuple(
                {"tag": item.tag, "request": item.request}
                for item in scenario.operations
            ),
            "scenario operations mismatch",
        ),
    )
    mismatch = next((message for valid, message in checks if not valid), None)
    if mismatch is not None:
        return mismatch
    if len(record.observations) != len(scenario.operations):
        return "scenario observation count mismatch"
    for operation, observation in zip(
        scenario.operations, record.observations, strict=True
    ):
        error = _observation_validation_error(
            registry.operations[operation.tag], observation
        )
        if error is not None:
            return error
    return None


def _run_adapter(
    *,
    spec: AdapterSpec,
    endpoint: SocketEndpoint,
    arguments: t.Sequence[str],
    timeout: float,
    scenario: ScenarioSpec,
    registry: ScenarioRegistry,
    binary: TmuxBinaryIdentity,
    source_commit: str,
    input_manifest_sha256: str,
    semantic_digest: str,
    cwd: pathlib.Path,
) -> AdapterOutcome:
    """Run one adapter as argv and retain every process outcome.

    Parameters
    ----------
    spec : AdapterSpec
        Exact adapter name and argv prefix.
    endpoint : SocketEndpoint
        Structured endpoint bound to this outcome.
    arguments : Sequence[str]
        Adapter-specific arguments excluding ``--output``.
    timeout : float
        Positive execution timeout in seconds.
    scenario : ScenarioSpec
        Executed scenario contract.
    registry : ScenarioRegistry
        Registry owning canonical observation schemas.
    binary : TmuxBinaryIdentity
        Shared tmux executable identity.
    source_commit : str
        Recorded Python source commit.
    input_manifest_sha256 : str
        Authoritative input-manifest digest.
    semantic_digest : str
        Recomputed semantic contract digest.
    cwd : pathlib.Path
        Frozen child working directory shared with identity resolution.

    Returns
    -------
    AdapterOutcome
        Immutable success, process failure, timeout, or validation result.

    Examples
    --------
    >>> callable(_run_adapter)
    True
    """
    if timeout <= 0:
        msg = "adapter timeout must be positive"
        raise ValueError(msg)
    plan = _build_execution_plan(spec, cwd=cwd)
    with tempfile.TemporaryDirectory(prefix="libtmux-differential-") as directory:
        output = pathlib.Path(directory) / "record.json"
        argv = [*plan.argv, *arguments, *endpoint.arguments(), "--output", str(output)]
        startup_cwd = plan.cwd
        environment: dict[str, str] | None = None
        if plan.stdin is not None:
            startup_cwd = pathlib.Path(directory)
            environment = os.environ.copy()
            environment.pop("PYTHONPATH", None)
        try:
            result = subprocess.run(
                argv,
                cwd=startup_cwd,
                check=False,
                capture_output=True,
                env=environment,
                input=plan.stdin,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired as error:
            outcome = AdapterOutcome(
                spec.name,
                plan.digest,
                endpoint,
                None,
                True,
                error.stdout or b"",
                error.stderr or b"",
                None,
                "adapter timed out",
            )
        else:
            if result.returncode != 0:
                outcome = AdapterOutcome(
                    spec.name,
                    plan.digest,
                    endpoint,
                    result.returncode,
                    False,
                    result.stdout,
                    result.stderr,
                    None,
                    None,
                )
            else:
                try:
                    record = scenario_record_from_document(_load_json_object(output))
                    validation_error = _validate_record(
                        record,
                        scenario=scenario,
                        registry=registry,
                        binary=binary,
                        source_commit=source_commit,
                        input_manifest_sha256=input_manifest_sha256,
                        semantic_digest=semantic_digest,
                    )
                except (TypeError, ValueError) as error:
                    record = None
                    validation_error = str(error)
                outcome = AdapterOutcome(
                    spec.name,
                    plan.digest,
                    endpoint,
                    result.returncode,
                    False,
                    result.stdout,
                    result.stderr,
                    record,
                    validation_error,
                )
        if _execution_plan_changed(plan):
            outcome = dataclasses.replace(
                outcome,
                record=None,
                validation_error="adapter identity changed",
            )
        return outcome


def compare_adapters(
    *,
    reference: AdapterSpec,
    comparison: AdapterSpec,
    reference_endpoint: SocketEndpoint,
    comparison_endpoint: SocketEndpoint,
    tmux_binary: pathlib.Path,
    scenario_path: pathlib.Path,
    schema_path: pathlib.Path,
    registry_path: pathlib.Path,
    parity_manifest_path: pathlib.Path,
    repository: pathlib.Path,
    observation_path: pathlib.Path,
    input_manifest_path: pathlib.Path,
    timeout: float,
) -> ExecutionReceipt:
    """Run reference and comparison adapters under one immutable receipt.

    The reference receives the recorded repository inputs. The comparison
    receives only their verified source and manifest identities. Both receive
    one resolved tmux binary and distinct structured endpoints.

    Parameters
    ----------
    reference : AdapterSpec
        Python reference adapter.
    comparison : AdapterSpec
        Fake or future C++ comparison adapter.
    reference_endpoint : SocketEndpoint
        Reference fixture endpoint.
    comparison_endpoint : SocketEndpoint
        Distinct comparison fixture endpoint.
    tmux_binary : pathlib.Path
        Single executable used by both adapters.
    scenario_path : pathlib.Path
        Validated scenario JSON.
    schema_path : pathlib.Path
        Closed scenario schema.
    registry_path : pathlib.Path
        Closed operation registry.
    parity_manifest_path : pathlib.Path
        Explicit synchronized parity manifest.
    repository : pathlib.Path
        Git repository containing recorded Python objects.
    observation_path : pathlib.Path
        Recorded development observation.
    input_manifest_path : pathlib.Path
        Authoritative input manifest.
    timeout : float
        Per-adapter timeout in seconds.

    Returns
    -------
    ExecutionReceipt
        Frozen outer process and provenance receipt.

    Raises
    ------
    ValueError
        Raised by preflight schema, provenance, binary, or endpoint validation.

    Examples
    --------
    >>> callable(compare_adapters)
    True
    """
    if reference_endpoint == comparison_endpoint:
        msg = "differential adapters require distinct socket endpoints"
        raise ValueError(msg)
    resolved_scenario = scenario_path.resolve()
    contract_root = resolved_scenario.parent.parent
    if (
        resolved_scenario.parent.name != "scenarios"
        or schema_path.resolve() != contract_root / "scenario.schema.json"
        or registry_path.resolve() != contract_root / "scenario_registry.json"
    ):
        msg = "scenario, schema, and registry must share one scenario contract root"
        raise ValueError(msg)
    registry = load_registry(registry_path)
    scenario = load_scenario(scenario_path, schema_path, registry)
    manifest = _load_json_object(parity_manifest_path)
    semantic_digest = _semantic_identity(manifest)
    observation = _load_json_object(observation_path)
    input_manifest = _load_json_object(input_manifest_path)
    source_commit, input_digest = _expected_provenance(
        manifest, observation, input_manifest
    )
    binary = resolve_tmux_binary(tmux_binary)
    canonical_reference_endpoint = _query_socket_identity(binary, reference_endpoint)
    canonical_comparison_endpoint = _query_socket_identity(binary, comparison_endpoint)
    if canonical_reference_endpoint == canonical_comparison_endpoint:
        msg = "differential adapters require distinct socket identities"
        raise ValueError(msg)
    execution_cwd = pathlib.Path.cwd().resolve(strict=True)
    common = [
        "--tmux-bin",
        str(binary.path),
        "--scenario",
        str(scenario_path.resolve()),
        "--semantic-contract-sha256",
        semantic_digest,
    ]
    reference_outcome = _run_adapter(
        spec=reference,
        endpoint=canonical_reference_endpoint,
        arguments=[
            *common,
            "--repository",
            str(repository.resolve()),
            "--observation",
            str(observation_path.resolve()),
            "--input-manifest",
            str(input_manifest_path.resolve()),
        ],
        timeout=timeout,
        scenario=scenario,
        registry=registry,
        binary=binary,
        source_commit=source_commit,
        input_manifest_sha256=input_digest,
        semantic_digest=semantic_digest,
        cwd=execution_cwd,
    )
    comparison_outcome = _run_adapter(
        spec=comparison,
        endpoint=canonical_comparison_endpoint,
        arguments=[
            *common,
            "--python-source-commit",
            source_commit,
            "--python-input-manifest-sha256",
            input_digest,
        ],
        timeout=timeout,
        scenario=scenario,
        registry=registry,
        binary=binary,
        source_commit=source_commit,
        input_manifest_sha256=input_digest,
        semantic_digest=semantic_digest,
        cwd=execution_cwd,
    )
    differences = (
        structural_diff(
            scenario_record_document(reference_outcome.record),
            scenario_record_document(comparison_outcome.record),
        )
        if reference_outcome.record is not None
        and comparison_outcome.record is not None
        else ()
    )
    scenario_document = _load_json_object(scenario_path)
    return ExecutionReceipt(
        scenario.scenario_id,
        canonical_sha256(scenario_document),
        registry.digest,
        semantic_digest,
        binary,
        canonical_reference_endpoint,
        canonical_comparison_endpoint,
        reference_outcome,
        comparison_outcome,
        tuple(differences),
    )
