// Which server a handle reaches when its caller names no socket.
//
// `LIBTMUX_SOCKET_PATH` selects a socket path (`-S`), else `LIBTMUX_SOCKET` a
// socket name (`-L`), else tmux picks: the server `TMUX` names inside a pane,
// the default socket under `TMUX_TMPDIR` elsewhere. A socket the caller names
// comes first and the variables are then not read.
//
// `SocketSelection` reads the arguments tmux receives, through a recording
// stand-in for it, so nothing here depends on what a server does. `SocketReach`
// runs a real tmux and asks the server it reached which server that is. Every
// case starts from an environment that selects nothing and sets what it is
// about; the suite's sandbox already does that, and the cases do not rely on it.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>
// mkdtemp is POSIX and glibc declares it in <stdlib.h>; <cstdlib> promises
// only the std:: names.
#include <stdlib.h>

#include <gtest/gtest.h>

#include "libtmux/server.hpp"
#include "libtmux/socket.hpp"
#include "libtmux/testing/environment_guard.hpp"
#include "libtmux/testing/scoped_server.hpp"
#include "support/sandbox.hpp"

namespace {

namespace fs = std::filesystem;
using libtmux::Server;
using libtmux::SocketSource;
using libtmux::test::EnvironmentGuard;
using libtmux::test::ScopedTmuxServer;
using libtmux::test_support::ScopedUnset;

// Nothing that selects a server, for the length of a case.
struct NoSelection final {
  ScopedUnset tmux{"TMUX"};
  ScopedUnset pane{"TMUX_PANE"};
  ScopedUnset name{"LIBTMUX_SOCKET"};
  ScopedUnset path{"LIBTMUX_SOCKET_PATH"};
};

// A directory that goes away with the scope.
class Scratch final {
public:
  Scratch() {
    std::error_code failed;
    auto pattern =
        (fs::temp_directory_path(failed) / "libtmux-cxx-sock-XXXXXX").string();
    if (!failed && ::mkdtemp(pattern.data()) != nullptr) {
      path_ = pattern;
    }
  }
  ~Scratch() {
    if (!path_.empty()) {
      std::error_code ignored;
      fs::remove_all(path_, ignored);
    }
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
  fs::path path_;
};

struct Invocation {
  std::vector<std::string> arguments;
  bool tmux_set{};

  [[nodiscard]] bool has(std::string_view argument) const {
    return std::ranges::find(arguments, argument) != arguments.end();
  }
  // `-S path` and `-L name` as tmux reads them: the flag, then its value.
  [[nodiscard]] bool selects(std::string_view flag, std::string_view value) const {
    for (std::size_t index = 0; index + 1U < arguments.size(); ++index) {
      if (arguments[index] == flag && arguments[index + 1U] == value) {
        return true;
      }
    }
    return false;
  }
};

// A `tmux` ahead of the real one on `PATH` that records each invocation: its
// arguments, and whether `TMUX` reached it. It answers nothing and exits 0, so
// nothing reaches a server whatever the environment is. The trace holds the
// arguments, which are the thing under test, and booleans about the environment,
// never its values.
class RecordingTmux final {
public:
  RecordingTmux()
      : trace_{scratch_.path() / "trace"}, path_{"PATH", scratch_.path().string()},
        recorded_{"RECORDER_TRACE", trace_.string()} {
    const fs::path script = scratch_.path() / "tmux";
    std::ofstream{script} << R"sh(#!/bin/sh
{
  for argument in "$@"; do printf '%s\0' "$argument"; done
  printf 'END\0'
  if [ -n "${TMUX+x}" ]; then printf 'TMUX=set\0'; else printf 'TMUX=unset\0'; fi
} >> "$RECORDER_TRACE"
exit 0
)sh";
    ::chmod(script.c_str(), 0700);
  }

  [[nodiscard]] bool ready() const { return !scratch_.path().empty(); }

  [[nodiscard]] std::vector<Invocation> invocations() const {
    std::ifstream input{trace_, std::ios::binary};
    const std::string bytes{std::istreambuf_iterator<char>{input},
                            std::istreambuf_iterator<char>{}};
    std::vector<Invocation> found;
    Invocation current;
    bool ended = false;
    std::size_t start = 0;
    while (start < bytes.size()) {
      const auto stop = bytes.find('\0', start);
      if (stop == std::string::npos) {
        break;
      }
      std::string token = bytes.substr(start, stop - start);
      start = stop + 1U;
      if (ended) {
        current.tmux_set = token == "TMUX=set";
        found.push_back(std::move(current));
        current = Invocation{};
        ended = false;
      } else if (token == "END") {
        ended = true;
      } else {
        current.arguments.push_back(std::move(token));
      }
    }
    return found;
  }

  void forget() const {
    std::error_code ignored;
    fs::remove(trace_, ignored);
  }

private:
  Scratch scratch_;
  fs::path trace_;
  EnvironmentGuard path_;
  EnvironmentGuard recorded_;
};

// Sends one command through a handle that may start a server, so tmux is run,
// and leaves what it received in the recorder.
void send_through(const libtmux::expected<Server, libtmux::CommandFailure>& handle) {
  ASSERT_TRUE(handle.has_value()) << handle.error().diagnostic;
  static_cast<void>(handle->new_session("recorded"));
}

bool any_selects(const std::vector<Invocation>& seen, std::string_view flag,
                 std::string_view value) {
  return std::ranges::any_of(
      seen, [&](const Invocation& call) { return call.selects(flag, value); });
}

bool any_has(const std::vector<Invocation>& seen, std::string_view flag) {
  return std::ranges::any_of(seen,
                             [&](const Invocation& call) { return call.has(flag); });
}

struct Selection {
  const char* label;
  std::optional<std::string> path;
  std::optional<std::string> name;
  SocketSource source;
  // What the source's value is, as given.
  std::string value;
};

const std::vector<Selection>& selections() {
  static const std::vector<Selection> table{
      {"nothing", std::nullopt, std::nullopt, SocketSource::none, ""},
      {"path alone", "/sock/dev", std::nullopt, SocketSource::path_variable,
       "/sock/dev"},
      {"name alone", std::nullopt, "dev", SocketSource::name_variable, "dev"},
      {"both: the path wins", "/sock/dev", "dev", SocketSource::path_variable,
       "/sock/dev"},
      {"an empty path leaves the name", "", "dev", SocketSource::name_variable, "dev"},
      {"a blank path leaves the name", " \t ", "dev", SocketSource::name_variable,
       "dev"},
      {"an empty name leaves the path", "/sock/dev", "", SocketSource::path_variable,
       "/sock/dev"},
      {"a blank name alone is nothing", std::nullopt, "  ", SocketSource::none, ""},
      {"a relative path is as given", "rel/dev.sock", std::nullopt,
       SocketSource::path_variable, "rel/dev.sock"},
      {"padding is not trimmed", std::nullopt, " dev ", SocketSource::name_variable,
       " dev "},
  };
  return table;
}

// Sets exactly what a selection names, and nothing else.
class Environment final {
public:
  explicit Environment(const Selection& selection) {
    if (selection.path.has_value()) {
      path_.emplace("LIBTMUX_SOCKET_PATH", *selection.path);
    }
    if (selection.name.has_value()) {
      name_.emplace("LIBTMUX_SOCKET", *selection.name);
    }
  }

private:
  std::optional<EnvironmentGuard> path_;
  std::optional<EnvironmentGuard> name_;
};

} // namespace

TEST(SocketSelection, TheEnvironmentNamesThePathThenTheNameAndNeverTrims) {
  const NoSelection clean;
  for (const Selection& selection : selections()) {
    SCOPED_TRACE(selection.label);
    const Environment environment{selection};
    const auto chosen = libtmux::socket_from_environment();
    EXPECT_EQ(chosen.source, selection.source);
    EXPECT_EQ(chosen.value, selection.value);
  }
}

TEST(SocketSelection, ADefaultHandlePassesTmuxExactlyWhatTheEnvironmentSelects) {
  const NoSelection clean;
  for (const Selection& selection : selections()) {
    SCOPED_TRACE(selection.label);
    const RecordingTmux recorder;
    ASSERT_TRUE(recorder.ready());
    const Environment environment{selection};
    send_through(Server::startable_at_default(std::nullopt));
    const auto seen = recorder.invocations();
    ASSERT_FALSE(seen.empty()) << "the library never started tmux";

    switch (selection.source) {
    case SocketSource::none:
      EXPECT_FALSE(any_has(seen, "-S")) << "tmux was given -S";
      EXPECT_FALSE(any_has(seen, "-L")) << "tmux was given -L";
      break;
    case SocketSource::path_variable:
      EXPECT_TRUE(any_selects(seen, "-S", selection.value)) << "tmux was not given -S";
      EXPECT_FALSE(any_has(seen, "-L")) << "tmux was given -L";
      break;
    case SocketSource::name_variable:
      EXPECT_TRUE(any_selects(seen, "-L", selection.value)) << "tmux was not given -L";
      EXPECT_FALSE(any_has(seen, "-S")) << "tmux was given -S";
      break;
    }
  }
}

TEST(SocketSelection, ASocketTheCallerNamesComesBeforeBothVariables) {
  const NoSelection clean;
  const EnvironmentGuard path{"LIBTMUX_SOCKET_PATH", "/sock/from-variable"};
  const EnvironmentGuard name{"LIBTMUX_SOCKET", "from-variable"};
  {
    const RecordingTmux recorder;
    ASSERT_TRUE(recorder.ready());
    send_through(Server::startable_at_socket_name("explicit", std::nullopt));
    const auto seen = recorder.invocations();
    EXPECT_TRUE(any_selects(seen, "-L", "explicit"));
    EXPECT_FALSE(any_has(seen, "-S")) << "a variable was read beside a named socket";
    EXPECT_FALSE(any_selects(seen, "-L", "from-variable"));
  }
  {
    const RecordingTmux recorder;
    ASSERT_TRUE(recorder.ready());
    send_through(Server::startable_at_socket_path("/sock/explicit", std::nullopt));
    const auto seen = recorder.invocations();
    EXPECT_TRUE(any_selects(seen, "-S", "/sock/explicit"));
    EXPECT_FALSE(any_has(seen, "-L")) << "a variable was read beside a named socket";
    EXPECT_FALSE(any_selects(seen, "-S", "/sock/from-variable"));
  }
}

TEST(SocketSelection, TmuxIsLeftToFollowTheServerTmuxNames) {
  const NoSelection clean;
  const RecordingTmux recorder;
  ASSERT_TRUE(recorder.ready());
  // A server that is not there: the handle must not translate `TMUX` into a
  // socket of its own, and must not hide it from tmux, which reads it itself.
  const EnvironmentGuard inherited{"TMUX", "/sock/inside-a-pane,1234,0"};
  send_through(Server::startable_at_default(std::nullopt));
  const auto seen = recorder.invocations();
  ASSERT_FALSE(seen.empty());
  for (const Invocation& call : seen) {
    EXPECT_FALSE(call.has("-S")) << "tmux was given -S";
    EXPECT_FALSE(call.has("-L")) << "tmux was given -L";
    EXPECT_TRUE(call.tmux_set) << "tmux did not inherit TMUX";
  }
}

TEST(SocketSelection, AValueTheLibraryCannotUseNamesTheVariableAndNotTheValue) {
  const NoSelection clean;
  const std::string unfit(300, 'x');
  const std::string separator = "has/separator";
  const struct {
    const char* variable;
    const std::string* value;
  } refusals[]{{"LIBTMUX_SOCKET_PATH", &unfit}, {"LIBTMUX_SOCKET", &separator}};
  for (const auto& refusal : refusals) {
    SCOPED_TRACE(refusal.variable);
    const EnvironmentGuard variable{refusal.variable, *refusal.value};
    const auto plain = Server::at_default();
    ASSERT_FALSE(plain.has_value());
    const auto startable = Server::startable_at_default(std::nullopt);
    ASSERT_FALSE(startable.has_value());
    for (const auto& failure : {plain.error(), startable.error()}) {
      EXPECT_EQ(failure.kind, libtmux::FailureKind::validation);
      EXPECT_EQ(failure.delivery, libtmux::DeliveryStatus::not_started);
      EXPECT_NE(failure.diagnostic.find(refusal.variable), std::string::npos)
          << "the refusal does not name the variable";
      EXPECT_EQ(failure.diagnostic.find(*refusal.value), std::string::npos)
          << "the refusal quotes the value";
    }
  }
}

namespace {

// Stops the server a case started, and only that one: only when the socket the
// handle reports lies in the tree the case made, so a handle that reached
// anything else is left alone.
class StoppedWithTheCase final {
public:
  StoppedWithTheCase(Server server, fs::path tree)
      : server_{std::move(server)}, tree_{std::move(tree)} {}
  ~StoppedWithTheCase() {
    std::error_code failed;
    const auto socket =
        fs::weakly_canonical(std::string{server_.socket_path()}, failed);
    const auto root = fs::canonical(tree_, failed);
    const auto relative = socket.lexically_relative(root);
    if (!failed && !relative.empty() && *relative.begin() != "..") {
      static_cast<void>(server_.kill());
    }
  }
  StoppedWithTheCase(const StoppedWithTheCase&) = delete;
  StoppedWithTheCase& operator=(const StoppedWithTheCase&) = delete;

  [[nodiscard]] const Server& get() const noexcept { return server_; }

private:
  Server server_;
  fs::path tree_;
};

ScopedTmuxServer start_owner(libtmux::test::SocketMode mode, std::string session) {
  libtmux::test::ScopedTmuxServerOptions options;
  options.mode = mode;
  options.session_name = std::move(session);
  options.socket_namespace = libtmux::test::SocketNamespace::consumer("sockdef");
  auto owner = ScopedTmuxServer::start(std::move(options));
  EXPECT_TRUE(owner.has_value()) << owner.error();
  return *std::move(owner);
}

// The pid the server says it is, which is how a handle proves what it reached.
std::string pid_of(const Server& server) {
  const auto pid = server.expand("#{pid}");
  return pid.has_value() ? *pid : std::string{"unanswered"};
}

std::string reached(const Server& server) {
  const auto socket = server.expand("#{socket_path}");
  return socket.has_value() ? *socket : std::string{"unanswered"};
}

bool lists(const Server& server, std::string_view name) {
  const auto sessions = server.sessions();
  return sessions.has_value() &&
         std::ranges::any_of(*sessions, [&](const libtmux::Session& session) {
           return session.name() == name;
         });
}

bool same_file(const fs::path& left, const fs::path& right) {
  const auto answer = libtmux::test::same_socket_inode(left, right);
  return answer.has_value() && *answer;
}

} // namespace

TEST(SocketReach, LibtmuxSocketPathNamesTheServer) {
  const NoSelection clean;
  const auto owner = start_owner(libtmux::test::SocketMode::Path, "reach-path");
  const EnvironmentGuard path{"LIBTMUX_SOCKET_PATH", owner.socket_path().string()};
  const auto handle = Server::at_default();
  ASSERT_TRUE(handle.has_value()) << handle.error().diagnostic;
  EXPECT_TRUE(lists(*handle, "reach-path"));
  EXPECT_EQ(pid_of(*handle), std::to_string(owner.server_pid()));
  EXPECT_TRUE(same_file(handle->socket_path(), owner.socket_path()));
}

TEST(SocketReach, LibtmuxSocketNamesTheServerUnderTmuxTmpdir) {
  const NoSelection clean;
  const auto owner = start_owner(libtmux::test::SocketMode::Name, "reach-name");
  ASSERT_TRUE(owner.socket_name().has_value());
  const EnvironmentGuard tmpdir{"TMUX_TMPDIR", owner.tmux_tmpdir().string()};
  const EnvironmentGuard name{"LIBTMUX_SOCKET", *owner.socket_name()};
  const auto handle = Server::at_default();
  ASSERT_TRUE(handle.has_value()) << handle.error().diagnostic;
  EXPECT_TRUE(lists(*handle, "reach-name"));
  EXPECT_EQ(pid_of(*handle), std::to_string(owner.server_pid()));
  EXPECT_TRUE(same_file(handle->socket_path(), owner.socket_path()));
}

TEST(SocketReach, WhenBothAreSetThePathWins) {
  const NoSelection clean;
  const auto owner = start_owner(libtmux::test::SocketMode::Path, "reach-both");
  const EnvironmentGuard tmpdir{"TMUX_TMPDIR", owner.tmux_tmpdir().string()};
  const EnvironmentGuard path{"LIBTMUX_SOCKET_PATH", owner.socket_path().string()};
  const EnvironmentGuard name{"LIBTMUX_SOCKET", "a-name-nothing-listens-on"};
  const auto handle = Server::at_default();
  ASSERT_TRUE(handle.has_value()) << handle.error().diagnostic;
  EXPECT_TRUE(lists(*handle, "reach-both"));
  EXPECT_EQ(pid_of(*handle), std::to_string(owner.server_pid()));
}

TEST(SocketReach, ABlankPathLeavesTheNameInForce) {
  const NoSelection clean;
  const auto owner = start_owner(libtmux::test::SocketMode::Name, "reach-blank");
  ASSERT_TRUE(owner.socket_name().has_value());
  const EnvironmentGuard tmpdir{"TMUX_TMPDIR", owner.tmux_tmpdir().string()};
  const EnvironmentGuard path{"LIBTMUX_SOCKET_PATH", "  "};
  const EnvironmentGuard name{"LIBTMUX_SOCKET", *owner.socket_name()};
  const auto handle = Server::at_default();
  ASSERT_TRUE(handle.has_value()) << handle.error().diagnostic;
  EXPECT_TRUE(lists(*handle, "reach-blank"));
}

TEST(SocketReach, ANamedSocketBeatsBothVariables) {
  const NoSelection clean;
  const auto selected = start_owner(libtmux::test::SocketMode::Path, "by-variable");
  const auto named = start_owner(libtmux::test::SocketMode::Path, "by-argument");
  const EnvironmentGuard path{"LIBTMUX_SOCKET_PATH", selected.socket_path().string()};
  const auto handle = Server::at_socket_path(named.socket_path().string());
  ASSERT_TRUE(handle.has_value()) << handle.error().diagnostic;
  EXPECT_TRUE(lists(*handle, "by-argument"));
  EXPECT_FALSE(lists(*handle, "by-variable"));
  EXPECT_EQ(pid_of(*handle), std::to_string(named.server_pid()));
}

TEST(SocketReach, WithNothingSetTheDefaultSocketUnderTmuxTmpdirIsReached) {
  const NoSelection clean;
  const auto owner = start_owner(libtmux::test::SocketMode::Name, "reach-owner");
  const EnvironmentGuard tmpdir{"TMUX_TMPDIR", owner.tmux_tmpdir().string()};
  const fs::path expected =
      owner.tmux_tmpdir() / ("tmux-" + std::to_string(::getuid())) / "default";

  auto opened = Server::startable_at_default(fs::path{"/dev/null"});
  ASSERT_TRUE(opened.has_value()) << opened.error().diagnostic;
  const StoppedWithTheCase started{*std::move(opened), owner.tmux_tmpdir()};
  EXPECT_TRUE(fs::weakly_canonical(std::string{started.get().socket_path()}) ==
              fs::weakly_canonical(expected));
  ASSERT_TRUE(started.get().new_session("reach-default").has_value());

  EXPECT_TRUE(lists(started.get(), "reach-default"));
  EXPECT_NE(pid_of(started.get()), std::to_string(owner.server_pid()))
      << "the handle reached the owner's server, not the default socket's";
  EXPECT_TRUE(same_file(reached(started.get()), expected));
  EXPECT_FALSE(lists(started.get(), "reach-owner"));
}

TEST(SocketReach, AVariableBeatsTmux) {
  const NoSelection clean;
  const auto inside = start_owner(libtmux::test::SocketMode::Path, "in-pane");
  const auto chosen = start_owner(libtmux::test::SocketMode::Path, "by-variable");
  const EnvironmentGuard pane{"TMUX", inside.socket_path().string() + "," +
                                          std::to_string(inside.server_pid()) + ",0"};
  const EnvironmentGuard path{"LIBTMUX_SOCKET_PATH", chosen.socket_path().string()};
  const auto handle = Server::at_default();
  ASSERT_TRUE(handle.has_value()) << handle.error().diagnostic;
  EXPECT_TRUE(lists(*handle, "by-variable"));
  EXPECT_FALSE(lists(*handle, "in-pane"));
  EXPECT_EQ(pid_of(*handle), std::to_string(chosen.server_pid()));
}
