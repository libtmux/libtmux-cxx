"""The isolated psmux namespace the Windows runners create and prove they removed.

psmux expands aliases from its server configuration and scans a shared registry
under the user's profile before it parses ``-L``, so a private socket name is
not enough. The runners give each run an empty configuration file, strip every
variable that routes a client to a server, and afterwards kill only the exact
session they created and check that its registry files are gone.

``Server::new_session`` refuses psmux, because concurrent creators cannot prove
ownership, so the runners create the one session through ``tmux.exe`` themselves
and clean up through it.
"""

from __future__ import annotations

import pathlib
import subprocess
import time
from collections.abc import Callable, Mapping

# Everything that selects a server, a session or a configuration. PATHEXT is
# here because it decides which program a bare `tmux` resolves to.
ISOLATED_VARIABLES = (
    "PATHEXT",
    "PSMUX_ACTIVE",
    "PSMUX_CONFIG_FILE",
    "PSMUX_DATA_DIR",
    "PSMUX_NO_WARM",
    "PSMUX_REMOTE_ATTACH",
    "PSMUX_SESSION",
    "PSMUX_SESSION_NAME",
    "PSMUX_TARGET",
    "PSMUX_TARGET_FULL",
    "PSMUX_TARGET_SESSION",
    "TMUX",
    "TMUX_PANE",
)
EXECUTABLE_EXTENSIONS = ".COM;.EXE;.BAT;.CMD"
LIVE_SUFFIXES = (".port", ".key", ".pid")
KNOWN_SUFFIXES = (".sid", *LIVE_SUFFIXES)
SETTLE_SECONDS = 3.0
POLL_SECONDS = 0.1


class PsmuxError(Exception):
    """The fixture could not be created, run or proven clean."""


def shell_profile() -> str:
    """Return the profile the operating system names for this user, or ``""``."""
    try:
        return str(pathlib.Path("~").expanduser())
    except RuntimeError:
        return ""


def resolve_profile_root(
    environ: Mapping[str, str], *, folder_profile: str | None = None
) -> str:
    """Return the Windows profile psmux keeps its registry in.

    The order is ``USERPROFILE``, the profile the shell names, ``HOMEDRIVE`` and
    ``HOMEPATH`` when that directory exists, then ``HOME``.

    Parameters
    ----------
    environ : Mapping[str, str]
        The environment to read.
    folder_profile : str | None, optional
        The shell's answer; defaults to the operating system's.

    Raises
    ------
    PsmuxError
        None of the sources names a profile.

    Examples
    --------
    >>> resolve_profile_root({"USERPROFILE": "u", "HOME": "h"}, folder_profile="f")
    'u'
    >>> resolve_profile_root({"HOME": "h"}, folder_profile="f")
    'f'
    >>> resolve_profile_root({"HOME": "h"}, folder_profile="")
    'h'
    """
    if folder_profile is None:
        folder_profile = shell_profile()
    if environ.get("USERPROFILE", "").strip():
        return environ["USERPROFILE"]
    if folder_profile.strip():
        return folder_profile
    combined = environ.get("HOMEDRIVE", "") + environ.get("HOMEPATH", "")
    if (
        environ.get("HOMEDRIVE", "").strip()
        and environ.get("HOMEPATH", "").strip()
        and pathlib.Path(combined).is_dir()
    ):
        return combined
    if environ.get("HOME", "").strip():
        return environ["HOME"]
    message = "could not resolve the Windows profile used by psmux"
    raise PsmuxError(message)


def registry_entries(root: pathlib.Path, pattern: str) -> list[pathlib.Path]:
    """List the registry entries under ``root`` that match ``pattern`` exactly.

    A missing root is an empty registry. A root that is not a directory, or that
    cannot be read, is an error: reporting it as empty would pass a cleanup
    nobody could verify.

    Examples
    --------
    >>> import tempfile
    >>> with tempfile.TemporaryDirectory() as directory:
    ...     root = pathlib.Path(directory)
    ...     _ = (root / "owned.sid").write_text("")
    ...     _ = (root / "other.sid").write_text("")
    ...     [p.name for p in registry_entries(root, "owned.*")]
    ['owned.sid']
    >>> registry_entries(root, "owned.*")
    []
    """
    if not root.exists():
        return []
    if not root.is_dir():
        message = f"psmux registry root is not a directory: {root}"
        raise PsmuxError(message)
    try:
        return sorted(root.glob(pattern))
    except OSError as error:
        message = f"psmux registry could not be read: {error}"
        raise PsmuxError(message) from error


def isolated_environment(
    base: Mapping[str, str], config: pathlib.Path
) -> dict[str, str]:
    """Return ``base`` without routing variables, pointed at an empty config.

    Examples
    --------
    >>> env = isolated_environment(
    ...     {"TMUX": "x", "PSMUX_TARGET": "y", "KEEP": "z"}, pathlib.Path("c.conf")
    ... )
    >>> sorted(env)
    ['KEEP', 'PATHEXT', 'PSMUX_CONFIG_FILE', 'PSMUX_NO_WARM']
    >>> env["PSMUX_NO_WARM"]
    '1'
    """
    environment = {
        name: value for name, value in base.items() if name not in ISOLATED_VARIABLES
    }
    environment["PATHEXT"] = EXECUTABLE_EXTENSIONS
    environment["PSMUX_NO_WARM"] = "1"
    environment["PSMUX_CONFIG_FILE"] = str(config)
    return environment


def kill_session(
    tmux: str,
    namespace: str,
    session: str,
    environment: Mapping[str, str],
    *,
    run: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> str | None:
    """Kill exactly one session; return why that failed, or ``None``."""
    try:
        done = run(
            [tmux, "-u", "-L", namespace, "kill-session", "-t", session],
            env=dict(environment),
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as error:
        return f"exact psmux session cleanup failed: {error}"
    if done.returncode == 0:
        return None
    detail = "; ".join(
        part for part in (done.stdout.strip(), done.stderr.strip()) if part
    )
    failure = f"exact psmux session cleanup failed with exit {done.returncode}"
    return f"{failure}: {detail}" if detail else failure


def prove_registry_clean(
    registry: pathlib.Path,
    base: str,
    *,
    killed: bool,
    settle: float | None = None,
    poll: float | None = None,
    clock: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> tuple[bool, str | None]:
    """Wait for the exact session's registry files to go, then remove the rest.

    Only entries named ``<base>.*`` are considered. A ``.port``, ``.key`` or
    ``.pid`` file means a server still runs. Any other entry, or a directory, is
    unknown and is left alone. The ``.sid`` file is removed only once the
    session was killed and nothing live or unknown remains.

    Returns
    -------
    tuple[bool, str | None]
        Whether the fixture is proven gone, and the first reason it is not.
    """
    settle = SETTLE_SECONDS if settle is None else settle
    poll = POLL_SECONDS if poll is None else poll
    pattern = f"{base}.*"
    deadline = clock() + settle
    while True:
        residue = registry_entries(registry, pattern)
        live = [p for p in residue if not p.is_dir() and p.suffix in LIVE_SUFFIXES]
        if not live or clock() >= deadline:
            break
        sleep(poll)
    unknown = [p for p in residue if p.is_dir() or p.suffix not in KNOWN_SUFFIXES]
    failure: str | None = None
    if live:
        failure = f"psmux left {len(live)} exact live registry file(s)"
    elif unknown:
        failure = f"psmux left {len(unknown)} unknown exact registry file(s)"
    if killed and not live and not unknown:
        for entry in residue:
            if not entry.is_dir() and entry.suffix == ".sid":
                try:
                    entry.unlink()
                except OSError as error:
                    failure = failure or f"failed to remove exact SID file: {error}"
    remaining = registry_entries(registry, pattern)
    if remaining and failure is None:
        failure = f"psmux left {len(remaining)} exact registry file(s)"
    return killed and not remaining, failure


def remove_config(config: pathlib.Path) -> str | None:
    """Delete the isolated configuration and prove it is gone."""
    try:
        config.unlink(missing_ok=True)
    except OSError as error:
        return f"psmux config cleanup failed: {error}"
    if config.exists():
        return "the isolated psmux config still exists"
    return None


def recovery(
    failure: str, namespace: str, session: str, config: pathlib.Path, extra: str = ""
) -> str:
    """Return the failure with what a person needs to clean up by hand."""
    return (
        f"{failure}; recovery socket={namespace} session={session} config={config}"
        f"{extra}"
    )
