// Process behaviour pinned against the shipped library rather than the
// prototype that shares its kernel. These are the contracts a caller can
// observe through Server, which is the only surface that survives deletion of
// the transport spikes.
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/batch.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "support/descriptors.hpp"
#include "support/scoped_tmux_server.hpp"

namespace {

using libtmux::Server;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

TEST(ServerContract, ArgumentsReachTmuxWithoutAShell) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // A shell would expand these; exec does not.
  for (const std::string literal :
       {"$(echo pwned)", "a; echo pwned", "`id`", "a && echo pwned", "*"}) {
    const auto printed = server.run({"display-message", "-p", literal});
    ASSERT_TRUE(printed.has_value()) << printed.error().diagnostic;
    EXPECT_EQ(*printed, literal + "\n") << "argument was altered: " << literal;
  }
}

TEST(ServerContract, StdoutBytesSurviveUnchanged) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const std::string wide = "\xc3\xa4\xe2\x9d\xaf";
  const auto printed = server.run({"display-message", "-p", wide});
  ASSERT_TRUE(printed.has_value()) << printed.error().diagnostic;
  EXPECT_EQ(*printed, wide + "\n");
}

TEST(ServerContract, ANonzeroExitIsAReplyNotACrash) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto refused = server.run({"kill-session", "-t", "absent"});
  ASSERT_FALSE(refused.has_value());
  EXPECT_TRUE(refused.error().dispatched);
  EXPECT_GT(refused.error().exit_code, 0);
  EXPECT_FALSE(refused.error().diagnostic.empty());
}

TEST(ServerContract, AnUnknownSubcommandIsRefusedNotDispatchedBlindly) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto refused = server.run({"no-such-tmux-command"});
  ASSERT_FALSE(refused.has_value());
  EXPECT_TRUE(refused.error().dispatched);
  EXPECT_NE(refused.error().diagnostic.find("no-such-tmux-command"), std::string::npos);
}

TEST(ServerContract, AnUnreachableSocketFailsWithoutHanging) {
  const auto server = Server::at_socket_path("/nonexistent/libtmux/socket");
  ASSERT_TRUE(server.has_value());
  const auto refused = server->run({"list-sessions"});
  ASSERT_FALSE(refused.has_value());
  // tmux ran and could not connect; the process itself was dispatched.
  EXPECT_TRUE(refused.error().dispatched);
}

TEST(ServerContract, RepeatedRunsDoNotLeakDescriptors) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto count_open = libtmux::test::open_descriptor_count;
  ASSERT_TRUE(server.run({"display-message", "-p", "warm"}).has_value());
  const std::size_t before = count_open();
  for (int index = 0; index < 20; ++index) {
    ASSERT_TRUE(server.run({"display-message", "-p", "x"}).has_value());
  }
  EXPECT_EQ(count_open(), before);
}

TEST(ServerContract, ATimeoutIsItsOwnFailureNotARefusal) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // wait-for blocks until someone signals the channel, and nobody does.
  // attach-session is not usable here: without a terminal it refuses at once.
  const auto timed_out = server.run({"wait-for", "libtmux-never-signalled"},
                                    std::chrono::milliseconds{300});
  ASSERT_FALSE(timed_out.has_value());
  EXPECT_EQ(timed_out.error().kind, libtmux::FailureKind::timeout);
  // It reached tmux, so a caller must not assume nothing happened.
  EXPECT_TRUE(timed_out.error().dispatched);
}

TEST(ServerContract, ARefusalIsDistinguishableFromNeverRunning) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto refused = server.run({"kill-session", "-t", "absent"});
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, libtmux::FailureKind::refused);

  const auto empty = server.run_batch(libtmux::CommandBatch{});
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error().kind, libtmux::FailureKind::validation);
  EXPECT_FALSE(empty.error().dispatched);
}

} // namespace

TEST(ServerContract, TheVersionIsReadableWithoutAServer) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto running = server.tmux_version();
  ASSERT_TRUE(running.has_value()) << running.error().diagnostic;
  EXPECT_TRUE(libtmux::is_supported(*running));

  // `tmux -V` does not connect, so the answer survives the server's death.
  ASSERT_TRUE(server.kill().has_value());
  const auto after = server.tmux_version();
  ASSERT_TRUE(after.has_value()) << after.error().diagnostic;
  EXPECT_EQ(*after, *running);
}

TEST(ServerContract, LivenessIsAskedAndAnsweredWithoutThrowing) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  EXPECT_TRUE(server.is_alive());
  EXPECT_TRUE(server.check_alive().has_value());

  ASSERT_TRUE(server.kill().has_value());

  EXPECT_FALSE(server.is_alive());
  const auto dead = server.check_alive();
  ASSERT_FALSE(dead.has_value());
  // The reason is kept, which is the difference between the two questions.
  EXPECT_FALSE(dead.error().diagnostic.empty());
}

TEST(ServerContract, ASocketNobodyIsServingIsNotAlive) {
  // Inside the fixture's own directory, so the path is unique to this run and
  // is removed with it.
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto absent = fixture->socket_path().string() + ".absent";

  const auto server = Server::at_socket_path(absent);
  ASSERT_TRUE(server.has_value());
  EXPECT_FALSE(server->is_alive());
  EXPECT_FALSE(std::filesystem::exists(absent));
}

TEST(ServerContract, TheSeparatorSurvivesANonUnicodeLocale) {
  // tmux decides whether the terminal is UTF-8 from the environment, and a
  // tmux that decides it is not replaces the multi-byte field separator with
  // an underscore — at which point no row splits and every listing on the
  // server fails. The library passes -u so the answer does not depend on the
  // locale its caller happens to be running under.
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto argv = server.run({"display-message", "-p", "#{command}"});
  ASSERT_TRUE(argv.has_value()) << argv.error().diagnostic;

  const auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  EXPECT_EQ(sessions->size(), 1U);

  // The separator itself, round-tripped through tmux, comes back whole.
  const auto echoed =
      server.run({"display-message", "-p", std::string{libtmux::kFormatSeparator}});
  ASSERT_TRUE(echoed.has_value()) << echoed.error().diagnostic;
  EXPECT_EQ(*echoed, std::string{libtmux::kFormatSeparator} + "\n");
}

TEST(ServerContract, AnArgumentEndingInASeparatorIsNotACommandBoundary) {
  // tmux reads a trailing `;` on an argument as a command separator. Unescaped,
  // `set-option @v 'a;'` stores `a`, and in a batch whatever followed the
  // truncated argument becomes a command of its own — the argv for a two-member
  // batch ending in `kill-server` killed the server.
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  for (const std::string value : {"a;", "trailing;", "two;;", "back\\;"}) {
    ASSERT_TRUE(server.run({"set-option", "-g", "@probe", value}).has_value());
    const auto stored = server.run({"show-options", "-gv", "@probe"});
    ASSERT_TRUE(stored.has_value()) << stored.error().diagnostic;
    EXPECT_EQ(*stored, value + "\n") << "for " << value;
  }

  libtmux::CommandBatch batch;
  ASSERT_TRUE(batch.add({"set-option", "-g", "@first", "value;"}));
  ASSERT_TRUE(batch.add({"set-option", "-g", "@second", "kept"}));
  ASSERT_TRUE(server.run_batch(batch).has_value());
  EXPECT_TRUE(server.is_alive()) << "a batch member ran as its own command";
  const auto second = server.run({"show-options", "-gv", "@second"});
  ASSERT_TRUE(second.has_value()) << second.error().diagnostic;
  EXPECT_EQ(*second, "kept\n");
}

TEST(ServerContract, DataThatLooksLikeAFlagIsStillData) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  const libtmux::Session& session = sessions->at(0);

  ASSERT_TRUE(session.rename("-dashed").has_value());
  const auto renamed = session.refresh();
  ASSERT_TRUE(renamed.has_value()) << renamed.error().diagnostic;
  EXPECT_EQ(renamed->name(), "-dashed");

  const auto pane = session.active_pane();
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  EXPECT_TRUE(pane->send_text("-not-a-flag").has_value());
}

TEST(ServerContract, AnObserverSeesEveryCommandAndWhyOneFailed) {
  // Without this there is no way to find out what the library ran: a caller
  // debugging a tmux interaction has the failures and nothing else, and
  // nothing at all when things work.
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();

  std::vector<std::string> seen;
  std::vector<std::string> failed;
  auto server =
      Server::at_socket_path(fixture->socket_path().string(),
                             [&seen, &failed](std::string_view command,
                                              const libtmux::CommandFailure* failure) {
                               seen.emplace_back(command);
                               if (failure != nullptr) {
                                 failed.emplace_back(command);
                               }
                             });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;

  ASSERT_TRUE(server->sessions().has_value());
  ASSERT_FALSE(server->run({"kill-session", "-t", "absent"}).has_value());

  ASSERT_EQ(seen.size(), 2U);
  EXPECT_TRUE(seen.at(0).starts_with("list-sessions")) << seen.at(0);
  EXPECT_EQ(failed.size(), 1U);
  EXPECT_EQ(failed.at(0), "kill-session -t absent");
}

TEST(ServerContract, ARefusalNamesTheCommandAndCarriesNoStrayNewline) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto refused = server.run({"kill-session", "-t", "absent"});
  ASSERT_FALSE(refused.has_value());
  const std::string& diagnostic = refused.error().diagnostic;

  // What tmux said, and which command it said it about.
  EXPECT_NE(diagnostic.find("absent"), std::string::npos);
  EXPECT_NE(diagnostic.find("kill-session -t absent"), std::string::npos);
  // Both consumers put this straight into a message field.
  EXPECT_FALSE(diagnostic.ends_with("\n")) << diagnostic;
}

// Typed methods passed no deadline at all, so `window.rename(...)` against a
// tmux that never answers held the calling thread for the life of the process.
// "tmux is normally fast" is not a liveness guarantee; the policy is the floor
// under every call that did not name one of its own.
TEST(ServerContract, ATypedCallInheritsTheServersDeadline) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();

  const libtmux::ExecutionPolicy impatient{.timeout = std::chrono::milliseconds{150}};
  auto server = Server::at_socket_path(fixture->socket_path().string(), {}, impatient);
  ASSERT_TRUE(server.has_value());

  // `run-shell` without `-b` makes tmux wait for the command, so this is a
  // typed call that genuinely does not answer in time rather than one raced
  // against the clock.
  const auto started = std::chrono::steady_clock::now();
  const auto slow = server->run_shell("sleep 5");
  const auto elapsed = std::chrono::steady_clock::now() - started;

  ASSERT_FALSE(slow.has_value()) << "a 150ms deadline should not have been met";
  EXPECT_EQ(slow.error().kind, libtmux::FailureKind::timeout);
  // Reported as dispatched: tmux ran it, and what it did is not yet known.
  EXPECT_TRUE(slow.error().dispatched);
  EXPECT_LT(elapsed, std::chrono::seconds{3})
      << "the call outlived the deadline it was given";

  // The same server with a workable deadline answers, so the refusal above was
  // the policy and not a broken fixture.
  const libtmux::ExecutionPolicy patient{.timeout = std::chrono::seconds{20}};
  auto unhurried =
      Server::at_socket_path(fixture->socket_path().string(), {}, patient);
  ASSERT_TRUE(unhurried.has_value());
  const auto again = unhurried->sessions();
  ASSERT_TRUE(again.has_value()) << again.error().diagnostic;
  EXPECT_FALSE(again->empty());
}

// Waiting is the whole request, so `wait_for` is the one call the floor must
// not cut short. It reaches the transport directly for that reason.
TEST(ServerContract, WaitingOutlivesTheServersDeadline) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();

  const libtmux::ExecutionPolicy impatient{.timeout = std::chrono::milliseconds{1}};
  auto server = Server::at_socket_path(fixture->socket_path().string(), {}, impatient);
  ASSERT_TRUE(server.has_value());

  const auto started = std::chrono::steady_clock::now();
  const auto waited =
      server->wait_for("nobody-signals-this", std::chrono::milliseconds{300});
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_FALSE(waited.has_value());
  // The caller's 300ms, not the policy's 1ms.
  EXPECT_GE(elapsed, std::chrono::milliseconds{250})
      << "the policy cut short a wait the caller asked for";
}
