"""Byte-preserving version-one differential wire frames."""

from __future__ import annotations

import dataclasses

MAX_FRAME_SIZE = 1024 * 1024
"""Maximum payload bytes accepted by both wire implementations."""

_FIELD_COUNTS = {0x01: 0, 0x02: 2, 0x03: 3, 0x04: 0}


@dataclasses.dataclass(frozen=True, slots=True)
class WireFrame:
    """One decoded tag and its fixed byte fields.

    Attributes
    ----------
    tag : int
        Known one-byte record tag.
    fields : tuple[bytes, ...]
        Tag-defined raw byte fields.
    """

    tag: int
    fields: tuple[bytes, ...]


def _u32(value: int) -> bytes:
    r"""Encode one unsigned 32-bit big-endian integer.

    Parameters
    ----------
    value : int
        Integer to encode.

    Returns
    -------
    bytes
        Four encoded bytes.

    Raises
    ------
    ValueError
        Raised when the integer is outside the wire range.

    Examples
    --------
    >>> _u32(258)
    b'\x00\x00\x01\x02'
    """
    if value < 0 or value > 0xFFFFFFFF:
        msg = "wire length overflows uint32"
        raise ValueError(msg)
    return value.to_bytes(4, "big")


def encode_frame(frame: WireFrame) -> bytes:
    r"""Encode one bounded frame without a field-count byte.

    Parameters
    ----------
    frame : WireFrame
        Known tag and raw fields.

    Returns
    -------
    bytes
        Length-prefixed encoded frame.

    Raises
    ------
    ValueError
        Raised for an unknown tag, wrong field count, or oversized frame.

    Examples
    --------
    >>> encode_frame(WireFrame(1, ()))
    b'\x00\x00\x00\x01\x01'
    """
    expected = _FIELD_COUNTS.get(frame.tag)
    if expected is None:
        msg = f"unknown wire tag {frame.tag}"
        raise ValueError(msg)
    if len(frame.fields) != expected:
        msg = (
            f"wire tag {frame.tag} field count is {len(frame.fields)}, "
            f"expected {expected}"
        )
        raise ValueError(msg)
    payload_size = 1
    for field in frame.fields:
        payload_size += 4 + len(field)
        if payload_size > 0xFFFFFFFF:
            msg = "wire payload length overflows uint32"
            raise ValueError(msg)
    if payload_size > MAX_FRAME_SIZE:
        msg = "wire payload exceeds maximum frame size"
        raise ValueError(msg)
    payload = bytes((frame.tag,)) + b"".join(
        _u32(len(field)) + field for field in frame.fields
    )
    return _u32(len(payload)) + payload


def decode_frame(encoded: bytes) -> WireFrame:
    r"""Decode exactly one bounded frame and reject trailing bytes.

    Parameters
    ----------
    encoded : bytes
        Complete version-one frame.

    Returns
    -------
    WireFrame
        Tag and byte-preserving fields.

    Raises
    ------
    ValueError
        Raised for truncation, oversize, unknown tags, or trailing data.

    Examples
    --------
    >>> decode_frame(b'\x00\x00\x00\x01\x01')
    WireFrame(tag=1, fields=())
    """
    if len(encoded) < 5:
        msg = "truncated wire frame"
        raise ValueError(msg)
    payload_size = int.from_bytes(encoded[:4], "big")
    if payload_size > MAX_FRAME_SIZE:
        msg = "wire payload exceeds maximum frame size"
        raise ValueError(msg)
    total = 4 + payload_size
    if len(encoded) < total:
        msg = "truncated wire frame"
        raise ValueError(msg)
    if len(encoded) > total:
        msg = "trailing wire data"
        raise ValueError(msg)
    tag = encoded[4]
    expected = _FIELD_COUNTS.get(tag)
    if expected is None:
        msg = f"unknown wire tag {tag}"
        raise ValueError(msg)
    cursor = 5
    fields: list[bytes] = []
    for _ in range(expected):
        if cursor + 4 > total:
            msg = "truncated wire field length"
            raise ValueError(msg)
        size = int.from_bytes(encoded[cursor : cursor + 4], "big")
        cursor += 4
        if size > total - cursor:
            msg = "truncated wire field"
            raise ValueError(msg)
        fields.append(encoded[cursor : cursor + size])
        cursor += size
    if cursor != total:
        msg = "trailing wire payload data"
        raise ValueError(msg)
    return WireFrame(tag, tuple(fields))
