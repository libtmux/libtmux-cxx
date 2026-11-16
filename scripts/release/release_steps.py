#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
r"""Release workflow steps that hold logic a one-command run line cannot.

``release.yml`` calls one subcommand per step. Each reads the tag and commit
from the variables GitHub sets (``GITHUB_REF_NAME``, ``GITHUB_SHA``) and, when
``GITHUB_OUTPUT`` is set, appends the values a later step reads to it.

    uv run scripts/release/release_steps.py tag-fixed --fetch-master
    uv run scripts/release/release_steps.py tag-version
    uv run scripts/release/release_steps.py source-archive
    uv run scripts/release/release_steps.py port-hash --repository OWNER/NAME
    uv run scripts/release/release_steps.py name-asset --source A --output B
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import io
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import time
import typing as t
import urllib.error
import urllib.request

PORT = "ports/libtmux"
EXPECTED_PORT_FILES = ("ports/libtmux/portfile.cmake", "ports/libtmux/vcpkg.json")
GROUP_WRITE = 0o022


class ReleaseError(Exception):
    """A release check failed, with the reason in the message."""


def git(
    *args: str, repo: pathlib.Path, check: bool = True, data: bytes | None = None
) -> subprocess.CompletedProcess[bytes]:
    """Run git in ``repo``, returning the finished process with bytes output.

    Raises
    ------
    ReleaseError
        ``check`` is set and git exited nonzero.
    """
    done = subprocess.run(
        ["git", *args], cwd=repo, input=data, capture_output=True, check=False
    )
    if check and done.returncode != 0:
        message = f"git {' '.join(args)} failed: {done.stderr.decode(errors='replace')}"
        raise ReleaseError(message)
    return done


def git_text(*args: str, repo: pathlib.Path) -> str:
    """Return the stripped stdout of a git command that must succeed."""
    return git(*args, repo=repo).stdout.decode().strip()


def write_outputs(values: dict[str, str]) -> None:
    """Print each value and append it to ``GITHUB_OUTPUT`` when that is set."""
    path = os.environ.get("GITHUB_OUTPUT")
    for key, value in values.items():
        print(f"{key}={value}")
    if path:
        with pathlib.Path(path).open("a", encoding="utf-8") as output:
            for key, value in values.items():
                output.write(f"{key}={value}\n")


def remote_tag_commit(repo: pathlib.Path, tag: str) -> str:
    """Return the commit the origin's ``tag`` names, or an empty string."""
    for ref in (f"refs/tags/{tag}^{{}}", f"refs/tags/{tag}"):
        listed = git_text("ls-remote", "--tags", "origin", ref, repo=repo)
        if listed:
            return listed.split()[0]
    return ""


def check_tag(
    repo: pathlib.Path,
    *,
    sha: str,
    tag: str,
    local: bool = False,
    fetch_master: bool = False,
    ancestor_of: str | None = None,
    master_baseline: str | None = None,
) -> str:
    """Check the tag still names the commit that started the run.

    The event commit must be the commit the origin's tag names, and with
    ``local`` also the one the checked-out tag names. ``ancestor_of`` names a
    ref the commit must be merged into, and ``master_baseline`` the commit
    ``origin/master`` must still be at after ``fetch_master``.

    Returns
    -------
    str
        The event commit.

    Raises
    ------
    ReleaseError
        The tag moved, the commit is not merged, or master moved.
    """
    if fetch_master:
        git("fetch", "--no-tags", "origin", "master", repo=repo)
    event = git_text("rev-parse", f"{sha}^{{commit}}", repo=repo)
    remote = remote_tag_commit(repo, tag)
    commits = [remote]
    if local:
        commits.append(git_text("rev-parse", f"refs/tags/{tag}^{{commit}}", repo=repo))
    if any(commit != event for commit in commits):
        message = f"tag {tag} moved away from event commit {event}"
        raise ReleaseError(message)
    if ancestor_of is not None:
        merged = git(
            "merge-base", "--is-ancestor", event, ancestor_of, repo=repo, check=False
        )
        if merged.returncode != 0:
            message = f"tag commit {event} is not merged into {ancestor_of}"
            raise ReleaseError(message)
    if master_baseline is not None:
        current = git_text("rev-parse", "origin/master", repo=repo)
        if current != master_baseline:
            message = f"master moved from {master_baseline} to {current}"
            raise ReleaseError(message)
    return event


def check_head(repo: pathlib.Path, commit: str) -> None:
    """Require ``HEAD`` to be ``commit``."""
    current = git_text("rev-parse", "HEAD", repo=repo)
    if current != commit:
        message = f"HEAD is {current}, not the prepared baseline {commit}"
        raise ReleaseError(message)


def check_version(repo: pathlib.Path, tag: str) -> str:
    """Require the tag, without its ``v``, to equal the ``VERSION`` file."""
    tagged = tag.removeprefix("v")
    declared = "".join((repo / "VERSION").read_text().split())
    if tagged != declared:
        message = f"tag {tag} says {tagged}; VERSION says {declared}"
        raise ReleaseError(message)
    return f"both say {declared}"


def build_archive(repo: pathlib.Path, tag: str, sha: str) -> dict[str, str]:
    """Write the release source archive and return its version and SHA512."""
    version = tag.removeprefix("v")
    archive = repo / f"libtmux-cxx-{version}.tar.gz"
    git(
        "archive",
        "--format=tar.gz",
        f"--prefix=libtmux-cxx-{version}/",
        "-o",
        str(archive),
        f"{sha}^{{commit}}",
        repo=repo,
    )
    return {
        "version": version,
        "sha512": hashlib.sha512(archive.read_bytes()).hexdigest(),
    }


Entry = tuple[str, int, str, str]


def tar_entries(archive: tarfile.TarFile, *, strip: int) -> dict[str, Entry]:
    """Describe each member of a tarball by path, kind, mode, target and content.

    The group-write bit is dropped from the mode, as extracting under a umask
    would, and ``strip`` leading path components are removed.
    """
    entries: dict[str, Entry] = {}
    for member in archive:
        parts = member.name.strip("/").split("/")[strip:]
        if not parts or not parts[0]:
            continue
        kind = "d" if member.isdir() else "l" if member.issym() else "f"
        digest = ""
        if member.isfile():
            stream = archive.extractfile(member)
            digest = hashlib.sha256(stream.read() if stream else b"").hexdigest()
        entries["/".join(parts)] = (
            kind,
            member.mode & ~GROUP_WRITE,
            member.linkname if member.issym() else "",
            digest,
        )
    return entries


def download(url: str) -> bytes:
    """Fetch ``url``, following redirects."""
    with urllib.request.urlopen(url, timeout=60) as response:
        return t.cast("bytes", response.read())


def port_hash(
    repo: pathlib.Path,
    *,
    repository: str,
    tag: str,
    sha: str,
    fetch: t.Callable[[str], bytes] = download,
    attempts: int = 5,
    wait: float = 10,
    sleep: t.Callable[[float], None] = time.sleep,
) -> dict[str, str]:
    """Return the SHA512 of the archive GitHub generates for the tag.

    GitHub's archive of the tag must hold exactly the files the event commit
    does, with the same kinds, modes and link targets: it is the one the vcpkg
    port fetches. The download is retried, because the archive can lag the tag.
    """
    url = f"https://github.com/{repository}/archive/{tag}.tar.gz"
    generated = b""
    for attempt in range(1, attempts + 1):
        try:
            generated = fetch(url)
        except (OSError, urllib.error.URLError):  # noqa: PERF203
            print(f"attempt {attempt} failed; waiting", file=sys.stderr)
            sleep(wait)
        else:
            break
    if not generated:
        message = f"could not download {url}"
        raise ReleaseError(message)
    committed = git("archive", f"{sha}^{{commit}}", repo=repo).stdout
    with (
        tarfile.open(fileobj=io.BytesIO(committed)) as event_tar,
        tarfile.open(fileobj=io.BytesIO(generated)) as generated_tar,
    ):
        event = tar_entries(event_tar, strip=0)
        made = tar_entries(generated_tar, strip=1)
    if event != made:
        differing = sorted(
            name
            for name in event.keys() | made.keys()
            if event.get(name) != made.get(name)
        )
        message = "the generated archive differs from the event commit: " + ", ".join(
            differing[:10]
        )
        raise ReleaseError(message)
    return {"sha512": hashlib.sha512(generated).hexdigest()}


def apply_validated_port(
    repo: pathlib.Path,
    *,
    patch: pathlib.Path,
    patch_sha256: str,
    base_tree: str,
    validated_tree: str,
) -> dict[str, str]:
    """Apply the Windows-validated port transition, or find it already applied."""
    actual = hashlib.sha256(patch.read_bytes()).hexdigest()
    if actual != patch_sha256:
        message = f"{patch} has SHA256 {actual}, not {patch_sha256}"
        raise ReleaseError(message)
    current = git_text("rev-parse", f"HEAD:{PORT}", repo=repo)
    if current == validated_tree:
        return {"already": "true"}
    if current != base_tree:
        message = (
            f"master port tree {current} is neither tagged base {base_tree} "
            f"nor validated result {validated_tree}"
        )
        raise ReleaseError(message)
    git("apply", "--index", str(patch), repo=repo)
    root = git_text("write-tree", repo=repo)
    result = git_text("rev-parse", f"{root}:{PORT}", repo=repo)
    if result != validated_tree:
        message = (
            f"validated port tree mismatch: expected {validated_tree}, got {result}"
        )
        raise ReleaseError(message)
    return {"already": "false"}


def commit_versions(repo: pathlib.Path, *, version: str, vcpkg: pathlib.Path) -> str:
    """Commit the port transition and the versions database entries for it."""
    message = (
        f"vcpkg(feat[registry]): Serve {version}\n\n"
        "why: The tagged source passed every platform and package gate.\n\n"
        "what:\n"
        "- Publish the exact Windows-validated port transition\n"
        "- Record the git-tree served by the registry baseline\n"
    )
    git("commit", "-m", message, repo=repo)
    subprocess.run(
        [
            str(vcpkg),
            "--x-builtin-ports-root=./ports",
            "--x-builtin-registry-versions-dir=./versions",
            "x-add-version",
            "--all",
            "--verbose",
        ],
        cwd=repo,
        check=True,
    )
    git("add", "versions", repo=repo)
    if git("diff", "--cached", "--quiet", repo=repo, check=False).returncode == 0:
        message = f"x-add-version recorded no versions for {version}"
        raise ReleaseError(message)
    git("commit", "--amend", "--no-edit", repo=repo)
    return git_text("rev-parse", "HEAD", repo=repo)


def push_baseline(
    repo: pathlib.Path, *, repository: str, token: str, branch: str = "master"
) -> None:
    """Push ``HEAD`` to ``branch`` of the repository, authenticating with ``token``."""
    header = base64.b64encode(f"x-access-token:{token}".encode()).decode()
    print(f"::add-mask::{header}")
    environment = {
        **os.environ,
        "GIT_TERMINAL_PROMPT": "0",
        "GIT_CONFIG_COUNT": "1",
        "GIT_CONFIG_KEY_0": "http.https://github.com/.extraheader",
        "GIT_CONFIG_VALUE_0": f"AUTHORIZATION: basic {header}",
    }
    subprocess.run(
        ["git", "push", f"https://github.com/{repository}.git", f"HEAD:{branch}"],
        cwd=repo,
        env=environment,
        check=True,
    )


def name_asset(source: pathlib.Path, output: pathlib.Path, version: str) -> str:
    """Copy ``source`` to ``output`` and write ``output``'s SHA256 beside it.

    ``{version}`` in ``output`` is replaced by ``version``.
    """
    output = pathlib.Path(str(output).replace("{version}", version))
    shutil.copyfile(source, output)
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    checksum = output.with_name(output.name + ".sha256")
    checksum.write_text(f"{digest}  {output.name}\n", encoding="ascii")
    return f"{digest}  {output.name}"


def seal_port(repo: pathlib.Path, patch: pathlib.Path) -> dict[str, str]:
    """Capture the validated port transition as a patch and name its trees.

    Raises
    ------
    ReleaseError
        Anything but the portfile and the manifest changed.
    """
    changed = sorted(
        git_text("diff", "--name-only", "--", PORT, repo=repo).splitlines()
    )
    if changed != sorted(EXPECTED_PORT_FILES):
        message = f"unexpected release transition files: {', '.join(changed)}"
        raise ReleaseError(message)
    patch.parent.mkdir(parents=True, exist_ok=True)
    diff = git(
        "diff", "--binary", "--full-index", "--no-ext-diff", "--", PORT, repo=repo
    ).stdout
    patch.write_bytes(diff)
    base = git_text("rev-parse", f"HEAD:{PORT}", repo=repo)
    git("add", PORT, repo=repo)
    root = git_text("write-tree", repo=repo)
    validated = git_text("rev-parse", f"{root}:{PORT}", repo=repo)
    return {
        "base_tree": base,
        "sha256": hashlib.sha256(diff).hexdigest(),
        "validated_tree": validated,
    }


def main(argv: t.Sequence[str] | None = None) -> int:
    """Run one release step and return a process status."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=pathlib.Path, default=pathlib.Path())
    steps = parser.add_subparsers(dest="step", required=True)

    fixed = steps.add_parser("tag-fixed", help="the tag still names the event commit")
    fixed.add_argument("--local-tag", action="store_true")
    fixed.add_argument("--fetch-master", action="store_true")
    fixed.add_argument("--ancestor-of")
    fixed.add_argument("--master-baseline")

    steps.add_parser("tag-version", help="the tag and VERSION agree")
    steps.add_parser("source-archive", help="build the source archive and hash it")

    ported = steps.add_parser("port-hash", help="hash the archive the port fetches")
    ported.add_argument("--repository", required=True)

    applied = steps.add_parser("apply-validated-port", help="apply the sealed patch")
    applied.add_argument("--patch", type=pathlib.Path, required=True)
    applied.add_argument("--patch-sha256", required=True)
    applied.add_argument("--base-tree", required=True)
    applied.add_argument("--validated-tree", required=True)

    committed = steps.add_parser("commit-versions", help="record the new version")
    committed.add_argument("--version", required=True)
    committed.add_argument("--vcpkg", type=pathlib.Path, required=True)

    steps.add_parser("record-baseline", help="name HEAD as the registry baseline")

    headed = steps.add_parser("head-is", help="HEAD is the named commit")
    headed.add_argument("--commit", required=True)

    pushed = steps.add_parser("push-baseline", help="push HEAD to master")
    pushed.add_argument("--repository", required=True)

    named = steps.add_parser("name-asset", help="copy a file and write its SHA256")
    named.add_argument("--source", type=pathlib.Path, required=True)
    named.add_argument("--output", type=pathlib.Path, required=True)

    sealed = steps.add_parser("seal-port", help="capture the port transition")
    sealed.add_argument("--patch", type=pathlib.Path, required=True)

    arguments = parser.parse_args(argv)
    repo = arguments.repo.resolve()
    tag = os.environ.get("GITHUB_REF_NAME", "")
    sha = os.environ.get("GITHUB_SHA", "")
    try:
        if arguments.step == "tag-fixed":
            check_tag(
                repo,
                sha=sha,
                tag=tag,
                local=arguments.local_tag,
                fetch_master=arguments.fetch_master,
                ancestor_of=arguments.ancestor_of,
                master_baseline=arguments.master_baseline,
            )
        elif arguments.step == "tag-version":
            print(check_version(repo, tag))
        elif arguments.step == "source-archive":
            write_outputs(build_archive(repo, tag, sha))
        elif arguments.step == "port-hash":
            write_outputs(
                port_hash(repo, repository=arguments.repository, tag=tag, sha=sha)
            )
        elif arguments.step == "apply-validated-port":
            write_outputs(
                apply_validated_port(
                    repo,
                    patch=arguments.patch,
                    patch_sha256=arguments.patch_sha256,
                    base_tree=arguments.base_tree,
                    validated_tree=arguments.validated_tree,
                )
            )
        elif arguments.step == "commit-versions":
            commit_versions(repo, version=arguments.version, vcpkg=arguments.vcpkg)
        elif arguments.step == "record-baseline":
            write_outputs({"baseline": git_text("rev-parse", "HEAD", repo=repo)})
        elif arguments.step == "head-is":
            check_head(repo, arguments.commit)
        elif arguments.step == "push-baseline":
            push_baseline(
                repo,
                repository=arguments.repository,
                token=os.environ["GITHUB_TOKEN"],
            )
        elif arguments.step == "name-asset":
            print(name_asset(arguments.source, arguments.output, tag.removeprefix("v")))
        else:
            write_outputs(seal_port(repo, arguments.patch))
    except (ReleaseError, OSError, subprocess.CalledProcessError, KeyError) as error:
        print(f"{arguments.step}: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
