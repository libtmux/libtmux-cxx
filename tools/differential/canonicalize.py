"""Fail-closed canonicalization for declared differential fields."""

from __future__ import annotations

import copy
import dataclasses
import json
import re
import typing as t

from .model import EntityField

_ARRAY_INDEX = re.compile(r"(?:0|[1-9][0-9]*)\Z")


@dataclasses.dataclass(frozen=True, slots=True)
class CanonicalizationRules:
    """Exact pointers allowed to change during comparison.

    Attributes
    ----------
    entity_id_pointers : tuple[str, ...]
        Entity identity fields rewritten through stable bijections.
    remove_pointers : tuple[str, ...]
        Unstable fields removed from the semantic result.
    unordered_pointers : tuple[str, ...]
        Arrays sorted by canonical JSON value.
    """

    entity_id_pointers: tuple[str, ...]
    remove_pointers: tuple[str, ...]
    unordered_pointers: tuple[str, ...]


@dataclasses.dataclass(frozen=True, slots=True)
class _Location:
    """Resolved mutable-container location used during rewriting.

    Attributes
    ----------
    parent : dict[str, object] | list[object] | None
        Mutable parent container, or ``None`` for the document root.
    key : str | int | None
        Dictionary key or array index, or ``None`` for the root.
    value : object
        Value selected by the pointer.
    """

    parent: dict[str, object] | list[object] | None
    key: str | int | None
    value: object


def _parse_pointer(pointer: str) -> tuple[str, ...]:
    r"""Parse RFC 6901 escapes plus an array-only ``*`` segment.

    Parameters
    ----------
    pointer : str
        JSON pointer to parse.

    Returns
    -------
    tuple[str, ...]
        Decoded pointer tokens.

    Raises
    ------
    ValueError
        Raised for malformed escapes or partial wildcards.

    Examples
    --------
    >>> _parse_pointer('/a~1b/~0name/*')
    ('a/b', '~name', '*')
    """
    if pointer == "":
        return ()
    if not pointer.startswith("/"):
        msg = f"invalid JSON pointer {pointer!r}"
        raise ValueError(msg)
    decoded: list[str] = []
    for raw in pointer[1:].split("/"):
        if "*" in raw and raw != "*":
            msg = f"invalid wildcard segment in {pointer!r}"
            raise ValueError(msg)
        output: list[str] = []
        index = 0
        while index < len(raw):
            if raw[index] != "~":
                output.append(raw[index])
                index += 1
                continue
            if index + 1 >= len(raw) or raw[index + 1] not in {"0", "1"}:
                msg = f"invalid RFC 6901 escape in {pointer!r}"
                raise ValueError(msg)
            output.append("~" if raw[index + 1] == "0" else "/")
            index += 2
        decoded.append("".join(output))
    return tuple(decoded)


def _pointers_overlap(left: str, right: str) -> bool:
    """Return whether two extended pointer patterns can select one value.

    Parameters
    ----------
    left : str
        First validated pointer.
    right : str
        Second validated pointer.

    Returns
    -------
    bool
        Whether either pointer is a compatible prefix of the other.

    Examples
    --------
    >>> _pointers_overlap('/rows/*/id', '/rows/0/id')
    True
    >>> _pointers_overlap('/rows/*/id', '/rows/*/name')
    False
    """
    left_tokens = _parse_pointer(left)
    right_tokens = _parse_pointer(right)
    return all(
        left_token == right_token or "*" in {left_token, right_token}
        for left_token, right_token in zip(left_tokens, right_tokens, strict=False)
    )


def _resolve(root: object, pointer: str) -> list[_Location]:
    """Resolve an extended pointer against mutable JSON containers.

    Parameters
    ----------
    root : object
        JSON-compatible root value.
    pointer : str
        Pointer with optional complete array wildcards.

    Returns
    -------
    list[_Location]
        Concrete locations in semantic traversal order.

    Raises
    ------
    ValueError
        Raised when a component is missing or a wildcard sees an object.

    Examples
    --------
    >>> [item.value for item in _resolve({'rows': [{'id': 1}]}, '/rows/*/id')]
    [1]
    """
    current = [_Location(None, None, root)]
    for token in _parse_pointer(pointer):
        following: list[_Location] = []
        for location in current:
            value = location.value
            if token == "*":
                if not isinstance(value, list):
                    msg = f"wildcard in {pointer!r} requires an array"
                    raise ValueError(msg)
                following.extend(
                    _Location(value, index, item) for index, item in enumerate(value)
                )
            elif isinstance(value, dict):
                if token not in value:
                    msg = f"missing pointer {pointer!r}"
                    raise ValueError(msg)
                following.append(_Location(value, token, value[token]))
            elif isinstance(value, list):
                if _ARRAY_INDEX.fullmatch(token) is None:
                    msg = f"noncanonical array index in {pointer!r}"
                    raise ValueError(msg)
                index = int(token)
                if index >= len(value):
                    msg = f"missing pointer {pointer!r}"
                    raise ValueError(msg)
                following.append(_Location(value, index, value[index]))
            else:
                msg = f"missing pointer {pointer!r}"
                raise ValueError(msg)
        current = following
        if not current:
            return []
    return current


def _entity_kind(value: str) -> str:
    """Return the entity kind encoded by one tmux ID prefix.

    Parameters
    ----------
    value : str
        Raw tmux entity ID.

    Returns
    -------
    str
        ``session``, ``window``, or ``pane``.

    Raises
    ------
    ValueError
        Raised when the value has no recognized prefix.

    Examples
    --------
    >>> _entity_kind('@3')
    'window'
    """
    kinds = {"$": "session", "@": "window", "%": "pane"}
    if not value or value[0] not in kinds:
        msg = f"wrong-kind entity ID {value!r}"
        raise ValueError(msg)
    return kinds[value[0]]


def _token(kind: str, index: int) -> str:
    """Build one canonical per-kind entity token.

    Parameters
    ----------
    kind : str
        Entity namespace.
    index : int
        One-based first-occurrence index.

    Returns
    -------
    str
        Canonical tmux-shaped token.

    Examples
    --------
    >>> _token('pane', 2)
    '%PANE_2'
    """
    prefixes = {"session": "$", "window": "@", "pane": "%"}
    if kind not in prefixes:
        msg = f"unknown entity kind {kind!r}"
        raise ValueError(msg)
    return f"{prefixes[kind]}{kind.upper()}_{index}"


def _replace(location: _Location, value: object) -> None:
    """Replace one resolved mutable-container location.

    Parameters
    ----------
    location : _Location
        Concrete dictionary or array location.
    value : object
        Replacement value.

    Raises
    ------
    ValueError
        Raised when the location names the document root.

    Examples
    --------
    >>> row = {'id': '$9'}
    >>> _replace(_Location(row, 'id', '$9'), '$SESSION_1')
    >>> row['id']
    '$SESSION_1'
    """
    if isinstance(location.parent, dict) and isinstance(location.key, str):
        location.parent[location.key] = value
        return
    if isinstance(location.parent, list) and isinstance(location.key, int):
        location.parent[location.key] = value
        return
    msg = "cannot replace the document root"
    raise ValueError(msg)


def _rewrite_entities(
    root: object,
    rules: CanonicalizationRules,
    entity_fields: t.Sequence[EntityField],
) -> None:
    """Validate and rewrite declared entity definitions and references.

    Parameters
    ----------
    root : object
        Mutable copied semantic record.
    rules : CanonicalizationRules
        Declared identity pointers.
    entity_fields : Sequence[EntityField]
        Registry roles and entity kinds.

    Raises
    ------
    ValueError
        Raised for an invalid entity graph.

    Examples
    --------
    >>> value = {'sessions': [{'id': '$9'}]}
    >>> _rewrite_entities(value, CanonicalizationRules(('/sessions/*/id',), (), ()), ())
    >>> value['sessions'][0]['id']
    '$SESSION_1'
    """
    if not rules.entity_id_pointers:
        return
    if not entity_fields:
        maps: dict[str, dict[str, str]] = {
            kind: {} for kind in ("session", "window", "pane")
        }
        for pointer in rules.entity_id_pointers:
            for location in _resolve(root, pointer):
                if location.value is None:
                    msg = f"null entity ID at {pointer}"
                    raise ValueError(msg)
                if not isinstance(location.value, str):
                    msg = f"entity ID at {pointer} must be a string"
                    raise TypeError(msg)
                kind = _entity_kind(location.value)
                mapping = maps[kind]
                replacement = mapping.setdefault(
                    location.value, _token(kind, len(mapping) + 1)
                )
                _replace(location, replacement)
        return

    if len({field.pointer for field in entity_fields}) != len(entity_fields):
        msg = "duplicate entity field declaration"
        raise ValueError(msg)
    if {field.pointer for field in entity_fields} != set(rules.entity_id_pointers):
        msg = "entity field registry does not match canonicalization pointers"
        raise ValueError(msg)
    for field in entity_fields:
        if field.kind not in {"session", "window", "pane"}:
            msg = f"unknown entity kind {field.kind!r}"
            raise ValueError(msg)
        if field.role not in {"definition", "reference"}:
            msg = f"unknown entity role {field.role!r}"
            raise ValueError(msg)

    order: dict[tuple[int, str | int], int] = {}
    sequence = 0

    def visit(value: object) -> None:
        """Record mutable leaf order for deterministic definition mapping.

        Examples
        --------
        >>> callable(visit)
        True
        """
        nonlocal sequence
        if isinstance(value, dict):
            for key, item in value.items():
                order[(id(value), key)] = sequence
                sequence += 1
                visit(item)
        elif isinstance(value, list):
            for index, item in enumerate(value):
                order[(id(value), index)] = sequence
                sequence += 1
                visit(item)

    visit(root)
    resolved = [(field, _resolve(root, field.pointer)) for field in entity_fields]
    definitions: dict[str, dict[str, str]] = {
        kind: {} for kind in ("session", "window", "pane")
    }
    definition_locations = [
        (field, location)
        for field, locations in resolved
        if field.role == "definition"
        for location in locations
    ]
    definition_locations.sort(
        key=lambda item: order[(id(item[1].parent), t.cast(str | int, item[1].key))]
    )
    for field, location in definition_locations:
        if location.value is None:
            msg = f"null entity definition at {field.pointer}"
            raise ValueError(msg)
        if not isinstance(location.value, str):
            msg = f"entity definition at {field.pointer} must be a string"
            raise TypeError(msg)
        if _entity_kind(location.value) != field.kind:
            msg = f"wrong-kind definition at {field.pointer}"
            raise ValueError(msg)
        mapping = definitions[field.kind]
        if location.value in mapping:
            msg = f"duplicate {field.kind} definition {location.value!r}"
            raise ValueError(msg)
        mapping[location.value] = _token(field.kind, len(mapping) + 1)

    for field, locations in resolved:
        for location in locations:
            if location.value is None:
                msg = f"null entity ID at {field.pointer}"
                raise ValueError(msg)
            if not isinstance(location.value, str):
                msg = f"entity ID at {field.pointer} must be a string"
                raise TypeError(msg)
            if _entity_kind(location.value) != field.kind:
                msg = f"wrong-kind reference at {field.pointer}"
                raise ValueError(msg)
            mapping = definitions[field.kind]
            if field.role == "reference" and location.value not in mapping:
                msg = f"dangling {field.kind} reference {location.value!r}"
                raise ValueError(msg)
            replacement = mapping[location.value]
            _replace(location, replacement)


def canonicalize(
    record: object,
    rules: CanonicalizationRules,
    *,
    entity_fields: t.Sequence[EntityField] = (),
) -> object:
    """Apply only declared canonical transformations to a copied record.

    Parameters
    ----------
    record : object
        JSON-compatible semantic record.
    rules : CanonicalizationRules
        Exact pointers permitted to change.
    entity_fields : Sequence[EntityField], optional
        Registry declarations for whole-record entity-graph validation.

    Returns
    -------
    object
        Detached canonical record.

    Raises
    ------
    ValueError
        Raised for invalid pointers, missing fields, or invalid entity graphs.

    Examples
    --------
    >>> rules = CanonicalizationRules((), ('/pid',), ('/rows',))
    >>> canonicalize({'pid': 1, 'rows': [2, 1]}, rules)
    {'rows': [1, 2]}
    """
    groups = (
        rules.entity_id_pointers,
        rules.remove_pointers,
        rules.unordered_pointers,
    )
    flattened = [pointer for group in groups for pointer in group]
    for pointer in flattened:
        _parse_pointer(pointer)
        _resolve(record, pointer)
    declared = [
        *(("entity", pointer) for pointer in rules.entity_id_pointers),
        *(("remove", pointer) for pointer in rules.remove_pointers),
        *(("unordered", pointer) for pointer in rules.unordered_pointers),
    ]
    overlaps = any(
        _pointers_overlap(left_pointer, right_pointer)
        and (
            left_pointer == right_pointer
            or "remove" in {left_kind, right_kind}
            or left_kind == right_kind == "entity"
        )
        for index, (left_kind, left_pointer) in enumerate(declared)
        for right_kind, right_pointer in declared[index + 1 :]
    )
    if overlaps:
        msg = "canonicalization rules overlap"
        raise ValueError(msg)

    result = copy.deepcopy(record)
    _rewrite_entities(result, rules, entity_fields)
    for pointer in rules.remove_pointers:
        locations = _resolve(result, pointer)
        if not locations and "*" not in _parse_pointer(pointer):
            msg = f"missing pointer {pointer!r}"
            raise ValueError(msg)
        list_locations = [item for item in locations if isinstance(item.parent, list)]
        other_locations = [item for item in locations if isinstance(item.parent, dict)]
        for location in sorted(
            list_locations, key=lambda item: t.cast(int, item.key), reverse=True
        ):
            del t.cast(list[object], location.parent)[t.cast(int, location.key)]
        for location in other_locations:
            del t.cast(dict[str, object], location.parent)[t.cast(str, location.key)]
        if any(location.parent is None for location in locations):
            msg = "cannot remove the document root"
            raise ValueError(msg)
    for pointer in rules.unordered_pointers:
        locations = _resolve(result, pointer)
        if not locations and "*" not in _parse_pointer(pointer):
            msg = f"missing pointer {pointer!r}"
            raise ValueError(msg)
        for location in locations:
            if not isinstance(location.value, list):
                msg = f"unordered pointer {pointer!r} requires an array"
                raise TypeError(msg)
            location.value.sort(
                key=lambda value: json.dumps(
                    value,
                    sort_keys=True,
                    separators=(",", ":"),
                    ensure_ascii=False,
                    allow_nan=False,
                )
            )
    return result
