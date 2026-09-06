#include "mcp_swap.hpp"
#include "storage.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace libtmux::mcp_swap {
namespace {

Paths test_paths() {
  return Paths{"/tmp/mcp-swap-home", "/tmp/mcp-swap-config", "/tmp/mcp-swap-state",
               "/tmp/mcp-swap-repo"};
}

std::vector<std::string> names(std::span<const Client> clients) {
  std::vector<std::string> result;
  result.reserve(clients.size());
  std::ranges::transform(clients, std::back_inserter(result),
                         [](const Client& client) { return client.name; });
  return result;
}

TEST(McpSwapSelection, CanonicalisesEveryClientOrdering) {
  const auto clients = known_clients(test_paths());
  const auto expected = names(clients);
  auto ordering = expected;
  std::ranges::sort(ordering);
  std::size_t permutations = 0;

  do {
    EXPECT_EQ(names(select_clients(clients, ordering)), expected);
    ++permutations;
  } while (std::ranges::next_permutation(ordering).found);

  EXPECT_EQ(permutations, 40'320U);
}

TEST(McpSwapSelection, AcceptsAliasesCommaListsAndDuplicates) {
  const auto clients = known_clients(test_paths());
  const std::array selectors{std::string{"pi,antigravity"}, std::string{"claude"},
                             std::string{"agy"}};

  EXPECT_EQ(names(select_clients(clients, selectors)),
            (std::vector<std::string>{"claude", "agy", "pi"}));
}

TEST(McpSwapSelection, RejectsUnknownAndEmptySelectors) {
  const auto clients = known_clients(test_paths());
  const std::array unknown{std::string{"claude,other"}};
  const std::array empty{std::string{" , "}};

  EXPECT_THROW(static_cast<void>(select_clients(clients, unknown)), Error);
  EXPECT_THROW(static_cast<void>(select_clients(clients, empty)), Error);
}

TEST(McpSwapStorage, Sha256MatchesThePublishedVector) {
  EXPECT_EQ(detail::sha256("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(McpSwapStorage, RejectsImpossibleSerializedPathBindings) {
  const nlohmann::json identity{{"device", 1U}, {"inode", 2U}};
  const nlohmann::json node{{"position", 1U},
                            {"kind", "directory"},
                            {"link", nullptr},
                            {"identity", identity}};
  const nlohmann::json valid{{"resolved", "/tmp/config.json"},
                             {"nodes", nlohmann::json::array({node})},
                             {"parent", identity},
                             {"target", identity},
                             {"mode", 0600U}};

  auto invalid_mode = valid;
  invalid_mode["mode"] = 01000U;
  EXPECT_THROW(static_cast<void>(detail::binding_from_json(invalid_mode)), Error);

  auto unnormalised = valid;
  unnormalised["resolved"] = "/tmp/../tmp/config.json";
  EXPECT_THROW(static_cast<void>(detail::binding_from_json(unnormalised)), Error);

  auto repeated_position = valid;
  repeated_position["nodes"].push_back(node);
  EXPECT_THROW(static_cast<void>(detail::binding_from_json(repeated_position)), Error);

  auto empty_link = valid;
  empty_link["nodes"][0]["kind"] = "symlink";
  empty_link["nodes"][0]["link"] = "";
  EXPECT_THROW(static_cast<void>(detail::binding_from_json(empty_link)), Error);
}

TEST(McpSwapArguments, ParsesTheCompleteUseLocalContract) {
  const std::array arguments{
      std::string{"use-local"},
      std::string{"--source=published"},
      std::string{"--prefix"},
      std::string{"/opt/libtmux"},
      std::string{"--socket=/tmp/tmux"},
      std::string{"--server"},
      std::string{"tmux"},
      std::string{"--entry"},
      std::string{"server"},
      std::string{"--env"},
      std::string{"ONE=first"},
      std::string{"--env=TWO="},
      std::string{"--cli"},
      std::string{"pi,claude"},
      std::string{"--client=agy"},
      std::string{"--scope"},
      std::string{"user"},
      std::string{"--dry-run"},
      std::string{"--no-preflight"},
      std::string{"--repo=/checkout"},
  };

  const auto parsed = parse_arguments(arguments);

  EXPECT_EQ(parsed.command, Command::use_local);
  EXPECT_EQ(parsed.source, Source::published);
  EXPECT_EQ(parsed.repo, "/checkout");
  ASSERT_TRUE(parsed.prefix.has_value());
  EXPECT_EQ(*parsed.prefix, "/opt/libtmux");
  ASSERT_TRUE(parsed.socket.has_value());
  EXPECT_EQ(*parsed.socket, "/tmp/tmux");
  EXPECT_EQ(parsed.server, "tmux");
  EXPECT_EQ(parsed.entry, "server");
  EXPECT_EQ(parsed.environment, (std::vector<std::pair<std::string, std::string>>{
                                    {"ONE", "first"}, {"TWO", ""}}));
  EXPECT_EQ(parsed.clients, (std::vector<std::string>{"pi,claude", "agy"}));
  EXPECT_EQ(parsed.scope, Scope::user);
  EXPECT_TRUE(parsed.dry_run);
  EXPECT_TRUE(parsed.no_preflight);
}

TEST(McpSwapArguments, GivesBuildDirectoryAndPrefixTheirDocumentedSources) {
  const std::array local_args{std::string{"use-local"},
                              std::string{"--source=published"},
                              std::string{"--build-dir=build/test"}};
  const std::array published_args{std::string{"use-local"},
                                  std::string{"--source=local"},
                                  std::string{"--prefix=/opt/test"}};

  EXPECT_EQ(parse_arguments(local_args).source, Source::local);
  EXPECT_EQ(parse_arguments(published_args).source, Source::published);
}

TEST(McpSwapArguments, RejectsUnknownMissingAndMalformedOptions) {
  const std::array unknown{std::string{"status"}, std::string{"--write"}};
  const std::array missing{std::string{"use-local"}, std::string{"--env"}};
  const std::array malformed{std::string{"use-local"}, std::string{"--env=EMPTYKEY"}};
  const std::array invalid_scope{std::string{"revert"},
                                 std::string{"--scope=workspace"}};
  const std::array retired_safety{std::string{"use-local"},
                                  std::string{"--env=LIBTMUX_SAFETY=read"}};
  const std::array traversing_entry{std::string{"use-local"},
                                    std::string{"--entry=../server"}};

  EXPECT_THROW(static_cast<void>(parse_arguments(unknown)), Error);
  EXPECT_THROW(static_cast<void>(parse_arguments(missing)), Error);
  EXPECT_THROW(static_cast<void>(parse_arguments(malformed)), Error);
  EXPECT_THROW(static_cast<void>(parse_arguments(invalid_scope)), Error);
  EXPECT_THROW(static_cast<void>(parse_arguments(retired_safety)), Error);
  EXPECT_THROW(static_cast<void>(parse_arguments(traversing_entry)), Error);
}

TEST(McpSwapFormats, PlainJsonKeepsUnknownFieldsEnvironmentAndNewlineStyle) {
  const Client client{"cursor",     "cursor-agent",     "/tmp/cursor.json",
                      "mcpServers", ConfigFormat::json, EntryDialect::standard};
  const std::string original =
      R"({"mcpServers":{"tmux":{"command":"old","args":["before"],"env":{"KEEP":"yes","LIBTMUX_SAFETY":"read","LIBTMUX_TOOLSETS":"read"},"label":"retained"}},"theme":"dark"})";
  const ServerSpec replacement{"/build/server", {"/tmp/socket"}, {{"NEW", "2"}}};

  const auto rendered =
      render_server(client, original, "tmux", replacement, "/checkout", Scope::user);
  const auto spec =
      read_server(client, rendered.bytes, "tmux", "/checkout", Scope::user);

  ASSERT_TRUE(spec.has_value());
  EXPECT_EQ(*spec, (ServerSpec{"/build/server",
                               {"/tmp/socket"},
                               {{"KEEP", "yes"},
                                {"LIBTMUX_SAFETY", "read"},
                                {"LIBTMUX_TOOLSETS", "read"},
                                {"NEW", "2"}}}));
  EXPECT_EQ(rendered.action, ChangeAction::replaced);
  EXPECT_EQ(rendered.bytes.back(), '}');
  EXPECT_NE(rendered.bytes.find("\"label\": \"retained\""), std::string::npos);
  EXPECT_NE(rendered.bytes.find("\"theme\": \"dark\""), std::string::npos);
}

TEST(McpSwapFormats, ClaudeScopesRemainIndependent) {
  const Client client{"claude",     "claude",           "/tmp/.claude.json",
                      "mcpServers", ConfigFormat::json, EntryDialect::claude};
  const std::filesystem::path repository = "/checkout";
  const ServerSpec user{"/user/server", {}, {}};
  const ServerSpec project{"/project/server", {"socket"}, {}};

  auto rendered = render_server(client, "{}\n", "tmux", user, repository, Scope::user);
  rendered = render_server(client, rendered.bytes, "tmux", project, repository,
                           Scope::project);

  EXPECT_EQ(read_server(client, rendered.bytes, "tmux", repository, Scope::user), user);
  EXPECT_EQ(read_server(client, rendered.bytes, "tmux", repository, Scope::project),
            project);
}

TEST(McpSwapFormats, JsoncPreservesCommentsTrailingCommasAndOpencodeDialect) {
  const Client client{"opencode", "opencode",          "/tmp/opencode.jsonc",
                      "mcp",      ConfigFormat::jsonc, EntryDialect::opencode};
  const std::string original = R"({
  // retained root comment
  "theme": "dark",
  "mcp": {
    /* retained server comment */
    "other": {"type": "remote", "url": "https://example.test"},
  },
})";
  const ServerSpec replacement{"/build/server", {"/tmp/socket"}, {{"A", "B"}}};

  const auto rendered =
      render_server(client, original, "tmux", replacement, "/checkout", Scope::user);
  const auto spec =
      read_server(client, rendered.bytes, "tmux", "/checkout", Scope::user);

  ASSERT_TRUE(spec.has_value());
  EXPECT_EQ(*spec, replacement);
  EXPECT_NE(rendered.bytes.find("// retained root comment"), std::string::npos);
  EXPECT_NE(rendered.bytes.find("/* retained server comment */"), std::string::npos);
  EXPECT_NE(rendered.bytes.find("https://example.test"), std::string::npos);
  EXPECT_NE(rendered.bytes.find("\"command\": ["), std::string::npos);
  EXPECT_NE(rendered.bytes.find("\"environment\": {"), std::string::npos);
}

TEST(McpSwapFormats, JsoncKeepsCommentsInsideTheReplacedServerEntry) {
  const Client client{"opencode", "opencode",          "/tmp/opencode.jsonc",
                      "mcp",      ConfigFormat::jsonc, EntryDialect::opencode};
  const std::string original = R"({
  "mcp": {
    "tmux": {
      "type": "local",
      // why this command was selected
      "command": ["old"], // command note
      "environment": {
        // inherited login mode
        "KEEP": "yes",
        // retained policy note
        "LIBTMUX_SAFETY": "read",
        "LIBTMUX_TOOLSETS": "read",
      },
    },
  },
})";
  const ServerSpec replacement{"/build/server", {"/tmp/socket"}, {{"NEW", "2"}}};

  const auto rendered =
      render_server(client, original, "tmux", replacement, "/checkout", Scope::user);

  EXPECT_NE(rendered.bytes.find("// why this command was selected"), std::string::npos);
  EXPECT_NE(rendered.bytes.find("// command note"), std::string::npos);
  EXPECT_NE(rendered.bytes.find("// inherited login mode"), std::string::npos);
  EXPECT_NE(rendered.bytes.find("// retained policy note"), std::string::npos);
  EXPECT_NE(rendered.bytes.find("\"KEEP\": \"yes\""), std::string::npos);
  EXPECT_NE(rendered.bytes.find("LIBTMUX_SAFETY"), std::string::npos);
  EXPECT_NE(rendered.bytes.find("\"NEW\": \"2\""), std::string::npos);
  EXPECT_EQ(read_server(client, rendered.bytes, "tmux", "/checkout", Scope::user),
            (ServerSpec{"/build/server",
                        {"/tmp/socket"},
                        {{"KEEP", "yes"},
                         {"LIBTMUX_SAFETY", "read"},
                         {"LIBTMUX_TOOLSETS", "read"},
                         {"NEW", "2"}}}));
}

TEST(McpSwapFormats, TomlSplicesOnlyOwnedValuesAndKeepsComments) {
  const Client client{"codex",
                      "codex",
                      "/tmp/config.toml",
                      "mcp_servers",
                      ConfigFormat::toml,
                      EntryDialect::standard};
  const std::string original = R"(# root comment
model = "gpt"

[mcp_servers.tmux] # server comment
command = "old" # command note
args = ["before"]
timeout_sec = 30 # retained key

[mcp_servers.tmux.env]
KEEP = "yes" # retained env note
LIBTMUX_SAFETY = "read" # retained policy note
LIBTMUX_TOOLSETS = "read"

[history]
persistence = "save-all"
)";
  const ServerSpec replacement{"/build/server", {"/tmp/socket"}, {{"NEW", "2"}}};

  const auto rendered =
      render_server(client, original, "tmux", replacement, "/checkout", Scope::user);
  const auto spec =
      read_server(client, rendered.bytes, "tmux", "/checkout", Scope::user);

  ASSERT_TRUE(spec.has_value());
  EXPECT_EQ(*spec, (ServerSpec{"/build/server",
                               {"/tmp/socket"},
                               {{"KEEP", "yes"},
                                {"LIBTMUX_SAFETY", "read"},
                                {"LIBTMUX_TOOLSETS", "read"},
                                {"NEW", "2"}}}));
  EXPECT_NE(rendered.bytes.find("# root comment"), std::string::npos);
  EXPECT_NE(rendered.bytes.find("# server comment"), std::string::npos);
  EXPECT_NE(rendered.bytes.find("# command note"), std::string::npos);
  EXPECT_NE(rendered.bytes.find("timeout_sec = 30 # retained key"), std::string::npos);
  EXPECT_NE(rendered.bytes.find("KEEP = \"yes\" # retained env note"),
            std::string::npos);
  EXPECT_NE(rendered.bytes.find("# retained policy note"), std::string::npos);
  EXPECT_NE(rendered.bytes.find("LIBTMUX_SAFETY"), std::string::npos);
  EXPECT_NE(rendered.bytes.find("[history]"), std::string::npos);
}

TEST(McpSwapFormats, RetiredSafetyIsPreservedUntilAnExplicitToolsetMigration) {
  const std::array clients{Client{"cursor", "cursor-agent", "/tmp/config.json",
                                  "mcpServers", ConfigFormat::json,
                                  EntryDialect::standard},
                           Client{"opencode", "opencode", "/tmp/config.jsonc", "mcp",
                                  ConfigFormat::jsonc, EntryDialect::opencode},
                           Client{"codex", "codex", "/tmp/config.toml", "mcp_servers",
                                  ConfigFormat::toml, EntryDialect::standard}};
  const std::array documents{
      std::string{
          R"({"mcpServers":{"tmux":{"command":"old","env":{"LIBTMUX_SAFETY":"read"}}}})"},
      std::string{
          R"({"mcp":{"tmux":{"type":"local","command":["old"],"environment":{"LIBTMUX_SAFETY":"read"}}}})"},
      std::string{"[mcp_servers.tmux]\ncommand = \"old\"\n"
                  "env = { \"LIBTMUX_SAFETY\" = \"read\" } # retain this note\n"}};

  EXPECT_THROW(static_cast<void>(
                   render_server(clients[0], R"({"mcpServers":{}})", "tmux",
                                 ServerSpec{"server", {}, {{"LIBTMUX_SAFETY", "read"}}},
                                 "/checkout", Scope::user)),
               Error);

  for (std::size_t index = 0; index < clients.size(); ++index) {
    const auto preserved =
        render_server(clients[index], documents[index], "tmux",
                      ServerSpec{"server", {}, {}}, "/checkout", Scope::user);
    EXPECT_NE(preserved.bytes.find("LIBTMUX_SAFETY"), std::string::npos) << index;
    EXPECT_NE(std::ranges::find(
                  preserved.effective.environment,
                  std::pair<std::string, std::string>{"LIBTMUX_SAFETY", "read"}),
              preserved.effective.environment.end())
        << index;
    const auto rendered =
        render_server(clients[index], documents[index], "tmux",
                      ServerSpec{"server", {}, {{"LIBTMUX_TOOLSETS", "read"}}},
                      "/checkout", Scope::user);
    EXPECT_EQ(rendered.bytes.find("LIBTMUX_SAFETY"), std::string::npos) << index;
    if (clients[index].format == ConfigFormat::toml) {
      EXPECT_NE(rendered.bytes.find("# retain this note"), std::string::npos);
    }
  }
}

TEST(McpSwapFormats, TomlReadsMultilineArgumentsAndKeepsUnchangedEnvironment) {
  const Client client{"codex",
                      "codex",
                      "/tmp/config.toml",
                      "mcp_servers",
                      ConfigFormat::toml,
                      EntryDialect::standard};
  const std::string original = R"([mcp_servers.tmux]
command = 'old'
args = [
  '--env', # retained only until this owned value is replaced
  "KEY=value",
]

[mcp_servers.tmux.env]
BASIC = "tab\\tquote\\\"slash\\\\"
LITERAL = 'C:\Users\name'
COMMENTED = "readonly" # why this is restricted
)";
  const ServerSpec replacement{"/build/server", {"/tmp/socket"}, {{"NEW", "2"}}};

  const auto rendered =
      render_server(client, original, "tmux", replacement, "/checkout", Scope::user);
  const auto spec =
      read_server(client, rendered.bytes, "tmux", "/checkout", Scope::user);

  ASSERT_TRUE(spec.has_value());
  EXPECT_EQ(*spec, (ServerSpec{"/build/server",
                               {"/tmp/socket"},
                               {{"BASIC", "tab\\tquote\\\"slash\\\\"},
                                {"COMMENTED", "readonly"},
                                {"LITERAL", "C:\\Users\\name"},
                                {"NEW", "2"}}}));
  EXPECT_NE(rendered.bytes.find("BASIC = \"tab\\\\tquote\\\\\\\"slash\\\\\\\\\""),
            std::string::npos);
  EXPECT_NE(rendered.bytes.find("LITERAL = 'C:\\Users\\name'"), std::string::npos);
  EXPECT_NE(rendered.bytes.find("COMMENTED = \"readonly\" # why this is restricted"),
            std::string::npos);
}

TEST(McpSwapFormats, TomlHandlesQuotedKeysAndInlineEnvironment) {
  const Client client{"codex",
                      "codex",
                      "/tmp/config.toml",
                      "mcp_servers",
                      ConfigFormat::toml,
                      EntryDialect::standard};
  const std::string original = R"([mcp_servers.tmux]
command = "old"
args = []
env = { "A=B" = 'literal', KEEP = "yes" } # retain this note
)";
  const ServerSpec replacement{
      "/build/server", {"socket"}, {{"A=B", "override"}, {"NEW", "2"}}};

  const auto rendered =
      render_server(client, original, "tmux", replacement, "/checkout", Scope::user);

  EXPECT_EQ(read_server(client, rendered.bytes, "tmux", "/checkout", Scope::user),
            (ServerSpec{"/build/server",
                        {"socket"},
                        {{"A=B", "override"}, {"KEEP", "yes"}, {"NEW", "2"}}}));
  EXPECT_NE(rendered.bytes.find("# retain this note"), std::string::npos);
  EXPECT_EQ(rendered.bytes.find("[mcp_servers.tmux.env]"), std::string::npos);
}

TEST(McpSwapFormats, TomlRejectsMalformedUnrelatedSyntaxBeforeRendering) {
  const Client client{"codex",
                      "codex",
                      "/tmp/config.toml",
                      "mcp_servers",
                      ConfigFormat::toml,
                      EntryDialect::standard};
  const std::string malformed = R"(model = "gpt"
this is not an assignment

[mcp_servers.tmux]
command = "old"
)";

  EXPECT_THROW(static_cast<void>(render_server(client, malformed, "tmux",
                                               ServerSpec{"new", {}, {}}, "/checkout",
                                               Scope::user)),
               Error);
}

TEST(McpSwapFormats, TomlAcceptsTheCompleteDocumentBeforeSurgicalEdits) {
  const Client client{"codex",
                      "codex",
                      "/tmp/config.toml",
                      "mcp_servers",
                      ConfigFormat::toml,
                      EntryDialect::standard};
  const std::string original = R"(owner.name = "libtmux"
released = 1979-05-27T07:32:00Z

[mcp_servers.tmux]
command = "old"

[[profiles]]
name = "preserved"
)";

  const auto rendered = render_server(
      client, original, "tmux", ServerSpec{"new", {}, {}}, "/checkout", Scope::user);

  EXPECT_NE(rendered.bytes.find("owner.name = \"libtmux\""), std::string::npos);
  EXPECT_NE(rendered.bytes.find("released = 1979-05-27T07:32:00Z"), std::string::npos);
  EXPECT_NE(rendered.bytes.find("[[profiles]]"), std::string::npos);
}

TEST(McpSwapFormats, RefusesAContainerWithTheWrongShape) {
  const Client client{"gemini",     "gemini",           "/tmp/settings.json",
                      "mcpServers", ConfigFormat::json, EntryDialect::standard};

  EXPECT_THROW(static_cast<void>(render_server(client, R"({"mcpServers":[]})", "tmux",
                                               ServerSpec{"server", {}, {}},
                                               "/checkout", Scope::user)),
               Error);
}

TEST(McpSwapFormats, RejectsDuplicateJsonAndJsoncContainers) {
  const std::array clients{Client{"cursor", "cursor-agent", "/tmp/config.json",
                                  "mcpServers", ConfigFormat::json,
                                  EntryDialect::standard},
                           Client{"opencode", "opencode", "/tmp/config.jsonc", "mcp",
                                  ConfigFormat::jsonc, EntryDialect::opencode}};
  const std::array documents{
      std::string{R"({"mcpServers":{},"mcpServers":{"tmux":{"command":"old"}}})"},
      std::string{
          R"({"mcp":{},/* ambiguous */"mcp":{"tmux":{"type":"local","command":["old"]}}})"}};

  for (std::size_t index = 0; index < clients.size(); ++index) {
    EXPECT_THROW(static_cast<void>(render_server(clients[index], documents[index],
                                                 "tmux", ServerSpec{"new", {}, {}},
                                                 "/checkout", Scope::user)),
                 Error)
        << index;
  }
}

TEST(McpSwapFormats, RejectsMalformedUtf8AcrossEveryConfigurationFormat) {
  const std::array clients{Client{"cursor", "cursor-agent", "/tmp/config.json",
                                  "mcpServers", ConfigFormat::json,
                                  EntryDialect::standard},
                           Client{"pi", "pi", "/tmp/config.jsonc", "mcpServers",
                                  ConfigFormat::jsonc, EntryDialect::standard},
                           Client{"codex", "codex", "/tmp/config.toml", "mcp_servers",
                                  ConfigFormat::toml, EntryDialect::standard}};
  std::array<std::string, 3> documents{
      "{\"mcpServers\":{\"tmux\":{\"command\":\"old\"}},\"note\":\"",
      "{\n// invalid: ", "[mcp_servers.tmux]\ncommand = \"old\"\n# invalid: "};
  documents[0].push_back(static_cast<char>(0xff));
  documents[0] += "\"}";
  documents[1].push_back(static_cast<char>(0xff));
  documents[1] += "\n\"mcpServers\":{\"tmux\":{\"command\":\"old\"}}\n}";
  documents[2].push_back(static_cast<char>(0xff));
  documents[2].push_back('\n');

  for (std::size_t index = 0; index < clients.size(); ++index) {
    EXPECT_THROW(static_cast<void>(read_server(clients[index], documents[index], "tmux",
                                               "/checkout", Scope::user)),
                 Error)
        << clients[index].name;
    EXPECT_THROW(static_cast<void>(render_server(clients[index], documents[index],
                                                 "tmux", ServerSpec{"new", {}, {}},
                                                 "/checkout", Scope::user)),
                 Error)
        << clients[index].name;
  }
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::string pattern = "/tmp/libtmux-cxx-mcp-swap-XXXXXX";
    pattern.push_back('\0');
    if (::mkdtemp(pattern.data()) == nullptr) {
      throw std::runtime_error{std::strerror(errno)};
    }
    // Canonical, not as spelled: macOS makes `/tmp` a symlink to `/private/tmp`,
    // and the switcher records a canonical path in its transaction state while
    // binding the route it was handed. Given the two spellings it reports the
    // route as having changed under it, which is the tool being asked about the
    // platform rather than about itself.
    path_ = std::filesystem::canonical(pattern.data());
  }

  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

class ScopedChild final {
public:
  explicit ScopedChild(pid_t pid) : pid_(pid) {}
  ~ScopedChild() {
    if (pid_ <= 0) {
      return;
    }
    static_cast<void>(::kill(pid_, SIGKILL));
    while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
    }
  }

  ScopedChild(const ScopedChild&) = delete;
  ScopedChild& operator=(const ScopedChild&) = delete;

private:
  pid_t pid_;
};

int python_nonblocking_record_lock(const std::filesystem::path& path) {
  const auto child = ::fork();
  if (child < 0) {
    throw std::runtime_error{std::strerror(errno)};
  }
  if (child == 0) {
    const std::string program = "import fcntl,sys\n"
                                "f=open(sys.argv[1],'r+')\n"
                                "try:\n"
                                " fcntl.lockf(f,fcntl.LOCK_EX|fcntl.LOCK_NB)\n"
                                "except BlockingIOError:\n"
                                " raise SystemExit(0)\n"
                                "raise SystemExit(1)\n";
    ::execlp("python3", "python3", "-c", program.c_str(), path.c_str(), nullptr);
    ::_exit(127);
  }
  int status{};
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      throw std::runtime_error{std::strerror(errno)};
    }
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

void write_file(const std::filesystem::path& path, std::string_view bytes,
                mode_t mode = 0600) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  ASSERT_TRUE(stream.is_open());
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  stream.close();
  ASSERT_EQ(::chmod(path.c_str(), mode), 0) << std::strerror(errno);
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

mode_t file_mode(const std::filesystem::path& path) {
  struct stat details {};
  EXPECT_EQ(::stat(path.c_str(), &details), 0) << std::strerror(errno);
  return details.st_mode & 0777;
}

TEST(McpSwapStorage, PublicationRejectsAReplacedParentWithTheSameTarget) {
  TemporaryDirectory temporary;
  const auto directory = temporary.path() / "config";
  const auto displaced = temporary.path() / "displaced";
  const auto path = directory / "settings.json";
  write_file(path, "before", 0640);
  const auto binding = detail::capture_binding(path);

  std::filesystem::rename(directory, displaced);
  std::filesystem::create_directory(directory);
  std::filesystem::create_hard_link(displaced / path.filename(), path);

  EXPECT_THROW(detail::atomic_write(path, "after", 0640, binding.target,
                                    binding.parent_identity),
               Error);
  EXPECT_EQ(read_file(path), "before");
}

class McpSwapTransactionTest : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = temporary_.path();
    runtime_.paths = {root_ / "home", root_ / "config", root_ / "state",
                      root_ / "repo"};
    runtime_.search_path = "";
    runtime_.preflight = [&](const ServerSpec&) {
      ++preflight_calls_;
      return preflight_failure_;
    };
    runtime_.boundary = [](std::string_view, const std::filesystem::path&) {};
    runtime_.timestamp = [] { return "20260903010203"; };
    runtime_.transaction_id = [] { return "00112233445566778899aabbccddeeff"; };

    write_file(runtime_.paths.working_directory / "apps/mcp/CMakeLists.txt",
               "set_target_properties(server PROPERTIES OUTPUT_NAME "
               "libtmux-mcp-server)\n");
    binary_ =
        runtime_.paths.working_directory / "build/native/apps/mcp/libtmux-mcp-server";
    write_file(binary_, "#!/bin/sh\nexit 0\n", 0700);
    seed_configs();
  }

  void seed_configs() {
    const auto clients = known_clients(runtime_.paths);
    for (const auto& client : clients) {
      std::string contents;
      switch (client.format) {
      case ConfigFormat::json:
        if (client.name == "claude") {
          contents = "{\"mcpServers\":{\"other\":{\"type\":\"stdio\",\"command\":"
                     "\"other\",\"args\":[],\"env\":{}}},\"projects\":{}}";
        } else {
          contents = "{\n  \"mcpServers\": {\"other\": {\"command\": \"other\", "
                     "\"args\": []}},\n  \"theme\": \"dark\"\n}\n";
        }
        break;
      case ConfigFormat::jsonc:
        contents = client.name == "opencode"
                       ? "{\n  // keep\n  \"mcp\": {\"other\": {\"type\": \"remote\", "
                         "\"url\": \"https://example.test\"},},\n}\n"
                       : "{\n  // keep\n  \"mcpServers\": {\"other\": {\"command\": "
                         "\"other\", \"args\": []},},\n}\n";
        break;
      case ConfigFormat::toml:
        contents = "# keep\n[mcp_servers.other]\ncommand = \"other\"\nargs = []\n";
        break;
      }
      write_file(client.config_path, contents, 0640);
      original_[client.name] = {contents, 0640};
    }
  }

  std::vector<std::string> use_arguments(
      std::string clients = "claude,codex,cursor,gemini,grok,agy,opencode,pi") const {
    return {"use-local",
            "--repo",
            runtime_.paths.working_directory.string(),
            "--build-dir",
            (runtime_.paths.working_directory / "build/native").string(),
            "--socket",
            "/tmp/libtmux-private/socket",
            "--env",
            "EXTRA=value",
            "--cli",
            std::move(clients),
            "--no-preflight"};
  }

  static void erase_argument(std::vector<std::string>& arguments,
                             std::string_view value) {
    const auto found = std::ranges::find(arguments, value);
    if (found != arguments.end()) {
      arguments.erase(found);
    }
  }

  static void replace_argument(std::vector<std::string>& arguments,
                               std::string_view before, std::string after) {
    const auto found = std::ranges::find(arguments, before);
    ASSERT_NE(found, arguments.end());
    *found = std::move(after);
  }

  int run(std::vector<std::string> arguments) {
    output_.str({});
    error_.str({});
    return execute(arguments, runtime_, output_, error_);
  }

  std::filesystem::path swap_directory() const {
    return runtime_.paths.state_home / "libtmux-mcp-dev/swap";
  }

  std::filesystem::path state_file() const {
    return swap_directory() / "cxx/state.json";
  }

  std::filesystem::path lock_file() const { return swap_directory() / "state.lock"; }

  Client client_named(std::string_view name) const {
    const auto clients = known_clients(runtime_.paths);
    const auto found = std::ranges::find(clients, name, &Client::name);
    if (found == clients.end()) {
      throw std::runtime_error{"unknown test client"};
    }
    return *found;
  }

  std::vector<std::filesystem::path> backups() const {
    std::vector<std::filesystem::path> result;
    for (const auto& client : known_clients(runtime_.paths)) {
      for (const auto& entry :
           std::filesystem::directory_iterator{client.config_path.parent_path()}) {
        if (entry.path().filename().string().starts_with(
                client.config_path.filename().string() + ".bak.mcp-swap-cxx-")) {
          result.push_back(entry.path());
        }
      }
    }
    return result;
  }

  TemporaryDirectory temporary_;
  std::filesystem::path root_;
  Runtime runtime_;
  std::filesystem::path binary_;
  std::map<std::string, std::pair<std::string, mode_t>, std::less<>> original_;
  std::ostringstream output_;
  std::ostringstream error_;
  std::optional<std::string> preflight_failure_;
  int preflight_calls_{};
};

TEST_F(McpSwapTransactionTest, AllEightClientsRestoreExactBytesModesAndState) {
  ASSERT_EQ(run(use_arguments()), 0) << error_.str();

  const auto clients = known_clients(runtime_.paths);
  for (const auto& client : clients) {
    const auto scope = client.name == "claude" ? Scope::project : Scope::user;
    const auto spec = read_server(client, read_file(client.config_path), "libtmux",
                                  runtime_.paths.working_directory, scope);
    ASSERT_TRUE(spec.has_value()) << client.name;
    EXPECT_EQ(spec->command, std::filesystem::canonical(binary_).string());
    EXPECT_EQ(spec->arguments,
              (std::vector<std::string>{"/tmp/libtmux-private/socket"}));
    EXPECT_EQ(spec->environment,
              (std::vector<std::pair<std::string, std::string>>{{"EXTRA", "value"}}));
  }
  ASSERT_TRUE(std::filesystem::exists(state_file()));
  EXPECT_TRUE(std::filesystem::exists(lock_file()));
  EXPECT_FALSE(std::filesystem::exists(swap_directory() / "state.json"));
  EXPECT_EQ(file_mode(state_file()), 0600);
  const auto saved_backups = backups();
  ASSERT_EQ(saved_backups.size(), 8U);
  for (const auto& backup : saved_backups) {
    EXPECT_EQ(file_mode(backup), 0600);
  }

  ASSERT_EQ(run({"revert", "--cli", "pi,opencode,agy,grok,gemini,cursor,codex,claude"}),
            0)
      << error_.str();

  for (const auto& client : clients) {
    EXPECT_EQ(read_file(client.config_path), original_.at(client.name).first)
        << client.name;
    EXPECT_EQ(file_mode(client.config_path), original_.at(client.name).second)
        << client.name;
  }
  EXPECT_FALSE(std::filesystem::exists(state_file()));
  EXPECT_TRUE(backups().empty());
}

TEST_F(McpSwapTransactionTest, LeavesForeignRecoveryArtifactsUntouched) {
  detail::ensure_private_directory(swap_directory());
  const auto foreign_state = swap_directory() / "state.json";
  const auto cursor = client_named("cursor");
  const auto foreign_backup =
      std::filesystem::path{cursor.config_path.string() + ".bak.mcp-swap-foreign"};
  write_file(foreign_state, "foreign recovery state\n", 0600);
  write_file(foreign_backup, "foreign backup\n", 0600);

  ASSERT_EQ(run(use_arguments("cursor")), 0) << error_.str();
  EXPECT_EQ(read_file(foreign_state), "foreign recovery state\n");
  EXPECT_EQ(read_file(foreign_backup), "foreign backup\n");
  EXPECT_TRUE(std::filesystem::exists(state_file()));
  EXPECT_EQ(backups().size(), 1U);

  ASSERT_EQ(run({"revert", "--cli", "cursor"}), 0) << error_.str();
  EXPECT_EQ(read_file(foreign_state), "foreign recovery state\n");
  EXPECT_EQ(read_file(foreign_backup), "foreign backup\n");
  EXPECT_FALSE(std::filesystem::exists(state_file()));
  EXPECT_TRUE(backups().empty());
}

TEST_F(McpSwapTransactionTest, MalformedLaterConfigLeavesEveryClientUntouched) {
  const auto clients = known_clients(runtime_.paths);
  const auto cursor = *std::ranges::find(clients, "cursor", &Client::name);
  write_file(cursor.config_path, "{not json\n", 0640);
  original_["cursor"] = {read_file(cursor.config_path), 0640};

  EXPECT_EQ(run(use_arguments("claude,cursor")), 1);
  for (const auto& name : {"claude", "cursor"}) {
    const auto client = *std::ranges::find(clients, name, &Client::name);
    EXPECT_EQ(read_file(client.config_path), original_.at(name).first);
  }
  EXPECT_FALSE(std::filesystem::exists(state_file()));
  EXPECT_TRUE(backups().empty());
}

TEST_F(McpSwapTransactionTest, RepeatUseKeepsTheFirstBackup) {
  auto first = use_arguments("cursor");
  ASSERT_EQ(run(first), 0) << error_.str();
  const auto first_backups = backups();
  ASSERT_EQ(first_backups.size(), 1U);
  const auto first_backup_bytes = read_file(first_backups.front());

  auto second = use_arguments("cursor");
  replace_argument(second, "/tmp/libtmux-private/socket",
                   "/tmp/libtmux-private/second");
  ASSERT_EQ(run(second), 0) << error_.str();

  EXPECT_EQ(backups(), first_backups);
  EXPECT_EQ(read_file(first_backups.front()), first_backup_bytes);
  ASSERT_EQ(run({"revert", "--cli", "cursor"}), 0) << error_.str();
  const auto cursor = client_named("cursor");
  EXPECT_EQ(read_file(cursor.config_path), original_.at("cursor").first);
}

TEST_F(McpSwapTransactionTest, ClaudeScopesRevertInPhysicalLifoOrder) {
  ASSERT_EQ(run(use_arguments("claude")), 0) << error_.str();
  auto user = use_arguments("claude");
  user.insert(user.end() - 1, {"--scope", "user"});
  ASSERT_EQ(run(user), 0) << error_.str();
  ASSERT_EQ(backups().size(), 2U);

  ASSERT_EQ(run({"status", "--repo", runtime_.paths.working_directory.string(), "--cli",
                 "claude"}),
            0);
  EXPECT_NE(output_.str().find("[claude:user]"), std::string::npos);
  EXPECT_NE(output_.str().find("[claude:project]"), std::string::npos);
  ASSERT_EQ(run({"status", "--repo", runtime_.paths.working_directory.string(), "--cli",
                 "claude", "--scope", "user"}),
            0);
  EXPECT_NE(output_.str().find("[claude:user]"), std::string::npos);
  EXPECT_EQ(output_.str().find("[claude:project]"), std::string::npos);

  ASSERT_EQ(run({"revert", "--cli", "claude", "--scope", "user"}), 0) << error_.str();
  const auto claude = known_clients(runtime_.paths).front();
  const auto bytes = read_file(claude.config_path);
  EXPECT_FALSE(read_server(claude, bytes, "libtmux", runtime_.paths.working_directory,
                           Scope::user)
                   .has_value());
  EXPECT_TRUE(read_server(claude, bytes, "libtmux", runtime_.paths.working_directory,
                          Scope::project)
                  .has_value());

  ASSERT_EQ(run({"revert", "--cli", "claude", "--scope", "project"}), 0)
      << error_.str();
  EXPECT_EQ(read_file(claude.config_path), original_.at("claude").first);
  EXPECT_TRUE(backups().empty());

  ASSERT_EQ(run(use_arguments("claude")), 0) << error_.str();
  ASSERT_EQ(run(user), 0) << error_.str();
  ASSERT_EQ(run({"revert", "--cli", "claude"}), 0) << error_.str();
  const auto user_output = output_.str().find("[claude:user]");
  const auto project_output = output_.str().find("[claude:project]");
  ASSERT_NE(user_output, std::string::npos);
  ASSERT_NE(project_output, std::string::npos);
  EXPECT_LT(user_output, project_output);
  EXPECT_EQ(read_file(claude.config_path), original_.at("claude").first);
  EXPECT_TRUE(backups().empty());
}

TEST_F(McpSwapTransactionTest, HumanEditBlocksRevertAndKeepsRecovery) {
  ASSERT_EQ(run(use_arguments("cursor")), 0) << error_.str();
  const auto cursor = client_named("cursor");
  const std::string edited = read_file(cursor.config_path) + " \n";
  write_file(cursor.config_path, edited, 0640);

  EXPECT_EQ(run({"revert", "--cli", "cursor"}), 1);
  EXPECT_EQ(read_file(cursor.config_path), edited);
  EXPECT_TRUE(std::filesystem::exists(state_file()));
  EXPECT_EQ(backups().size(), 1U);
}

TEST_F(McpSwapTransactionTest, SameBytesOnAReplacementInodeBlockRevert) {
  ASSERT_EQ(run(use_arguments("cursor")), 0) << error_.str();
  const auto cursor = client_named("cursor");
  const auto swapped = read_file(cursor.config_path);
  const auto prior_inode = detail::capture_binding(cursor.config_path).target;
  const auto replacement = cursor.config_path.string() + ".replacement";
  write_file(replacement, swapped, 0640);
  std::filesystem::rename(replacement, cursor.config_path);

  EXPECT_EQ(run({"revert", "--cli", "cursor"}), 1);
  EXPECT_NE(detail::capture_binding(cursor.config_path).target, prior_inode);
  EXPECT_EQ(read_file(cursor.config_path), swapped);
  EXPECT_TRUE(std::filesystem::exists(state_file()));
  EXPECT_EQ(backups().size(), 1U);
}

TEST_F(McpSwapTransactionTest, ConfigModeChangeBlocksRevert) {
  ASSERT_EQ(run(use_arguments("cursor")), 0) << error_.str();
  const auto cursor = client_named("cursor");
  const auto swapped = read_file(cursor.config_path);
  ASSERT_EQ(::chmod(cursor.config_path.c_str(), 0600), 0) << std::strerror(errno);

  EXPECT_EQ(run({"revert", "--cli", "cursor"}), 1);
  EXPECT_EQ(read_file(cursor.config_path), swapped);
  EXPECT_EQ(file_mode(cursor.config_path), 0600);
  EXPECT_TRUE(std::filesystem::exists(state_file()));
  EXPECT_EQ(backups().size(), 1U);
}

TEST_F(McpSwapTransactionTest, RetargetedConfigSymlinkBlocksRevert) {
  const auto cursor = client_named("cursor");
  const auto old_target = root_ / "cursor-old.json";
  const auto new_target = root_ / "cursor-new.json";
  write_file(old_target, original_.at("cursor").first, 0640);
  write_file(new_target, "{\"mcpServers\":{\"human\":{\"command\":\"keep\"}}}\n", 0640);
  ASSERT_TRUE(std::filesystem::remove(cursor.config_path));
  std::filesystem::create_symlink(old_target, cursor.config_path);
  ASSERT_EQ(run(use_arguments("cursor")), 0) << error_.str();
  const auto swapped = read_file(old_target);

  ASSERT_TRUE(std::filesystem::remove(cursor.config_path));
  std::filesystem::create_symlink(new_target, cursor.config_path);
  EXPECT_EQ(run({"revert", "--cli", "cursor"}), 1);
  EXPECT_EQ(read_file(old_target), swapped);
  EXPECT_EQ(read_file(new_target),
            "{\"mcpServers\":{\"human\":{\"command\":\"keep\"}}}\n");
  EXPECT_TRUE(std::filesystem::exists(state_file()));
  EXPECT_EQ(backups().size(), 1U);
}

TEST_F(McpSwapTransactionTest, BackupEditBlocksRevertAndKeepsConfig) {
  ASSERT_EQ(run(use_arguments("cursor")), 0) << error_.str();
  const auto cursor = client_named("cursor");
  const auto swapped = read_file(cursor.config_path);
  const auto saved = backups();
  ASSERT_EQ(saved.size(), 1U);
  write_file(saved.front(), "untrusted\n", 0600);

  EXPECT_EQ(run({"revert", "--cli", "cursor"}), 1);
  EXPECT_EQ(read_file(cursor.config_path), swapped);
  EXPECT_TRUE(std::filesystem::exists(state_file()));
}

TEST_F(McpSwapTransactionTest, StateChecksumRejectsAnEditedRecord) {
  ASSERT_EQ(run(use_arguments("cursor")), 0) << error_.str();
  auto state = read_file(state_file());
  const auto version = state.find("\"version\": 2");
  ASSERT_NE(version, std::string::npos);
  state[version + std::string{"\"version\": "}.size()] = '3';
  write_file(state_file(), state, 0600);

  EXPECT_EQ(run({"revert", "--cli", "cursor"}), 1);
  EXPECT_TRUE(std::filesystem::exists(state_file()));
  EXPECT_EQ(backups().size(), 1U);
}

TEST_F(McpSwapTransactionTest, PendingStateRejectsUnknownChecksummedFields) {
  const auto claude = client_named("claude");
  const auto cursor = client_named("cursor");
  runtime_.boundary = [&](std::string_view event, const std::filesystem::path& path) {
    if (event == "before-revert-config-write" && path == claude.config_path) {
      write_file(cursor.config_path, read_file(cursor.config_path) + " \n", 0640);
      throw Error{"leave one pending revert"};
    }
  };
  ASSERT_EQ(run(use_arguments("claude,cursor")), 0) << error_.str();
  ASSERT_EQ(run({"revert", "--cli", "claude,cursor"}), 1);
  runtime_.boundary = [](std::string_view, const std::filesystem::path&) {};

  auto document = nlohmann::json::parse(read_file(state_file()));
  document["transaction"]["unexpected"] = true;
  auto payload = document;
  payload.erase("checksum");
  document["checksum"] = detail::sha256(payload.dump());
  write_file(state_file(), document.dump(2) + "\n", 0600);

  EXPECT_EQ(run({"doctor", "--repo", runtime_.paths.working_directory.string()}), 1);
}

TEST_F(McpSwapTransactionTest, ExistingLockMustAlreadyBePrivate) {
  const auto directory = swap_directory();
  const auto lock = lock_file();
  write_file(lock, "", 0644);
  ASSERT_EQ(::chmod(directory.c_str(), 0700), 0) << std::strerror(errno);

  EXPECT_EQ(run(use_arguments("cursor")), 1);
  EXPECT_EQ(file_mode(lock), 0644);
  EXPECT_EQ(read_file(client_named("cursor").config_path),
            original_.at("cursor").first);
  EXPECT_FALSE(std::filesystem::exists(state_file()));
  EXPECT_TRUE(backups().empty());
}

TEST_F(McpSwapTransactionTest, HeldLockSerializesAConcurrentTransaction) {
  const auto directory = runtime_.paths.state_home / "libtmux-mcp-dev/swap";
  std::array<int, 2> ready{};
  std::array<int, 2> release{};
  std::array<int, 2> acquired{};
  ASSERT_EQ(::pipe(ready.data()), 0) << std::strerror(errno);
  ASSERT_EQ(::pipe(release.data()), 0) << std::strerror(errno);
  ASSERT_EQ(::pipe(acquired.data()), 0) << std::strerror(errno);
  const auto holder = ::fork();
  ASSERT_GE(holder, 0) << std::strerror(errno);
  if (holder == 0) {
    static_cast<void>(::close(ready[0]));
    static_cast<void>(::close(release[1]));
    static_cast<void>(::close(acquired[0]));
    static_cast<void>(::close(acquired[1]));
    try {
      detail::TransactionLock held{directory};
      static_cast<void>(::write(ready[1], "1", 1));
      char byte{};
      static_cast<void>(::read(release[0], &byte, 1));
      ::_exit(0);
    } catch (...) {
      ::_exit(1);
    }
  }
  ScopedChild holder_guard{holder};
  static_cast<void>(::close(ready[1]));
  static_cast<void>(::close(release[0]));
  pollfd signal{ready[0], POLLIN, 0};
  ASSERT_EQ(::poll(&signal, 1, 2000), 1) << std::strerror(errno);
  char byte{};
  ASSERT_EQ(::read(ready[0], &byte, 1), 1) << std::strerror(errno);
  static_cast<void>(::close(ready[0]));

  const auto contender = ::fork();
  ASSERT_GE(contender, 0) << std::strerror(errno);
  if (contender == 0) {
    try {
      detail::TransactionLock waiting{directory};
      static_cast<void>(::write(acquired[1], "1", 1));
      ::_exit(0);
    } catch (...) {
      static_cast<void>(::write(acquired[1], "E", 1));
      ::_exit(1);
    }
  }
  ScopedChild contender_guard{contender};
  static_cast<void>(::close(acquired[1]));
  pollfd completion{acquired[0], POLLIN, 0};
  const auto before_release = ::poll(&completion, 1, 150);
  ASSERT_EQ(::write(release[1], "1", 1), 1) << std::strerror(errno);
  static_cast<void>(::close(release[1]));
  EXPECT_EQ(before_release, 0);
  ASSERT_EQ(::poll(&completion, 1, 2000), 1) << std::strerror(errno);
  ASSERT_EQ(::read(acquired[0], &byte, 1), 1) << std::strerror(errno);
  EXPECT_EQ(byte, '1');
  static_cast<void>(::close(acquired[0]));
}

TEST_F(McpSwapTransactionTest, PythonRecordLockSerializesNativeAcquisition) {
  detail::ensure_private_directory(swap_directory());
  write_file(lock_file(), "", 0600);
  std::array<int, 2> ready{};
  std::array<int, 2> release{};
  std::array<int, 2> acquired{};
  ASSERT_EQ(::pipe(ready.data()), 0) << std::strerror(errno);
  ASSERT_EQ(::pipe(release.data()), 0) << std::strerror(errno);
  ASSERT_EQ(::pipe(acquired.data()), 0) << std::strerror(errno);
  const auto holder = ::fork();
  ASSERT_GE(holder, 0) << std::strerror(errno);
  if (holder == 0) {
    static_cast<void>(::close(ready[0]));
    static_cast<void>(::close(release[1]));
    static_cast<void>(::close(acquired[0]));
    static_cast<void>(::close(acquired[1]));
    static_cast<void>(::dup2(ready[1], STDOUT_FILENO));
    static_cast<void>(::close(ready[1]));
    const std::string program = "import fcntl,os,sys\n"
                                "f=open(sys.argv[1],'r+')\n"
                                "fcntl.lockf(f,fcntl.LOCK_EX)\n"
                                "os.write(1,b'1')\n"
                                "os.read(int(sys.argv[2]),1)\n";
    const auto release_descriptor = std::to_string(release[0]);
    ::execlp("python3", "python3", "-c", program.c_str(), lock_file().c_str(),
             release_descriptor.c_str(), nullptr);
    ::_exit(127);
  }
  ScopedChild holder_guard{holder};
  static_cast<void>(::close(ready[1]));
  static_cast<void>(::close(release[0]));
  pollfd signal{ready[0], POLLIN, 0};
  ASSERT_EQ(::poll(&signal, 1, 2000), 1) << std::strerror(errno);
  char byte{};
  ASSERT_EQ(::read(ready[0], &byte, 1), 1) << std::strerror(errno);
  static_cast<void>(::close(ready[0]));

  const auto contender = ::fork();
  ASSERT_GE(contender, 0) << std::strerror(errno);
  if (contender == 0) {
    try {
      detail::TransactionLock waiting{swap_directory()};
      static_cast<void>(::write(acquired[1], "1", 1));
      ::_exit(0);
    } catch (...) {
      static_cast<void>(::write(acquired[1], "E", 1));
      ::_exit(1);
    }
  }
  ScopedChild contender_guard{contender};
  static_cast<void>(::close(acquired[1]));
  pollfd completion{acquired[0], POLLIN, 0};
  const auto before_release = ::poll(&completion, 1, 150);
  ASSERT_EQ(::write(release[1], "1", 1), 1) << std::strerror(errno);
  static_cast<void>(::close(release[1]));
  EXPECT_EQ(before_release, 0);
  ASSERT_EQ(::poll(&completion, 1, 2000), 1) << std::strerror(errno);
  ASSERT_EQ(::read(acquired[0], &byte, 1), 1) << std::strerror(errno);
  EXPECT_EQ(byte, '1');
  static_cast<void>(::close(acquired[0]));
}

TEST_F(McpSwapTransactionTest, PostAcquireChecksKeepTheRecordLockHeld) {
  detail::TransactionLock held{swap_directory()};
  const auto expect_contender_blocked = [&] {
    EXPECT_EQ(python_nonblocking_record_lock(held.path()), 0);
  };

  expect_contender_blocked();
  held.validate();
  expect_contender_blocked();
  static_cast<void>(held.path());
  expect_contender_blocked();
  static_cast<void>(held.binding());
  expect_contender_blocked();
}

TEST_F(McpSwapTransactionTest, ConfigAliasCannotReleaseLockDuringAuthenticatedRead) {
  detail::TransactionLock held{swap_directory()};
  const auto config = client_named("cursor").config_path;
  ASSERT_TRUE(std::filesystem::remove(config));
  std::filesystem::create_hard_link(held.path(), config);

  EXPECT_THROW(static_cast<void>(detail::read_bound(config, &held)), Error);
  EXPECT_EQ(python_nonblocking_record_lock(held.path()), 0);
}

TEST_F(McpSwapTransactionTest, PreflightCompletesBeforeWaitingForTheLock) {
  const auto directory = swap_directory();
  std::array<int, 2> ready{};
  std::array<int, 2> release{};
  std::array<int, 2> preflight{};
  ASSERT_EQ(::pipe(ready.data()), 0) << std::strerror(errno);
  ASSERT_EQ(::pipe(release.data()), 0) << std::strerror(errno);
  ASSERT_EQ(::pipe(preflight.data()), 0) << std::strerror(errno);
  const auto holder = ::fork();
  ASSERT_GE(holder, 0) << std::strerror(errno);
  if (holder == 0) {
    static_cast<void>(::close(ready[0]));
    static_cast<void>(::close(release[1]));
    static_cast<void>(::close(preflight[0]));
    static_cast<void>(::close(preflight[1]));
    try {
      detail::TransactionLock held{directory};
      static_cast<void>(::write(ready[1], "1", 1));
      char byte{};
      static_cast<void>(::read(release[0], &byte, 1));
      ::_exit(0);
    } catch (...) {
      ::_exit(1);
    }
  }
  ScopedChild holder_guard{holder};
  static_cast<void>(::close(ready[1]));
  static_cast<void>(::close(release[0]));
  pollfd signal{ready[0], POLLIN, 0};
  ASSERT_EQ(::poll(&signal, 1, 2000), 1) << std::strerror(errno);
  char byte{};
  ASSERT_EQ(::read(ready[0], &byte, 1), 1) << std::strerror(errno);
  static_cast<void>(::close(ready[0]));

  bool observed_before_timeout = false;
  std::thread releaser{[&] {
    pollfd notice{preflight[0], POLLIN, 0};
    observed_before_timeout = ::poll(&notice, 1, 500) == 1;
    static_cast<void>(::write(release[1], "1", 1));
  }};
  runtime_.preflight = [&](const ServerSpec&) {
    ++preflight_calls_;
    static_cast<void>(::write(preflight[1], "1", 1));
    return std::optional<std::string>{};
  };
  auto arguments = use_arguments("cursor");
  erase_argument(arguments, "--no-preflight");
  const auto result = run(arguments);
  releaser.join();
  static_cast<void>(::close(release[1]));
  static_cast<void>(::close(preflight[0]));
  static_cast<void>(::close(preflight[1]));

  EXPECT_TRUE(observed_before_timeout);
  EXPECT_EQ(result, 0) << error_.str();
  EXPECT_EQ(preflight_calls_, 1);
}

TEST_F(McpSwapTransactionTest, StateBoundaryRechecksEveryUnselectedRoute) {
  const auto cursor = client_named("cursor");
  const auto gemini = client_named("gemini");
  const auto external = original_.at("gemini").first + " \n";
  bool changed = false;
  bool state_was_published = false;
  runtime_.boundary = [&](std::string_view event, const std::filesystem::path&) {
    if (event == "before-state-write" && !changed) {
      write_file(gemini.config_path, external, 0640);
      changed = true;
    } else if (event == "before-backup-write" &&
               std::filesystem::exists(state_file())) {
      state_was_published = true;
    }
  };

  EXPECT_EQ(run(use_arguments("cursor")), 1);
  EXPECT_EQ(read_file(cursor.config_path), original_.at("cursor").first);
  EXPECT_EQ(read_file(gemini.config_path), external);
  EXPECT_FALSE(state_was_published);
  EXPECT_FALSE(std::filesystem::exists(state_file()));
  EXPECT_TRUE(backups().empty());
}

TEST_F(McpSwapTransactionTest, ClaudeProjectUsesTheMainWorktreeKey) {
  const auto main = root_ / "main";
  const auto git_directory = main / ".git/worktrees/topic";
  std::filesystem::create_directories(git_directory);
  write_file(runtime_.paths.working_directory / ".git",
             "gitdir: " + git_directory.string() + "\n");

  ASSERT_EQ(run(use_arguments("claude")), 0) << error_.str();
  const auto contents = read_file(client_named("claude").config_path);
  const auto document = nlohmann::json::parse(contents);

  ASSERT_TRUE(document["projects"].is_object());
  EXPECT_TRUE(document["projects"].contains(std::filesystem::canonical(main).string()));
  EXPECT_FALSE(document["projects"].contains(
      std::filesystem::canonical(runtime_.paths.working_directory).string()));
}

TEST_F(McpSwapTransactionTest, SelectedConfigCannotAliasAnUnselectedClient) {
  const auto clients = known_clients(runtime_.paths);
  const auto cursor = *std::ranges::find(clients, "cursor", &Client::name);
  const auto gemini = *std::ranges::find(clients, "gemini", &Client::name);
  std::filesystem::remove(cursor.config_path);
  std::filesystem::create_symlink(gemini.config_path, cursor.config_path);
  const auto before = read_file(gemini.config_path);

  EXPECT_EQ(run(use_arguments("cursor")), 1);
  EXPECT_EQ(read_file(gemini.config_path), before);
  EXPECT_TRUE(std::filesystem::is_symlink(cursor.config_path));
  EXPECT_FALSE(std::filesystem::exists(state_file()));
  EXPECT_TRUE(backups().empty());
}

TEST_F(McpSwapTransactionTest, NewConfigCannotAliasAnExistingRecoveryBackup) {
  ASSERT_EQ(run(use_arguments("cursor")), 0) << error_.str();
  const auto cursor = client_named("cursor");
  const auto gemini = client_named("gemini");
  const auto saved = backups();
  ASSERT_EQ(saved.size(), 1U);
  const auto backup_bytes = read_file(saved.front());
  const auto swapped_cursor = read_file(cursor.config_path);
  std::filesystem::remove(gemini.config_path);
  std::filesystem::create_symlink(saved.front(), gemini.config_path);

  EXPECT_EQ(run(use_arguments("gemini")), 1);
  EXPECT_EQ(read_file(saved.front()), backup_bytes);
  EXPECT_EQ(read_file(cursor.config_path), swapped_cursor);

  std::filesystem::remove(gemini.config_path);
  write_file(gemini.config_path, original_.at("gemini").first,
             original_.at("gemini").second);
  ASSERT_EQ(run({"revert", "--cli", "cursor"}), 0) << error_.str();
  EXPECT_EQ(read_file(cursor.config_path), original_.at("cursor").first);
}

TEST_F(McpSwapTransactionTest, SymlinkedConfigDirectoryRestoresExactly) {
  const auto cursor = client_named("cursor");
  const auto linked = root_ / "linked-cursor";
  std::filesystem::rename(cursor.config_path.parent_path(), linked);
  std::filesystem::create_directory_symlink(linked, cursor.config_path.parent_path());

  ASSERT_EQ(run(use_arguments("cursor")), 0) << error_.str();
  ASSERT_EQ(run({"revert", "--cli", "cursor"}), 0) << error_.str();

  EXPECT_TRUE(std::filesystem::is_symlink(cursor.config_path.parent_path()));
  EXPECT_EQ(read_file(cursor.config_path), original_.at("cursor").first);
  EXPECT_FALSE(std::filesystem::exists(state_file()));
  EXPECT_TRUE(backups().empty());
}

TEST_F(McpSwapTransactionTest, LateBackupClaimDoesNotChooseAnotherPath) {
  bool claimed = false;
  runtime_.boundary = [&](std::string_view event, const std::filesystem::path& path) {
    if (!claimed && event == "before-backup-write") {
      write_file(path, "attacker-owned\n", 0600);
      claimed = true;
    }
  };

  EXPECT_EQ(run(use_arguments("cursor")), 1);
  ASSERT_TRUE(claimed);
  const auto saved = backups();
  ASSERT_EQ(saved.size(), 1U);
  EXPECT_EQ(read_file(saved.front()), "attacker-owned\n");
  EXPECT_FALSE(std::filesystem::exists(state_file()));
  const auto cursor = client_named("cursor");
  EXPECT_EQ(read_file(cursor.config_path), original_.at("cursor").first);
}

TEST_F(McpSwapTransactionTest, LateWriteFailureRollsBackEarlierConfigs) {
  const auto clients = known_clients(runtime_.paths);
  const auto cursor = *std::ranges::find(clients, "cursor", &Client::name);
  runtime_.boundary = [&](std::string_view event, const std::filesystem::path& path) {
    if (event == "before-config-write" && path == cursor.config_path) {
      throw Error{"injected late write failure"};
    }
  };

  EXPECT_EQ(run(use_arguments("claude,cursor")), 1);
  for (const auto& name : {"claude", "cursor"}) {
    const auto client = *std::ranges::find(clients, name, &Client::name);
    EXPECT_EQ(read_file(client.config_path), original_.at(name).first);
  }
  EXPECT_FALSE(std::filesystem::exists(state_file()));
  EXPECT_TRUE(backups().empty());
}

TEST_F(McpSwapTransactionTest, InterveningEditLeavesFailClosedRecovery) {
  const auto clients = known_clients(runtime_.paths);
  const auto claude = *std::ranges::find(clients, "claude", &Client::name);
  const auto cursor = *std::ranges::find(clients, "cursor", &Client::name);
  std::string intervening;
  runtime_.boundary = [&](std::string_view event, const std::filesystem::path& path) {
    if (event == "before-config-write" && path == cursor.config_path) {
      intervening = read_file(claude.config_path) + " \n";
      write_file(claude.config_path, intervening, 0640);
      throw Error{"injected write failure after external edit"};
    }
  };

  EXPECT_EQ(run(use_arguments("claude,cursor")), 1);
  EXPECT_EQ(read_file(claude.config_path), intervening);
  EXPECT_TRUE(std::filesystem::exists(state_file()));
  EXPECT_EQ(backups().size(), 2U);
}

TEST_F(McpSwapTransactionTest, UnselectedRouteIsRecheckedBeforePublish) {
  const auto clients = known_clients(runtime_.paths);
  const auto cursor = *std::ranges::find(clients, "cursor", &Client::name);
  const auto gemini = *std::ranges::find(clients, "gemini", &Client::name);
  const std::string external = original_.at("gemini").first + " \n";
  runtime_.boundary = [&](std::string_view event, const std::filesystem::path&) {
    if (event == "before-config-write") {
      write_file(gemini.config_path, external, 0640);
    }
  };

  EXPECT_EQ(run(use_arguments("cursor")), 1);
  EXPECT_EQ(read_file(cursor.config_path), original_.at("cursor").first);
  EXPECT_EQ(read_file(gemini.config_path), external);
  EXPECT_FALSE(std::filesystem::exists(state_file()));
}

TEST_F(McpSwapTransactionTest, DryRunPlansEverythingWithoutPreflightOrWrites) {
  auto arguments = use_arguments("claude,cursor");
  erase_argument(arguments, "--no-preflight");
  arguments.push_back("--dry-run");

  EXPECT_EQ(run(arguments), 0) << error_.str();
  EXPECT_EQ(preflight_calls_, 0);
  EXPECT_NE(output_.str().find("@@ -1,"), std::string::npos);
  EXPECT_NE(output_.str().find("-{"), std::string::npos);
  EXPECT_NE(output_.str().find("+{"), std::string::npos);
  for (const auto& name : {"claude", "cursor"}) {
    const auto client = client_named(name);
    EXPECT_EQ(read_file(client.config_path), original_.at(name).first);
  }
  EXPECT_FALSE(std::filesystem::exists(state_file()));
  EXPECT_TRUE(backups().empty());
}

TEST_F(McpSwapTransactionTest, DetectExplainsTheOptionalPiAdapter) {
  EXPECT_EQ(run({"detect"}), 0);
  EXPECT_NE(output_.str().find(
                "needs the pi-mcp-adapter package; pi has no built-in MCP client"),
            std::string::npos);

  std::filesystem::create_directories(runtime_.paths.home /
                                      ".pi/agent/npm/node_modules/pi-mcp-adapter");
  EXPECT_EQ(run({"detect"}), 0);
  EXPECT_EQ(output_.str().find("needs the pi-mcp-adapter package"), std::string::npos);
}

TEST_F(McpSwapTransactionTest, StatusClassifiesTheConfiguredServer) {
  EXPECT_EQ(run(use_arguments("cursor")), 0) << error_.str();

  EXPECT_EQ(run({"status", "--repo", runtime_.paths.working_directory.string(), "--cli",
                 "cursor"}),
            0);
  EXPECT_NE(output_.str().find("(local build: build/native/apps/mcp/"
                               "libtmux-mcp-server)"),
            std::string::npos);
}

TEST_F(McpSwapTransactionTest, DoctorReportsNamingAndOrphanedRecoveryHazards) {
  const auto cursor = client_named("cursor");
  const auto current = read_file(cursor.config_path);
  const auto rendered =
      render_server(cursor, current, "tmux", ServerSpec{binary_.string(), {}, {}},
                    runtime_.paths.working_directory, Scope::user);
  write_file(cursor.config_path, rendered.bytes, 0640);
  write_file(cursor.config_path.string() + ".bak.mcp-swap-cxx-orphan", "old", 0600);

  EXPECT_EQ(run({"doctor", "--repo", runtime_.paths.working_directory.string()}), 0)
      << error_.str();
  EXPECT_NE(output_.str().find("[cursor] tmux = local: this repo  (other name)"),
            std::string::npos);
  EXPECT_NE(output_.str().find("server name mismatch"), std::string::npos);
  EXPECT_NE(output_.str().find("orphaned backups: 1 file(s), 3 bytes"),
            std::string::npos);
}

TEST_F(McpSwapTransactionTest, FailedPreflightLeavesEveryConfigUntouched) {
  auto arguments = use_arguments("claude,cursor");
  erase_argument(arguments, "--no-preflight");
  preflight_failure_ = "synthetic lifecycle failure";

  EXPECT_EQ(run(arguments), 1);
  EXPECT_EQ(preflight_calls_, 1);
  for (const auto& name : {"claude", "cursor"}) {
    const auto client = client_named(name);
    EXPECT_EQ(read_file(client.config_path), original_.at(name).first);
  }
  EXPECT_FALSE(std::filesystem::exists(state_file()));
  EXPECT_TRUE(backups().empty());
}

TEST_F(McpSwapTransactionTest, PreflightsEveryFinalMergedEnvironmentBeforeWrites) {
  const auto cursor = client_named("cursor");
  const auto gemini = client_named("gemini");
  for (const auto& [client, name] : std::array<std::pair<Client, std::string>, 2>{
           {{cursor, "CURSOR_ONLY"}, {gemini, "GEMINI_ONLY"}}}) {
    const auto seeded = render_server(client, read_file(client.config_path), "libtmux",
                                      ServerSpec{"old-server", {}, {{name, "present"}}},
                                      runtime_.paths.working_directory, Scope::user);
    write_file(client.config_path, seeded.bytes, 0640);
    original_[client.name] = {seeded.bytes, 0640};
  }
  std::vector<ServerSpec> inspected;
  runtime_.preflight = [&](const ServerSpec& spec) {
    inspected.push_back(spec);
    const auto has_gemini =
        std::ranges::find(spec.environment,
                          std::pair<std::string, std::string>{
                              "GEMINI_ONLY", "present"}) != spec.environment.end();
    return has_gemini ? std::optional<std::string>{"reject later environment"}
                      : std::nullopt;
  };
  auto arguments = use_arguments("cursor,gemini");
  erase_argument(arguments, "--no-preflight");

  EXPECT_EQ(run(arguments), 1);
  ASSERT_EQ(inspected.size(), 2U);
  EXPECT_NE(
      std::ranges::find(inspected[0].environment,
                        std::pair<std::string, std::string>{"CURSOR_ONLY", "present"}),
      inspected[0].environment.end());
  EXPECT_NE(
      std::ranges::find(inspected[1].environment,
                        std::pair<std::string, std::string>{"GEMINI_ONLY", "present"}),
      inspected[1].environment.end());
  EXPECT_EQ(read_file(cursor.config_path), original_.at("cursor").first);
  EXPECT_EQ(read_file(gemini.config_path), original_.at("gemini").first);
  EXPECT_FALSE(std::filesystem::exists(state_file()));
  EXPECT_TRUE(backups().empty());
}

TEST_F(McpSwapTransactionTest, LateRevertFailureRollsEarlierConfigsForward) {
  ASSERT_EQ(run(use_arguments("claude,cursor")), 0) << error_.str();
  const auto claude = client_named("claude");
  const auto cursor = client_named("cursor");
  const auto swapped_claude = read_file(claude.config_path);
  const auto swapped_cursor = read_file(cursor.config_path);
  runtime_.boundary = [&](std::string_view event, const std::filesystem::path& path) {
    if (event == "before-revert-config-write" && path == claude.config_path) {
      throw Error{"injected late revert failure"};
    }
  };

  EXPECT_EQ(run({"revert", "--cli", "claude,cursor"}), 1);
  EXPECT_EQ(read_file(claude.config_path), swapped_claude);
  EXPECT_EQ(read_file(cursor.config_path), swapped_cursor);
  EXPECT_TRUE(std::filesystem::exists(state_file()));
  EXPECT_EQ(backups().size(), 2U);

  runtime_.boundary = [](std::string_view, const std::filesystem::path&) {};
  ASSERT_EQ(run({"revert", "--cli", "claude,cursor"}), 0) << error_.str();
  EXPECT_EQ(read_file(claude.config_path), original_.at("claude").first);
  EXPECT_EQ(read_file(cursor.config_path), original_.at("cursor").first);
}

TEST_F(McpSwapTransactionTest, CleanupFailureRecreatesRecoveryAndRollsForward) {
  ASSERT_EQ(run(use_arguments("claude,cursor")), 0) << error_.str();
  const auto claude = client_named("claude");
  const auto cursor = client_named("cursor");
  const auto swapped_claude = read_file(claude.config_path);
  const auto swapped_cursor = read_file(cursor.config_path);
  int removals = 0;
  runtime_.boundary = [&](std::string_view event, const std::filesystem::path&) {
    if (event == "before-backup-remove" && ++removals == 2) {
      throw Error{"injected cleanup failure"};
    }
  };

  EXPECT_EQ(run({"revert", "--cli", "claude,cursor"}), 1);
  EXPECT_EQ(read_file(claude.config_path), swapped_claude);
  EXPECT_EQ(read_file(cursor.config_path), swapped_cursor);
  EXPECT_TRUE(std::filesystem::exists(state_file()));
  EXPECT_EQ(backups().size(), 2U);

  runtime_.boundary = [](std::string_view, const std::filesystem::path&) {};
  ASSERT_EQ(run({"revert", "--cli", "claude,cursor"}), 0) << error_.str();
  EXPECT_EQ(read_file(claude.config_path), original_.at("claude").first);
  EXPECT_EQ(read_file(cursor.config_path), original_.at("cursor").first);
}

TEST_F(McpSwapTransactionTest, FailedRepeatUseRestoresAUsablePriorRecovery) {
  ASSERT_EQ(run(use_arguments("cursor")), 0) << error_.str();
  const auto cursor = client_named("cursor");
  const auto gemini = client_named("gemini");
  const auto first_swap = read_file(cursor.config_path);
  auto repeat = use_arguments("cursor,gemini");
  replace_argument(repeat, "/tmp/libtmux-private/socket",
                   "/tmp/libtmux-private/second");
  runtime_.boundary = [&](std::string_view event, const std::filesystem::path& path) {
    if (event == "before-config-write" && path == gemini.config_path) {
      throw Error{"injected failure after repeated layer"};
    }
  };

  EXPECT_EQ(run(repeat), 1);
  EXPECT_EQ(read_file(cursor.config_path), first_swap);
  EXPECT_EQ(read_file(gemini.config_path), original_.at("gemini").first);
  runtime_.boundary = [](std::string_view, const std::filesystem::path&) {};

  ASSERT_EQ(run({"revert", "--cli", "cursor"}), 0) << error_.str();
  EXPECT_EQ(read_file(cursor.config_path), original_.at("cursor").first);
}

TEST_F(McpSwapTransactionTest, EditDuringFailedRevertLeavesPendingEvidence) {
  ASSERT_EQ(run(use_arguments("claude,cursor")), 0) << error_.str();
  const auto claude = client_named("claude");
  const auto cursor = client_named("cursor");
  std::string intervening;
  runtime_.boundary = [&](std::string_view event, const std::filesystem::path& path) {
    if (event == "before-revert-config-write" && path == claude.config_path) {
      intervening = read_file(cursor.config_path) + " \n";
      write_file(cursor.config_path, intervening, 0640);
      throw Error{"injected external edit during revert"};
    }
  };

  EXPECT_EQ(run({"revert", "--cli", "claude,cursor"}), 1);
  EXPECT_EQ(read_file(cursor.config_path), intervening);
  EXPECT_NE(read_file(state_file()).find("\"kind\": \"revert\""), std::string::npos);
  EXPECT_EQ(backups().size(), 2U);
}

ServerSpec fake_server(std::string mode) {
  return ServerSpec{MCP_SWAP_FAKE_SERVER_PATH, {std::move(mode)}, {}};
}

TEST(McpSwapPreflight, AcceptsCompleteLifecycleAndCatalog) {
  EXPECT_EQ(preflight_spec(fake_server("ok"), std::chrono::seconds{2}), std::nullopt);
}

TEST(McpSwapPreflight, AcceptsAResponsiveLongLivedServerAndReapsItsGroup) {
  TemporaryDirectory temporary;
  const auto heartbeat = temporary.path() / "heartbeat";
  const auto pid_file = temporary.path() / "pid";
  auto spec = fake_server("stay-alive-tree");
  spec.environment = {{"MCP_SWAP_FAKE_HEARTBEAT", heartbeat.string()},
                      {"MCP_SWAP_FAKE_PID", pid_file.string()}};

  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(preflight_spec(spec, std::chrono::milliseconds{500}), std::nullopt);
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds{2});

  const auto pid_text = read_file(pid_file);
  pid_t pid{};
  const auto parsed =
      std::from_chars(pid_text.data(), pid_text.data() + pid_text.size(), pid);
  ASSERT_EQ(parsed.ec, std::errc{});
  ASSERT_EQ(parsed.ptr, pid_text.data() + pid_text.size());
  errno = 0;
  EXPECT_EQ(::kill(pid, 0), -1);
  EXPECT_EQ(errno, ESRCH);

  const auto before = std::filesystem::file_size(heartbeat);
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  EXPECT_EQ(std::filesystem::file_size(heartbeat), before);
}

TEST(McpSwapPreflight, RejectsWrongVersionIdentityAndMissingTools) {
  EXPECT_EQ(preflight_spec(fake_server("wrong-version"), std::chrono::seconds{2}),
            "server did not echo the requested MCP protocol version");
  EXPECT_EQ(preflight_spec(fake_server("wrong-server"), std::chrono::seconds{2}),
            "initialize response did not identify a libtmux MCP server");
  EXPECT_EQ(preflight_spec(fake_server("missing-tools"), std::chrono::seconds{2}),
            "server initialized but did not answer tools/list");
  EXPECT_EQ(preflight_spec(fake_server("empty-tools"), std::chrono::seconds{2}),
            "server tool catalog is missing: get_server_info, list_panes, "
            "list_sessions, list_windows");
}

TEST(McpSwapPreflight, RejectsDuplicateDirtyAndCrashingTransports) {
  EXPECT_EQ(preflight_spec(fake_server("duplicate-reply"), std::chrono::seconds{2}),
            "server answered an MCP preflight request more than once");
  EXPECT_EQ(preflight_spec(fake_server("garbage-stdout"), std::chrono::seconds{2}),
            "server wrote non-JSON data to the MCP stdout transport");
  EXPECT_EQ(preflight_spec(fake_server("crash-after-reply"), std::chrono::seconds{2}),
            "synthetic crash");
}

TEST(McpSwapPreflight, PassesConfiguredEnvironmentAndBoundsTimeout) {
  auto spec = fake_server("require-env");
  spec.environment.emplace_back("MCP_SWAP_FAKE_ENV", "present");
  EXPECT_EQ(preflight_spec(spec, std::chrono::seconds{2}), std::nullopt);

  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(preflight_spec(fake_server("timeout"), std::chrono::milliseconds{100}),
            "no MCP response within 0.1s");
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds{2});
}

TEST(McpSwapPreflight, TimeoutStopsTheInheritedProcessGroup) {
  TemporaryDirectory temporary;
  const auto heartbeat = temporary.path() / "heartbeat";
  auto spec = fake_server("timeout-tree");
  spec.environment = {{"MCP_SWAP_FAKE_HEARTBEAT", heartbeat.string()}};

  EXPECT_EQ(preflight_spec(spec, std::chrono::milliseconds{100}),
            "no MCP response within 0.1s");
  const auto before = std::filesystem::file_size(heartbeat);
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  EXPECT_EQ(std::filesystem::file_size(heartbeat), before);
}

TEST(McpSwapPreflight, BoundsOutputAndReturnsStderrDiagnostics) {
  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(preflight_spec(fake_server("oversize"), std::chrono::seconds{2}),
            "server output exceeded the MCP preflight limit");
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds{2});
  EXPECT_EQ(preflight_spec(fake_server("stderr-failure"), std::chrono::seconds{2}),
            "synthetic preflight stderr");
}

TEST(McpSwapPreflight, OversizeStopsLongLivedStdoutAndStderrProcessTrees) {
  for (const auto mode : {"oversize-tree-stdout", "oversize-tree-stderr"}) {
    TemporaryDirectory temporary;
    const auto heartbeat = temporary.path() / "heartbeat";
    auto spec = fake_server(mode);
    spec.environment = {{"MCP_SWAP_FAKE_HEARTBEAT", heartbeat.string()}};

    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(preflight_spec(spec, std::chrono::seconds{2}),
              "server output exceeded the MCP preflight limit");
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds{2});
    const auto before = std::filesystem::file_size(heartbeat);
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    EXPECT_EQ(std::filesystem::file_size(heartbeat), before) << mode;
  }
}

} // namespace
} // namespace libtmux::mcp_swap
