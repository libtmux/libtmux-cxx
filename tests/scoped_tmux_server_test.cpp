#include "libtmux/testing/scoped_server.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

class ScopedEnvironment final {
public:
  ScopedEnvironment(std::string name, std::optional<std::string> value)
      : name_(std::move(name)) {
    const auto* existing = std::getenv(name_.c_str());
    if (existing != nullptr) {
      previous_ = existing;
    }
    if (value.has_value()) {
      if (::setenv(name_.c_str(), value->c_str(), 1) != 0) {
        std::abort();
      }
    } else if (::unsetenv(name_.c_str()) != 0) {
      std::abort();
    }
  }

  ~ScopedEnvironment() {
    if (previous_.has_value()) {
      static_cast<void>(::setenv(name_.c_str(), previous_->c_str(), 1));
    } else {
      static_cast<void>(::unsetenv(name_.c_str()));
    }
  }

  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
  std::string name_;
  std::optional<std::string> previous_;
};

std::filesystem::path unique_test_directory(std::string_view stem) {
  static std::atomic<unsigned long> sequence{0};
  auto path = std::filesystem::temp_directory_path() /
              (std::string{stem} + "-" + std::to_string(::getpid()) + "-" +
               std::to_string(sequence.fetch_add(1)));
  std::filesystem::create_directories(path);
  return path;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input{path};
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

static_assert(!std::is_copy_constructible_v<libtmux::test::ScopedTmuxServer>);
static_assert(!std::is_copy_assignable_v<libtmux::test::ScopedTmuxServer>);
static_assert(std::is_nothrow_move_constructible_v<libtmux::test::ScopedTmuxServer>);
static_assert(std::is_nothrow_move_assignable_v<libtmux::test::ScopedTmuxServer>);

TEST(ScopedTmuxServer, StartsByNameAndExposesResolvedPath) {
  auto server =
      libtmux::test::ScopedTmuxServer::start({.mode = libtmux::test::SocketMode::Name});
  ASSERT_TRUE(server.has_value());
  EXPECT_EQ(server->socket_mode(), libtmux::test::SocketMode::Name);
  EXPECT_TRUE(server->socket_name().has_value());
  EXPECT_FALSE(server->socket_path().empty());
  const auto prefix = server->command_prefix();
  ASSERT_EQ(prefix.size(), 3U);
  EXPECT_EQ(prefix[1], "-S");
  EXPECT_EQ(prefix[2], server->socket_path().string());
  EXPECT_TRUE(server->is_alive());
}

// The socket name is spent from a budget, so it is fixed.
//
// `tmux -L` resolves under `$TMUX_TMPDIR`, which is already the namespaced
// private tree, so a name carrying the namespace adds no isolation — only
// length, to a path that must fit in `sockaddr_un::sun_path`. That is 104
// bytes on macOS, where `$TMPDIR` is a `/var/folders` path that canonicalises
// to `/private/var/...` and spends around sixty of them before this fixture
// adds anything. A namespaced name once cost ten more and the server would not
// start, reporting only "File name too long".
TEST(ScopedTmuxServer, SocketNameDoesNotGrowWithTheNamespace) {
  auto brief = libtmux::test::ScopedTmuxServer::start(
      {.mode = libtmux::test::SocketMode::Name, .socket_namespace = {.label = "a"}});
  auto verbose = libtmux::test::ScopedTmuxServer::start(
      {.mode = libtmux::test::SocketMode::Name,
       .socket_namespace = {.label = "libtmux-cxx-suite"}});
  ASSERT_TRUE(brief.has_value()) << brief.error();
  ASSERT_TRUE(verbose.has_value()) << verbose.error();

  ASSERT_TRUE(brief->socket_name().has_value());
  ASSERT_TRUE(verbose->socket_name().has_value());
  EXPECT_EQ(*brief->socket_name(), *verbose->socket_name());

  // The namespace is still what tells the two trees apart.
  EXPECT_NE(brief->tmux_tmpdir(), verbose->tmux_tmpdir());
  EXPECT_NE(verbose->tmux_tmpdir().string().find("libtmux-cxx-suite"),
            std::string::npos)
      << verbose->tmux_tmpdir();
}

TEST(ScopedTmuxServer, StartsEightServersConcurrentlyWithoutSocketCollisions) {
  std::vector<std::future<std::string>> futures;
  futures.reserve(8U);
  for (int index = 0; index < 8; ++index) {
    futures.push_back(std::async(std::launch::async, [] {
      auto server = libtmux::test::ScopedTmuxServer::start();
      if (!server.has_value() || !server->is_alive()) {
        return std::string{};
      }
      return server->socket_path().string();
    }));
  }

  std::set<std::string> socket_paths;
  for (auto& future : futures) {
    auto socket_path = future.get();
    ASSERT_FALSE(socket_path.empty());
    socket_paths.insert(std::move(socket_path));
  }
  EXPECT_EQ(socket_paths.size(), 8U);
}

TEST(ScopedTmuxServer, SerializesConcurrentSharedTeardownReports) {
  constexpr int fixture_count = 8;
  auto report = std::make_shared<libtmux::test::TeardownReport>();
  std::barrier teardown_barrier{fixture_count};
  std::vector<std::future<std::string>> futures;
  futures.reserve(fixture_count);
  for (int index = 0; index < fixture_count; ++index) {
    futures.push_back(std::async(std::launch::async, [&] {
      auto server = libtmux::test::ScopedTmuxServer::start(
          {.tmux_binary = LIBTMUX_FAKE_TMUX_PATH, .teardown_report = report});
      if (!server.has_value()) {
        teardown_barrier.arrive_and_drop();
        return server.error();
      }
      teardown_barrier.arrive_and_wait();
      return std::string{};
    }));
  }

  for (auto& future : futures) {
    EXPECT_TRUE(future.get().empty());
  }
  EXPECT_EQ(report->messages.size(), static_cast<std::size_t>(fixture_count));
}

TEST(ScopedTmuxServer, MoveConstructionAndAssignmentTransferOwnership) {
  auto first = libtmux::test::ScopedTmuxServer::start();
  auto second = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(first.has_value()) << first.error();
  ASSERT_TRUE(second.has_value()) << second.error();
  const auto first_pid = first->server_pid();
  const auto second_pid = second->server_pid();

  libtmux::test::ScopedTmuxServer moved{std::move(*first)};
  EXPECT_EQ(moved.server_pid(), first_pid);
  moved = std::move(*second);
  EXPECT_EQ(moved.server_pid(), second_pid);

  errno = 0;
  EXPECT_EQ(::waitpid(first_pid, nullptr, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
  EXPECT_TRUE(moved.is_alive());
}

TEST(ScopedTmuxServer, FakeTraceProvesArgvAndSanitizedChildEnvironment) {
  const auto root = unique_test_directory("libtmux-fake-trace");
  const auto trace = root / "trace";
  ScopedEnvironment trace_environment{"LIBTMUX_FAKE_TRACE", trace.string()};
  ScopedEnvironment home_environment{"HOME", "/preserved-parent-home"};
  ScopedEnvironment tmux_environment{"TMUX", "/ambient/socket,1,0"};
  ScopedEnvironment pane_environment{"TMUX_PANE", "%99"};
  const auto* original_home = std::getenv("HOME");
  ASSERT_NE(original_home, nullptr);
  const std::string parent_home{original_home};

  {
    auto by_name = libtmux::test::ScopedTmuxServer::start(
        {.tmux_binary = LIBTMUX_FAKE_TMUX_PATH,
         .mode = libtmux::test::SocketMode::Name});
    ASSERT_TRUE(by_name.has_value()) << by_name.error();
  }
  {
    auto by_path = libtmux::test::ScopedTmuxServer::start(
        {.tmux_binary = LIBTMUX_FAKE_TMUX_PATH,
         .mode = libtmux::test::SocketMode::Path});
    ASSERT_TRUE(by_path.has_value()) << by_path.error();
  }

  ASSERT_NE(std::getenv("HOME"), nullptr);
  EXPECT_EQ(std::string{std::getenv("HOME")}, parent_home);
  ASSERT_NE(std::getenv("TMUX"), nullptr);
  EXPECT_EQ(std::string{std::getenv("TMUX")}, "/ambient/socket,1,0");
  ASSERT_NE(std::getenv("TMUX_PANE"), nullptr);
  EXPECT_EQ(std::string{std::getenv("TMUX_PANE")}, "%99");

  const auto contents = read_file(trace);
  const std::string fake_binary = LIBTMUX_FAKE_TMUX_PATH;
  EXPECT_NE(contents.find("server\t" + fake_binary + "\t-D\t-u\t-f\t/dev/null\t-L\t"),
            std::string::npos);
  EXPECT_NE(contents.find("server\t" + fake_binary + "\t-D\t-u\t-f\t/dev/null\t-S\t"),
            std::string::npos);
  bool queried_name_socket_path = false;
  bool queried_exact_socket_path = false;
  std::size_t position = 0;
  while (position < contents.size()) {
    const auto end = contents.find('\n', position);
    const auto line = contents.substr(position, end - position);
    if (line.starts_with("client\t") &&
        line.find("\tdisplay-message\t-p\t#{socket_path}") != std::string::npos) {
      queried_name_socket_path =
          queried_name_socket_path || line.find("\t-N\t-L\t") != std::string::npos;
      queried_exact_socket_path =
          queried_exact_socket_path || line.find("\t-N\t-S\t") != std::string::npos;
    }
    if (end == std::string::npos) {
      break;
    }
    position = end + 1U;
  }
  EXPECT_TRUE(queried_name_socket_path);
  EXPECT_TRUE(queried_exact_socket_path);
  EXPECT_NE(contents.find("\tif-shell\t-F\t#{==:#{pid},"), std::string::npos);
  EXPECT_NE(contents.find("\tnew-session -d -P -F libtmux-session-created -s "
                          "\\154\\151\\142\\164\\155\\165\\170\\137\\164"
                          "\\145\\163\\164\tdisplay-message -p "
                          "libtmux-session-rejected"),
            std::string::npos);
  // HOME is replaced, not preserved. A pane runs a shell, a shell reads its
  // startup files, and a test that depends on whose machine it runs on is not
  // a test: an interactive zsh with an unfamiliar HOME opens a first-run
  // wizard and swallows every keystroke sent to it.
  EXPECT_EQ(contents.find("HOME=/preserved-parent-home"), std::string::npos);
  EXPECT_NE(contents.find("SHELL=/bin/sh"), std::string::npos);
  EXPECT_NE(contents.find("TMUX=<unset>"), std::string::npos);
  EXPECT_NE(contents.find("TMUX_PANE=<unset>"), std::string::npos);

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(ScopedTmuxServer, EncodesSessionNameInsideConditionalCommandList) {
  const auto root = unique_test_directory("libtmux-session-command-trace");
  const auto trace = root / "trace";
  ScopedEnvironment trace_environment{"LIBTMUX_FAKE_TRACE", trace.string()};
  constexpr std::string_view session_name = "owned; kill-server";

  auto server = libtmux::test::ScopedTmuxServer::start(
      {.tmux_binary = LIBTMUX_FAKE_TMUX_PATH,
       .session_name = std::string{session_name}});
  ASSERT_TRUE(server.has_value()) << server.error();
  EXPECT_EQ(server->session_name(), session_name);

  const auto contents = read_file(trace);
  EXPECT_EQ(contents.find(session_name), std::string::npos);
  EXPECT_NE(contents.find("\\073"), std::string::npos);

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

} // namespace

TEST(ScopedTmuxServer, StartsByExactPath) {
  auto server =
      libtmux::test::ScopedTmuxServer::start({.mode = libtmux::test::SocketMode::Path});
  ASSERT_TRUE(server.has_value());
  EXPECT_EQ(server->socket_mode(), libtmux::test::SocketMode::Path);
  EXPECT_FALSE(server->socket_name().has_value());
  const auto prefix = server->command_prefix();
  ASSERT_EQ(prefix.size(), 3U);
  EXPECT_EQ(prefix[1], "-S");
  EXPECT_EQ(prefix[2], server->socket_path().string());
  EXPECT_TRUE(server->is_alive());
}
