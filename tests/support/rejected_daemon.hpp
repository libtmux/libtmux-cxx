#pragma once

// A daemon older than the floor, for the suites that hold a surface to
// refusing it. The rejection lanes in CI name a tmux release below 3.3a in
// LIBTMUX_TEST_REJECTED_TMUX and put a 3.3a client first on PATH. A suite that
// asks for the daemon skips without the variable, and fails when
// LIBTMUX_REQUIRE_REJECTED_TMUX is set, because a skipped refusal test
// qualifies nothing.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <stdlib.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "libtmux/command.hpp"

namespace libtmux::test::rejected {

inline std::string slurp(const std::filesystem::path& path) {
  std::ifstream file{path};
  return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

inline std::string shell_quote(const std::string& text) {
  std::string quoted = "'";
  for (const char character : text) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(character);
    }
  }
  return quoted + "'";
}

// A 0700 directory removed with its contents.
class PrivateDirectory final {
public:
  PrivateDirectory() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "libtmux-ver-XXXXXX").string();
    if (::mkdtemp(pattern.data()) != nullptr) {
      path_ = pattern;
    }
  }
  ~PrivateDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  PrivateDirectory(const PrivateDirectory&) = delete;
  PrivateDirectory& operator=(const PrivateDirectory&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

// A daemon of any release, started by its own client. The scoped fixture
// starts servers with `-D`, which tmux 3.1c does not have. A pre-3.2 daemon
// also keeps the pipe of every client it answers open after that client has
// exited, so its answers go to files and never to a pipe read to its end.
class OldDaemon final {
public:
  explicit OldDaemon(std::filesystem::path binary) : binary_{std::move(binary)} {
    socket_ = directory_.path() / "daemon.sock";
    ask({"-f", "/dev/null", "new-session", "-d", "-s", "sentinel"});
  }
  ~OldDaemon() { ask({"kill-server"}); }
  OldDaemon(const OldDaemon&) = delete;
  OldDaemon& operator=(const OldDaemon&) = delete;

  [[nodiscard]] const std::filesystem::path& socket_path() const noexcept {
    return socket_;
  }
  [[nodiscard]] std::string sessions() {
    return ask({"list-sessions", "-F", "#{session_name}"});
  }
  [[nodiscard]] std::string clients() { return ask({"list-clients"}); }
  [[nodiscard]] bool is_alive() {
    return sessions().find("sentinel") != std::string::npos;
  }

private:
  std::string ask(const std::vector<std::string>& arguments) {
    const auto answer = directory_.path() / "answer";
    std::string command =
        shell_quote(binary_.string()) + " -S " + shell_quote(socket_.string());
    for (const std::string& argument : arguments) {
      command += " " + shell_quote(argument);
    }
    command += " >" + shell_quote(answer.string()) + " 2>&1 </dev/null";
    static_cast<void>(std::system(command.c_str()));
    return slurp(answer);
  }

  PrivateDirectory directory_;
  std::filesystem::path binary_;
  std::filesystem::path socket_;
};

inline std::optional<std::filesystem::path> rejected_tmux() {
  if (const char* const configured = std::getenv("LIBTMUX_TEST_REJECTED_TMUX");
      configured != nullptr && *configured != '\0') {
    return std::filesystem::path{configured};
  }
  return std::nullopt;
}

// The release the lane's tmux prints for `-V`, such as "3.2a".
inline std::string rejected_release() {
  const auto binary = rejected_tmux();
  if (!binary.has_value())
    return {};
  const std::string command = shell_quote(binary->string()) + " -V 2>&1 </dev/null";
  std::string printed;
  if (FILE* const pipe = ::popen(command.c_str(), "r")) {
    std::array<char, 128> buffer{};
    while (const auto count = std::fread(buffer.data(), 1, buffer.size(), pipe))
      printed.append(buffer.data(), count);
    ::pclose(pipe);
  }
  while (!printed.empty() && (printed.back() == '\n' || printed.back() == ' '))
    printed.pop_back();
  return printed.rfind("tmux ", 0) == 0 ? printed.substr(5) : printed;
}

// Whether a daemon of this release can run the version probe, so that its
// release is reported rather than refused as older than 3.2. Pinned per lane:
// a release added to the matrix has to be listed, and the test fails until it
// is.
inline bool rejected_release_runs_the_probe(const std::string& release) {
  static const std::map<std::string, bool> lanes{
      {"3.0a", false}, {"3.1c", false}, {"3.2a", true}, {"3.3", true}};
  const auto lane = lanes.find(release);
  if (lane == lanes.end()) {
    ADD_FAILURE() << "no expected answer is pinned for tmux '" << release << "'";
    return true;
  }
  return lane->second;
}

// What every refusal says: the floor, then what to do about it, as a second
// sentence.
inline void expect_floor_and_remedy(const std::string& text) {
  EXPECT_NE(text.find("3.3a"), std::string::npos) << text;
  EXPECT_NE(text.find(". Upgrade tmux and restart the server, or stay on the previous "
                      "libtmux release."),
            std::string::npos)
      << text;
}

class RejectedDaemonTest : public testing::Test {
protected:
  void SetUp() override {
    const auto binary = rejected_tmux();
    // The rejection lane sets the second variable: there a missing or
    // unusable rejected tmux is a failure, because a skipped refusal test
    // qualifies nothing.
    if (std::getenv("LIBTMUX_REQUIRE_REJECTED_TMUX") != nullptr &&
        (!binary.has_value() || ::access(binary->c_str(), X_OK) != 0)) {
      FAIL() << "the rejection lane must name an executable tmux to reject in "
                "LIBTMUX_TEST_REJECTED_TMUX";
    }
    if (!binary.has_value()) {
      GTEST_SKIP() << "LIBTMUX_TEST_REJECTED_TMUX names the tmux older than 3.3a "
                      "that the rejection lanes run these against";
    }
    daemon_ = std::make_unique<OldDaemon>(*binary);
    ASSERT_EQ(daemon_->sessions(), "sentinel\n");
  }

  void expect_untouched() { EXPECT_EQ(daemon_->sessions(), "sentinel\n"); }

  std::unique_ptr<OldDaemon> daemon_;
};

} // namespace libtmux::test::rejected
