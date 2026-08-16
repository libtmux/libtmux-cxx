// The entity surface, dispatched over one open control connection.
//
// Nothing in these assertions is about control mode. That is the point: the
// same calls, against the same tmux, over a transport that launches no
// process per command.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/batch.hpp"
#include "libtmux/cardinality.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "support/environment_guard.hpp"
#include "support/scoped_tmux_server.hpp"

namespace {

using namespace std::chrono_literals;

using libtmux::first;
using libtmux::matching;
using libtmux::Server;
using libtmux::Session;
namespace window = libtmux::window;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

// One function that opens a streaming Server and then uses it, propagating
// either failure with a bare `return unexpected(...)`.
//
// This is the property, and it is enforced by compiling: two unrelated error
// types leave a caller doing both with nowhere to put the first one, and the
// way out is a hand conversion that discards the kind, the exit code, and
// whether tmux ran — the three things a caller needs to decide whether
// retrying is safe.
libtmux::expected<std::size_t, libtmux::CommandFailure>
count_windows_over_control(const Server& server, std::string_view session) {
  auto streamed = server.over_control(session);
  if (!streamed.has_value()) {
    return libtmux::unexpected(streamed.error());
  }
  auto windows = streamed->windows();
  if (!windows.has_value()) {
    return libtmux::unexpected(windows.error());
  }
  return windows->size();
}

TEST(ControlDispatch, OpeningAndUsingAStreamingServerShareOneErrorType) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();

  const auto counted =
      count_windows_over_control(connect(*fixture), fixture->session_name());
  ASSERT_TRUE(counted.has_value()) << counted.error().diagnostic;
  EXPECT_GE(*counted, 1U);
}

// The value of a socket name is where tmux resolves it to, and the library
// resolves it the same way tmux does — so a server selected by `-L`, or by
// nothing at all, reaches the faster transport too. It could not before: a
// control client is launched against a path, and only `-S` had one to hand it,
// which left the measured speedup behind whichever constructor a caller
// happened to pick.
//
// `TMUX_TMPDIR` is set here because it is what tmux reads and what the fixture
// started its server under. That is not a workaround, it is the arrangement a
// caller of `-L` has: the name means a path, and the path depends on the
// environment both ends share.
TEST(ControlDispatch, AServerSelectedByNameOpensAControlConnection) {
  libtmux::test::ScopedTmuxServerOptions options;
  options.mode = libtmux::test::SocketMode::Name;
  auto fixture = libtmux::test::ScopedTmuxServer::start(std::move(options));
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto name = fixture->socket_name();
  ASSERT_TRUE(name.has_value());

  const libtmux::test::EnvironmentGuard tmpdir{"TMUX_TMPDIR",
                                               fixture->tmux_tmpdir().string()};
  auto server = Server::at_socket_name(std::string{*name});
  ASSERT_TRUE(server.has_value());

  // It really is the fixture's server, not one the name found somewhere else.
  const auto socket = server->expand("#{socket_path}");
  ASSERT_TRUE(socket.has_value()) << socket.error().diagnostic;

  const auto streamed = server->over_control(fixture->session_name());
  ASSERT_TRUE(streamed.has_value()) << streamed.error().diagnostic;

  const auto sessions = streamed->sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  EXPECT_FALSE(sessions->empty());

  // And the two spellings of one server are one server, so values from the
  // subprocess side and the control side are the same objects.
  const auto over_subprocess = server->sessions();
  ASSERT_TRUE(over_subprocess.has_value()) << over_subprocess.error().diagnostic;
  EXPECT_EQ(over_subprocess->front(), sessions->front());
}

TEST(ControlDispatch, EveryEntityOperationWorksOverAConnection) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server subprocess = connect(*fixture);

  const auto streamed = subprocess.over_control(fixture->session_name());
  ASSERT_TRUE(streamed.has_value()) << streamed.error().diagnostic;

  const auto sessions = streamed->sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  ASSERT_EQ(sessions->size(), 1U);
  const Session& session = sessions->front();
  EXPECT_EQ(session.name(), fixture->session_name());

  const auto created = session.new_window("streamed");
  ASSERT_TRUE(created.has_value()) << created.error().diagnostic;
  EXPECT_EQ(created->name(), "streamed");

  const auto added = created->split();
  ASSERT_TRUE(added.has_value()) << added.error().diagnostic;
  EXPECT_EQ(added->window_id(), created->id());

  const auto panes = created->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  EXPECT_EQ(panes->size(), 2U);

  auto named = *streamed->windows() | matching(window::name == "streamed");
  EXPECT_TRUE(first(named).has_value());

  ASSERT_TRUE(created->rename("renamed").has_value());
  const auto refreshed = created->refresh();
  ASSERT_TRUE(refreshed.has_value()) << refreshed.error().diagnostic;
  EXPECT_EQ(refreshed->name(), "renamed");

  // The work is visible to the other transport, because it is the same tmux.
  const auto seen = subprocess.windows();
  ASSERT_TRUE(seen.has_value()) << seen.error().diagnostic;
  EXPECT_EQ(seen->size(), 2U);
}

TEST(ControlDispatch, TheStreamThatJustifiesTheOpenConnectionIsReadable) {
  // Notifications are the one thing a persistent connection can do that a
  // process per command cannot. Discarding them leaves the connection paying
  // for a capability nobody can use.
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server subprocess = connect(*fixture);
  const auto streamed = subprocess.over_control(fixture->session_name());
  ASSERT_TRUE(streamed.has_value()) << streamed.error().diagnostic;

  // A transport that hears nothing between commands says so.
  EXPECT_TRUE(subprocess.take_notifications().empty());
  EXPECT_EQ(subprocess.dropped_notifications(), 0U);

  const auto sessions = streamed->sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  ASSERT_TRUE(sessions->at(0).new_window("watched").has_value());

  const auto text = [](const libtmux::Notification& notification) {
    std::string out;
    for (const std::byte byte : notification.body) {
      out.push_back(static_cast<char>(byte));
    }
    return out;
  };

  // Taking drains, so a caller watching for something accumulates rather than
  // replacing: the batch that happens to be ready first is whatever tmux said
  // when the connection attached.
  std::vector<libtmux::Notification> heard;
  bool announced = false;
  for (int attempt = 0; attempt < 200 && !announced; ++attempt) {
    for (libtmux::Notification& notification : streamed->take_notifications()) {
      announced = announced || text(notification).starts_with("%window-add");
      heard.push_back(std::move(notification));
    }
    if (!announced) {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
  }
  ASSERT_FALSE(heard.empty()) << "the connection heard nothing at all";
  EXPECT_TRUE(announced) << "nothing in the stream announced the new window";

  // Nothing was dropped at this volume, and what was taken is not handed out
  // a second time.
  EXPECT_EQ(streamed->dropped_notifications(), 0U);
  EXPECT_TRUE(streamed->take_notifications().empty());
}

TEST(ControlDispatch, AFailureCarriesWhatTmuxSaid) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server subprocess = connect(*fixture);
  const auto streamed = subprocess.over_control(fixture->session_name());
  ASSERT_TRUE(streamed.has_value()) << streamed.error().diagnostic;

  const auto refused = streamed->run({"kill-session", "-t", "absent"});
  ASSERT_FALSE(refused.has_value());
  EXPECT_TRUE(refused.error().dispatched);
  EXPECT_NE(refused.error().diagnostic.find("absent"), std::string::npos);

  // A missing object is still a missing object over this transport.
  const auto gone = streamed->window("@999");
  ASSERT_FALSE(gone.has_value());
  EXPECT_EQ(gone.error().kind, libtmux::FailureKind::missing);
}

TEST(ControlDispatch, TheVersionIsAskedForAsAFormat) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server subprocess = connect(*fixture);
  const auto streamed = subprocess.over_control(fixture->session_name());
  ASSERT_TRUE(streamed.has_value()) << streamed.error().diagnostic;

  // `tmux -V` is a flag of the binary, so the two transports have to ask
  // different questions and must still agree on the answer.
  const auto over_pipe = subprocess.tmux_version();
  const auto over_connection = streamed->tmux_version();
  ASSERT_TRUE(over_pipe.has_value()) << over_pipe.error().diagnostic;
  ASSERT_TRUE(over_connection.has_value()) << over_connection.error().diagnostic;
  EXPECT_EQ(*over_pipe, *over_connection);
}

TEST(ControlDispatch, ConcurrentCallersShareOneConversation) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server subprocess = connect(*fixture);
  const auto streamed = subprocess.over_control(fixture->session_name());
  ASSERT_TRUE(streamed.has_value()) << streamed.error().diagnostic;

  // One connection matches replies to commands by order, so the transport has
  // to serialize. Four threads asking at once must all get their own answer.
  constexpr int kThreads = 4;
  constexpr int kEach = 5;
  std::vector<std::thread> callers;
  std::atomic<int> answered{0};
  for (int thread = 0; thread < kThreads; ++thread) {
    callers.emplace_back([&streamed, &answered] {
      for (int round = 0; round < kEach; ++round) {
        const auto listed = streamed->sessions();
        if (listed.has_value() && listed->size() == 1U) {
          answered.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& caller : callers) {
    caller.join();
  }
  EXPECT_EQ(answered.load(), kThreads * kEach);
}

TEST(ControlDispatch, EveryCommandInABatchRunsOverAControlConnection) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  auto server = Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  auto streamed = server->over_control(fixture->session_name());
  ASSERT_TRUE(streamed.has_value()) << streamed.error().diagnostic;

  // Counted, not assumed. Flattened to one argv the batch separator is
  // escaped like any other byte, so tmux read it as an argument to the first
  // command, ran that alone, and reported success — fifty windows asked for
  // and one made, with nothing to say so.
  constexpr int kWanted = 50;
  const std::string target = "=" + std::string{fixture->session_name()};
  libtmux::CommandBatch batch;
  for (int index = 0; index < kWanted; ++index) {
    ASSERT_TRUE(batch.add(
        {"new-window", "-d", "-t", target, "-n", "b" + std::to_string(index)}));
  }

  const auto ran = streamed->run_batch(batch);
  ASSERT_TRUE(ran.has_value()) << ran.error().diagnostic;

  const auto session = streamed->session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto windows = session->windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  // The one the session started with, and every one asked for.
  EXPECT_EQ(windows->size(), static_cast<std::size_t>(kWanted) + 1U);
}

TEST(ControlDispatch, ABatchSaysWhichCommandStoppedIt) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  auto server = Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  auto streamed = server->over_control(fixture->session_name());
  ASSERT_TRUE(streamed.has_value()) << streamed.error().diagnostic;

  // Fail-fast, and control mode gives each command its own reply, so which
  // one stopped the group is knowable here where the subprocess transport
  // can only report one status for all of them.
  const std::string target = "=" + std::string{fixture->session_name()};
  libtmux::CommandBatch batch;
  ASSERT_TRUE(batch.add({"new-window", "-d", "-t", target, "-n", "first"}));
  ASSERT_TRUE(batch.add({"kill-window", "-t", "=no-such-session:99"}));
  ASSERT_TRUE(batch.add({"new-window", "-d", "-t", target, "-n", "never"}));

  const auto ran = streamed->run_batch(batch);
  ASSERT_FALSE(ran.has_value());
  EXPECT_EQ(ran.error().kind, libtmux::FailureKind::refused);
  EXPECT_NE(ran.error().diagnostic.find("command 2 of 3"), std::string::npos)
      << ran.error().diagnostic;

  // And it stopped there: the first ran, the third did not.
  const auto session = streamed->session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto windows = session->windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  const auto named = [&windows](std::string_view wanted) {
    return std::ranges::any_of(*windows, [wanted](const libtmux::Window& one) {
      return one.name() == wanted;
    });
  };
  EXPECT_TRUE(named("first"));
  EXPECT_FALSE(named("never"));
}

TEST(ControlDispatch, AFloodIsDroppedOldestFirstAndTheCommandChannelSurvives) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  auto server = Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());
  auto streamed = server->over_control(fixture->session_name());
  ASSERT_TRUE(streamed.has_value()) << streamed.error().diagnostic;

  // `%session-changed` arrives once, when the connection attaches. If the
  // bound evicts oldest-first then a big enough flood must push it out, and
  // that is a cleaner marker than a window id: there is exactly one of it and
  // it is the first thing said.
  const auto text = [](const libtmux::Notification& one) {
    return std::string{reinterpret_cast<const char*>(one.body.data()), one.body.size()};
  };
  const auto says = [&text](const std::vector<libtmux::Notification>& all,
                            std::string_view wanted) {
    return std::ranges::any_of(all, [&](const libtmux::Notification& one) {
      return text(one).find(wanted) != std::string::npos;
    });
  };

  // Renaming emits one notification per command and costs tmux almost
  // nothing, where creating and killing a window costs a process each. The
  // buffer holds 4096, so this goes past it without taking a minute to do it.
  const std::string target = "=" + std::string{fixture->session_name()};
  constexpr int kBatches = 11;
  constexpr int kPerBatch = 400;
  for (int batch_number = 0; batch_number < kBatches; ++batch_number) {
    libtmux::CommandBatch batch;
    for (int renamed = 0; renamed < kPerBatch; ++renamed) {
      ASSERT_TRUE(batch.add(
          {"rename-window", "-t", target,
           "flood-" + std::to_string(batch_number) + "-" + std::to_string(renamed)}));
    }
    const auto ran = streamed->run_batch(batch);
    ASSERT_TRUE(ran.has_value()) << ran.error().diagnostic;
  }

  // A batch returns when tmux has run it, which is before the reader has
  // taken everything tmux then said. Waited for rather than sampled: reading
  // the count straight away measures the reader's head start, not the bound.
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  while (streamed->dropped_notifications() == 0U &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_GT(streamed->dropped_notifications(), 0U);

  const auto held = streamed->take_notifications();
  // The oldest went first, so the one thing said at the start is gone.
  EXPECT_FALSE(says(held, "%session-changed"));
  EXPECT_TRUE(says(held, "%window-renamed"));
  // Exactly the bound: full, and no more than full.
  EXPECT_EQ(held.size(), 4096U);

  // And the point of bounding rather than failing: the connection still
  // answers. Dropping degrades what tmux said, not what it will do.
  const auto after = streamed->sessions();
  ASSERT_TRUE(after.has_value()) << after.error().diagnostic;
  EXPECT_FALSE(after->empty());
  const auto expanded = streamed->expand("#{socket_path}");
  ASSERT_TRUE(expanded.has_value()) << expanded.error().diagnostic;
  EXPECT_EQ(*expanded, fixture->socket_path().string());
}

// `CommandObserver` promises it runs with nothing held, so that an observer
// which itself talks to tmux does not deadlock against the call that told it.
// Over one serialized connection that promise is load-bearing rather than
// decorative: the observer used to run inside the lock the next command needs,
// and an observer is exactly where someone reaches for tmux.
//
// A regression hangs, so this test carries the suite's ctest timeout rather
// than a wait of its own — there is nothing to time out against once the
// thread that would report is the thread that is stuck.
TEST(ControlDispatch, AnObserverMayCallTmuxAgainOverTheSameConnection) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();

  const Server* streaming = nullptr;
  int reentered = 0;
  std::size_t observed = 0;
  auto observer = [&streaming, &reentered, &observed](std::string_view,
                                                      const libtmux::CommandFailure*) {
    ++observed;
    // Once. The nested call is observed too, and an observer that recursed on
    // its own report would never come back.
    if (streaming == nullptr || reentered > 0) {
      return;
    }
    ++reentered;
    const auto nested = streaming->sessions();
    EXPECT_TRUE(nested.has_value());
  };

  auto server = Server::at_socket_path(fixture->socket_path().string(), observer);
  ASSERT_TRUE(server.has_value());
  const auto streamed = server->over_control(fixture->session_name());
  ASSERT_TRUE(streamed.has_value()) << streamed.error().diagnostic;
  streaming = &*streamed;

  const auto windows = streamed->windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  EXPECT_FALSE(windows->empty());
  EXPECT_EQ(reentered, 1);
  // The outer command and the one the observer ran from inside it.
  EXPECT_GE(observed, 2U);
}

// Every command reaches the observer, including the ones that failed before
// tmux answered. Those are the commands a caller turned the observer on for.
TEST(ControlDispatch, AFailedCommandReachesTheObserver) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();

  std::vector<std::string> failures;
  auto observer = [&failures](std::string_view command,
                              const libtmux::CommandFailure* failure) {
    if (failure != nullptr) {
      failures.emplace_back(command);
    }
  };

  auto server = Server::at_socket_path(fixture->socket_path().string(), observer);
  ASSERT_TRUE(server.has_value());

  // Refused by tmux, over both transports, so neither can drop it quietly.
  EXPECT_FALSE(server->run({"kill-session", "-t", "$99999"}).has_value());
  const auto streamed = server->over_control(fixture->session_name());
  ASSERT_TRUE(streamed.has_value()) << streamed.error().diagnostic;
  EXPECT_FALSE(streamed->run({"kill-session", "-t", "$99999"}).has_value());

  EXPECT_EQ(failures.size(), 2U);
}

} // namespace
