// Which tmux server a value came from, and what depends on knowing.
//
// tmux numbers ids per server. Two servers started a second apart both hold
// `$0`, `@0` and `%0`, so an id carried from one to the other does not fail to
// resolve — it resolves to something else, and tmux reports success. Every
// assertion here rests on that: the fixtures deliberately collide.

#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <unordered_set>

#if !defined(_WIN32)
#include <cerrno>
#include <csignal>
#include <cstring>
// mkdtemp is POSIX and glibc declares it in <stdlib.h>; <cstdlib>
// promises only the std:: names.
#include <stdlib.h>
#endif

#include <gtest/gtest.h>

#include "libtmux/cardinality.hpp"
#include "libtmux/command.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "libtmux/testing/environment_guard.hpp"
#include "libtmux/testing/scoped_server.hpp"
#include "support/sandbox.hpp"

namespace {

using libtmux::Server;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

class ServerCleanup final {
public:
  explicit ServerCleanup(Server server) : server_{std::move(server)} {}
  ~ServerCleanup() { static_cast<void>(server_.kill()); }

  ServerCleanup(const ServerCleanup&) = delete;
  ServerCleanup& operator=(const ServerCleanup&) = delete;

private:
  Server server_;
};

struct ServerIdentity {
  std::string pid;
  std::string socket;
};

// Which server answers this handle, as that server says.
std::optional<ServerIdentity> identify(const Server& server) {
  const auto pid = server.expand("#{pid}");
  const auto socket = server.expand("#{socket_path}");
  if (!pid.has_value() || !socket.has_value()) {
    return std::nullopt;
  }
  return ServerIdentity{*pid, *socket};
}

bool is_under(const std::filesystem::path& path, const std::filesystem::path& tree) {
  std::error_code failed;
  const auto resolved = std::filesystem::weakly_canonical(path, failed);
  const auto root = std::filesystem::canonical(tree, failed);
  if (failed) {
    return false;
  }
  const auto relative = resolved.lexically_relative(root);
  return !relative.empty() && *relative.begin() != "..";
}

bool process_gone(const std::string& text) {
  long number = 0;
  if (std::from_chars(text.data(), text.data() + text.size(), number).ec !=
      std::errc{}) {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (std::chrono::steady_clock::now() < deadline) {
    if (::kill(static_cast<pid_t>(number), 0) != 0 && errno == ESRCH) {
      return true;
    }
#if defined(__linux__)
    // Dead but not yet reaped by whatever adopted the daemon.
    std::ifstream stat{"/proc/" + text + "/stat"};
    std::string line;
    if (std::getline(stat, line) && line.rfind(") Z ") != std::string::npos) {
      return true;
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  return false;
}

// Ends the server a case started, and only that one. A handle that names no
// socket sends `kill-server` to whichever server tmux resolves for itself,
// which inside a pane is the server the suite was started from, so the server
// that answers is identified first and the command is withheld unless it is
// the one `record()` saw, in the case's private tree.
class OwnServerCleanup final {
public:
  OwnServerCleanup(Server server, std::filesystem::path private_tree)
      : server_{std::move(server)}, tree_{std::move(private_tree)} {}
  ~OwnServerCleanup() { finish(); }

  OwnServerCleanup(const OwnServerCleanup&) = delete;
  OwnServerCleanup& operator=(const OwnServerCleanup&) = delete;

  // Call once the server exists.
  std::optional<ServerIdentity> record() {
    started_ = identify(server_);
    return started_;
  }

  void finish() {
    if (finished_) {
      return;
    }
    finished_ = true;
    const auto answering = identify(server_);
    if (!answering.has_value()) {
      // Nothing answers: either the server is gone or its socket is, and a
      // server whose socket is gone can no longer be told to exit.
      if (started_.has_value()) {
        EXPECT_TRUE(process_gone(started_->pid))
            << "the server this case started no longer answers and is still running";
      }
      return;
    }
    const bool private_socket = is_under(answering->socket, tree_);
    const bool started_here = !started_.has_value() || started_->pid == answering->pid;
    if (!private_socket || !started_here) {
      ADD_FAILURE() << "kill-server withheld: "
                    << (private_socket
                            ? "a server this case did not start answered"
                            : "a server outside the case's private tree answered");
      return;
    }
    static_cast<void>(server_.kill());
    EXPECT_TRUE(process_gone(answering->pid))
        << "the server this case started is still running after kill-server";
  }

private:
  Server server_;
  std::filesystem::path tree_;
  std::optional<ServerIdentity> started_;
  bool finished_{false};
};

class ReplacedSocket final {
public:
  ReplacedSocket(std::filesystem::path selected, std::filesystem::path retained,
                 const std::filesystem::path& replacement)
      : selected_{std::move(selected)}, retained_{std::move(retained)} {
    removed_ = std::filesystem::remove(selected_, error_);
    if (error_ || !removed_) {
      if (!error_) {
        error_ = std::make_error_code(std::errc::no_such_file_or_directory);
      }
      return;
    }
    std::filesystem::create_hard_link(replacement, selected_, error_);
  }

  ~ReplacedSocket() { static_cast<void>(restore()); }

  ReplacedSocket(const ReplacedSocket&) = delete;
  ReplacedSocket& operator=(const ReplacedSocket&) = delete;

  [[nodiscard]] const std::error_code& error() const noexcept { return error_; }

  [[nodiscard]] std::error_code restore() noexcept {
    if (!removed_) {
      return {};
    }
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(selected_, ignored));
    std::error_code restored;
    std::filesystem::create_hard_link(retained_, selected_, restored);
    if (!restored) {
      removed_ = false;
    }
    return restored;
  }

private:
  std::filesystem::path selected_;
  std::filesystem::path retained_;
  std::error_code error_;
  bool removed_{};
};

enum class StartableSelector { path, name, default_ };

class StartableServerIdentity : public testing::TestWithParam<StartableSelector> {};

std::string
selector_name(const testing::TestParamInfo<StartableSelector>& information) {
  switch (information.param) {
  case StartableSelector::path:
    return "Path";
  case StartableSelector::name:
    return "Name";
  case StartableSelector::default_:
    return "Default";
  }
  return "Unknown";
}

TEST_P(StartableServerIdentity, PublishesTheCreatedServersExactIdentity) {
  libtmux::test::ScopedTmuxServerOptions owner_options;
  owner_options.mode = libtmux::test::SocketMode::Name;
  auto owner = libtmux::test::ScopedTmuxServer::start(std::move(owner_options));
  ASSERT_TRUE(owner.has_value()) << owner.error();
  const libtmux::test::EnvironmentGuard tmpdir{"TMUX_TMPDIR",
                                               owner->tmux_tmpdir().string()};
  // `startable_at_default()` names no socket, so tmux chooses the server: the
  // one TMUX names when it is set, which inside a pane is the server this suite
  // was started from.
  const libtmux::test_support::ScopedUnset no_tmux{"TMUX"};
  const libtmux::test_support::ScopedUnset no_pane{"TMUX_PANE"};
  const libtmux::test_support::ScopedUnset no_socket_name{"LIBTMUX_SOCKET"};
  const libtmux::test_support::ScopedUnset no_socket_path{"LIBTMUX_SOCKET_PATH"};
  const std::filesystem::path selected = owner->tmux_tmpdir() / "startable.sock";
  // tmux spends `$TMUX_TMPDIR/tmux-<uid>/` before this name, and macOS gives
  // `sun_path` four fewer bytes than Linux does. `startable-name` overran it by
  // one byte under a real `$TMPDIR`.
  constexpr std::string_view socket_name{"startable"};

  const auto open = [&]() -> libtmux::expected<Server, libtmux::CommandFailure> {
    switch (GetParam()) {
    case StartableSelector::path:
      return Server::startable_at_socket_path(selected.string(),
                                              std::filesystem::path{"/dev/null"});
    case StartableSelector::name:
      return Server::startable_at_socket_name(socket_name,
                                              std::filesystem::path{"/dev/null"});
    case StartableSelector::default_:
      return Server::startable_at_default(std::filesystem::path{"/dev/null"});
    }
    return Server::startable_at_default(std::filesystem::path{"/dev/null"});
  };
  auto opened = open();
  ASSERT_TRUE(opened.has_value()) << opened.error().diagnostic;
  OwnServerCleanup cleanup{*opened, owner->tmux_tmpdir()};

  const auto created = opened->new_session("startable-original");
  ASSERT_TRUE(created.has_value()) << created.error().diagnostic;
  // The server this case started, and what the end of it may kill.
  const auto started = cleanup.record();
  ASSERT_TRUE(started.has_value());
  EXPECT_TRUE(is_under(started->socket, owner->tmux_tmpdir()))
      << "the server this case started listens outside its private tree";
  EXPECT_NE(started->pid, std::to_string(owner->server_pid()))
      << "the case reached the owner's server, not one of its own";
  const auto attach = created->attach_command();
  ASSERT_TRUE(attach.has_value()) << attach.error().diagnostic;
  ASSERT_GE(attach->argv().size(), 5U);
  EXPECT_EQ(attach->argv()[1], "-S");
  EXPECT_NE(attach->argv()[2], opened->socket_path());
  const auto aliased =
      libtmux::test::same_socket_inode(attach->argv()[2], opened->socket_path());
  ASSERT_TRUE(aliased.has_value()) << aliased.error();
  EXPECT_TRUE(*aliased);

  const auto reopen = [&]() -> libtmux::expected<Server, libtmux::CommandFailure> {
    switch (GetParam()) {
    case StartableSelector::path:
      return Server::at_socket_path(opened->socket_path());
    case StartableSelector::name:
      return Server::at_socket_name(socket_name);
    case StartableSelector::default_:
      return Server::at_default();
    }
    return Server::at_default();
  };
  auto reopened = reopen();
  ASSERT_TRUE(reopened.has_value()) << reopened.error().diagnostic;
  const auto sessions = reopened->sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  const auto same = std::ranges::find(*sessions, created->id(), &libtmux::Session::id);
  ASSERT_NE(same, sessions->end());
  EXPECT_EQ(*created, *same);

  const auto windows = opened->windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  ASSERT_FALSE(windows->empty());
  const auto destination = reopened->new_session("startable-cross-handle");
  ASSERT_TRUE(destination.has_value()) << destination.error().diagnostic;
  EXPECT_TRUE(windows->front().link_to(*destination).has_value());

  cleanup.finish();
  EXPECT_TRUE(owner->is_alive()) << "kill-server reached the owner's server";
}

INSTANTIATE_TEST_SUITE_P(AllSelectors, StartableServerIdentity,
                         testing::Values(StartableSelector::path,
                                         StartableSelector::name,
                                         StartableSelector::default_),
                         selector_name);

#if !defined(_WIN32)
// PublishesTheCreatedServersExactIdentity above points TMUX_TMPDIR at an
// owner ScopedTmuxServer's own directory, whose startup already created
// `tmux-<uid>/` as a side effect — masking the defect this proves fixed:
// `startable_at_*` pinned its very first command, the one that has to
// create that directory, to `-S <resolved path>`. tmux only creates a
// missing `tmux-<uid>/` when it resolves the path itself (no selector, or
// `-L`); handed one directly with `-S`, it prints "error creating ..." on
// stderr and still exits 0, so nothing upstream saw it fail. This points
// TMUX_TMPDIR at a directory nothing has touched, so `tmux-<uid>/` is
// missing exactly as it would be for a first run.
TEST(ServerIdentity, StartableAtSocketNameSucceedsUnderAFreshTmuxTmpdir) {
  std::error_code parent_error;
  const auto parent =
      std::filesystem::canonical(std::filesystem::temp_directory_path(), parent_error);
  ASSERT_FALSE(parent_error) << parent_error.message();
  auto pattern = (parent / "libtmux-cxx-fresh-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr) << std::strerror(errno);
  const std::filesystem::path fresh_tmpdir{pattern};
  const libtmux::test::EnvironmentGuard tmpdir{"TMUX_TMPDIR", fresh_tmpdir.string()};

  auto opened = Server::startable_at_socket_name("cxx12-fresh",
                                                 std::filesystem::path{"/dev/null"});
  ASSERT_TRUE(opened.has_value()) << opened.error().diagnostic;
  ServerCleanup cleanup{*opened};

  const auto created = opened->new_session("fresh-tmpdir-session");
  ASSERT_TRUE(created.has_value()) << created.error().diagnostic;

  std::error_code removed;
  std::filesystem::remove_all(fresh_tmpdir, removed);
}
#endif

TEST(ServerIdentity, ConcurrentFirstStartPinsTheOriginalServer) {
  auto owner = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(owner.has_value()) << owner.error();
  const std::filesystem::path selected = owner->tmux_tmpdir() / "concurrent.sock";
  auto opened = Server::startable_at_socket_path(selected.string(),
                                                 std::filesystem::path{"/dev/null"});
  ASSERT_TRUE(opened.has_value()) << opened.error().diagnostic;
  ServerCleanup cleanup{*opened};

  auto first = std::async(std::launch::async,
                          [&opened] { return opened->new_session("concurrent-one"); });
  auto second = std::async(std::launch::async,
                           [&opened] { return opened->new_session("concurrent-two"); });
  const auto one = first.get();
  const auto two = second.get();
  ASSERT_TRUE(one.has_value()) << one.error().diagnostic;
  ASSERT_TRUE(two.has_value()) << two.error().diagnostic;
  EXPECT_EQ(one->connection_identity(), two->connection_identity());
  const auto attach = one->attach_command();
  ASSERT_TRUE(attach.has_value()) << attach.error().diagnostic;
  ASSERT_GE(attach->argv().size(), 3U);
  const std::filesystem::path retained = attach->argv()[2];

  auto replacement = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(replacement.has_value()) << replacement.error();
  {
    ReplacedSocket replaced{selected, retained, replacement->socket_path()};
    ASSERT_FALSE(replaced.error()) << replaced.error().message();
    const auto still_original = opened->sessions();
    ASSERT_TRUE(still_original.has_value()) << still_original.error().diagnostic;
    std::vector<std::string> names;
    for (const libtmux::Session& session : *still_original) {
      names.emplace_back(session.name());
    }
    std::ranges::sort(names);
    EXPECT_EQ(names, (std::vector<std::string>{"concurrent-one", "concurrent-two"}));

    auto now_selected = Server::at_socket_path(selected.string());
    ASSERT_TRUE(now_selected.has_value()) << now_selected.error().diagnostic;
    const auto replacement_sessions = now_selected->sessions();
    ASSERT_TRUE(replacement_sessions.has_value())
        << replacement_sessions.error().diagnostic;
    ASSERT_FALSE(replacement_sessions->empty());
    EXPECT_NE(one.value(), replacement_sessions->front());
    const auto windows = opened->windows();
    ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
    ASSERT_FALSE(windows->empty());
    const auto crossed = windows->front().link_to(replacement_sessions->front());
    ASSERT_FALSE(crossed.has_value());
    EXPECT_EQ(crossed.error().kind, libtmux::FailureKind::validation);

    const std::error_code restored = replaced.restore();
    ASSERT_FALSE(restored) << restored.message();
  }
  const auto aliased = libtmux::test::same_socket_inode(selected, retained);
  ASSERT_TRUE(aliased.has_value()) << aliased.error();
  EXPECT_TRUE(*aliased);
}

// A `-S` selector under a parent directory that does not exist yet is a
// shape tmux itself cannot self-heal (unlike `-L`/no-selector, which get
// its own `tmux-<uid>/` auto-mkdir).
// Raw tmux prints "error creating <path> (No such file or directory)" to
// stderr and still exits 0, so the failure is invisible unless that stderr
// is read - the same quirk `StartableAtSocketNameSucceedsUnderAFreshTmuxTmpdir`
// guards for the auto-mkdir path. The previous message here gave no reason
// at all. The diagnostic has to carry tmux's reason when there is one and say
// there was none when there is not - never a bare message a reader cannot act
// on.
TEST(ServerIdentity, StartableAtSocketPathUnderAMissingParentSurfacesTmuxsOwnReason) {
  auto scratch = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(scratch.has_value()) << scratch.error();
  const std::filesystem::path selected =
      scratch->tmux_tmpdir() / "not-yet-created" / "mysock";

  auto opened = Server::startable_at_socket_path(selected.string(),
                                                 std::filesystem::path{"/dev/null"});
  ASSERT_TRUE(opened.has_value()) << opened.error().diagnostic;
  ServerCleanup cleanup{*opened};

  const auto created = opened->new_session("first");
  ASSERT_FALSE(created.has_value());
  EXPECT_FALSE(opened->is_alive());
  const bool explained =
      created.error().diagnostic.find("error creating") != std::string::npos ||
      created.error().diagnostic.find("tmux gave no reason") != std::string::npos;
  EXPECT_TRUE(explained) << created.error().diagnostic;
}

// The two servers really do use the same ids, which is what makes every
// refusal below load-bearing rather than theoretical.
TEST(ServerIdentity, TwoServersNumberTheirObjectsTheSameWay) {
  auto left = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(left.has_value()) << left.error();
  auto right = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(right.has_value()) << right.error();

  const auto here = connect(*left).panes();
  ASSERT_TRUE(here.has_value()) << here.error().diagnostic;
  const auto there = connect(*right).panes();
  ASSERT_TRUE(there.has_value()) << there.error().diagnostic;
  ASSERT_FALSE(here->empty());
  ASSERT_FALSE(there->empty());

  EXPECT_EQ(here->front().id().value(), there->front().id().value());
  // Same id, different servers, so not the same pane.
  EXPECT_NE(here->front(), there->front());
}

TEST(ServerIdentity, SwappingPanesAcrossServersIsRefusedRatherThanMisdirected) {
  auto left = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(left.has_value()) << left.error();
  auto right = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(right.has_value()) << right.error();

  const Server here = connect(*left);
  const Server there = connect(*right);
  const auto mine = here.panes();
  ASSERT_TRUE(mine.has_value()) << mine.error().diagnostic;
  const auto theirs = there.panes();
  ASSERT_TRUE(theirs.has_value()) << theirs.error().diagnostic;

  // Without the check this ran against `left` carrying `right`'s pane id,
  // found the pane of that id on `left`, swapped it, and answered success.
  const auto refused = mine->front().swap_with(theirs->front());
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, libtmux::FailureKind::validation);
  EXPECT_EQ(refused.error().delivery, libtmux::DeliveryStatus::not_started)
      << "a refusal before dispatch is what makes it safe to report";
  EXPECT_NE(refused.error().diagnostic.find("different tmux servers"),
            std::string::npos)
      << "diagnostic was: " << refused.error().diagnostic;
}

TEST(ServerIdentity, EveryCommandCombiningTwoValuesChecksTheirServers) {
  auto left = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(left.has_value()) << left.error();
  auto right = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(right.has_value()) << right.error();

  const Server here = connect(*left);
  const Server there = connect(*right);

  const auto my_sessions = here.sessions();
  ASSERT_TRUE(my_sessions.has_value()) << my_sessions.error().diagnostic;
  const auto their_sessions = there.sessions();
  ASSERT_TRUE(their_sessions.has_value()) << their_sessions.error().diagnostic;
  const auto my_windows = here.windows();
  ASSERT_TRUE(my_windows.has_value()) << my_windows.error().diagnostic;
  const auto their_windows = there.windows();
  ASSERT_TRUE(their_windows.has_value()) << their_windows.error().diagnostic;
  const auto my_panes = here.panes();
  ASSERT_TRUE(my_panes.has_value()) << my_panes.error().diagnostic;
  const auto their_panes = there.panes();
  ASSERT_TRUE(their_panes.has_value()) << their_panes.error().diagnostic;

  const auto refuses = [](const auto& outcome, std::string_view what) {
    ASSERT_FALSE(outcome.has_value()) << what << " should have been refused";
    EXPECT_EQ(outcome.error().kind, libtmux::FailureKind::validation) << what;
    EXPECT_EQ(outcome.error().delivery, libtmux::DeliveryStatus::not_started) << what;
  };

  refuses(my_windows->front().link_to(their_sessions->front()), "link_to");
  refuses(my_windows->front().swap_with(their_windows->front()), "window swap_with");
  refuses(my_panes->front().swap_with(their_panes->front()), "pane swap_with");
  refuses(my_panes->front().join(their_windows->front()), "join");

  ASSERT_TRUE(there.set_buffer("crossed", "content").has_value());
  const auto their_buffers = there.buffers();
  ASSERT_TRUE(their_buffers.has_value()) << their_buffers.error().diagnostic;
  ASSERT_FALSE(their_buffers->empty());
  refuses(my_panes->front().paste(their_buffers->front()), "paste");
}

// The same server reached twice is one server. Two `Server` values over one
// socket used to produce entities that compared unequal, because equality was
// which handle they came through rather than which tmux they name.
TEST(ServerIdentity, TwoHandlesOnOneSocketDescribeTheSameObjects) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();

  const Server first = connect(*fixture);
  const Server second = connect(*fixture);
  ASSERT_NE(&first, &second);

  const auto here = first.sessions();
  ASSERT_TRUE(here.has_value()) << here.error().diagnostic;
  const auto there = second.sessions();
  ASSERT_TRUE(there.has_value()) << there.error().diagnostic;
  ASSERT_FALSE(here->empty());
  ASSERT_FALSE(there->empty());

  EXPECT_EQ(here->front(), there->front());
  // Equal values hash alike, so one keys a container the other can look in.
  std::unordered_set<libtmux::Session> seen;
  seen.insert(here->front());
  EXPECT_EQ(seen.count(there->front()), 1U);

  // And a command combining them is allowed, because it is one server.
  const auto windows = first.windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  const auto other = second.new_session("elsewhere");
  ASSERT_TRUE(other.has_value()) << other.error().diagnostic;
  EXPECT_TRUE(windows->front().link_to(*other).has_value());
}

// Every command is dispatched through the private hard-link alias
// `pin_under` substitutes for the socket selector (the `sockaddr_un`
// length workaround), so a message tmux builds
// from its own invocation - "no server running on <socket>" - names that
// alias rather than the path the operator configured. Kill the server out
// from under a live handle so the next command's failure is tmux's own,
// genuinely mentioning a socket path, then check which path it names.
TEST(ServerIdentity, ADeadServerDiagnosticNamesTheOperatorsSocketNotTheAlias) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const std::string operator_path = fixture->socket_path().string();

  // Forces the alias to be pinned before the server dies, matching ordinary
  // use: a handle used at all before its server disappears.
  const auto before = server.sessions();
  ASSERT_TRUE(before.has_value()) << before.error().diagnostic;

  ASSERT_TRUE(server.kill().has_value());
  const auto stopped_by = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (fixture->is_alive() && std::chrono::steady_clock::now() < stopped_by) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  ASSERT_FALSE(fixture->is_alive());

  const auto after = server.sessions();
  ASSERT_FALSE(after.has_value());
  EXPECT_NE(after.error().diagnostic.find(operator_path), std::string::npos)
      << after.error().diagnostic;
  EXPECT_EQ(after.error().diagnostic.find("/.libtmux-"), std::string::npos)
      << after.error().diagnostic;
}

TEST(ServerIdentity, RestartAtTheSameSocketIsANewServer) {
  auto original = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(original.has_value()) << original.error();
  const std::filesystem::path socket = original->socket_path();

  const Server stale = connect(*original);
  const auto stale_sessions = stale.sessions();
  ASSERT_TRUE(stale_sessions.has_value()) << stale_sessions.error().diagnostic;
  const auto stale_windows = stale.windows();
  ASSERT_TRUE(stale_windows.has_value()) << stale_windows.error().diagnostic;
  ASSERT_FALSE(stale_sessions->empty());
  ASSERT_FALSE(stale_windows->empty());

  ASSERT_TRUE(stale.kill().has_value());
  const auto stopped_by = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (original->is_alive() && std::chrono::steady_clock::now() < stopped_by) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  ASSERT_FALSE(original->is_alive());
  std::error_code removed;
  ASSERT_TRUE(std::filesystem::remove(socket, removed));
  ASSERT_FALSE(removed) << removed.message();

  auto replacement = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(replacement.has_value()) << replacement.error();
  std::error_code linked;
  std::filesystem::create_hard_link(replacement->socket_path(), socket, linked);
  ASSERT_FALSE(linked) << linked.message();

  const auto current = Server::at_socket_path(socket.string());
  ASSERT_TRUE(current.has_value()) << current.error().diagnostic;
  const auto current_sessions = current->sessions();
  ASSERT_TRUE(current_sessions.has_value()) << current_sessions.error().diagnostic;
  ASSERT_FALSE(current_sessions->empty());
  EXPECT_EQ(stale_sessions->front().id().value(),
            current_sessions->front().id().value());
  EXPECT_NE(stale_sessions->front(), current_sessions->front());

  std::unordered_set<libtmux::Session> sessions;
  sessions.insert(stale_sessions->front());
  sessions.insert(current_sessions->front());
  EXPECT_EQ(sessions.size(), 2U);

  const auto renamed = stale_sessions->front().rename("stale-write");
  EXPECT_FALSE(renamed.has_value());
  const auto after_rename = current->sessions();
  ASSERT_TRUE(after_rename.has_value()) << after_rename.error().diagnostic;
  EXPECT_EQ(after_rename->front().name(), replacement->session_name());

  const auto destination = current->new_session("destination");
  ASSERT_TRUE(destination.has_value()) << destination.error().diagnostic;
  const auto crossed = stale_windows->front().link_to(*destination);
  ASSERT_FALSE(crossed.has_value());
  EXPECT_EQ(crossed.error().kind, libtmux::FailureKind::validation);
  EXPECT_EQ(crossed.error().delivery, libtmux::DeliveryStatus::not_started);
}

TEST(ServerIdentity, AHandleOpenedBeforeTheSocketExistsNeverAcquiresAServer) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto late_socket = fixture->tmux_tmpdir() / "late-socket";

  const auto stale = Server::at_socket_path(late_socket.string());
  ASSERT_TRUE(stale.has_value()) << stale.error().diagnostic;

  const auto creation = stale->new_session("must-not-start");
  ASSERT_FALSE(creation.has_value());
  EXPECT_EQ(creation.error().kind, libtmux::FailureKind::missing);
  EXPECT_EQ(creation.error().delivery, libtmux::DeliveryStatus::not_started);
  EXPECT_FALSE(stale->is_alive());

  const auto direct_control = stale->control("must-not-start");
  ASSERT_FALSE(direct_control.has_value());
  EXPECT_NE(direct_control.error().message.find("no socket"), std::string::npos);

  std::error_code linked;
  std::filesystem::create_hard_link(fixture->socket_path(), late_socket, linked);
  ASSERT_FALSE(linked) << linked.message();

  const auto refused = stale->sessions();
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, libtmux::FailureKind::missing);
  EXPECT_EQ(refused.error().delivery, libtmux::DeliveryStatus::not_started);
  EXPECT_NE(refused.error().diagnostic.find("reopen it after the server starts"),
            std::string::npos)
      << refused.error().diagnostic;

  const auto current = Server::at_socket_path(late_socket.string());
  ASSERT_TRUE(current.has_value()) << current.error().diagnostic;
  const auto sessions = current->sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  EXPECT_FALSE(sessions->empty());
}

// A path and the name that resolves to it select one server, so values from
// the two spellings are the same values. Comparing the argv instead called
// them two.
TEST(ServerIdentity, ANameAndThePathItResolvesToAreOneServer) {
  libtmux::test::ScopedTmuxServerOptions options;
  options.mode = libtmux::test::SocketMode::Name;
  auto fixture = libtmux::test::ScopedTmuxServer::start(std::move(options));
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto name = fixture->socket_name();
  ASSERT_TRUE(name.has_value());

  const libtmux::test::EnvironmentGuard tmpdir{"TMUX_TMPDIR",
                                               fixture->tmux_tmpdir().string()};
  const auto by_name = Server::at_socket_name(std::string{*name});
  ASSERT_TRUE(by_name.has_value());

  // Ask tmux where that name landed, then reach the same server by that path.
  const auto socket = by_name->expand("#{socket_path}");
  ASSERT_TRUE(socket.has_value()) << socket.error().diagnostic;
  const auto by_path = Server::at_socket_path(*socket);
  ASSERT_TRUE(by_path.has_value());

  const auto named = by_name->sessions();
  ASSERT_TRUE(named.has_value()) << named.error().diagnostic;
  const auto pathed = by_path->sessions();
  ASSERT_TRUE(pathed.has_value()) << pathed.error().diagnostic;
  ASSERT_FALSE(named->empty());
  ASSERT_FALSE(pathed->empty());

  EXPECT_EQ(named->front(), pathed->front())
      << "the same tmux reached two ways is the same tmux";
}

// The resolution here is a copy of tmux's, so tmux is the oracle: if a release
// changes where a label lands, this fails rather than the library quietly
// deciding two servers are one.
TEST(ServerIdentity, ResolutionAgreesWithTheRunningTmux) {
  libtmux::test::ScopedTmuxServerOptions options;
  options.mode = libtmux::test::SocketMode::Name;
  auto fixture = libtmux::test::ScopedTmuxServer::start(std::move(options));
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto name = fixture->socket_name();
  ASSERT_TRUE(name.has_value());

  const libtmux::test::EnvironmentGuard tmpdir{"TMUX_TMPDIR",
                                               fixture->tmux_tmpdir().string()};
  const auto server = Server::at_socket_name(std::string{*name});
  ASSERT_TRUE(server.has_value());

  const auto reported = server->expand("#{socket_path}");
  ASSERT_TRUE(reported.has_value()) << reported.error().diagnostic;

  // A path-selected server over the same socket must identify as the same
  // string the name-selected one resolved to, which is only true when the
  // resolution matched what tmux did.
  const auto twin = Server::at_socket_path(*reported);
  ASSERT_TRUE(twin.has_value());
  const auto sessions = server->sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  const auto twin_sessions = twin->sessions();
  ASSERT_TRUE(twin_sessions.has_value()) << twin_sessions.error().diagnostic;
  ASSERT_FALSE(sessions->empty());
  ASSERT_FALSE(twin_sessions->empty());
  EXPECT_EQ(sessions->front().connection_identity(),
            twin_sessions->front().connection_identity())
      << "resolved to " << sessions->front().connection_identity() << ", tmux says "
      << *reported;
}

} // namespace

// `Server::from_env()` — the one factory that reaches a server this process
// did not create.
//
// It reads `$TMUX`, which tmux sets inside every pane as
// `<socket path>,<server pid>,<session id>`. A socket path may itself contain
// a comma, so the split is at the last one that could begin the pid rather
// than the first found — and until these tests there was nothing holding that
// apart from the simpler reading that breaks on such a path.

TEST(ServerFromEnvironment, RefusesWhenNotRunningInsideTmux) {
  const libtmux::test::EnvironmentGuard outside{"TMUX", ""};
  const auto server = Server::from_env();
  ASSERT_FALSE(server.has_value());
  EXPECT_EQ(server.error().kind, libtmux::FailureKind::validation);
  EXPECT_EQ(server.error().delivery, libtmux::DeliveryStatus::not_started);
  EXPECT_NE(server.error().diagnostic.find("not running inside tmux"),
            std::string::npos)
      << server.error().diagnostic;
}

TEST(ServerFromEnvironment, RefusesAValueThatNamesNoSocket) {
  const libtmux::test::EnvironmentGuard inside{"TMUX", ",1,0"};
  const auto server = Server::from_env();
  ASSERT_FALSE(server.has_value());
  EXPECT_EQ(server.error().kind, libtmux::FailureKind::validation);
  EXPECT_NE(server.error().diagnostic.find("no socket path"), std::string::npos)
      << server.error().diagnostic;
}

TEST(ServerFromEnvironment, ReachesTheServerTheValueNames) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();

  const libtmux::test::EnvironmentGuard inside{
      "TMUX", fixture->socket_path().string() + "," +
                  std::to_string(fixture->server_pid()) + ",0"};
  const auto server = Server::from_env();
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;

  const auto sessions = server->sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  bool found = false;
  for (const libtmux::Session& session : *sessions) {
    found = found || session.name() == fixture->session_name();
  }
  EXPECT_TRUE(found) << "reached a server, but not the one $TMUX named";
}

// The case the split exists for. `mkdtemp` never produces a comma, so this
// path cannot arise by accident and would go unnoticed until someone's
// directory had one.
TEST(ServerFromEnvironment, ReadsASocketPathThatContainsACommaItself) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();

  const auto comma_directory = fixture->tmux_tmpdir() / "a,b";
  std::error_code created;
  std::filesystem::create_directory(comma_directory, created);
  ASSERT_FALSE(created) << created.message();

  const auto socket = comma_directory / "socket";
  std::error_code linked;
  std::filesystem::create_symlink(fixture->socket_path(), socket, linked);
  ASSERT_FALSE(linked) << linked.message();

  const libtmux::test::EnvironmentGuard inside{
      "TMUX", socket.string() + "," + std::to_string(fixture->server_pid()) + ",0"};
  const auto server = Server::from_env();
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  const auto sessions = server->sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  EXPECT_FALSE(sessions->empty());
}
