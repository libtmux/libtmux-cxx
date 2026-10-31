"""Tests for resolving and building a tmux ref, against a local repository.

The repository stands in for github.com/tmux/tmux. It has a lightweight tag, an
annotated tag, a branch, and a tag and a branch that share a name, which is the
case where "tag first" decides what a person gets.
"""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest

from tools.ci import plan, tmux_ref

AUTOGEN = "cp config.template configure && chmod +x configure\n"
CONFIGURE = """#!/bin/sh
for arg in "$@"; do
  case "$arg" in --prefix=*) prefix="${arg#--prefix=}" ;; esac
done
sed "s|@PREFIX@|$prefix|" Makefile.in > Makefile
"""
MAKEFILE = (
    "all:\n\t@:\n"
    "install:\n\tmkdir -p @PREFIX@/bin\n\tcp tmux.sh @PREFIX@/bin/tmux\n"
    "\tchmod +x @PREFIX@/bin/tmux\n"
)


def git(root: pathlib.Path, *args: str) -> str:
    """Run git in ``root`` and return its trimmed output."""
    done = subprocess.run(
        [
            "git",
            "-c",
            "user.name=t",
            "-c",
            "user.email=t@example.invalid",
            "-C",
            str(root),
            *args,
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    return done.stdout.strip()


class TmuxRefTest(unittest.TestCase):
    """Resolve and build against a repository with known refs."""

    def setUp(self) -> None:
        """Create the stand-in upstream and record the commits of its refs."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.remote = self.root / "upstream"
        self.remote.mkdir()
        git(self.remote, "init", "-q", "-b", "master")
        for name, text in (
            ("autogen.sh", AUTOGEN),
            ("config.template", CONFIGURE),
            ("Makefile.in", MAKEFILE),
        ):
            (self.remote / name).write_text(text)
        self.commit("one")
        self.one = git(self.remote, "rev-parse", "HEAD")
        git(self.remote, "tag", "1.0")
        self.commit("two")
        self.two = git(self.remote, "rev-parse", "HEAD")
        git(self.remote, "tag", "-a", "2.0", "-m", "annotated")
        self.commit("three")
        self.three = git(self.remote, "rev-parse", "HEAD")
        git(self.remote, "branch", "topic", self.two)
        git(self.remote, "tag", "dual", self.one)
        git(self.remote, "branch", "dual", self.three)

    def commit(self, label: str) -> None:
        """Commit a fake tmux that reports ``label``."""
        (self.remote / "tmux.sh").write_text(f"#!/bin/sh\necho {label}\n")
        git(self.remote, "add", "-A")
        git(self.remote, "commit", "-q", "-m", label)

    def resolve(self, ref: str) -> str:
        """Resolve ``ref`` against the stand-in upstream."""
        return tmux_ref.resolve(ref, str(self.remote))

    def test_a_tag_names_its_commit(self) -> None:
        """A release tag resolves to the commit it marks."""
        self.assertEqual(self.resolve("1.0"), self.one)

    def test_an_annotated_tag_resolves_to_the_commit(self) -> None:
        """The tag object's own id is not a commit to build."""
        tag_object = git(self.remote, "rev-parse", "2.0")
        self.assertNotEqual(tag_object, self.two)
        self.assertEqual(self.resolve("2.0"), self.two)

    def test_a_branch_names_its_tip(self) -> None:
        """A branch resolves to where it points now."""
        self.assertEqual(self.resolve("topic"), self.two)
        self.assertEqual(self.resolve("master"), self.three)

    def test_a_tag_beats_a_branch_of_the_same_name(self) -> None:
        """The released tag is what a version number means."""
        self.assertNotEqual(self.one, self.three)
        self.assertEqual(self.resolve("dual"), self.one)

    def test_a_full_commit_names_itself(self) -> None:
        """No network is needed to resolve a 40-hex commit."""
        self.assertEqual(tmux_ref.resolve(self.two, "/nonexistent"), self.two)

    def test_an_unknown_ref_is_an_error(self) -> None:
        """A ref that matches nothing never yields an empty cache key."""
        with self.assertRaises(plan.PlanError):
            self.resolve("9.9")

    def test_an_abbreviated_commit_is_an_error(self) -> None:
        """Only a full commit can be fetched by id."""
        with self.assertRaises(plan.PlanError):
            self.resolve(self.two[:7])

    def test_a_malformed_ref_is_an_error(self) -> None:
        """Nothing outside the allowed characters reaches git."""
        for bad in ("--upload-pack=x", "a b", "x;id", "", "y" * 65):
            with self.subTest(ref=bad), self.assertRaises(plan.PlanError):
                self.resolve(bad)

    def test_build_installs_the_tagged_commit(self) -> None:
        """A tag builds its own tree even though master has moved on."""
        prefix = self.root / "prefix"
        tmux_ref.build(self.one, prefix, self.root / "src", str(self.remote), jobs=1)
        reported = subprocess.run(
            [str(prefix / "bin" / "tmux")], capture_output=True, text=True, check=True
        ).stdout.strip()
        self.assertEqual(reported, "one")

    def test_build_installs_a_commit_that_no_ref_names(self) -> None:
        """A bare commit builds the same way as a tag."""
        orphan = self.two
        git(self.remote, "branch", "-D", "topic")
        git(self.remote, "tag", "-d", "2.0")
        prefix = self.root / "prefix"
        tmux_ref.build(orphan, prefix, self.root / "src", str(self.remote), jobs=1)
        reported = subprocess.run(
            [str(prefix / "bin" / "tmux")], capture_output=True, text=True, check=True
        ).stdout.strip()
        self.assertEqual(reported, "two")

    def test_build_takes_only_a_resolved_commit(self) -> None:
        """A name must be resolved first, so the cache key and the build agree."""
        with self.assertRaises(plan.PlanError):
            tmux_ref.build("1.0", self.root / "p", self.root / "s", str(self.remote))

    def test_resolve_writes_github_output(self) -> None:
        """The workflow reads the commit from the step output."""
        output = self.root / "out"
        status = tmux_ref.main(
            [
                "--remote",
                str(self.remote),
                "resolve",
                "1.0",
                "--github-output",
                str(output),
            ]
        )
        self.assertEqual(status, 0)
        self.assertEqual(output.read_text(), f"sha={self.one}\n")

    def test_resolve_failure_exits_nonzero(self) -> None:
        """An unresolvable ref fails the step."""
        self.assertEqual(
            tmux_ref.main(["--remote", str(self.remote), "resolve", "9.9"]), 1
        )


if __name__ == "__main__":
    unittest.main()
