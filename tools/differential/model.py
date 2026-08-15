"""Frozen models and closed parsing for differential artifacts."""

from __future__ import annotations

import copy
import dataclasses
import json
import pathlib
import re
import typing as t
import urllib.parse

import jsonschema

from cxx.tools.parity.generate import canonical_sha256

if t.TYPE_CHECKING:
    from .canonicalize import CanonicalizationRules
    from .compare import StructuralDifference

_DIGEST = re.compile(r"sha256:[0-9a-f]{64}\Z")
_COMMIT = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})\Z")


@dataclasses.dataclass(frozen=True, slots=True)
class EntityField:
    """One registry-declared entity identity field.

    Attributes
    ----------
    pointer : str
        Extended JSON pointer locating the field.
    kind : str
        Entity namespace: ``session``, ``window``, or ``pane``.
    role : str
        ``definition`` or ``reference``.
    """

    pointer: str
    kind: str
    role: str


@dataclasses.dataclass(frozen=True, slots=True)
class ScenarioRecord:
    """Comparable semantic result for one adapter execution.

    Attributes
    ----------
    scenario_id : str
        Closed scenario identifier.
    tmux_version : str
        Raw tmux version line without its line ending.
    tmux_binary_sha256 : str
        Digest of the executable used for the observation.
    python_source_commit : str
        Pinned Python reference commit.
    python_input_manifest_sha256 : str
        Digest of the authoritative input manifest.
    semantic_contract_sha256 : str
        Evidence-independent parity contract digest.
    operations : tuple[dict[str, object], ...]
        Exact validated operation requests.
    observations : tuple[dict[str, object], ...]
        Exact canonical adapter observations.
    """

    scenario_id: str
    tmux_version: str
    tmux_binary_sha256: str
    python_source_commit: str
    python_input_manifest_sha256: str
    semantic_contract_sha256: str
    operations: tuple[dict[str, object], ...]
    observations: tuple[dict[str, object], ...]


@dataclasses.dataclass(frozen=True, slots=True)
class AdapterSpec:
    """Shell-free adapter identity and base argv.

    Attributes
    ----------
    name : str
        Human-readable adapter name.
    argv : tuple[str, ...]
        Nonempty argv prefix passed directly to ``subprocess``.
    """

    name: str
    argv: tuple[str, ...]


@dataclasses.dataclass(frozen=True, slots=True)
class RegistryOperation:
    """Closed registry entry for one operation tag.

    Attributes
    ----------
    tag : str
        Unique operation tag.
    scenario_id : str
        Owning scenario.
    request_schema : dict[str, object]
        Closed request JSON schema.
    response_schema : dict[str, object]
        Closed observation JSON schema.
    python_handler : str
        Python dispatcher key.
    cpp_handler : str
        Future C++ dispatcher key.
    request_wire_tag : int
        One-byte request record tag.
    response_wire_tags : tuple[int, ...]
        Allowed one-byte response record tags.
    entity_fields : tuple[EntityField, ...]
        Definition and reference fields contributed by the operation.
    """

    tag: str
    scenario_id: str
    request_schema: dict[str, object]
    response_schema: dict[str, object]
    python_handler: str
    cpp_handler: str
    request_wire_tag: int
    response_wire_tags: tuple[int, ...]
    entity_fields: tuple[EntityField, ...]


@dataclasses.dataclass(frozen=True, slots=True)
class ScenarioRegistry:
    """Validated closed operation registry.

    Attributes
    ----------
    schema_version : int
        Registry major version.
    operations : dict[str, RegistryOperation]
        Operations keyed by their exact tags.
    digest : str
        Canonical digest of the registry document.
    """

    schema_version: int
    operations: dict[str, RegistryOperation]
    digest: str


@dataclasses.dataclass(frozen=True, slots=True)
class ScenarioOperation:
    """One validated scenario request.

    Attributes
    ----------
    tag : str
        Registered operation tag.
    request : dict[str, object]
        Closed validated request object.
    """

    tag: str
    request: dict[str, object]


@dataclasses.dataclass(frozen=True, slots=True)
class ScenarioSpec:
    """Validated scenario and its registry-derived entity graph.

    Attributes
    ----------
    scenario_id : str
        Closed scenario identifier.
    operations : tuple[ScenarioOperation, ...]
        Ordered operation requests.
    canonicalization : CanonicalizationRules
        Exact three-field canonicalization rules.
    entity_fields : tuple[EntityField, ...]
        Unique registry definitions and references used by the scenario.
    """

    scenario_id: str
    operations: tuple[ScenarioOperation, ...]
    canonicalization: CanonicalizationRules
    entity_fields: tuple[EntityField, ...]


@dataclasses.dataclass(frozen=True, slots=True)
class SocketEndpoint:
    """One structured tmux socket selector.

    Attributes
    ----------
    mode : Literal["name", "path"]
        Selector kind.
    value : str
        Nonempty selector value.
    """

    mode: t.Literal["name", "path"]
    value: str

    def __post_init__(self) -> None:
        """Reject unknown or empty socket selectors.

        Raises
        ------
        ValueError
            Raised when the selector is not explicit.

        Examples
        --------
        >>> SocketEndpoint("path", "/tmp/tmux.sock").value
        '/tmp/tmux.sock'
        """
        if self.mode not in {"name", "path"} or not self.value:
            msg = "socket endpoint requires name or path and a value"
            raise ValueError(msg)

    def arguments(self) -> tuple[str, str]:
        """Return the adapter argv for this selector.

        Returns
        -------
        tuple[str, str]
            Flag and selector value.

        Examples
        --------
        >>> SocketEndpoint("name", "reference").arguments()
        ('--socket-name', 'reference')
        """
        return (f"--socket-{self.mode}", self.value)

    def tmux_arguments(self) -> tuple[str, str]:
        """Return the tmux argv for querying this structured selector.

        Returns
        -------
        tuple[str, str]
            Tmux selector flag and exact selector value.

        Examples
        --------
        >>> SocketEndpoint("name", "reference").tmux_arguments()
        ('-L', 'reference')
        >>> SocketEndpoint("path", "/tmp/reference.sock").tmux_arguments()
        ('-S', '/tmp/reference.sock')
        """
        flag = "-L" if self.mode == "name" else "-S"
        return (flag, self.value)


@dataclasses.dataclass(frozen=True, slots=True)
class TmuxBinaryIdentity:
    """One immutable resolved tmux executable identity.

    Attributes
    ----------
    path : pathlib.Path
        Canonical executable path.
    sha256 : str
        Digest of the executable bytes.
    version_output : bytes
        Raw standard output from ``tmux -V``.
    """

    path: pathlib.Path
    sha256: str
    version_output: bytes

    @property
    def version(self) -> str:
        r"""Return the UTF-8 version line without its line ending.

        Returns
        -------
        str
            Record-compatible version text.

        Examples
        --------
        >>> identity = TmuxBinaryIdentity(
        ...     pathlib.Path('/tmux'), 'sha256:' + '0' * 64, b'tmux 3.7\n'
        ... )
        >>> identity.version
        'tmux 3.7'
        """
        return self.version_output.decode("utf-8").rstrip("\r\n")


@dataclasses.dataclass(frozen=True, slots=True)
class AdapterOutcome:
    """Immutable outcome of one shell-free adapter execution.

    Attributes
    ----------
    adapter_name : str
        Adapter name from its exact specification.
    adapter_sha256 : str
        Digest binding its argv and file inputs.
    endpoint : SocketEndpoint
        Structured socket passed to this adapter.
    returncode : int | None
        Process exit code, or ``None`` after timeout.
    timed_out : bool
        Whether the process exceeded its deadline.
    stdout : bytes
        Captured standard output.
    stderr : bytes
        Captured standard error retained verbatim.
    record : ScenarioRecord | None
        Validated result when available.
    validation_error : str | None
        Output or identity validation failure.
    """

    adapter_name: str
    adapter_sha256: str
    endpoint: SocketEndpoint
    returncode: int | None
    timed_out: bool
    stdout: bytes
    stderr: bytes
    record: ScenarioRecord | None
    validation_error: str | None


@dataclasses.dataclass(frozen=True, slots=True)
class ExecutionReceipt:
    """Outer execution provenance kept separate from comparable records.

    Attributes
    ----------
    scenario_id : str
        Executed scenario ID.
    scenario_sha256 : str
        Digest of the exact scenario document.
    registry_sha256 : str
        Digest of the closed operation registry.
    semantic_contract_sha256 : str
        Recomputed evidence-independent parity identity.
    tmux_binary : TmuxBinaryIdentity
        Single executable identity shared by both adapters.
    reference_endpoint : SocketEndpoint
        Explicit socket for the reference adapter.
    comparison_endpoint : SocketEndpoint
        Distinct explicit socket for the comparison adapter.
    reference : AdapterOutcome
        Reference execution outcome.
    comparison : AdapterOutcome
        Comparison execution outcome.
    differences : tuple[StructuralDifference, ...]
        Exact structural differences between validated records.
    """

    scenario_id: str
    scenario_sha256: str
    registry_sha256: str
    semantic_contract_sha256: str
    tmux_binary: TmuxBinaryIdentity
    reference_endpoint: SocketEndpoint
    comparison_endpoint: SocketEndpoint
    reference: AdapterOutcome
    comparison: AdapterOutcome
    differences: tuple[StructuralDifference, ...]

    @property
    def successful(self) -> bool:
        """Return whether both adapters produced one exact matching record.

        Returns
        -------
        bool
            ``True`` only for two successful validated processes and no diff.

        Examples
        --------
        >>> hasattr(ExecutionReceipt, 'successful')
        True
        """
        outcomes = (self.reference, self.comparison)
        return (
            all(
                outcome.returncode == 0
                and not outcome.timed_out
                and outcome.validation_error is None
                and outcome.record is not None
                for outcome in outcomes
            )
            and not self.differences
        )


def scenario_record_from_document(document: t.Mapping[str, object]) -> ScenarioRecord:
    """Parse one closed eight-field scenario record.

    Parameters
    ----------
    document : Mapping[str, object]
        Decoded JSON object.

    Returns
    -------
    ScenarioRecord
        Frozen outer record with copied operation and observation rows.

    Raises
    ------
    ValueError
        Raised for missing, extra, or malformed fields.

    Examples
    --------
    >>> digest = "sha256:" + "0" * 64
    >>> record = scenario_record_from_document({
    ...     "scenario_id": "example", "tmux_version": "tmux 3.7",
    ...     "tmux_binary_sha256": digest, "python_source_commit": "a" * 40,
    ...     "python_input_manifest_sha256": digest,
    ...     "semantic_contract_sha256": digest,
    ...     "operations": [], "observations": [],
    ... })
    >>> record.scenario_id
    'example'
    """
    expected = {
        "scenario_id",
        "tmux_version",
        "tmux_binary_sha256",
        "python_source_commit",
        "python_input_manifest_sha256",
        "semantic_contract_sha256",
        "operations",
        "observations",
    }
    actual = set(document)
    if actual != expected:
        missing = sorted(expected - actual)
        unexpected = sorted(actual - expected)
        msg = f"record fields missing={missing} unexpected={unexpected}"
        raise ValueError(msg)
    string_fields = expected - {"operations", "observations"}
    for field in string_fields:
        if not isinstance(document[field], str) or not document[field]:
            msg = f"{field} must be a nonempty string"
            raise ValueError(msg)
    for field in (
        "tmux_binary_sha256",
        "python_input_manifest_sha256",
        "semantic_contract_sha256",
    ):
        if _DIGEST.fullmatch(t.cast(str, document[field])) is None:
            msg = f"{field} must be a sha256 digest"
            raise ValueError(msg)
    if _COMMIT.fullmatch(t.cast(str, document["python_source_commit"])) is None:
        msg = "python_source_commit must be a full Git object ID"
        raise ValueError(msg)
    rows: dict[str, tuple[dict[str, object], ...]] = {}
    for field in ("operations", "observations"):
        raw = document[field]
        if not isinstance(raw, list) or not all(isinstance(row, dict) for row in raw):
            msg = f"{field} must be an array of objects"
            raise ValueError(msg)
        rows[field] = tuple(copy.deepcopy(t.cast(list[dict[str, object]], raw)))
    return ScenarioRecord(
        scenario_id=t.cast(str, document["scenario_id"]),
        tmux_version=t.cast(str, document["tmux_version"]),
        tmux_binary_sha256=t.cast(str, document["tmux_binary_sha256"]),
        python_source_commit=t.cast(str, document["python_source_commit"]),
        python_input_manifest_sha256=t.cast(
            str, document["python_input_manifest_sha256"]
        ),
        semantic_contract_sha256=t.cast(str, document["semantic_contract_sha256"]),
        operations=rows["operations"],
        observations=rows["observations"],
    )


def scenario_record_document(record: ScenarioRecord) -> dict[str, object]:
    """Convert a scenario record to a detached JSON document.

    Parameters
    ----------
    record : ScenarioRecord
        Record to serialize.

    Returns
    -------
    dict[str, object]
        JSON-compatible copied data.

    Examples
    --------
    >>> digest = "sha256:" + "0" * 64
    >>> record = ScenarioRecord("x", "tmux", digest, "a" * 40, digest, digest, (), ())
    >>> scenario_record_document(record)["scenario_id"]
    'x'
    """
    return {
        "scenario_id": record.scenario_id,
        "tmux_version": record.tmux_version,
        "tmux_binary_sha256": record.tmux_binary_sha256,
        "python_source_commit": record.python_source_commit,
        "python_input_manifest_sha256": record.python_input_manifest_sha256,
        "semantic_contract_sha256": record.semantic_contract_sha256,
        "operations": copy.deepcopy(list(record.operations)),
        "observations": copy.deepcopy(list(record.observations)),
    }


def _load_json_object(path: pathlib.Path) -> dict[str, object]:
    """Load a UTF-8 JSON object from disk.

    Parameters
    ----------
    path : pathlib.Path
        JSON artifact path.

    Returns
    -------
    dict[str, object]
        Decoded object.

    Raises
    ------
    ValueError
        Raised when the document root is not an object.

    Examples
    --------
    >>> callable(_load_json_object)
    True
    """
    value: object = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        msg = f"{path}: JSON root must be an object"
        raise TypeError(msg)
    return t.cast(dict[str, object], value)


_REGISTRY_SCHEMA_KEYWORDS = {
    "$comment",
    "$defs",
    "$ref",
    "additionalProperties",
    "allOf",
    "anyOf",
    "const",
    "contains",
    "default",
    "dependentRequired",
    "dependentSchemas",
    "deprecated",
    "description",
    "else",
    "enum",
    "examples",
    "exclusiveMaximum",
    "exclusiveMinimum",
    "format",
    "if",
    "items",
    "maxContains",
    "maximum",
    "maxItems",
    "maxLength",
    "maxProperties",
    "minContains",
    "minimum",
    "minItems",
    "minLength",
    "minProperties",
    "multipleOf",
    "not",
    "oneOf",
    "pattern",
    "patternProperties",
    "prefixItems",
    "properties",
    "propertyNames",
    "readOnly",
    "required",
    "then",
    "title",
    "type",
    "unevaluatedItems",
    "unevaluatedProperties",
    "uniqueItems",
    "writeOnly",
}


def _local_definition(
    root: t.Mapping[str, object], reference: object, location: str
) -> tuple[str, object]:
    """Resolve one restricted local ``$defs`` reference.

    Parameters
    ----------
    root : Mapping[str, object]
        Root registry schema containing the definition table.
    reference : object
        Candidate reference string.
    location : str
        Diagnostic schema location.

    Returns
    -------
    tuple[str, object]
        Canonical reference and referenced schema node.

    Raises
    ------
    ValueError
        Raised for external, escaping, malformed, or missing references.

    Examples
    --------
    >>> _local_definition({'$defs': {'row': {'type': 'string'}}}, '#/$defs/row', '$')
    ('#/$defs/row', {'type': 'string'})
    """
    prefix = "#/$defs/"
    if not isinstance(reference, str) or not reference.startswith(prefix):
        msg = f"{location}: registry schema dialect requires local $defs references"
        raise ValueError(msg)
    encoded_name = reference[len(prefix) :]
    index = 0
    while index < len(encoded_name):
        if encoded_name[index] != "%":
            index += 1
            continue
        if index + 2 >= len(encoded_name):
            msg = f"{location}: registry schema dialect reference is malformed"
            raise ValueError(msg)
        try:
            int(encoded_name[index + 1 : index + 3], 16)
        except ValueError as error:
            msg = f"{location}: registry schema dialect reference is malformed"
            raise ValueError(msg) from error
        index += 3
    try:
        raw_name = urllib.parse.unquote(encoded_name, errors="strict")
    except UnicodeDecodeError as error:
        msg = f"{location}: registry schema dialect reference is malformed"
        raise ValueError(msg) from error
    if not raw_name or "/" in raw_name:
        msg = f"{location}: registry schema dialect reference escapes $defs"
        raise ValueError(msg)
    output: list[str] = []
    index = 0
    while index < len(raw_name):
        if raw_name[index] != "~":
            output.append(raw_name[index])
            index += 1
            continue
        if index + 1 >= len(raw_name) or raw_name[index + 1] not in {"0", "1"}:
            msg = f"{location}: registry schema dialect reference is malformed"
            raise ValueError(msg)
        output.append("~" if raw_name[index + 1] == "0" else "/")
        index += 2
    definitions = root.get("$defs")
    name = "".join(output)
    if not isinstance(definitions, dict) or name not in definitions:
        msg = f"{location}: registry schema dialect reference is missing"
        raise ValueError(msg)
    canonical_name = name.replace("~", "~0").replace("/", "~1")
    return f"{prefix}{canonical_name}", definitions[name]


def _registry_schema_closes(
    schema: object,
    *,
    root: t.Mapping[str, object],
    location: str,
    reference_stack: tuple[str, ...] = (),
) -> bool:
    """Return whether one schema intrinsically closes object instances.

    Parameters
    ----------
    schema : object
        Draft 2020-12 schema node.
    root : Mapping[str, object]
        Root schema used for local reference resolution.
    location : str
        Diagnostic path for this node.
    reference_stack : tuple[str, ...], default=()
        Active local references used to reject cycles.

    Returns
    -------
    bool
        Whether this node supplies closure without borrowing outer context.

    Raises
    ------
    ValueError
        Raised for a malformed, missing, escaping, or cyclic local reference.

    Examples
    --------
    >>> schema = {'allOf': [{'type': 'object', 'additionalProperties': False}]}
    >>> _registry_schema_closes(schema, root=schema, location='$')
    True
    """
    if not isinstance(schema, dict):
        return False
    object_keywords = {
        "properties",
        "patternProperties",
        "required",
        "dependentRequired",
        "dependentSchemas",
        "additionalProperties",
        "unevaluatedProperties",
        "propertyNames",
        "minProperties",
        "maxProperties",
    }
    raw_type = schema.get("type")
    schema_types = (
        {raw_type}
        if isinstance(raw_type, str)
        else set(t.cast(list[str], raw_type))
        if isinstance(raw_type, list)
        else set()
    )
    object_shape = "object" in schema_types or bool(
        object_keywords.intersection(schema)
    )
    own_closed = object_shape and (
        schema.get("additionalProperties") is False
        or schema.get("unevaluatedProperties") is False
    )
    reference_closed = False
    if "$ref" in schema:
        reference, target = _local_definition(root, schema["$ref"], location)
        if reference in reference_stack:
            msg = f"{location}: registry schema dialect reference cycle"
            raise ValueError(msg)
        reference_closed = _registry_schema_closes(
            target,
            root=root,
            location=f"{location}/{reference}",
            reference_stack=(*reference_stack, reference),
        )
    composition_closed = False
    all_of = schema.get("allOf")
    if isinstance(all_of, list) and all_of:
        composition_closed = any(
            _registry_schema_closes(
                child,
                root=root,
                location=f"{location}/allOf/{index}",
                reference_stack=reference_stack,
            )
            for index, child in enumerate(all_of)
        )
    for keyword in ("anyOf", "oneOf"):
        children = schema.get(keyword)
        if isinstance(children, list) and children:
            composition_closed = composition_closed or all(
                _registry_schema_closes(
                    child,
                    root=root,
                    location=f"{location}/{keyword}/{index}",
                    reference_stack=reference_stack,
                )
                for index, child in enumerate(children)
            )
    return own_closed or reference_closed or composition_closed


def _validate_registry_schema_dialect(
    schema: object,
    *,
    root: t.Mapping[str, object],
    location: str,
    constraint_context: bool = False,
    reference_stack: tuple[str, ...] = (),
) -> tuple[bool, bool]:
    """Validate the restricted closed Draft 2020-12 registry dialect.

    The dialect accepts nonempty mapping schemas and the rejecting ``false``
    schema. Object shapes close with ``additionalProperties: false`` or
    ``unevaluatedProperties: false``. Same-instance composition constraints may
    inherit that closure, while property values, array values, and definitions
    start a fresh context. ``allOf`` closes when any conjunct closes;
    ``anyOf`` and ``oneOf`` close only when every alternative closes. Arrays
    require constrained ``items``. References are acyclic local references to a
    single root ``$defs`` member.

    Parameters
    ----------
    schema : object
        Draft 2020-12 schema node.
    root : Mapping[str, object]
        Root schema used for local reference resolution.
    location : str
        Diagnostic path for this node.
    constraint_context : bool, default=False
        Whether an enclosing same-instance object already supplies closure.
    reference_stack : tuple[str, ...], default=()
        Active local references used to reject cycles.

    Returns
    -------
    tuple[bool, bool]
        Whether the node independently constrains values and closes objects.

    Raises
    ------
    ValueError
        Raised for an unsupported or permissive schema node.

    Examples
    --------
    >>> schema = {'type': 'object', 'additionalProperties': False}
    >>> _validate_registry_schema_dialect(schema, root=schema, location='$')
    (True, True)
    """
    if isinstance(schema, bool):
        if schema:
            msg = f"{location}: registry schema dialect forbids true"
            raise ValueError(msg)
        return True, False
    if not isinstance(schema, dict) or not schema:
        msg = f"{location}: registry schema dialect forbids unconstrained schemas"
        raise ValueError(msg)
    unsupported = set(schema) - _REGISTRY_SCHEMA_KEYWORDS
    if unsupported:
        msg = f"{location}: registry schema dialect keywords {sorted(unsupported)!r}"
        raise ValueError(msg)
    intrinsic_closed = _registry_schema_closes(
        schema,
        root=root,
        location=location,
        reference_stack=reference_stack,
    )

    definitions = schema.get("$defs")
    if definitions is not None:
        if not isinstance(definitions, dict):
            msg = f"{location}: registry schema dialect $defs must be an object"
            raise ValueError(msg)
        for name, child in definitions.items():
            _validate_registry_schema_dialect(
                child,
                root=root,
                location=f"{location}/$defs/{name}",
                reference_stack=reference_stack,
            )

    reference_safe = False
    if "$ref" in schema:
        reference, target = _local_definition(root, schema["$ref"], location)
        if reference in reference_stack:
            msg = f"{location}: registry schema dialect reference cycle"
            raise ValueError(msg)
        reference_safe, _reference_closed = _validate_registry_schema_dialect(
            target,
            root=root,
            location=f"{location}/{reference}",
            reference_stack=(*reference_stack, reference),
        )

    raw_type = schema.get("type")
    schema_types = (
        {raw_type}
        if isinstance(raw_type, str)
        else set(t.cast(list[str], raw_type))
        if isinstance(raw_type, list)
        else set()
    )
    object_keywords = {
        "properties",
        "patternProperties",
        "required",
        "dependentRequired",
        "dependentSchemas",
        "additionalProperties",
        "unevaluatedProperties",
        "propertyNames",
        "minProperties",
        "maxProperties",
    }
    object_shape = "object" in schema_types or bool(
        object_keywords.intersection(schema)
    )
    if "additionalProperties" in schema and schema["additionalProperties"] is not False:
        msg = (
            f"{location}: registry schema dialect requires "
            "additionalProperties:false or unevaluatedProperties:false"
        )
        raise ValueError(msg)
    if (
        "unevaluatedProperties" in schema
        and schema["unevaluatedProperties"] is not False
    ):
        msg = (
            f"{location}: registry schema dialect requires "
            "additionalProperties:false or unevaluatedProperties:false"
        )
        raise ValueError(msg)
    own_closed = object_shape and (
        schema.get("additionalProperties") is False
        or schema.get("unevaluatedProperties") is False
    )
    effective_closed = constraint_context or intrinsic_closed
    if object_shape and not effective_closed:
        msg = (
            f"{location}: registry schema dialect requires "
            "additionalProperties:false or unevaluatedProperties:false"
        )
        raise ValueError(msg)

    for keyword in ("properties", "patternProperties"):
        children = schema.get(keyword)
        if isinstance(children, dict):
            for name, child in children.items():
                _validate_registry_schema_dialect(
                    child,
                    root=root,
                    location=f"{location}/{keyword}/{name}",
                    reference_stack=reference_stack,
                )
    property_names = schema.get("propertyNames")
    if property_names is not None:
        _validate_registry_schema_dialect(
            property_names,
            root=root,
            location=f"{location}/propertyNames",
            constraint_context=True,
            reference_stack=reference_stack,
        )

    array_typed = "array" in schema_types
    if array_typed and "items" not in schema:
        msg = f"{location}: registry schema dialect requires constrained items"
        raise ValueError(msg)
    if "items" in schema:
        _validate_registry_schema_dialect(
            schema["items"],
            root=root,
            location=f"{location}/items",
            reference_stack=reference_stack,
        )
    prefix_items = schema.get("prefixItems")
    if isinstance(prefix_items, list):
        for index, child in enumerate(prefix_items):
            _validate_registry_schema_dialect(
                child,
                root=root,
                location=f"{location}/prefixItems/{index}",
                reference_stack=reference_stack,
            )
    if "contains" in schema:
        _validate_registry_schema_dialect(
            schema["contains"],
            root=root,
            location=f"{location}/contains",
            reference_stack=reference_stack,
        )
    if "unevaluatedItems" in schema and schema["unevaluatedItems"] is not False:
        msg = f"{location}: registry schema dialect requires closed items"
        raise ValueError(msg)

    dependent_schemas = schema.get("dependentSchemas")
    if isinstance(dependent_schemas, dict):
        for name, child in dependent_schemas.items():
            _validate_registry_schema_dialect(
                child,
                root=root,
                location=f"{location}/dependentSchemas/{name}",
                constraint_context=effective_closed,
                reference_stack=reference_stack,
            )
    branch_results: dict[str, list[tuple[bool, bool]]] = {}
    for keyword in ("allOf", "anyOf", "oneOf"):
        children = schema.get(keyword)
        if isinstance(children, list):
            branch_results[keyword] = [
                _validate_registry_schema_dialect(
                    child,
                    root=root,
                    location=f"{location}/{keyword}/{index}",
                    constraint_context=effective_closed,
                    reference_stack=reference_stack,
                )
                for index, child in enumerate(children)
            ]
    for keyword in ("not", "if", "then", "else"):
        if keyword in schema:
            _validate_registry_schema_dialect(
                schema[keyword],
                root=root,
                location=f"{location}/{keyword}",
                constraint_context=effective_closed,
                reference_stack=reference_stack,
            )

    independently_safe = (
        bool(schema_types)
        or own_closed
        or reference_safe
        or any(keyword in schema for keyword in ("const", "enum"))
    )
    if branch_results.get("allOf"):
        independently_safe = independently_safe or any(
            safe for safe, _closed in branch_results["allOf"]
        )
    for keyword in ("anyOf", "oneOf"):
        if branch_results.get(keyword):
            independently_safe = independently_safe or all(
                safe for safe, _closed in branch_results[keyword]
            )
    if not independently_safe and not constraint_context:
        msg = f"{location}: registry schema dialect forbids unconstrained schemas"
        raise ValueError(msg)
    return independently_safe, intrinsic_closed


def load_registry(path: pathlib.Path) -> ScenarioRegistry:
    """Load and validate the closed operation registry.

    Parameters
    ----------
    path : pathlib.Path
        Registry JSON path.

    Returns
    -------
    ScenarioRegistry
        Validated immutable outer registry.

    Raises
    ------
    ValueError
        Raised for unknown fields, invalid schemas, or duplicate wire tags.

    Examples
    --------
    >>> path = pathlib.Path('cxx/tests/differential/scenario_registry.json')
    >>> registry = load_registry(path)
    >>> sorted(registry.operations)
    ['server.list_sessions']
    """
    document = _load_json_object(path)
    if set(document) != {"schema_version", "operations"}:
        msg = "registry fields are not closed"
        raise ValueError(msg)
    if type(document["schema_version"]) is not int or document["schema_version"] != 1:
        msg = "registry schema_version must equal 1"
        raise ValueError(msg)
    raw_operations = document["operations"]
    if not isinstance(raw_operations, dict) or not raw_operations:
        msg = "registry operations must be a nonempty object"
        raise ValueError(msg)
    operations: dict[str, RegistryOperation] = {}
    used_wire_tags: set[int] = set()
    expected_fields = {
        "scenario_id",
        "request_schema",
        "response_schema",
        "python_handler",
        "cpp_handler",
        "wire",
        "entity_fields",
    }
    for tag, raw in raw_operations.items():
        if not isinstance(tag, str) or not tag or not isinstance(raw, dict):
            msg = "registry operation tags and rows must be objects"
            raise ValueError(msg)
        if set(raw) != expected_fields:
            msg = f"{tag}: registry operation fields are not closed"
            raise ValueError(msg)
        for field in ("scenario_id", "python_handler", "cpp_handler"):
            if not isinstance(raw[field], str) or not raw[field]:
                msg = f"{tag}: {field} must be a nonempty string"
                raise ValueError(msg)
        request_schema = raw["request_schema"]
        response_schema = raw["response_schema"]
        if not isinstance(request_schema, (dict, bool)) or not isinstance(
            response_schema, (dict, bool)
        ):
            msg = f"{tag}: registry schema dialect requires schema values"
            raise TypeError(msg)
        try:
            jsonschema.Draft202012Validator.check_schema(request_schema)
            jsonschema.Draft202012Validator.check_schema(response_schema)
        except jsonschema.SchemaError as error:
            msg = f"{tag}: invalid operation schema: {error.message}"
            raise ValueError(msg) from error
        for field, schema in (
            ("request_schema", request_schema),
            ("response_schema", response_schema),
        ):
            root = schema if isinstance(schema, dict) else {}
            _validate_registry_schema_dialect(
                schema,
                root=root,
                location=f"{tag}/{field}",
            )
            if not isinstance(schema, dict):
                msg = f"{tag}/{field}: registry schema dialect requires an object root"
                raise TypeError(msg)
        wire = raw["wire"]
        if not isinstance(wire, dict) or set(wire) != {
            "request_tag",
            "response_tags",
        }:
            msg = f"{tag}: wire mapping is not closed"
            raise ValueError(msg)
        request_tag = wire["request_tag"]
        response_tags = wire["response_tags"]
        if (
            type(request_tag) is not int
            or not 0 < request_tag <= 255
            or not isinstance(response_tags, list)
            or not response_tags
            or any(
                type(item) is not int or not 0 < item <= 255 for item in response_tags
            )
        ):
            msg = f"{tag}: wire tags must be nonzero bytes"
            raise ValueError(msg)
        tags = [request_tag, *response_tags]
        if len(tags) != len(set(tags)) or used_wire_tags.intersection(tags):
            msg = f"{tag}: duplicate wire tag"
            raise ValueError(msg)
        used_wire_tags.update(tags)
        raw_entities = raw["entity_fields"]
        if not isinstance(raw_entities, list):
            msg = f"{tag}: entity_fields must be an array"
            raise TypeError(msg)
        entities: list[EntityField] = []
        for entity in raw_entities:
            if not isinstance(entity, dict) or set(entity) != {
                "pointer",
                "kind",
                "role",
            }:
                msg = f"{tag}: entity field is not closed"
                raise ValueError(msg)
            if entity["kind"] not in {"session", "window", "pane"}:
                msg = f"{tag}: invalid entity kind"
                raise ValueError(msg)
            if entity["role"] not in {"definition", "reference"}:
                msg = f"{tag}: invalid entity role"
                raise ValueError(msg)
            if not isinstance(entity["pointer"], str):
                msg = f"{tag}: entity pointer must be a string"
                raise TypeError(msg)
            entities.append(
                EntityField(
                    entity["pointer"],
                    t.cast(str, entity["kind"]),
                    t.cast(str, entity["role"]),
                )
            )
        operations[tag] = RegistryOperation(
            tag=tag,
            scenario_id=t.cast(str, raw["scenario_id"]),
            request_schema=copy.deepcopy(t.cast(dict[str, object], request_schema)),
            response_schema=copy.deepcopy(t.cast(dict[str, object], response_schema)),
            python_handler=t.cast(str, raw["python_handler"]),
            cpp_handler=t.cast(str, raw["cpp_handler"]),
            request_wire_tag=request_tag,
            response_wire_tags=tuple(t.cast(list[int], response_tags)),
            entity_fields=tuple(entities),
        )
    return ScenarioRegistry(1, operations, canonical_sha256(document))


def load_scenario(
    path: pathlib.Path,
    schema_path: pathlib.Path,
    registry: ScenarioRegistry,
) -> ScenarioSpec:
    """Load a scenario and validate every request against its registry entry.

    Parameters
    ----------
    path : pathlib.Path
        Scenario JSON path.
    schema_path : pathlib.Path
        Closed scenario schema path.
    registry : ScenarioRegistry
        Operation registry used for dispatch validation.

    Returns
    -------
    ScenarioSpec
        Validated scenario with registry-derived entity declarations.

    Raises
    ------
    ValueError
        Raised for schema, ownership, or request violations.

    Examples
    --------
    >>> root = pathlib.Path('cxx/tests/differential')
    >>> registry = load_registry(root / 'scenario_registry.json')
    >>> spec = load_scenario(
    ...     root / 'scenarios/server-lifecycle.json',
    ...     root / 'scenario.schema.json',
    ...     registry,
    ... )
    >>> spec.scenario_id
    'server-lifecycle'
    """
    from .canonicalize import CanonicalizationRules

    document = _load_json_object(path)
    schema = _load_json_object(schema_path)
    try:
        jsonschema.Draft202012Validator(schema).validate(document)
    except jsonschema.ValidationError as error:
        msg = f"scenario schema violation: {error.message}"
        raise ValueError(msg) from error
    scenario_id = t.cast(str, document["scenario_id"])
    raw_operations = t.cast(list[dict[str, object]], document["operations"])
    operations: list[ScenarioOperation] = []
    declarations: dict[str, EntityField] = {}
    for raw in raw_operations:
        tag = t.cast(str, raw["tag"])
        if tag not in registry.operations:
            msg = f"unregistered operation tag {tag!r}"
            raise ValueError(msg)
        registered = registry.operations[tag]
        if registered.scenario_id != scenario_id:
            msg = f"{tag}: operation does not belong to owning scenario"
            raise ValueError(msg)
        request = t.cast(dict[str, object], raw["request"])
        try:
            jsonschema.Draft202012Validator(registered.request_schema).validate(request)
        except jsonschema.ValidationError as error:
            msg = f"{tag}: request schema violation: {error.message}"
            raise ValueError(msg) from error
        operations.append(ScenarioOperation(tag, copy.deepcopy(request)))
        for declaration in registered.entity_fields:
            previous = declarations.setdefault(declaration.pointer, declaration)
            if previous != declaration:
                msg = f"{tag}: conflicting entity declaration at {declaration.pointer}"
                raise ValueError(msg)
    raw_rules = t.cast(dict[str, list[str]], document["canonicalization"])
    rules = CanonicalizationRules(
        tuple(raw_rules["entity_id_pointers"]),
        tuple(raw_rules["remove_pointers"]),
        tuple(raw_rules["unordered_pointers"]),
    )
    if set(rules.entity_id_pointers) != set(declarations):
        msg = "scenario entity pointers do not match registry declarations"
        raise ValueError(msg)
    return ScenarioSpec(
        scenario_id,
        tuple(operations),
        rules,
        tuple(declarations[pointer] for pointer in rules.entity_id_pointers),
    )


def validate_observation(
    operation: RegistryOperation, observation: t.Mapping[str, object]
) -> None:
    """Validate one adapter observation against its registered response schema.

    Parameters
    ----------
    operation : RegistryOperation
        Registry row owning the observation.
    observation : Mapping[str, object]
        Decoded observation object.

    Raises
    ------
    ValueError
        Raised when the observation shape is not registered.

    Examples
    --------
    >>> path = pathlib.Path('cxx/tests/differential/scenario_registry.json')
    >>> operation = load_registry(path).operations['server.list_sessions']
    >>> observation = {
    ...     'tag': 'server.list_sessions',
    ...     'sessions': [],
    ...     'stderr': [],
    ...     'warnings': [],
    ... }
    >>> validate_observation(operation, observation)
    """
    try:
        jsonschema.Draft202012Validator(operation.response_schema).validate(observation)
    except jsonschema.ValidationError as error:
        msg = f"{operation.tag}: response schema violation: {error.message}"
        raise ValueError(msg) from error
