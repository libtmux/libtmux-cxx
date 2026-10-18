"""Regression tests for the README command checker.

Each rule is shown failing beside a clean control, so a checker that stopped
looking would fail here instead of passing every README quietly.
"""

from __future__ import annotations

import pathlib
import stat
import tempfile
import textwrap
import unittest

from tools.docs import check_readme_commands as commands

README = pathlib.Path("README.md")


def option(
    *aliases: str, takes: bool = False, positional: bool = False, required: bool = False
) -> dict:
    """One entry of the command tree the workspace binary prints."""
    return {
        "name": aliases[0] if aliases else "",
        "aliases": [] if positional else list(aliases),
        "positional": positional,
        "required": required,
        "minimum": 1 if (takes or positional) else 0,
        "maximum": 1 if (takes or positional) else 0,
    }


TREE = {
    "name": "tmux-workspace",
    "options": [option("--help", "-h"), option("--json")],
    "children": [
        {
            "name": "load",
            "options": [
                option("workspace-file", positional=True, required=True),
                option("-L", takes=True),
                option("-d"),
            ],
            "children": [],
        },
        {
            "name": "import",
            "options": [],
            "children": [
                {
                    "name": "tmuxinator",
                    "options": [
                        option("workspace-file", positional=True, required=True),
                        option("--save-to", takes=True),
                    ],
                    "children": [],
                }
            ],
        },
        {
            "name": "shell",
            "options": [option("-c", takes=True)],
            "children": [{"name": "", "options": [option("--code")], "children": []}],
        },
    ],
}


def problems_for(line: str) -> list[str]:
    """Return what the checker says about one `tmux-workspace` command line."""
    text = f"```console\n$ {line}\n```\n"
    found = commands.console_commands(README, text)
    assert len(found) == 1, found
    return [str(p) for p in commands.check_workspace(found[0], TREE)]


class WorkspaceCommandTest(unittest.TestCase):
    """`tmux-workspace` commands are walked through the command tree."""

    def test_accepts_a_documented_command(self) -> None:
        """A subcommand, a valued flag, a bare flag and a positional all pass."""
        self.assertEqual(problems_for("tmux-workspace load -L project -d p.yaml"), [])

    def test_accepts_a_global_flag_after_the_subcommand(self) -> None:
        """`--json` belongs to the root and works after the command name."""
        self.assertEqual(problems_for("tmux-workspace load p.yaml --json"), [])

    def test_accepts_a_nested_subcommand_and_an_option_group(self) -> None:
        """`import tmuxinator` descends twice, and `shell --code` is a group flag."""
        self.assertEqual(
            problems_for("tmux-workspace import tmuxinator p.yml --save-to p.json"), []
        )
        self.assertEqual(problems_for("tmux-workspace shell --code"), [])

    def test_refuses_an_unknown_subcommand(self) -> None:
        """A command the binary does not have is named."""
        found = problems_for("tmux-workspace unload p.yaml")
        self.assertEqual(len(found), 1)
        self.assertIn("no subcommand unload", found[0])

    def test_refuses_an_unknown_flag(self) -> None:
        """A flag the command lacks is named with the command it was given to."""
        found = problems_for("tmux-workspace load --attach p.yaml")
        self.assertEqual(len(found), 1)
        self.assertIn("`tmux-workspace load` has no option --attach", found[0])

    def test_refuses_a_flag_missing_its_value(self) -> None:
        """`-L` takes a socket name, so ending the line there is an error."""
        found = problems_for("tmux-workspace load p.yaml -L")
        self.assertEqual(len(found), 1)
        self.assertIn("-L needs a value", found[0])

    def test_refuses_a_missing_required_argument(self) -> None:
        """`load` with nothing to load is not a command."""
        found = problems_for("tmux-workspace load -d")
        self.assertEqual(len(found), 1)
        self.assertIn("needs 1 argument", found[0])

    def test_refuses_an_extra_argument(self) -> None:
        """A second path where one is allowed is reported."""
        found = problems_for("tmux-workspace import tmuxinator a.yml b.yml")
        self.assertEqual(len(found), 1)
        self.assertIn("takes no such argument", found[0])


class ExtractionTest(unittest.TestCase):
    """Commands are read the way a shell would read them."""

    def test_joins_continuations_and_splits_chains(self) -> None:
        """A backslash line continues, and `&&` starts a second command."""
        text = textwrap.dedent(
            """\
            ```console
            $ cmake -S . -B build \\
                -DLIBTMUX_BUILD_TESTS=OFF \\
                && cmake --build build
            ```
            """
        )

        found = commands.console_commands(README, text)

        self.assertEqual([c.program for c in found], ["cmake", "cmake"])
        self.assertIn("-DLIBTMUX_BUILD_TESTS=OFF", found[0].arguments)
        self.assertEqual(found[1].arguments, ("cmake", "--build", "build"))

    def test_peels_environment_assignments(self) -> None:
        """`NAME=value command` sets the variable for that command only."""
        text = (
            "```console\n$ LIBTMUX_TOOLSETS=inspect libtmux-mcp-server --version\n```\n"
        )

        (found,) = commands.console_commands(README, text)

        self.assertEqual(found.environment, {"LIBTMUX_TOOLSETS": "inspect"})
        self.assertEqual(found.arguments, ("libtmux-mcp-server", "--version"))

    def test_reads_the_server_out_of_a_client_registration(self) -> None:
        """Everything after `--` is the server, and `-e` is its environment."""
        text = textwrap.dedent(
            """\
            ```console
            $ claude mcp add tmux \\
                -e LIBTMUX_TOOLSETS=inspect \\
                -- ~/.local/bin/libtmux-mcp-server --socket-name agent
            ```
            """
        )

        (registration,) = commands.console_commands(README, text)
        server = commands.registered_server(registration)

        self.assertIsNotNone(server)
        self.assertEqual(server.program, "libtmux-mcp-server")
        self.assertEqual(server.arguments[1:], ("--socket-name", "agent"))
        self.assertEqual(server.environment, {"LIBTMUX_TOOLSETS": "inspect"})

    def test_reads_the_server_out_of_a_json_client_entry(self) -> None:
        """`command`, `args` and `env` under `mcpServers` are one invocation."""
        text = textwrap.dedent(
            """\
            ```json
            {"mcpServers": {"tmux": {
              "command": "/home/you/.local/bin/libtmux-mcp-server",
              "args": ["--socket-name", "agent"],
              "env": {"LIBTMUX_TOOLSETS": "inspect"}}}}
            ```
            """
        )

        (found,) = commands.client_commands(README, text)

        self.assertEqual(found.program, "libtmux-mcp-server")
        self.assertEqual(found.arguments[1:], ("--socket-name", "agent"))
        self.assertEqual(found.environment, {"LIBTMUX_TOOLSETS": "inspect"})


class CMakeCommandTest(unittest.TestCase):
    """`-DLIBTMUX_*` options and presets must exist in the project."""

    options = frozenset({"LIBTMUX_BUILD_TESTS"})
    presets = frozenset({"cxx-dev"})

    def problems(self, line: str) -> list[str]:
        """Return what the checker says about one cmake command line."""
        (found,) = commands.console_commands(README, f"```console\n$ {line}\n```\n")
        return [
            str(p)
            for p in commands.check_cmake(found, set(self.options), set(self.presets))
        ]

    def test_accepts_a_defined_option_and_preset(self) -> None:
        """A real option and a real preset are clean."""
        self.assertEqual(
            self.problems("cmake --preset cxx-dev -DLIBTMUX_BUILD_TESTS=OFF"), []
        )

    def test_refuses_an_option_the_project_does_not_define(self) -> None:
        """A misspelt option would be ignored by cmake, so it is caught here."""
        found = self.problems("cmake -S . -B build -DLIBTMUX_BUILD_TEST=OFF")
        self.assertEqual(len(found), 1)
        self.assertIn("LIBTMUX_BUILD_TEST is not an option", found[0])

    def test_refuses_a_preset_that_is_not_defined(self) -> None:
        """`--preset` names must be in CMakePresets.json."""
        found = self.problems("cmake --preset cxx-missing")
        self.assertEqual(len(found), 1)
        self.assertIn("cxx-missing", found[0])

    def test_ignores_options_of_other_projects(self) -> None:
        """Only this project's `LIBTMUX_` options are held to its CMake files."""
        self.assertEqual(self.problems("cmake -DCMAKE_BUILD_TYPE=Release ."), [])


SERVER = """\
#!/bin/sh
case "$1" in
  --version) echo 0.0.0; exit 0 ;;
  --socket-name) ;;
  *) echo "unknown argument: $1" >&2; exit 2 ;;
esac
if [ "${LIBTMUX_TOOLSETS:-}" = "bogus" ]; then
  echo "unknown LIBTMUX_TOOLSETS name" >&2
  exit 2
fi
while read -r line; do
  case "$line" in
    *tools/list*) echo '{"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"t"}]}}' ;;
    *) ;;
  esac
done
"""


class ServerCommandTest(unittest.TestCase):
    """The server is started as documented, so a refusal at startup is found."""

    def run_documented(self, environment: dict[str, str], *arguments: str) -> list[str]:
        """Run a stand-in server the way the checker runs the real one."""
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "libtmux-mcp-server"
            binary.write_text(SERVER, encoding="utf-8")
            binary.chmod(binary.stat().st_mode | stat.S_IXUSR)
            command = commands.Command(
                README, 1, environment, ("libtmux-mcp-server", *arguments)
            )
            return [str(p) for p in commands.run_server(command, binary)]

    def test_accepts_arguments_the_server_takes(self) -> None:
        """A server that lists its tools and exits zero is clean."""
        self.assertEqual(self.run_documented({}, "--socket-name", "agent"), [])
        self.assertEqual(self.run_documented({}, "--version"), [])

    def test_refuses_an_argument_the_server_rejects(self) -> None:
        """A flag the server turns away fails with its own stderr."""
        found = self.run_documented({}, "--socket-nam", "agent")
        self.assertEqual(len(found), 1)
        self.assertIn("exited 2", found[0])
        self.assertIn("unknown argument", found[0])

    def test_refuses_an_environment_the_server_rejects(self) -> None:
        """A documented variable the server refuses at startup is caught."""
        found = self.run_documented(
            {"LIBTMUX_TOOLSETS": "bogus"}, "--socket-name", "agent"
        )
        self.assertEqual(len(found), 1)
        self.assertIn("unknown LIBTMUX_TOOLSETS", found[0])


if __name__ == "__main__":
    unittest.main()
