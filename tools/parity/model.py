"""Immutable records emitted by the parity extractor."""

from __future__ import annotations

import dataclasses


@dataclasses.dataclass(frozen=True, slots=True)
class InputSpec:
    """Configured parity input, optionally narrowed to metadata fields.

    Attributes
    ----------
    path : str
        Repository-relative path.
    fields : tuple[str, ...]
        Selected TOML fields. An empty tuple records the full Git object.
    """

    path: str
    fields: tuple[str, ...] = ()


@dataclasses.dataclass(frozen=True, slots=True)
class SourceIdentity:
    """Identity and cleanliness policy for observed source.

    Attributes
    ----------
    revision : str
        User-supplied Git revision.
    commit : str
        Resolved commit object ID.
    tree : str
        Tree object ID for the resolved commit.
    generator_version : int
        Extractor schema version.
    clean_policy : str
        Meaning assigned to ``clean``.
    clean : bool
        Whether the recorded inputs match their Git objects.
    """

    revision: str
    commit: str
    tree: str
    generator_version: int
    clean_policy: str
    clean: bool


@dataclasses.dataclass(frozen=True, slots=True)
class InputObject:
    """A configured input and the Git object that identifies it.

    Attributes
    ----------
    path : str
        Repository-relative input path.
    kind : str
        Git object kind, such as ``"blob"`` or ``"tree"``.
    object_id : str
        Object ID recorded from the requested revision.
    """

    path: str
    kind: str
    object_id: str


@dataclasses.dataclass(frozen=True, slots=True)
class ApiEntry:
    """A deterministic static observation of one Python API element.

    Attributes
    ----------
    entry_id : str
        Stable identifier within the observed module.
    kind : str
        Observed element kind.
    module : str
        Python module name inferred from the source path.
    qualname : str
        Qualified name inside the module.
    source_path : str
        Repository-relative Python source path.
    signature : str | None
        Static callable signature when the entry is callable.
    value_shape : object | None
        Literal value when the AST permits safe evaluation.
    decorators : tuple[str, ...]
        Decorator expressions recorded without evaluation.
    bases : tuple[str, ...]
        Class bases recorded without evaluation.
    observable_protocols : tuple[str, ...]
        Observable special-method or contract markers.
    """

    entry_id: str
    kind: str
    module: str
    qualname: str
    source_path: str
    signature: str | None
    value_shape: object | None = None
    decorators: tuple[str, ...] = ()
    bases: tuple[str, ...] = ()
    observable_protocols: tuple[str, ...] = ()


@dataclasses.dataclass(frozen=True, slots=True)
class ApiObservation:
    """Complete reproducible observation for one requested revision.

    Attributes
    ----------
    source : SourceIdentity
        Resolved source identity and clean result.
    inputs : tuple[InputObject, ...]
        Git objects defining the parity boundary.
    entries : tuple[ApiEntry, ...]
        Static API observations sorted by identifier.
    """

    source: SourceIdentity
    inputs: tuple[InputObject, ...]
    entries: tuple[ApiEntry, ...]

    @property
    def clean(self) -> bool:
        """Return whether the recorded parity inputs match the revision.

        Returns
        -------
        bool
            Whether every recorded full object and selected field matches.

        Examples
        --------
        >>> source = SourceIdentity("HEAD", "a", "b", 1, "recorded_inputs", True)
        >>> ApiObservation(source, (), ()).clean
        True
        """
        return self.source.clean
