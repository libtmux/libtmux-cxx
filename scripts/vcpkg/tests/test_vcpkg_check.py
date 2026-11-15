"""Tests for the registry consistency check."""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import typing as t

import pytest
import vcpkg_check
from vcpkg_git import git


@pytest.fixture(autouse=True)
def clean_git_environment(monkeypatch: pytest.MonkeyPatch) -> None:
    """Keep inherited ``GIT_*`` variables from redirecting the fixture repository."""
    for name in [name for name in os.environ if name.startswith("GIT_")]:
        monkeypatch.delenv(name)


def commit(root: pathlib.Path, message: str) -> None:
    """Commit everything under ``root`` without any user configuration."""
    subprocess.run(["git", "add", "--all"], cwd=root, check=True)
    subprocess.run(
        [
            "git",
            "-c",
            "user.name=test",
            "-c",
            "user.email=test@example.invalid",
            "-c",
            "commit.gpgsign=false",
            "commit",
            "--quiet",
            "--message",
            message,
        ],
        cwd=root,
        check=True,
    )


def write_json(path: pathlib.Path, document: dict[str, t.Any]) -> None:
    """Write ``document`` to ``path``, creating its directory."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document))


@pytest.fixture
def registry(tmp_path: pathlib.Path) -> pathlib.Path:
    """Provide a committed registry whose versions database matches its one port."""
    subprocess.run(["git", "init", "--quiet"], cwd=tmp_path, check=True)
    write_json(
        tmp_path / "ports/libtmux/vcpkg.json",
        {"name": "libtmux", "version-semver": "1.2.3", "port-version": 1},
    )
    (tmp_path / "ports/libtmux/portfile.cmake").write_text("# port\n")
    commit(tmp_path, "port")
    tree = git("rev-parse", "HEAD:ports/libtmux", repo=tmp_path)
    write_json(
        tmp_path / "versions/l-/libtmux.json",
        {
            "versions": [
                {"version-semver": "1.2.3", "port-version": 1, "git-tree": tree}
            ]
        },
    )
    write_json(
        tmp_path / "versions/baseline.json",
        {"default": {"libtmux": {"baseline": "1.2.3", "port-version": 1}}},
    )
    commit(tmp_path, "versions")
    return tmp_path


def check(root: pathlib.Path, capsys: pytest.CaptureFixture[str]) -> tuple[int, str]:
    """Run the check and return its status with everything it printed."""
    status = vcpkg_check.run(root)
    captured = capsys.readouterr()
    return status, captured.out + captured.err


def test_a_consistent_registry_passes(
    registry: pathlib.Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """The versions database and the committed port agree."""
    status, output = check(registry, capsys)
    assert (status, output) == (0, "registry consistent: libtmux\n")


def test_a_port_edited_without_a_new_version_entry_is_reported(
    registry: pathlib.Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """The registry would serve the tree recorded for the old port."""
    (registry / "ports/libtmux/portfile.cmake").write_text("# edited\n")
    commit(registry, "edit the port")
    status, output = check(registry, capsys)
    assert status == 1
    assert "The registry serves the old port." in output


def test_uncommitted_port_changes_are_reported(
    registry: pathlib.Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """The recorded git-tree describes HEAD, so a dirty tree cannot be judged."""
    (registry / "ports/libtmux/portfile.cmake").write_text("# edited\n")
    status, output = check(registry, capsys)
    assert status == 1
    assert "ports/ has uncommitted changes" in output


def test_a_stray_file_under_ports_is_reported(
    registry: pathlib.Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """``x-add-version --all`` rejects anything under ``ports/`` but a port."""
    (registry / "ports/NOTES.md").write_text("prose\n")
    commit(registry, "stray")
    status, output = check(registry, capsys)
    assert status == 1
    assert "ports/NOTES.md: not a port directory" in output


@pytest.mark.parametrize(
    ("break_registry", "message"),
    [
        pytest.param(
            lambda root: (root / "versions/l-/libtmux.json").unlink(),
            "versions/l-/libtmux.json: missing",
            id="missing-versions-file",
        ),
        pytest.param(
            lambda root: write_json(
                root / "versions/l-/libtmux.json",
                {"versions": [{"version-semver": "0.0.1", "git-tree": "a"}]},
            ),
            "has no entry for 1.2.3#1",
            id="no-entry-for-the-declared-version",
        ),
        pytest.param(
            lambda root: write_json(
                root / "versions/l-/libtmux.json",
                {
                    "versions": [
                        {"version-semver": "1.2.3", "port-version": 1, "git-tree": "a"},
                        {"version-semver": "1.2.3", "port-version": 1, "git-tree": "b"},
                    ]
                },
            ),
            "a version is published once",
            id="duplicate-entries",
        ),
        pytest.param(
            lambda root: write_json(
                root / "versions/baseline.json",
                {"default": {"libtmux": {"baseline": "1.2.2", "port-version": 1}}},
            ),
            "libtmux defaults to 1.2.2#1",
            id="baseline-differs",
        ),
        pytest.param(
            lambda root: write_json(root / "versions/baseline.json", {"default": {}}),
            "no default version for libtmux",
            id="baseline-missing",
        ),
        pytest.param(
            lambda root: write_json(
                root / "ports/libtmux/vcpkg.json",
                {"name": "other", "version-semver": "1.2.3", "port-version": 1},
            ),
            "declares the name 'other'",
            id="name-differs-from-directory",
        ),
        pytest.param(
            lambda root: write_json(
                root / "ports/libtmux/vcpkg.json", {"name": "libtmux"}
            ),
            "declares none of",
            id="no-version-field",
        ),
    ],
)
def test_each_disagreement_is_reported(
    registry: pathlib.Path,
    capsys: pytest.CaptureFixture[str],
    break_registry: t.Callable[[pathlib.Path], object],
    message: str,
) -> None:
    """A registry that disagrees with its port fails and says how."""
    break_registry(registry)
    commit(registry, "break the registry")
    status, output = check(registry, capsys)
    assert status == 1
    assert message in output


def test_a_missing_ports_directory_is_a_usage_error(
    tmp_path: pathlib.Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """There is nothing to check, which differs from a failed check."""
    status, output = check(tmp_path, capsys)
    assert status == 2
    assert "no ports directory" in output


def test_git_answers_empty_outside_a_repository(tmp_path: pathlib.Path) -> None:
    """A failed git call is the same answer as a tree that is not there."""
    assert git("rev-parse", "HEAD", repo=tmp_path) == ""
