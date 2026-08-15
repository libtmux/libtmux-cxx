"""Exact structural comparison for canonical differential records."""

from __future__ import annotations

import dataclasses


@dataclasses.dataclass(frozen=True, slots=True)
class StructuralDifference:
    """One exact mismatch between two canonical JSON values.

    Attributes
    ----------
    pointer : str
        RFC 6901 pointer to the mismatch.
    kind : str
        ``type``, ``value``, ``missing``, or ``unexpected``.
    expected : object
        Expected value, or ``None`` when unexpected.
    actual : object
        Actual value, or ``None`` when missing.
    """

    pointer: str
    kind: str
    expected: object
    actual: object


def _escape(token: str) -> str:
    """Escape one RFC 6901 pointer token.

    Parameters
    ----------
    token : str
        Object key.

    Returns
    -------
    str
        Escaped token.

    Examples
    --------
    >>> _escape('a/b~c')
    'a~1b~0c'
    """
    return token.replace("~", "~0").replace("/", "~1")


def structural_diff(
    expected: object, actual: object
) -> tuple[StructuralDifference, ...]:
    """Return every deterministic exact structural difference.

    Parameters
    ----------
    expected : object
        Canonical expected JSON value.
    actual : object
        Canonical actual JSON value.

    Returns
    -------
    tuple[StructuralDifference, ...]
        Pointer-sorted mismatches; empty means exact equality.

    Examples
    --------
    >>> structural_diff({'value': 1}, {'value': 2})[0].pointer
    '/value'
    """
    differences: list[StructuralDifference] = []

    def compare(left: object, right: object, pointer: str) -> None:
        """Append mismatches below one pointer.

        Examples
        --------
        >>> callable(compare)
        True
        """
        if type(left) is not type(right):
            differences.append(StructuralDifference(pointer, "type", left, right))
            return
        if isinstance(left, dict) and isinstance(right, dict):
            for key in sorted(set(left) | set(right)):
                child = f"{pointer}/{_escape(str(key))}"
                if key not in right:
                    differences.append(
                        StructuralDifference(child, "missing", left[key], None)
                    )
                elif key not in left:
                    differences.append(
                        StructuralDifference(child, "unexpected", None, right[key])
                    )
                else:
                    compare(left[key], right[key], child)
            return
        if isinstance(left, list) and isinstance(right, list):
            shared = min(len(left), len(right))
            for index in range(shared):
                compare(left[index], right[index], f"{pointer}/{index}")
            differences.extend(
                StructuralDifference(f"{pointer}/{index}", "missing", left[index], None)
                for index in range(shared, len(left))
            )
            differences.extend(
                StructuralDifference(
                    f"{pointer}/{index}", "unexpected", None, right[index]
                )
                for index in range(shared, len(right))
            )
            return
        if left != right:
            differences.append(StructuralDifference(pointer, "value", left, right))

    compare(expected, actual, "")
    return tuple(differences)
