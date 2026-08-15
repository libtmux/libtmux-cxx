"""Deterministic static extraction of Python API observations."""

from __future__ import annotations

import ast
import json
import pathlib
import typing as t

from . import git_objects
from .drift import find_drift, selected_field_digest
from .model import ApiEntry, ApiObservation, InputObject, InputSpec, SourceIdentity

GENERATOR_VERSION = 1
"""Schema version of the deterministic AST observation format."""

_PROTOCOL_METHODS = {
    "__eq__": "equality",
    "__repr__": "repr",
    "__iter__": "iteration",
    "__getitem__": "indexing",
    "__hash__": "hashing",
    "__enter__": "context_manager",
    "__exit__": "context_manager",
}


def extract_revision(
    repo: pathlib.Path,
    revision: str,
    paths: t.Sequence[str | InputSpec],
) -> ApiObservation:
    """Extract selected Python source directly from a Git revision.

    The requested revision is read only through Git plumbing.  The target
    package is never imported or checked out.

    Parameters
    ----------
    repo : pathlib.Path
        Repository containing the requested revision.
    revision : str
        Git revision expression to observe.
    paths : Sequence[str | InputSpec]
        Exact full-object and field-scoped parity inputs.

    Returns
    -------
    ApiObservation
        Static API observation and recorded-input clean result.

    Raises
    ------
    subprocess.CalledProcessError
        Raised when a required Git object cannot be read.
    SyntaxError
        Raised when a selected Python source object is invalid.
    ValueError
        Raised when a selected TOML field is absent or invalid.

    Examples
    --------
    >>> observation = extract_revision(pathlib.Path.cwd(), "HEAD", ("cxx",))
    >>> observation.source.clean_policy
    'recorded_inputs'
    """
    commit = git_objects.rev_parse(repo, f"{revision}^{{commit}}")
    tree = git_objects.rev_parse(repo, f"{commit}^{{tree}}")
    specs = _input_specs(paths)
    inputs = _input_objects(repo, commit, specs)
    entries: list[ApiEntry] = []
    for source_path in _python_sources(
        repo, commit, tuple(spec.path for spec in specs)
    ):
        source = git_objects.show(repo, commit, source_path).decode("utf-8")
        entries.extend(_extract_module(source, source_path))
    return ApiObservation(
        source=SourceIdentity(
            revision=revision,
            commit=commit,
            tree=tree,
            generator_version=GENERATOR_VERSION,
            clean_policy="recorded_inputs",
            clean=not find_drift(repo, commit, paths),
        ),
        inputs=inputs,
        entries=tuple(sorted(entries, key=lambda entry: entry.entry_id)),
    )


def _input_objects(
    repo: pathlib.Path,
    revision: str,
    specs: t.Sequence[InputSpec],
) -> tuple[InputObject, ...]:
    """Record the Git object identity of each configured input path.

    Parameters
    ----------
    repo : pathlib.Path
        Repository containing the input objects.
    revision : str
        Resolved commit object ID.
    specs : Sequence[InputSpec]
        Full-object and field-scoped input specifications.

    Returns
    -------
    tuple[InputObject, ...]
        Input objects sorted by repository-relative path.

    Raises
    ------
    subprocess.CalledProcessError
        Raised when Git cannot read an input object.
    ValueError
        Raised when a requested TOML selector is absent.

    Examples
    --------
    >>> bool(_input_objects(pathlib.Path.cwd(), "HEAD", (InputSpec("pyproject.toml"),)))
    True
    """
    inputs: list[InputObject] = []
    for spec in specs:
        if spec.fields:
            inputs.append(
                InputObject(
                    path=spec.path,
                    kind="toml_fields",
                    object_id=selected_field_digest(
                        git_objects.show(repo, revision, spec.path),
                        spec.fields,
                    ),
                )
            )
            continue
        inputs.extend(
            InputObject(
                path=entry.path,
                kind=entry.kind,
                object_id=entry.object_id,
            )
            for entry in git_objects.ls_tree(repo, revision, (spec.path,))
        )
    return tuple(sorted(inputs, key=lambda item: item.path))


def _python_sources(
    repo: pathlib.Path,
    revision: str,
    paths: t.Sequence[str],
) -> tuple[str, ...]:
    """Return Python files below selected paths in stable path order.

    Parameters
    ----------
    repo : pathlib.Path
        Repository containing the selected tree.
    revision : str
        Resolved commit object ID.
    paths : Sequence[str]
        Repository-relative paths to search recursively.

    Returns
    -------
    tuple[str, ...]
        Python blob paths in Git tree order.

    Examples
    --------
    >>> isinstance(_python_sources(
    ...     pathlib.Path.cwd(), "HEAD", ("cxx",)
    ... ), tuple)
    True
    """
    entries = git_objects.ls_tree(repo, revision, paths, recursive=True)
    return tuple(
        entry.path
        for entry in entries
        if entry.kind == "blob" and entry.path.endswith(".py")
    )


def _input_specs(paths: t.Sequence[str | InputSpec]) -> tuple[InputSpec, ...]:
    """Normalize simple configured paths to immutable input specifications.

    Parameters
    ----------
    paths : Sequence[str | InputSpec]
        Configured input paths or existing specifications.

    Returns
    -------
    tuple[InputSpec, ...]
        Immutable specifications in the supplied order.

    Examples
    --------
    >>> _input_specs(("src",))[0]
    InputSpec(path='src', fields=())
    """
    return tuple(
        path if isinstance(path, InputSpec) else InputSpec(path) for path in paths
    )


def _extract_module(source: str, source_path: str) -> tuple[ApiEntry, ...]:
    """Extract deterministic observations from one unimported source string.

    Parameters
    ----------
    source : str
        Python source text from a Git object.
    source_path : str
        Repository-relative path used for module identity and syntax errors.

    Returns
    -------
    tuple[ApiEntry, ...]
        Deduplicated static entries from the module.

    Raises
    ------
    SyntaxError
        Raised when *source* is invalid Python syntax.

    Examples
    --------
    >>> source = "def public() -> None: ..."
    >>> [entry.qualname for entry in _extract_module(source, "api.py")]
    ['public']
    """
    module = _module_name(source_path)
    tree = ast.parse(source, filename=source_path)
    entries = _extract_body(tree.body, module, source_path, ())
    return tuple(_deduplicate_entries(entries))


def _extract_body(
    body: t.Sequence[ast.stmt],
    module: str,
    source_path: str,
    scope: tuple[str, ...],
    *,
    class_kind: str | None = None,
) -> list[ApiEntry]:
    """Extract entries from a module or class suite.

    Parameters
    ----------
    body : Sequence[ast.stmt]
        Statements from a module or class suite.
    module : str
        Module identity for entries.
    source_path : str
        Repository-relative source path.
    scope : tuple[str, ...]
        Enclosing class names.
    class_kind : str | None, optional
        Enclosing class category used for field classification.

    Returns
    -------
    list[ApiEntry]
        Static entries in source order.

    Examples
    --------
    >>> tree = ast.parse("VALUE = 1")
    >>> _extract_body(tree.body, "sample", "sample.py", ())[0].qualname
    'VALUE'
    """
    entries: list[ApiEntry] = []
    for node in body:
        if isinstance(node, ast.ClassDef):
            entries.extend(_extract_class(node, module, source_path, scope))
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            entries.append(_function_entry(node, module, source_path, scope))
        elif isinstance(node, ast.AnnAssign):
            entry = _assignment_entry(
                node,
                module,
                source_path,
                scope,
                class_kind=class_kind,
            )
            if entry is not None:
                entries.append(entry)
        elif isinstance(node, ast.Assign):
            entries.extend(
                _assignments_entries(
                    node,
                    module,
                    source_path,
                    scope,
                    class_kind=class_kind,
                )
            )
        elif isinstance(node, ast.If):
            entries.extend(
                _extract_body(
                    node.body,
                    module,
                    source_path,
                    scope,
                    class_kind=class_kind,
                )
            )
    return entries


def _extract_class(
    node: ast.ClassDef,
    module: str,
    source_path: str,
    scope: tuple[str, ...],
) -> list[ApiEntry]:
    """Extract one class plus fields and methods nested beneath it.

    Parameters
    ----------
    node : ast.ClassDef
        Class definition to observe.
    module : str
        Module identity for entries.
    source_path : str
        Repository-relative source path.
    scope : tuple[str, ...]
        Enclosing class names.

    Returns
    -------
    list[ApiEntry]
        Class entry followed by its observable nested entries.

    Examples
    --------
    >>> tree = ast.parse("class Item: pass")
    >>> entry = _extract_class(
    ...     t.cast(ast.ClassDef, tree.body[0]), "sample", "sample.py", ()
    ... )[0]
    >>> entry.kind
    'class'
    """
    decorators = _decorators(node.decorator_list)
    bases = tuple(_node_text(base) for base in node.bases)
    kind = _class_kind(bases, decorators)
    qualname = _qualname(scope, node.name)
    protocols = _class_protocols(node.body)
    if _is_deprecated(node.decorator_list, node):
        protocols = (*protocols, "deprecated")
    entries = [
        ApiEntry(
            entry_id=_entry_id(module, kind, qualname),
            kind=kind,
            module=module,
            qualname=qualname,
            source_path=source_path,
            signature=None,
            decorators=decorators,
            bases=bases,
            observable_protocols=protocols,
        )
    ]
    entries.extend(
        _extract_body(
            node.body,
            module,
            source_path,
            (*scope, node.name),
            class_kind=kind,
        )
    )
    return entries


def _function_entry(
    node: ast.FunctionDef | ast.AsyncFunctionDef,
    module: str,
    source_path: str,
    scope: tuple[str, ...],
) -> ApiEntry:
    """Build one callable observation without evaluating annotations.

    Parameters
    ----------
    node : ast.FunctionDef | ast.AsyncFunctionDef
        Callable definition to observe.
    module : str
        Module identity for the entry.
    source_path : str
        Repository-relative source path.
    scope : tuple[str, ...]
        Enclosing class names.

    Returns
    -------
    ApiEntry
        Static callable entry with signature and decorators.

    Examples
    --------
    >>> node = ast.parse("def item(value: int = 1) -> str: ...").body[0]
    >>> entry = _function_entry(
    ...     t.cast(ast.FunctionDef, node), "sample", "sample.py", ()
    ... )
    >>> entry.signature
    '(value: int = 1) -> str'
    """
    decorators = _decorators(node.decorator_list)
    qualname = _qualname(scope, node.name)
    kind = _function_kind(decorators)
    protocols: tuple[str, ...] = ()
    if _is_deprecated(node.decorator_list, node):
        protocols = ("deprecated",)
    return ApiEntry(
        entry_id=_entry_id(module, kind, qualname),
        kind=kind,
        module=module,
        qualname=qualname,
        source_path=source_path,
        signature=_signature(node),
        decorators=decorators,
        observable_protocols=protocols,
    )


def _assignment_entry(
    node: ast.AnnAssign,
    module: str,
    source_path: str,
    scope: tuple[str, ...],
    *,
    class_kind: str | None,
) -> ApiEntry | None:
    """Extract an annotated constant, type alias, or class field.

    Parameters
    ----------
    node : ast.AnnAssign
        Annotated assignment to observe.
    module : str
        Module identity for the entry.
    source_path : str
        Repository-relative source path.
    scope : tuple[str, ...]
        Enclosing class names.
    class_kind : str | None
        Enclosing class category used for field classification.

    Returns
    -------
    ApiEntry | None
        Entry for a contract-relevant assignment, if any.

    Examples
    --------
    >>> node = ast.parse("VALUE: int = 1").body[0]
    >>> entry = _assignment_entry(
    ...     t.cast(ast.AnnAssign, node), "sample", "sample.py", (), class_kind=None
    ... )
    >>> entry is not None and entry.kind
    'constant'
    """
    if not isinstance(node.target, ast.Name):
        return None
    name = node.target.id
    kind = _assignment_kind(name, node.annotation, class_kind)
    if kind is None:
        return None
    return ApiEntry(
        entry_id=_entry_id(module, kind, _qualname(scope, name)),
        kind=kind,
        module=module,
        qualname=_qualname(scope, name),
        source_path=source_path,
        signature=None,
        value_shape=_literal_value(node.value),
    )


def _assignments_entries(
    node: ast.Assign,
    module: str,
    source_path: str,
    scope: tuple[str, ...],
    *,
    class_kind: str | None,
) -> list[ApiEntry]:
    """Extract literal top-level constants and enum members.

    Parameters
    ----------
    node : ast.Assign
        Assignment statement to observe.
    module : str
        Module identity for entries.
    source_path : str
        Repository-relative source path.
    scope : tuple[str, ...]
        Enclosing class names.
    class_kind : str | None
        Enclosing class category used for field classification.

    Returns
    -------
    list[ApiEntry]
        Entries for contract-relevant assignment targets.

    Examples
    --------
    >>> node = ast.parse("VALUE = 1").body[0]
    >>> entries = _assignments_entries(
    ...     t.cast(ast.Assign, node), "sample", "sample.py", (), class_kind=None
    ... )
    >>> entries[0].kind
    'constant'
    """
    entries: list[ApiEntry] = []
    for target in node.targets:
        if not isinstance(target, ast.Name):
            continue
        kind = _assignment_kind(target.id, None, class_kind)
        if kind is None:
            continue
        qualname = _qualname(scope, target.id)
        entries.append(
            ApiEntry(
                entry_id=_entry_id(module, kind, qualname),
                kind=kind,
                module=module,
                qualname=qualname,
                source_path=source_path,
                signature=None,
                value_shape=_literal_value(node.value),
            )
        )
    return entries


def _assignment_kind(
    name: str,
    annotation: ast.expr | None,
    class_kind: str | None,
) -> str | None:
    """Classify an assignment without resolving names or expressions.

    Parameters
    ----------
    name : str
        Assignment target name.
    annotation : ast.expr | None
        Optional annotation on the assignment.
    class_kind : str | None
        Enclosing class category, if any.

    Returns
    -------
    str | None
        Contract category, or ``None`` when the assignment is not observed.

    Examples
    --------
    >>> _assignment_kind("__all__", None, None)
    'export_list'
    """
    if class_kind == "enum":
        return "enum_member" if not name.startswith("_") else None
    if class_kind == "dataclass":
        return "dataclass_field" if not name.startswith("_") else None
    if class_kind == "namedtuple":
        return "namedtuple_field" if not name.startswith("_") else None
    if name == "__all__":
        return "export_list"
    if annotation is not None and "TypeAlias" in _node_text(annotation):
        return "type_alias"
    if name.isupper():
        return "constant"
    return None


def _class_kind(bases: tuple[str, ...], decorators: tuple[str, ...]) -> str:
    """Classify classes with contract-relevant inheritance.

    Parameters
    ----------
    bases : tuple[str, ...]
        Rendered base-class expressions.
    decorators : tuple[str, ...]
        Rendered decorator expressions.

    Returns
    -------
    str
        Contract category for the class.

    Examples
    --------
    >>> _class_kind(("enum.Enum",), ())
    'enum'
    """
    if any(base.endswith("Protocol") for base in bases):
        return "protocol"
    if any(base.endswith("NamedTuple") for base in bases):
        return "namedtuple"
    if any(base.endswith("Enum") for base in bases):
        return "enum"
    if any(
        _decorator_target(decorator).endswith(".dataclass")
        or _decorator_target(decorator) == "dataclass"
        for decorator in decorators
    ):
        return "dataclass"
    if any(base.endswith(("Error", "Exception")) for base in bases):
        return "exception"
    return "class"


def _function_kind(decorators: tuple[str, ...]) -> str:
    """Classify callable decorators that affect observable Python behavior.

    Parameters
    ----------
    decorators : tuple[str, ...]
        Rendered decorator expressions.

    Returns
    -------
    str
        Contract category for the callable.

    Examples
    --------
    >>> _function_kind(("pytest.fixture",))
    'pytest_fixture'
    """
    if any(
        _decorator_target(decorator).endswith(".fixture")
        or _decorator_target(decorator) == "fixture"
        for decorator in decorators
    ):
        return "pytest_fixture"
    if any(
        _decorator_target(decorator).endswith(".overload")
        or _decorator_target(decorator) == "overload"
        for decorator in decorators
    ):
        return "overload"
    if any(
        _decorator_target(decorator) == "property"
        or _decorator_target(decorator).endswith(".property")
        for decorator in decorators
    ):
        return "property"
    return "function"


def _class_protocols(body: t.Sequence[ast.stmt]) -> tuple[str, ...]:
    r"""Record special methods that define observable object protocols.

    Parameters
    ----------
    body : Sequence[ast.stmt]
        Statements in the class suite.

    Returns
    -------
    tuple[str, ...]
        Observable protocol categories in source order.

    Examples
    --------
    >>> body = ast.parse("class Item:\n    def __iter__(self): ...").body[0].body
    >>> _class_protocols(body)  # type: ignore[arg-type]
    ('iteration',)
    """
    protocols: list[str] = []
    for node in body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            protocol = _PROTOCOL_METHODS.get(node.name)
            if protocol is not None and protocol not in protocols:
                protocols.append(protocol)
    return tuple(protocols)


def _is_deprecated(
    decorators: t.Sequence[ast.expr],
    node: ast.ClassDef | ast.FunctionDef | ast.AsyncFunctionDef,
) -> bool:
    """Detect deprecation markers without calling target decorators.

    Parameters
    ----------
    decorators : Sequence[ast.expr]
        Decorator expressions attached to the definition.
    node : ast.ClassDef | ast.FunctionDef | ast.AsyncFunctionDef
        Definition whose docstring is inspected.

    Returns
    -------
    bool
        Whether a decorator or docstring marks the definition deprecated.

    Examples
    --------
    >>> node = ast.parse("def item(): ...").body[0]
    >>> _is_deprecated((), t.cast(ast.FunctionDef, node))
    False
    """
    return any("deprecated" in _node_text(item).lower() for item in decorators) or (
        ".. deprecated::" in (ast.get_docstring(node) or "").lower()
    )


def _decorators(decorators: t.Sequence[ast.expr]) -> tuple[str, ...]:
    r"""Return decorator syntax in source-independent AST form.

    Parameters
    ----------
    decorators : Sequence[ast.expr]
        Decorator expressions to render.

    Returns
    -------
    tuple[str, ...]
        Rendered decorator expressions in source order.

    Examples
    --------
    >>> _decorators(ast.parse("@property\ndef value(): ...").body[0].decorator_list)
    ('property',)
    """
    return tuple(_node_text(decorator) for decorator in decorators)


def _decorator_target(decorator: str) -> str:
    """Return the callable portion of a rendered decorator expression.

    Parameters
    ----------
    decorator : str
        Source-independent decorator expression.

    Returns
    -------
    str
        Decorator target before its call arguments, when present.

    Examples
    --------
    >>> _decorator_target("pytest.fixture(scope='session')")
    'pytest.fixture'
    """
    return decorator.split("(", maxsplit=1)[0]


def _signature(node: ast.FunctionDef | ast.AsyncFunctionDef) -> str:
    """Render callable parameters and return annotation without evaluation.

    Parameters
    ----------
    node : ast.FunctionDef | ast.AsyncFunctionDef
        Callable definition whose signature is rendered.

    Returns
    -------
    str
        Source-independent static signature.

    Examples
    --------
    >>> node = ast.parse("def value(item: int = 1) -> str: ...").body[0]
    >>> _signature(node)  # type: ignore[arg-type]
    '(item: int = 1) -> str'
    """
    arguments = node.args
    positional = (*arguments.posonlyargs, *arguments.args)
    defaults = (None,) * (len(positional) - len(arguments.defaults)) + tuple(
        arguments.defaults
    )
    parts = [
        _parameter(argument, default)
        for argument, default in zip(positional, defaults, strict=True)
    ]
    if arguments.posonlyargs:
        parts.insert(len(arguments.posonlyargs), "/")
    if arguments.vararg is not None:
        parts.append(f"*{_parameter(arguments.vararg, None)}")
    elif arguments.kwonlyargs:
        parts.append("*")
    parts.extend(
        _parameter(argument, default)
        for argument, default in zip(
            arguments.kwonlyargs,
            arguments.kw_defaults,
            strict=True,
        )
    )
    if arguments.kwarg is not None:
        parts.append(f"**{_parameter(arguments.kwarg, None)}")
    suffix = "" if node.returns is None else f" -> {_node_text(node.returns)}"
    return f"({', '.join(parts)}){suffix}"


def _parameter(argument: ast.arg, default: ast.expr | None) -> str:
    """Render one function parameter without evaluating its default.

    Parameters
    ----------
    argument : ast.arg
        Parameter node to render.
    default : ast.expr | None
        Default expression aligned with *argument*, if any.

    Returns
    -------
    str
        Static parameter text with annotation and default.

    Examples
    --------
    >>> argument = ast.parse("def value(item: int): ...").body[0].args.args[0]
    >>> _parameter(argument, ast.parse("1").body[0].value)  # type: ignore[arg-type]
    'item: int = 1'
    """
    annotation = (
        "" if argument.annotation is None else f": {_node_text(argument.annotation)}"
    )
    default_text = "" if default is None else f" = {_node_text(default)}"
    return f"{argument.arg}{annotation}{default_text}"


def _literal_value(node: ast.expr | None) -> object | None:
    """Evaluate literals and describe other expressions without executing them.

    Parameters
    ----------
    node : ast.expr | None
        Expression to inspect without importing source code.

    Returns
    -------
    object | None
        Tagged literal data, an absent-expression tag, or AST shape data.

    Examples
    --------
    >>> result = _literal_value(ast.parse("('one', 2)", mode="eval").body)
    >>> result["literal_type"], result["items"][1]["value"]  # type: ignore[index]
    ('tuple', '2')
    >>> _literal_value(ast.parse("factory()").body[0].value)  # type: ignore[arg-type]
    {'expression': "Call(Name('factory', Load()))"}
    """
    if node is None:
        return {"literal_type": "absent"}
    try:
        return _literal_json(t.cast(object, ast.literal_eval(node)))
    except (SyntaxError, ValueError):
        return {"expression": ast.dump(node, annotate_fields=False)}


def _literal_json(value: object) -> dict[str, object]:
    """Return an injective tagged JSON encoding for one Python literal.

    Parameters
    ----------
    value : object
        Value accepted by :func:`ast.literal_eval`.

    Returns
    -------
    dict[str, object]
        Recursively tagged literal representation that cannot collide with a
        user mapping.

    Examples
    --------
    >>> result = _literal_json(("one", 2))
    >>> result["literal_type"], result["items"][1]["value"]  # type: ignore[index]
    ('tuple', '2')
    """
    if value is None:
        return {"literal_type": "none"}
    if type(value) is bool:
        return {"literal_type": "bool", "value": value}
    if type(value) is int:
        return {"literal_type": "int", "value": str(value)}
    if type(value) is float:
        return {"literal_type": "float", "hex": value.hex()}
    if type(value) is str:
        return {"literal_type": "str", "value": value}
    if type(value) is bytes:
        return {"literal_type": "bytes", "hex": value.hex()}
    if type(value) is complex:
        return {
            "literal_type": "complex",
            "real": _literal_json(value.real),
            "imag": _literal_json(value.imag),
        }
    if value is Ellipsis:
        return {"literal_type": "ellipsis"}
    if isinstance(value, list):
        return {
            "literal_type": "list",
            "items": [_literal_json(item) for item in value],
        }
    if isinstance(value, tuple):
        return {
            "literal_type": "tuple",
            "items": [_literal_json(item) for item in value],
        }
    if isinstance(value, set):
        items = [_literal_json(item) for item in value]
        items.sort(key=_literal_json_sort_key)
        return {"literal_type": "set", "items": items}
    if isinstance(value, frozenset):
        items = [_literal_json(item) for item in value]
        items.sort(key=_literal_json_sort_key)
        return {"literal_type": "frozenset", "items": items}
    if isinstance(value, dict):
        items = [
            {"key": _literal_json(key), "value": _literal_json(item)}
            for key, item in value.items()
        ]
        items.sort(key=_literal_json_sort_key)
        return {"literal_type": "mapping", "items": items}
    msg = f"unsupported Python literal type: {type(value).__name__}"
    raise ValueError(msg)


def _literal_json_sort_key(value: object) -> str:
    """Return a canonical sort key for an encoded literal item.

    Parameters
    ----------
    value : object
        Tagged literal item.

    Returns
    -------
    str
        Canonical compact JSON used only for deterministic ordering.

    Examples
    --------
    >>> _literal_json_sort_key({"literal_type": "int", "value": "1"})
    '{"literal_type":"int","value":"1"}'
    """
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
        allow_nan=False,
    )


def _node_text(node: ast.AST) -> str:
    """Return a stable display form for an AST node without evaluating it.

    Parameters
    ----------
    node : ast.AST
        Syntax node to render.

    Returns
    -------
    str
        Python syntax reconstructed from the AST.

    Examples
    --------
    >>> _node_text(ast.parse("item.value").body[0].value)  # type: ignore[arg-type]
    'item.value'
    """
    return ast.unparse(node)


def _module_name(source_path: str) -> str:
    """Infer a module name from a repository-relative Python path.

    Parameters
    ----------
    source_path : str
        Repository-relative Python source path.

    Returns
    -------
    str
        Dotted module name without a ``src`` prefix or ``__init__`` suffix.

    Examples
    --------
    >>> _module_name("src/libtmux/__init__.py")
    'libtmux'
    """
    path = pathlib.PurePosixPath(source_path)
    parts = list(path.with_suffix("").parts)
    if parts[:1] == ["src"]:
        parts = parts[1:]
    if parts[-1:] == ["__init__"]:
        parts.pop()
    return ".".join(parts)


def _qualname(scope: tuple[str, ...], name: str) -> str:
    """Join an AST nesting scope with a local name.

    Parameters
    ----------
    scope : tuple[str, ...]
        Enclosing class names.
    name : str
        Local definition name.

    Returns
    -------
    str
        Dotted qualified name.

    Examples
    --------
    >>> _qualname(("Item",), "value")
    'Item.value'
    """
    return ".".join((*scope, name))


def _entry_id(module: str, kind: str, qualname: str) -> str:
    """Create a deterministic entry ID independent of source line numbers.

    Parameters
    ----------
    module : str
        Dotted module name.
    kind : str
        Contract category for the entry.
    qualname : str
        Qualified name inside the module.

    Returns
    -------
    str
        Stable identifier composed from the supplied components.

    Examples
    --------
    >>> _entry_id("sample", "function", "value")
    'sample:function:value'
    """
    return f"{module}:{kind}:{qualname}"


def _deduplicate_entries(entries: t.Iterable[ApiEntry]) -> t.Iterable[ApiEntry]:
    """Disambiguate repeated IDs without depending on source line numbers.

    Parameters
    ----------
    entries : Iterable[ApiEntry]
        Entries in source order.

    Yields
    ------
    ApiEntry
        Original entries with deterministic numeric suffixes on duplicate IDs.

    Examples
    --------
    >>> item = ApiEntry(
    ...     "sample:function:value", "function", "sample", "value", "sample.py", None
    ... )
    >>> [entry.entry_id for entry in _deduplicate_entries((item, item))]
    ['sample:function:value', 'sample:function:value#2']
    """
    counts: dict[str, int] = {}
    for entry in entries:
        count = counts.get(entry.entry_id, 0) + 1
        counts[entry.entry_id] = count
        if count == 1:
            yield entry
            continue
        yield ApiEntry(
            entry_id=f"{entry.entry_id}#{count}",
            kind=entry.kind,
            module=entry.module,
            qualname=entry.qualname,
            source_path=entry.source_path,
            signature=entry.signature,
            value_shape=entry.value_shape,
            decorators=entry.decorators,
            bases=entry.bases,
            observable_protocols=entry.observable_protocols,
        )
