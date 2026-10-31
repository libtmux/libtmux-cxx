// The environment ctest gives every test, checked from inside one.
//
// This is the control for cmake/LibtmuxTestSandbox.cmake, so the binary links
// the plain GoogleTest main: the shared one refuses to start in the very
// environment these cases exist to name. `SandboxCanary` asserts what the
// harness left behind; `SandboxGuard` asserts the check the shared main makes.
// CMake runs each canary case a second time in an environment a tmux pane
// would have, and requires it to fail.

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
// mkdtemp is POSIX and glibc declares it in <stdlib.h>; <cstdlib> promises
// only the std:: names.
#include <stdlib.h>

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include "libtmux/server.hpp"
#include "libtmux/testing/environment_guard.hpp"
#include "support/sandbox.hpp"

namespace {

namespace fs = std::filesystem;
using libtmux::test_support::sandbox_violations;
using libtmux::test_support::ScopedUnset;

// The directory the build tree gave ctest as TMUX_TMPDIR.
const fs::path& sandbox_root() {
  static const fs::path root{LIBTMUX_TEST_SANDBOX_ROOT};
  return root;
}

std::optional<std::string> variable(const char* name) {
  if (const char* const value = std::getenv(name); value != nullptr) {
    return std::string{value};
  }
  return std::nullopt;
}

// The sockets in a tree that a server answers on, relative to it. Links are not
// followed, and a socket file nothing answers on is not listed.
std::vector<std::string> answering_sockets(const fs::path& tree) {
  const ScopedUnset no_tmux{"TMUX"};
  std::vector<std::string> names;
  std::error_code failed;
  for (fs::recursive_directory_iterator entry{tree, failed}, end;
       !failed && entry != end; entry.increment(failed)) {
    if (fs::symlink_status(entry->path(), failed).type() != fs::file_type::socket) {
      continue;
    }
    const auto server = libtmux::Server::at_socket_path(entry->path().string());
    if (server.has_value() && server->is_alive(std::chrono::seconds{2})) {
      names.push_back(entry->path().lexically_relative(tree).string());
    }
  }
  return names;
}

// A scratch directory under the sandbox, removed with the scope. Mode 0700
// unless the case needs otherwise. A directory that holds a socket a server
// still answers on is kept and reported: once its path is gone nothing can tell
// that server to exit.
class ScratchDirectory final {
public:
  ScratchDirectory() {
    std::string pattern = (sandbox_root() / "canary-XXXXXX").string();
    if (::mkdtemp(pattern.data()) != nullptr) {
      path_ = pattern;
    }
  }
  ~ScratchDirectory() {
    if (path_.empty()) {
      return;
    }
    const auto answering = answering_sockets(path_);
    if (!answering.empty()) {
      std::string names;
      for (const auto& name : answering) {
        names += (names.empty() ? "" : ", ") + name;
      }
      ADD_FAILURE() << "kept a scratch directory a server still answers in: " << names;
      return;
    }
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  ScratchDirectory(const ScratchDirectory&) = delete;
  ScratchDirectory& operator=(const ScratchDirectory&) = delete;

  [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
  fs::path path_;
};

std::string uid_directory() { return "tmux-" + std::to_string(::getuid()); }

// Nothing may answer on the sandbox's default socket. A file nothing answers on
// is what a killed server leaves behind, and the library pins an existing file
// whatever is or is not listening, so it is removed here, as tmux would before it
// started a server there. The probe is `-N -S` on that path, run without TMUX.
::testing::AssertionResult sandbox_default_is_free(const fs::path& socket) {
  struct stat metadata = {};
  if (::lstat(socket.c_str(), &metadata) != 0) {
    return ::testing::AssertionSuccess();
  }
  if (!S_ISSOCK(metadata.st_mode) || metadata.st_uid != ::geteuid()) {
    return ::testing::AssertionFailure()
           << "the sandbox's default socket path holds something that is not "
              "this user's socket";
  }
  {
    const ScopedUnset no_tmux{"TMUX"};
    const auto occupant = libtmux::Server::at_socket_path(socket.string());
    if (occupant.has_value() && occupant->is_alive(std::chrono::seconds{2})) {
      return ::testing::AssertionFailure()
             << "a server answers on the sandbox's default socket; an earlier run "
                "left it behind";
    }
  }
  std::error_code failed;
  fs::remove(socket, failed);
  if (failed) {
    return ::testing::AssertionFailure()
           << "could not remove a stale socket from the sandbox: " << failed.message();
  }
  return ::testing::AssertionSuccess();
}

bool mentions(const std::vector<std::string>& findings, std::string_view text) {
  for (const auto& finding : findings) {
    if (finding.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

} // namespace

TEST(SandboxCanary, NothingSelectsAServer) {
  for (const auto name : libtmux::test_support::server_selectors) {
    const std::string key{name};
    EXPECT_FALSE(variable(key.c_str()).has_value()) << key << " is set";
  }
}

TEST(SandboxCanary, TmuxTmpdirIsTheBuildTreesSandbox) {
  const auto configured = variable("TMUX_TMPDIR");
  ASSERT_TRUE(configured.has_value()) << "TMUX_TMPDIR is not set";
  std::error_code failed;
  const auto actual = fs::canonical(*configured, failed);
  ASSERT_FALSE(failed) << "TMUX_TMPDIR is not a directory: " << failed.message();
  const auto expected = fs::canonical(sandbox_root(), failed);
  ASSERT_FALSE(failed) << sandbox_root() << ": " << failed.message();
  EXPECT_TRUE(actual == expected) << "TMUX_TMPDIR is not the build tree's sandbox";
}

// The path tmux uses with neither -L nor -S, read the way the library reads it.
// Sending no command: the handle only resolves.
TEST(SandboxCanary, TheDefaultSocketResolvesUnderTheSandbox) {
  std::error_code failed;
  const auto root = fs::canonical(sandbox_root(), failed);
  ASSERT_FALSE(failed) << sandbox_root() << ": " << failed.message();
  const fs::path expected = root / uid_directory() / "default";
  ASSERT_TRUE(sandbox_default_is_free(expected));

  const auto server = libtmux::Server::startable_at_default(std::nullopt);
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  const fs::path socket{server->socket_path()};
  EXPECT_TRUE(socket == expected)
      << "the default socket does not resolve under the sandbox";
  // tmux fails a longer path with "File name too long", naming nothing else.
  EXPECT_LT(socket.string().size(), sizeof(sockaddr_un::sun_path))
      << "the sandbox's default socket path does not fit a unix socket address; "
         "configure a build directory with a shorter path";
}

// What a tmux child would see when the library reaches for the default server.
// The `tmux` on PATH only records, so nothing here reaches a server whatever
// the environment is.
TEST(SandboxCanary, ADefaultServerCommandCarriesTheSandboxToTmux) {
  std::error_code failed;
  const auto root = fs::canonical(sandbox_root(), failed);
  ASSERT_FALSE(failed) << sandbox_root() << ": " << failed.message();
  ASSERT_TRUE(sandbox_default_is_free(root / uid_directory() / "default"));
  const ScratchDirectory scratch;
  ASSERT_FALSE(scratch.path().empty()) << std::strerror(errno);
  const fs::path recorder = scratch.path() / "tmux";
  const fs::path trace = scratch.path() / "trace";
  // Records whether each variable arrived and, for TMUX_TMPDIR, whether it was
  // the sandbox: words, never the values.
  std::ofstream{recorder} << R"sh(#!/bin/sh
{
  printf 'ARGV'
  for argument in "$@"; do printf ' [%s]' "$argument"; done
  printf '\n'
  if [ -n "${TMUX+x}" ]; then echo 'TMUX=set'; else echo 'TMUX=unset'; fi
  if [ -n "${TMUX_PANE+x}" ]; then echo 'TMUX_PANE=set'; else echo 'TMUX_PANE=unset'; fi
  if [ -z "${TMUX_TMPDIR+x}" ]; then
    echo 'TMUX_TMPDIR=unset'
  elif [ "$TMUX_TMPDIR" = "$CANARY_SANDBOX" ]; then
    echo 'TMUX_TMPDIR=sandbox'
  else
    echo 'TMUX_TMPDIR=other'
  fi
} >> "$CANARY_TRACE"
exit 0
)sh";
  ASSERT_EQ(::chmod(recorder.c_str(), 0700), 0) << std::strerror(errno);

  const libtmux::test::EnvironmentGuard path{"PATH", scratch.path().string()};
  const libtmux::test::EnvironmentGuard recorded{"CANARY_TRACE", trace.string()};
  const libtmux::test::EnvironmentGuard sandbox{"CANARY_SANDBOX",
                                                sandbox_root().string()};
  // A startable handle sends its first command with no selector, which is the
  // one tmux resolves for itself: from TMUX when it is set.
  const auto server = libtmux::Server::startable_at_default(std::nullopt);
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  static_cast<void>(server->new_session("canary"));

  std::ifstream input{trace};
  ASSERT_TRUE(input.is_open()) << "the library never started tmux";
  std::size_t invocations = 0;
  for (std::string line; std::getline(input, line);) {
    if (line.starts_with("ARGV")) {
      ++invocations;
      EXPECT_EQ(line.find(" [-S]"), std::string::npos) << "tmux was given -S";
      EXPECT_EQ(line.find(" [-L]"), std::string::npos) << "tmux was given -L";
    } else if (line.starts_with("TMUX=")) {
      EXPECT_TRUE(line == "TMUX=unset") << "tmux inherited TMUX";
    } else if (line.starts_with("TMUX_PANE=")) {
      EXPECT_TRUE(line == "TMUX_PANE=unset") << "tmux inherited TMUX_PANE";
    } else if (line.starts_with("TMUX_TMPDIR=")) {
      EXPECT_TRUE(line == "TMUX_TMPDIR=sandbox")
          << "tmux did not get the sandbox as TMUX_TMPDIR";
    }
  }
  EXPECT_GT(invocations, 0U);
}

TEST(SandboxGuard, ScopedUnsetRemovesAndRestores) {
  const libtmux::test::EnvironmentGuard set{"LIBTMUX_CANARY_VARIABLE", "kept"};
  {
    const ScopedUnset unset{"LIBTMUX_CANARY_VARIABLE"};
    EXPECT_FALSE(variable("LIBTMUX_CANARY_VARIABLE").has_value());
  }
  EXPECT_EQ(variable("LIBTMUX_CANARY_VARIABLE"), "kept");
}

// Intercepts the failure the destructor reports, so the case can look at what it
// left: the directory is kept, with its server, until this case stops it.
TEST(SandboxGuard, ScratchDirectoryIsKeptWhileAServerAnswersInIt) {
  fs::path kept;
  testing::TestPartResultArray reports;
  {
    testing::ScopedFakeTestPartResultReporter reporter{
        testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD,
        &reports};
    const ScratchDirectory scratch;
    kept = scratch.path();
    const auto server = libtmux::Server::startable_at_socket_path(
        (kept / "held.sock").string(), fs::path{"/dev/null"});
    if (server.has_value()) {
      static_cast<void>(server->new_session("held"));
    }
  }
  ASSERT_FALSE(kept.empty());
  std::error_code failed;
  const bool still_there = fs::exists(kept, failed);
  const auto held = libtmux::Server::at_socket_path((kept / "held.sock").string());
  if (held.has_value()) {
    static_cast<void>(held->kill());
  }
  fs::remove_all(kept, failed);
  EXPECT_EQ(reports.size(), 1) << "the scratch directory said nothing";
  EXPECT_TRUE(still_there) << "the scratch directory was removed under a live server";
}

TEST(SandboxGuard, AcceptsTheHarnessEnvironment) {
  const auto findings = sandbox_violations();
  EXPECT_TRUE(findings.empty()) << findings.front();
}

TEST(SandboxGuard, NamesEachVariableThatSelectsAServer) {
  for (const auto name : libtmux::test_support::server_selectors) {
    const std::string key{name};
    // Empty counts: a set variable is a decision someone made.
    for (const char* const value : {"", "/tmp/decoy,1,0"}) {
      const libtmux::test::EnvironmentGuard selected{key, value};
      const auto findings = sandbox_violations();
      EXPECT_TRUE(mentions(findings, key + " is set")) << key << "=" << value;
    }
  }
}

TEST(SandboxGuard, RefusesATmuxTmpdirNobodyVouchedFor) {
  ScratchDirectory elsewhere;
  ASSERT_FALSE(elsewhere.path().empty()) << std::strerror(errno);

  {
    const ScopedUnset unset{"TMUX_TMPDIR"};
    EXPECT_TRUE(mentions(sandbox_violations(), "TMUX_TMPDIR is not set"));
  }
  {
    const libtmux::test::EnvironmentGuard tmpdir{"TMUX_TMPDIR",
                                                 elsewhere.path().string()};
    EXPECT_TRUE(mentions(sandbox_violations(), "is not the directory"));
  }
  {
    const ScopedUnset unset{std::string{libtmux::test_support::sandbox_marker}};
    EXPECT_TRUE(mentions(sandbox_violations(), "is not vouched for"));
  }
  {
    const std::string missing = (elsewhere.path() / "absent").string();
    const libtmux::test::EnvironmentGuard tmpdir{"TMUX_TMPDIR", missing};
    EXPECT_TRUE(mentions(sandbox_violations(), "is not a directory"));
  }
}

TEST(SandboxGuard, RefusesADirectoryOthersCanUse) {
  ScratchDirectory shared;
  ASSERT_FALSE(shared.path().empty()) << std::strerror(errno);
  const libtmux::test::EnvironmentGuard tmpdir{"TMUX_TMPDIR", shared.path().string()};
  const libtmux::test::EnvironmentGuard marker{
      std::string{libtmux::test_support::sandbox_marker}, shared.path().string()};
  // Group or other bits of any kind: readable, writable or searchable.
  for (const mode_t mode :
       std::initializer_list<mode_t>{0770, 0750, 0705, 0755, 0711}) {
    ASSERT_EQ(::chmod(shared.path().c_str(), mode), 0) << std::strerror(errno);
    EXPECT_TRUE(mentions(sandbox_violations(), "only this user can use"))
        << std::oct << mode;
  }
  ASSERT_EQ(::chmod(shared.path().c_str(), 0700), 0) << std::strerror(errno);
  EXPECT_FALSE(mentions(sandbox_violations(), "only this user can use"));
}

TEST(SandboxGuard, RefusesADefaultSocketPathThatDoesNotFit) {
  ScratchDirectory scratch;
  ASSERT_FALSE(scratch.path().empty()) << std::strerror(errno);
  // Owner-only and vouched for, so the length is the only finding.
  const fs::path deep = scratch.path() / std::string(90, 'd');
  std::error_code failed;
  fs::create_directories(deep, failed);
  ASSERT_FALSE(failed) << failed.message();
  ASSERT_EQ(::chmod(deep.c_str(), 0700), 0) << std::strerror(errno);
  const libtmux::test::EnvironmentGuard tmpdir{"TMUX_TMPDIR", deep.string()};
  const libtmux::test::EnvironmentGuard marker{
      std::string{libtmux::test_support::sandbox_marker}, deep.string()};
  const auto findings = sandbox_violations();
  EXPECT_TRUE(mentions(findings, "is longer than"));
  EXPECT_EQ(findings.size(), 1U);
}
