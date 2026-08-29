// Process behaviour pinned against the shipped library rather than the
// prototype that shares its kernel. These are the contracts a caller can
// observe through Server, which is the only surface that survives deletion of
// the transport spikes.
#include <chrono>
#include <filesystem>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/batch.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "libtmux/testing/scoped_server.hpp"
#include "support/descriptors.hpp"

namespace {

using libtmux::DeliveryStatus;
using libtmux::Server;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

// Every typed call now crosses the engine's threads, so a Server shared
// between callers is a Server whose commands are interleaved rather than
// serialised behind one another. Each answer must still be the answer to the
// question that caller asked.
// Submitting sends the command and keeps the answer for later, so a program
// with several questions asks them all before collecting any. Each answer must
// still belong to the question it was asked for.
TEST(ServerContract, SubmittedCommandsAreCollectedLater) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  constexpr int asked = 8;
  std::vector<libtmux::CommandOperation> sent;
  sent.reserve(asked);
  for (int index = 0; index < asked; ++index) {
    auto submitted =
        server.submit({"display-message", "-p", "asked-" + std::to_string(index)});
    ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
    sent.push_back(*std::move(submitted));
  }

  for (int index = 0; index < asked; ++index) {
    auto answer = std::move(sent[static_cast<std::size_t>(index)]).wait();
    ASSERT_TRUE(answer.has_value()) << answer.error().diagnostic;
    EXPECT_EQ(*answer, "asked-" + std::to_string(index) + "\n");
  }
}

// What `run` bounds per call, `submit` bounds per call too. The one long
// question in a batch is exactly the one needing a bound of its own, and a
// submission that could only take the server-wide default would leave a
// caller opening a second server to ask it.
TEST(ServerContract, ASubmissionTakesTheBoundTheCallerGaveIt) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  auto submitted = server.submit({"display-message", "-p", "longer than one byte"},
                                 {}, std::size_t{1});
  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
  auto answer = std::move(*submitted).wait();

  ASSERT_FALSE(answer.has_value()) << "the call's own bound was not applied";
  EXPECT_EQ(answer.error().kind, libtmux::FailureKind::truncated);
  EXPECT_EQ(answer.error().delivery, DeliveryStatus::replied);
  // Which bound was passed, not merely that one was. The diagnostic names the
  // number a caller has to change, so naming the server's instead sends them
  // to a setting that was never in force.
  EXPECT_NE(answer.error().diagnostic.find("the 1 byte limit"), std::string::npos)
      << answer.error().diagnostic;
}

// A submitted command reports what tmux said about it, not merely that it ran.
TEST(ServerContract, ASubmittedFailureCarriesWhatTmuxSaid) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  auto submitted = server.submit({"kill-session", "-t", "no-such-session"});
  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
  const auto answer = (*std::move(submitted)).wait();

  ASSERT_FALSE(answer.has_value());
  EXPECT_EQ(answer.error().kind, libtmux::FailureKind::refused);
  EXPECT_NE(answer.error().diagnostic.find("no-such-session"), std::string::npos);
}

TEST(ServerContract, ConcurrentCallersEachGetTheirOwnAnswer) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  constexpr int callers = 12;
  std::vector<std::string> answers(callers);
  std::vector<std::thread> asking;
  asking.reserve(callers);
  std::latch ready{callers};
  for (int index = 0; index < callers; ++index) {
    asking.emplace_back([&, index] {
      const std::string mine = "caller-" + std::to_string(index);
      ready.arrive_and_wait();
      const auto printed = server.run({"display-message", "-p", mine});
      if (printed.has_value()) {
        answers[static_cast<std::size_t>(index)] = *printed;
      }
    });
  }
  for (auto& thread : asking) {
    thread.join();
  }

  for (int index = 0; index < callers; ++index) {
    EXPECT_EQ(answers[static_cast<std::size_t>(index)],
              "caller-" + std::to_string(index) + "\n");
  }
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
  EXPECT_EQ(refused.error().delivery, DeliveryStatus::replied);
  EXPECT_GT(refused.error().exit_code, 0);
  EXPECT_FALSE(refused.error().diagnostic.empty());
}

TEST(ServerContract, AnUnknownSubcommandIsRefusedNotDispatchedBlindly) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto refused = server.run({"no-such-tmux-command"});
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().delivery, DeliveryStatus::replied);
  EXPECT_NE(refused.error().diagnostic.find("no-such-tmux-command"), std::string::npos);
}

TEST(ServerContract, AnUnreachableSocketFailsWithoutHanging) {
  const auto server = Server::at_socket_path("/nonexistent/libtmux/socket");
  ASSERT_TRUE(server.has_value());
  const auto refused = server->run({"list-sessions"});
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, libtmux::FailureKind::missing);
  EXPECT_EQ(refused.error().delivery, DeliveryStatus::not_started);
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
  // The child started, but the transport cannot prove whether the command
  // reached tmux before it was terminated.
  EXPECT_EQ(timed_out.error().delivery, DeliveryStatus::indeterminate);
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
  EXPECT_EQ(empty.error().delivery, libtmux::DeliveryStatus::not_started);
}

} // namespace

TEST(ServerContract, TheVersionIsReadableWithoutAServer) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto running = server.tmux_version();
  ASSERT_TRUE(running.has_value()) << running.error().diagnostic;
  EXPECT_TRUE(libtmux::is_supported(*running));

  const auto absent =
      Server::at_socket_path((fixture->tmux_tmpdir() / "absent-version").string());
  ASSERT_TRUE(absent.has_value()) << absent.error().diagnostic;
  const auto without_server = absent->tmux_version();
  ASSERT_TRUE(without_server.has_value()) << without_server.error().diagnostic;
  EXPECT_EQ(*without_server, *running);

  // `tmux -V` does not connect, so the answer survives the server's death.
  ASSERT_TRUE(server.kill().has_value());
  const auto after = server.tmux_version();
  ASSERT_TRUE(after.has_value()) << after.error().diagnostic;
  EXPECT_EQ(*after, *running);
}

TEST(ServerContract, SubprocessVersionHonoursTheServersOutputBound) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const libtmux::ExecutionPolicy bounded{.output_limit = 1U};
  auto server = Server::at_socket_path(fixture->socket_path().string(), {}, bounded);
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;

  const auto version = server->tmux_version();

  ASSERT_FALSE(version.has_value());
  EXPECT_EQ(version.error().kind, libtmux::FailureKind::truncated);
  EXPECT_EQ(version.error().delivery, DeliveryStatus::replied);
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

TEST(ServerContract, AnObserverNeverSeesAnEnvironmentValue) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();

  std::vector<std::string> seen;
  std::vector<std::string> diagnostics;
  auto server = Server::at_socket_path(
      fixture->socket_path().string(),
      [&seen, &diagnostics](std::string_view command,
                            const libtmux::CommandFailure* failure) {
        seen.emplace_back(command);
        if (failure != nullptr) {
          diagnostics.push_back(failure->diagnostic);
        }
      });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;

  constexpr std::string_view secret = "known-only-to-tmux-$[] with spaces;";
  libtmux::CommandRequest raw{"display-message", "-p"};
  raw.push_back(libtmux::CommandArgument::sensitive(std::string{secret}));
  const auto printed = server->run(raw);
  ASSERT_TRUE(printed.has_value()) << printed.error().diagnostic;
  EXPECT_EQ(*printed, std::string{secret} + "\n");

  libtmux::NewSessionOptions options;
  options.name = "redacted";
  options.environment = {{"LIBTMUX_SECRET", std::string{secret}}};
  const auto session = server->new_session(std::move(options));
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;

  const auto set_option = server->set_global_option("@secret", secret);
  ASSERT_TRUE(set_option.has_value()) << set_option.error().diagnostic;

  libtmux::CommandBatch batch;
  ASSERT_TRUE(batch.add({"display-message", "-p", "public-prefix"}));
  ASSERT_TRUE(batch.add(raw));
  const auto batched = server->run_batch(batch);
  ASSERT_TRUE(batched.has_value()) << batched.error().diagnostic;
  EXPECT_EQ(*batched, "public-prefix\n" + std::string{secret} + "\n");
  const std::string shell_secret{"shell-secret with spaces"};
  const std::string shell_command =
      "tmux set-option -g @shell-secret '" + shell_secret + "'";
  ASSERT_TRUE(server->run_shell(shell_command).has_value());
  const auto shell_value = server->run({"show-options", "-gv", "@shell-secret"});
  ASSERT_TRUE(shell_value.has_value()) << shell_value.error().diagnostic;
  EXPECT_EQ(*shell_value, shell_secret + "\n");

  const std::string hook_command = "display-message '" + shell_secret + "'";
  ASSERT_TRUE(server->set_global_hook("alert-bell", hook_command).has_value());
  const auto pane = session->active_pane();
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  ASSERT_TRUE(pane->pipe_to("cat >/dev/null # " + shell_secret).has_value());
  ASSERT_TRUE(pane->stop_piping().has_value());

  libtmux::CommandRequest failing{"kill-session", "-t"};
  failing.push_back(libtmux::CommandArgument::sensitive(std::string{secret}));
  const auto refused = server->run(failing);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().diagnostic.find(secret), std::string::npos)
      << refused.error().diagnostic;
  libtmux::CommandBatch failing_batch;
  ASSERT_TRUE(failing_batch.add({"display-message", "-p", "safe-before-failure"}));
  ASSERT_TRUE(failing_batch.add(failing));
  const auto refused_batch = server->run_batch(failing_batch);
  ASSERT_FALSE(refused_batch.has_value());
  EXPECT_EQ(refused_batch.error().diagnostic.find(secret), std::string::npos)
      << refused_batch.error().diagnostic;
  const auto value = server->run(
      {"show-environment", "-t", std::string{session->id()}, "LIBTMUX_SECRET"});
  ASSERT_TRUE(value.has_value()) << value.error().diagnostic;
  EXPECT_EQ(*value, "LIBTMUX_SECRET=" + std::string{secret} + "\n");

  bool replaced = false;
  for (const std::string& command : seen) {
    EXPECT_EQ(command.find(secret), std::string::npos) << command;
    EXPECT_EQ(command.find(shell_secret), std::string::npos) << command;
    replaced = replaced || command.find("[REDACTED]") != std::string::npos;
  }
  EXPECT_TRUE(replaced);
  for (const std::string& diagnostic : diagnostics) {
    EXPECT_EQ(diagnostic.find(secret), std::string::npos) << diagnostic;
    EXPECT_EQ(diagnostic.find(shell_secret), std::string::npos) << diagnostic;
  }
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
  EXPECT_EQ(slow.error().delivery, DeliveryStatus::indeterminate);
  EXPECT_LT(elapsed, std::chrono::seconds{3})
      << "the call outlived the deadline it was given";

  // The same server with a workable deadline answers, so the refusal above was
  // the policy and not a broken fixture.
  const libtmux::ExecutionPolicy patient{.timeout = std::chrono::seconds{20}};
  auto unhurried = Server::at_socket_path(fixture->socket_path().string(), {}, patient);
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
