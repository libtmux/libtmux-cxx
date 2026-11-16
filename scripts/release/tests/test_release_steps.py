"""Each release step passes the state it expects and fails the ones it guards."""

from __future__ import annotations

import hashlib
import io
import pathlib
import subprocess
import sys
import tarfile

import pytest
import release_steps


def run(*args: str, cwd: pathlib.Path) -> str:
    """Run git in ``cwd`` as a fixed author and return its stdout."""
    return subprocess.run(
        [
            "git",
            "-c",
            "user.name=Release Test",
            "-c",
            "user.email=test@example.invalid",
            "-c",
            "init.defaultBranch=master",
            *args,
        ],
        cwd=cwd,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()


@pytest.fixture(autouse=True)
def commit_identity(monkeypatch: pytest.MonkeyPatch) -> None:
    """Give every git call an author, as the workflow's earlier steps do."""
    for role in ("AUTHOR", "COMMITTER"):
        monkeypatch.setenv(f"GIT_{role}_NAME", "Release Test")
        monkeypatch.setenv(f"GIT_{role}_EMAIL", "test@example.invalid")


@pytest.fixture
def repository(tmp_path: pathlib.Path) -> pathlib.Path:
    """Return a clone of a bare origin holding one commit tagged ``v1.2.3``."""
    origin = tmp_path / "origin.git"
    origin.mkdir()
    run("init", "--bare", cwd=origin)
    work = tmp_path / "work"
    run("clone", str(origin), str(work), cwd=tmp_path)
    (work / "VERSION").write_text("1.2.3\n")
    (work / "ports/libtmux").mkdir(parents=True)
    (work / "ports/libtmux/portfile.cmake").write_text("old\n")
    (work / "ports/libtmux/vcpkg.json").write_text("{}\n")
    (work / "versions").mkdir()
    (work / "versions/baseline.json").write_text("{}\n")
    run("add", ".", cwd=work)
    run("commit", "-m", "release", cwd=work)
    run("push", "origin", "HEAD:master", cwd=work)
    run("tag", "v1.2.3", cwd=work)
    run("push", "origin", "v1.2.3", cwd=work)
    return work


def head(repo: pathlib.Path) -> str:
    """Return the commit ``repo`` is at."""
    return run("rev-parse", "HEAD", cwd=repo)


def test_a_tag_that_still_names_the_event_commit_passes(
    repository: pathlib.Path,
) -> None:
    """The event, local and remote tags agree and the commit is on master."""
    sha = head(repository)
    assert (
        release_steps.check_tag(
            repository,
            sha=sha,
            tag="v1.2.3",
            local=True,
            fetch_master=True,
            ancestor_of="origin/master",
            master_baseline=sha,
        )
        == sha
    )


def test_a_tag_moved_on_the_origin_is_refused(repository: pathlib.Path) -> None:
    """Re-tagging the origin after the run started breaks the release."""
    sha = head(repository)
    (repository / "later").write_text("x")
    run("add", ".", cwd=repository)
    run("commit", "-m", "later", cwd=repository)
    run("tag", "-f", "v1.2.3", cwd=repository)
    run("push", "-f", "origin", "v1.2.3", cwd=repository)
    with pytest.raises(release_steps.ReleaseError, match="moved away"):
        release_steps.check_tag(repository, sha=sha, tag="v1.2.3")


def test_a_local_tag_that_differs_is_refused_only_when_asked(
    repository: pathlib.Path,
) -> None:
    """The checked-out tag is compared with ``local``."""
    sha = head(repository)
    (repository / "later").write_text("x")
    run("add", ".", cwd=repository)
    run("commit", "-m", "later", cwd=repository)
    run("tag", "-f", "v1.2.3", cwd=repository)
    release_steps.check_tag(repository, sha=sha, tag="v1.2.3")
    with pytest.raises(release_steps.ReleaseError, match="moved away"):
        release_steps.check_tag(repository, sha=sha, tag="v1.2.3", local=True)


def test_a_commit_that_is_not_merged_or_a_master_that_moved_is_refused(
    repository: pathlib.Path,
) -> None:
    """Ancestry and the baseline are each checked."""
    sha = head(repository)
    with pytest.raises(release_steps.ReleaseError, match="not merged"):
        release_steps.check_tag(
            repository, sha=sha, tag="v1.2.3", ancestor_of="refs/heads/nothing"
        )
    with pytest.raises(release_steps.ReleaseError, match="master moved"):
        release_steps.check_tag(
            repository,
            sha=sha,
            tag="v1.2.3",
            fetch_master=True,
            master_baseline="0" * 40,
        )


def test_the_tag_must_match_the_version_file(repository: pathlib.Path) -> None:
    """``v1.2.3`` and ``1.2.3`` agree; ``v1.2.4`` does not."""
    assert release_steps.check_version(repository, "v1.2.3") == "both say 1.2.3"
    with pytest.raises(release_steps.ReleaseError, match=r"VERSION says 1\.2\.3"):
        release_steps.check_version(repository, "v1.2.4")


def test_the_source_archive_is_hashed(repository: pathlib.Path) -> None:
    """The archive is prefixed with the version and its SHA512 is reported."""
    outputs = release_steps.build_archive(repository, "v1.2.3", head(repository))
    archive = repository / "libtmux-cxx-1.2.3.tar.gz"
    assert outputs == {
        "version": "1.2.3",
        "sha512": hashlib.sha512(archive.read_bytes()).hexdigest(),
    }
    with tarfile.open(archive) as opened:
        assert "libtmux-cxx-1.2.3/VERSION" in opened.getnames()


def generated_archive(repo: pathlib.Path, extra: bool = False) -> bytes:
    """Build the tarball GitHub would generate for the tag, optionally altered."""
    plain = subprocess.run(
        ["git", "archive", "--prefix=libtmux-cxx-v1.2.3/", "HEAD"],
        cwd=repo,
        capture_output=True,
        check=True,
    ).stdout
    if not extra:
        data = io.BytesIO()
        with (
            tarfile.open(fileobj=io.BytesIO(plain)) as source,
            tarfile.open(fileobj=data, mode="w:gz") as target,
        ):
            for member in source:
                target.addfile(member, source.extractfile(member))
        return data.getvalue()
    data = io.BytesIO()
    with (
        tarfile.open(fileobj=io.BytesIO(plain)) as source,
        tarfile.open(fileobj=data, mode="w:gz") as target,
    ):
        for member in source:
            target.addfile(member, source.extractfile(member))
        added = tarfile.TarInfo("libtmux-cxx-v1.2.3/extra")
        added.size = 1
        target.addfile(added, io.BytesIO(b"x"))
    return data.getvalue()


def test_the_generated_archive_must_match_the_event_commit(
    repository: pathlib.Path,
) -> None:
    """A tarball with the same files passes; one with an extra file does not."""
    good = generated_archive(repository)
    result = release_steps.port_hash(
        repository,
        repository="o/r",
        tag="v1.2.3",
        sha=head(repository),
        fetch=lambda _url: good,
    )
    assert result == {"sha512": hashlib.sha512(good).hexdigest()}
    bad = generated_archive(repository, extra=True)
    with pytest.raises(release_steps.ReleaseError, match=r"differs.*extra"):
        release_steps.port_hash(
            repository,
            repository="o/r",
            tag="v1.2.3",
            sha=head(repository),
            fetch=lambda _url: bad,
        )


def test_the_download_is_retried_and_then_given_up(repository: pathlib.Path) -> None:
    """Two failures then success passes; five failures raise."""
    good = generated_archive(repository)
    attempts: list[int] = []
    waits: list[float] = []

    def flaky(_url: str) -> bytes:
        attempts.append(1)
        if len(attempts) < 3:
            message = "not yet"
            raise OSError(message)
        return good

    release_steps.port_hash(
        repository,
        repository="o/r",
        tag="v1.2.3",
        sha=head(repository),
        fetch=flaky,
        sleep=waits.append,
    )
    assert len(attempts) == 3
    assert waits == [10, 10]

    def never(_url: str) -> bytes:
        message = "down"
        raise OSError(message)

    with pytest.raises(release_steps.ReleaseError, match="could not download"):
        release_steps.port_hash(
            repository,
            repository="o/r",
            tag="v1.2.3",
            sha=head(repository),
            fetch=never,
            sleep=lambda _seconds: None,
        )


def change_port(repo: pathlib.Path) -> None:
    """Edit the two port files, as the release transition does."""
    (repo / "ports/libtmux/portfile.cmake").write_text("new\n")
    (repo / "ports/libtmux/vcpkg.json").write_text('{"version": "1.2.3"}\n')


def test_the_port_transition_is_sealed_then_applied_once(
    repository: pathlib.Path, tmp_path: pathlib.Path
) -> None:
    """A clone at the base applies the patch, and a second apply finds it done."""
    clone = tmp_path / "clone"
    run("clone", str(tmp_path / "origin.git"), str(clone), cwd=tmp_path)
    change_port(repository)
    patch = repository / "build/validated-port.patch"
    sealed = release_steps.seal_port(repository, patch)
    assert sealed["sha256"] == hashlib.sha256(patch.read_bytes()).hexdigest()
    applied = release_steps.apply_validated_port(
        clone,
        patch=patch,
        patch_sha256=sealed["sha256"],
        base_tree=sealed["base_tree"],
        validated_tree=sealed["validated_tree"],
    )
    assert applied == {"already": "false"}
    assert (clone / "ports/libtmux/portfile.cmake").read_text() == "new\n"
    run("commit", "-m", "serve", cwd=clone)
    again = release_steps.apply_validated_port(
        clone,
        patch=patch,
        patch_sha256=sealed["sha256"],
        base_tree=sealed["base_tree"],
        validated_tree=sealed["validated_tree"],
    )
    assert again == {"already": "true"}


def test_a_patch_or_tree_that_does_not_match_is_refused(
    repository: pathlib.Path, tmp_path: pathlib.Path
) -> None:
    """A different patch digest, or a port tree from neither side, fails."""
    change_port(repository)
    patch = repository / "build/validated-port.patch"
    sealed = release_steps.seal_port(repository, patch)
    clone = tmp_path / "clone"
    run("clone", str(tmp_path / "origin.git"), str(clone), cwd=tmp_path)
    with pytest.raises(release_steps.ReleaseError, match="has SHA256"):
        release_steps.apply_validated_port(
            clone,
            patch=patch,
            patch_sha256="0" * 64,
            base_tree=sealed["base_tree"],
            validated_tree=sealed["validated_tree"],
        )
    with pytest.raises(release_steps.ReleaseError, match="neither tagged base"):
        release_steps.apply_validated_port(
            clone,
            patch=patch,
            patch_sha256=sealed["sha256"],
            base_tree="1" * 40,
            validated_tree=sealed["validated_tree"],
        )


def test_a_transition_that_touches_another_file_is_not_sealed(
    repository: pathlib.Path,
) -> None:
    """Only the portfile and the manifest may change."""
    change_port(repository)
    (repository / "ports/libtmux/extra.cmake").write_text("x")
    run("add", "-N", "ports/libtmux/extra.cmake", cwd=repository)
    with pytest.raises(release_steps.ReleaseError, match="unexpected release"):
        release_steps.seal_port(repository, repository / "build/p.patch")


VCPKG_STAND_IN = """#!{python}
import pathlib
import sys

if "{write}" == "yes":
    pathlib.Path("versions").mkdir(exist_ok=True)
    pathlib.Path("versions/libtmux.json").write_text("{{}}\\n")
"""


@pytest.mark.parametrize("write", ["yes", "no"])
def test_the_versions_database_commit_needs_a_recorded_version(
    repository: pathlib.Path, tmp_path: pathlib.Path, write: str
) -> None:
    """A vcpkg that records nothing is an error; one that records is amended in."""
    vcpkg = tmp_path / "vcpkg"
    vcpkg.write_text(VCPKG_STAND_IN.format(python=sys.executable, write=write))
    vcpkg.chmod(0o755)
    change_port(repository)
    run("add", "ports", cwd=repository)
    if write == "no":
        with pytest.raises(release_steps.ReleaseError, match="recorded no versions"):
            release_steps.commit_versions(repository, version="1.2.3", vcpkg=vcpkg)
        return
    release_steps.commit_versions(repository, version="1.2.3", vcpkg=vcpkg)
    assert run("log", "-1", "--format=%s", cwd=repository) == (
        "vcpkg(feat[registry]): Serve 1.2.3"
    )
    assert "versions/libtmux.json" in run(
        "show", "--name-only", "--format=", cwd=repository
    )


def test_an_asset_is_copied_with_its_checksum(tmp_path: pathlib.Path) -> None:
    """The checksum file names the copy, in sha256sum's format."""
    source = tmp_path / "server"
    source.write_bytes(b"binary")
    output = tmp_path / "libtmux-mcp-server-{version}-linux"
    line = release_steps.name_asset(source, output, "1.2.3")
    named = tmp_path / "libtmux-mcp-server-1.2.3-linux"
    digest = hashlib.sha256(b"binary").hexdigest()
    assert line == f"{digest}  {named.name}"
    assert (tmp_path / f"{named.name}.sha256").read_text() == line + "\n"


def test_head_must_be_the_prepared_baseline(repository: pathlib.Path) -> None:
    """A checkout that moved off the baseline is refused."""
    release_steps.check_head(repository, head(repository))
    with pytest.raises(release_steps.ReleaseError, match="not the prepared baseline"):
        release_steps.check_head(repository, "0" * 40)


def test_main_reports_a_failed_step_on_stderr_and_returns_one(
    repository: pathlib.Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """The command line wraps the same checks."""
    monkeypatch.setenv("GITHUB_REF_NAME", "v1.2.4")
    assert release_steps.main(["--repo", str(repository), "tag-version"]) == 1
    assert "VERSION says 1.2.3" in capsys.readouterr().err
    monkeypatch.setenv("GITHUB_REF_NAME", "v1.2.3")
    assert release_steps.main(["--repo", str(repository), "tag-version"]) == 0
