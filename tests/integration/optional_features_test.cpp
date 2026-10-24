// Optional tmux features and the releases that cannot do them safely.
//
// The core floor is 3.3a; these operations need more, and each is refused
// before it has an effect when the daemon is older. Every test runs against
// whichever tmux is on PATH, so a lane exercises the refusal below the
// threshold and the supported behaviour at or above it; the sentinel state
// proves that a refusal changed nothing.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "libtmux/control.hpp"
#include "libtmux/server.hpp"
#include "libtmux/testing/scoped_server.hpp"
#include "libtmux/version.hpp"

namespace {

using libtmux::DeliveryStatus;
using libtmux::FailureKind;
using libtmux::Server;
using libtmux::Version;

struct Lane {
  libtmux::test::ScopedTmuxServer fixture;
  Server server;
  Version running;
};

Lane start_lane() {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  EXPECT_TRUE(fixture.has_value()) << (fixture ? "" : fixture.error());
  auto server = Server::at_socket_path(fixture->socket_path().string());
  EXPECT_TRUE(server.has_value());
  const auto version = server->tmux_version();
  EXPECT_TRUE(version.has_value());
  return Lane{std::move(*fixture), *std::move(server), *version};
}

TEST(OptionalFeatures, ControlModeIsRefusedBeforeAnyClientAttachesBelow37) {
  Lane lane = start_lane();
  libtmux::ConnectionOptions options;
  options.socket_path = lane.fixture.socket_path();
  options.session_name = std::string{lane.fixture.session_name()};
  auto connection = libtmux::Connection::connect(options);

  if (lane.running >= Version{.major = 3, .minor = 7}) {
    ASSERT_TRUE(connection.has_value()) << connection.error().message;
    EXPECT_TRUE(
        connection->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds{2})
            .has_value());
    return;
  }
  ASSERT_FALSE(connection.has_value());
  EXPECT_NE(connection.error().message.find("3.7"), std::string::npos);
  EXPECT_EQ(connection.error().delivery, DeliveryStatus::not_started);
  const auto clients = lane.server.clients();
  ASSERT_TRUE(clients.has_value());
  EXPECT_TRUE(clients->empty()) << "a refused control attach must not have attached";

  // The explicit Server entry points share the refusal.
  EXPECT_FALSE(lane.server.control(lane.fixture.session_name()).has_value());
  EXPECT_FALSE(lane.server.over_control(lane.fixture.session_name()).has_value());
  EXPECT_TRUE(lane.server.clients()->empty());
}

TEST(OptionalFeatures, WaitForTextOnlyStreamsFrom37AndPollsBefore) {
  Lane lane = start_lane();
  auto pane = lane.server.pane(lane.fixture.session_name());
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  const std::string command = "sh -c 'sleep 0.5; echo streamed-$(echo marker)'";
  ASSERT_TRUE(pane->send_line(command).has_value());

  int attached = 0;
  libtmux::WaitOptions options;
  options.timeout = std::chrono::seconds{8};
  options.sent = [&command](std::string_view) {
    return std::vector<std::string>{command};
  };
  options.observe_control_client = [&attached](std::int64_t) {
    ++attached;
    return std::shared_ptr<void>{};
  };
  const auto waited = pane->wait_for_text("streamed-marker", options);
  ASSERT_TRUE(waited.has_value()) << waited.error().diagnostic;
  EXPECT_TRUE(waited->matched) << waited->text;
  if (lane.running < Version{.major = 3, .minor = 7}) {
    EXPECT_EQ(attached, 0) << "a control client must not attach below 3.7";
    EXPECT_EQ(waited->path, libtmux::WaitPath::capture_polling);
  }
}

TEST(OptionalFeatures, NamedBufferDeletionNeeds34) {
  Lane lane = start_lane();
  ASSERT_TRUE(lane.server.set_buffer("keep-top", "top").has_value());
  ASSERT_TRUE(lane.server.set_buffer("victim", "doomed").has_value());
  auto buffers = lane.server.buffers();
  ASSERT_TRUE(buffers.has_value());
  std::optional<libtmux::Buffer> victim;
  for (const auto& buffer : *buffers) {
    if (buffer.name() == "victim") {
      victim = buffer;
    }
  }
  ASSERT_TRUE(victim.has_value());
  const auto before = buffers->size();

  const auto removed = victim->remove();
  if (lane.running >= Version{.major = 3, .minor = 4}) {
    EXPECT_TRUE(removed.has_value()) << (removed ? "" : removed.error().diagnostic);
    EXPECT_EQ(lane.server.buffers()->size(), before - 1U);
    return;
  }
  ASSERT_FALSE(removed.has_value());
  EXPECT_EQ(removed.error().kind, FailureKind::unsupported);
  EXPECT_EQ(removed.error().delivery, DeliveryStatus::not_started);
  EXPECT_NE(removed.error().diagnostic.find("3.4"), std::string::npos);
  EXPECT_EQ(lane.server.buffers()->size(), before) << "no buffer may be deleted";
}

TEST(OptionalFeatures, DotsAndColonsInNamesAreRefusedOnExactly37) {
  Lane lane = start_lane();
  const auto created = lane.server.new_session("dotted.name");
  const bool exact_37 = lane.running == Version{.major = 3, .minor = 7};
  if (exact_37) {
    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().kind, FailureKind::validation);
    EXPECT_EQ(created.error().delivery, DeliveryStatus::not_started);
    EXPECT_NE(created.error().diagnostic.find("3.7"), std::string::npos);
    EXPECT_EQ(lane.server.sessions()->size(), 1U);
    const auto window = lane.server.sessions()->front().new_window(
        libtmux::NewWindowOptions{.name = "a:b"});
    ASSERT_FALSE(window.has_value());
    EXPECT_EQ(window.error().kind, FailureKind::validation);
  } else if (!created.has_value()) {
    // tmux may itself decline on another release; this library must not.
    EXPECT_EQ(created.error().diagnostic.find("tmux 3.7 refuses"), std::string::npos);
  }
  // A plain name is accepted on every release.
  EXPECT_TRUE(lane.server.new_session("plain-name").has_value());
}

// Every spelling tmux accepts for an unnamed break-pane, each followed by a
// check that the server answers and that no window appeared.
TEST(OptionalFeatures, AnUnnamedRawBreakPaneIsRefusedOnExactly37) {
  Lane lane = start_lane();
  ASSERT_TRUE(lane.server.run({"split-window", "-d"}).has_value());
  auto pane = lane.server.pane(lane.fixture.session_name());
  ASSERT_TRUE(pane.has_value());
  const std::string source{pane->id().value()};

  const std::vector<std::vector<std::string>> spellings{
      {"break-pane", "-d", "-s", source},
      {"breakp", "-d", "-s", source},
      {"break-p", "-d", "-s", source},
      {"break", "-d", "-s", source},
      {"split-window", "-d", ";", "break-p", "-d", "-s", source},
      {"split-window", "-d;", "breakp", "-d", "-s", source},
  };
  const bool exact_37 = lane.running == Version{.major = 3, .minor = 7};
  for (const auto& spelling : spellings) {
    const auto before = lane.server.windows();
    ASSERT_TRUE(before.has_value());
    const auto raw = lane.server.run(spelling);
    if (!exact_37) {
      // Other releases are not this library's to refuse.
      EXPECT_TRUE(raw.has_value() || raw.error().kind != FailureKind::validation)
          << spelling.front();
      return;
    }
    ASSERT_FALSE(raw.has_value()) << spelling.front();
    EXPECT_EQ(raw.error().kind, FailureKind::validation) << spelling.front();
    EXPECT_EQ(raw.error().delivery, DeliveryStatus::not_started) << spelling.front();
    const auto after = lane.server.windows();
    ASSERT_TRUE(after.has_value()) << spelling.front() << ": the server must survive";
    EXPECT_EQ(after->size(), before->size()) << "nothing may be broken out or split";
  }
  // The typed call is the supported route and still works.
  const auto typed = pane->break_out();
  EXPECT_TRUE(typed.has_value()) << (typed ? "" : typed.error().diagnostic);
}

// tmux rejects an ambiguous or unknown command word and runs nothing, so the
// library has nothing to add: the refusal must come from tmux, not from it.
TEST(OptionalFeatures, AnAmbiguousOrUnknownRawCommandWordIsLeftToTmux) {
  Lane lane = start_lane();
  for (const char* word : {"b", "breakpane"}) {
    const auto raw = lane.server.run({word, "-d"});
    ASSERT_FALSE(raw.has_value()) << word;
    EXPECT_NE(raw.error().kind, FailureKind::validation) << word;
    EXPECT_EQ(raw.error().diagnostic.find("tmux 3.7 crashes"), std::string::npos)
        << word;
    EXPECT_TRUE(lane.server.windows().has_value()) << word;
  }
}

std::vector<std::string> buffer_names(const Server& server) {
  std::vector<std::string> names;
  const auto buffers = server.buffers();
  EXPECT_TRUE(buffers.has_value());
  for (const auto& buffer : buffers.value_or(std::vector<libtmux::Buffer>{})) {
    names.emplace_back(buffer.name());
  }
  std::ranges::sort(names);
  return names;
}

// A literal command is held to the gate its typed counterpart has. Each spelling
// names a buffer that does not exist, next to two that do: below 3.4 tmux would
// act on the top buffer instead, so every buffer must still be there afterwards.
TEST(OptionalFeatures, RawNamedBufferCommandsNeed34InEverySpelling) {
  Lane lane = start_lane();
  ASSERT_TRUE(lane.server.set_buffer("keep-top", "top").has_value());
  ASSERT_TRUE(lane.server.set_buffer("victim", "doomed").has_value());
  const auto before = buffer_names(lane.server);
  const bool gated = lane.running < Version{.major = 3, .minor = 4};

  const std::vector<std::vector<std::string>> spellings{
      {"delete-buffer", "-b", "missing"},
      {"deleteb", "-b", "missing"},
      {"delete-b", "-bmissing"},
      {"set-buffer", "-b", "missing", "-n", "renamed"},
      {"setb", "-b", "missing", "-n", "renamed"},
      {"list-buffers", ";", "delete-buffer", "-b", "missing"},
  };
  for (const auto& spelling : spellings) {
    const auto raw = lane.server.run(spelling);
    ASSERT_FALSE(raw.has_value()) << spelling.front();
    const bool refused_by_library =
        raw.error().diagnostic.find("needs tmux 3.4") != std::string::npos;
    EXPECT_EQ(refused_by_library, gated)
        << spelling.front() << ": " << raw.error().diagnostic;
    if (gated) {
      EXPECT_EQ(raw.error().kind, FailureKind::unsupported) << spelling.front();
      EXPECT_EQ(raw.error().delivery, DeliveryStatus::not_started) << spelling.front();
    }
    EXPECT_EQ(buffer_names(lane.server), before)
        << spelling.front() << ": no buffer may be deleted, renamed or added";
  }
  // The top buffer and a plain write are not gated.
  EXPECT_TRUE(lane.server.run({"set-buffer", "-b", "later", "data"}).has_value());
}

TEST(OptionalFeatures, RawLayoutsAreCheckedBeforeTmuxSeesThem) {
  Lane lane = start_lane();
  const std::string target{lane.fixture.session_name()};
  const bool mirrored = lane.running >= Version{.major = 3, .minor = 5};

  // tmux 3.3 and 3.3a end their server on a layout they cannot read.
  for (const char* layout : {"no-such-layout", "ffff,80x24,0,0,0", "{\"V\":2}"}) {
    const auto raw = lane.server.run({"select-layout", "-t", target, "--", layout});
    ASSERT_FALSE(raw.has_value()) << layout;
    EXPECT_EQ(raw.error().kind, FailureKind::validation) << layout;
    EXPECT_EQ(raw.error().delivery, DeliveryStatus::not_started) << layout;
    EXPECT_TRUE(lane.server.windows().has_value())
        << layout << ": the server must survive";
  }
  for (const auto& spelling : {std::vector<std::string>{"select-layout", "-t", target,
                                                        "main-vertical-mirrored"},
                               std::vector<std::string>{"selectl", "-t", target, "--",
                                                        "main-horizontal-mirrored"}}) {
    const auto raw = lane.server.run(spelling);
    if (mirrored) {
      EXPECT_TRUE(raw.has_value()) << (raw ? "" : raw.error().diagnostic);
      continue;
    }
    ASSERT_FALSE(raw.has_value()) << spelling.front();
    EXPECT_EQ(raw.error().kind, FailureKind::validation) << spelling.front();
    EXPECT_EQ(raw.error().delivery, DeliveryStatus::not_started) << spelling.front();
    EXPECT_TRUE(lane.server.windows().has_value()) << "the server must survive";
  }
  EXPECT_TRUE(lane.server.run({"select-layout", "-t", target, "tiled"}).has_value());
}

TEST(OptionalFeatures, RawNamesWithSeparatorsAreRefusedOnExactly37) {
  Lane lane = start_lane();
  const std::string session{lane.fixture.session_name()};
  const bool exact_37 = lane.running == Version{.major = 3, .minor = 7};
  const auto sessions = lane.server.sessions()->size();
  const auto windows = lane.server.windows()->size();

  const std::vector<std::vector<std::string>> spellings{
      {"new-session", "-d", "-s", "a.b"},
      {"new", "-ds", "a:b"},
      {"new-window", "-t", session, "-n", "a:b"},
      {"neww", "-t", session, "-nx.y"},
      {"rename-session", "-t", session, "a.b"},
      {"rename", "-t", session, "a:b"},
      {"rename-window", "-t", session, "a.b"},
      {"renamew", "-t", session, "--", "x:y"},
      {"list-windows", ";", "neww", "-t", session, "-n", "a.b"},
  };
  for (const auto& spelling : spellings) {
    const auto raw = lane.server.run(spelling);
    if (!exact_37) {
      // Other releases are not this library's to refuse.
      EXPECT_TRUE(raw.has_value() ||
                  raw.error().diagnostic.find("tmux 3.7 refuses") == std::string::npos)
          << spelling.front();
      return;
    }
    ASSERT_FALSE(raw.has_value()) << spelling.front();
    EXPECT_EQ(raw.error().kind, FailureKind::validation) << spelling.front();
    EXPECT_EQ(raw.error().delivery, DeliveryStatus::not_started) << spelling.front();
    EXPECT_EQ(lane.server.sessions()->size(), sessions) << spelling.front();
    EXPECT_EQ(lane.server.windows()->size(), windows) << spelling.front();
  }
  EXPECT_EQ(lane.server.sessions()->front().name(), session);
}

// A real terminal client attached to the fixture's session. The clipboard
// query only misbehaves with one attached, so a control client will not do.
class TerminalClient {
public:
  TerminalClient(const std::filesystem::path& socket, std::string_view session) {
    pid_ = ::forkpty(&master_, nullptr, nullptr, nullptr);
    if (pid_ == 0) {
      ::setenv("TERM", "xterm-256color", 1);
      ::unsetenv("TMUX");
      const std::string target{session};
      ::execlp("tmux", "tmux", "-S", socket.c_str(), "attach", "-t", target.c_str(),
               static_cast<char*>(nullptr));
      ::_exit(127);
    }
    if (master_ >= 0) {
      ::fcntl(master_, F_SETFL, ::fcntl(master_, F_GETFL) | O_NONBLOCK);
    }
  }
  TerminalClient(const TerminalClient&) = delete;
  TerminalClient& operator=(const TerminalClient&) = delete;
  ~TerminalClient() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, nullptr, 0);
    }
    if (master_ >= 0) {
      ::close(master_);
    }
  }
  /// Read what tmux has drawn, so the client never blocks on a full terminal.
  void drain() const {
    char buffer[4096];
    while (master_ >= 0 && ::read(master_, buffer, sizeof buffer) > 0) {
    }
  }

private:
  pid_t pid_{-1};
  int master_{-1};
};

// `refresh-client -l` ends a 3.3a server that has a client attached, and a 3.4
// through 3.6 server when that client leaves before answering, so below 3.6a a
// literal one is refused first under every spelling tmux accepts for it.
TEST(OptionalFeatures, ARawClipboardQueryIsRefusedBelow36aWithAClientAttached) {
  Lane lane = start_lane();
  TerminalClient terminal{lane.fixture.socket_path(), lane.fixture.session_name()};
  std::string client;
  for (int attempt = 0; attempt < 200 && client.empty(); ++attempt) {
    terminal.drain();
    const auto listed = lane.server.run({"list-clients", "-F", "#{client_name}"});
    if (listed.has_value()) {
      client = *listed;
      while (!client.empty() && client.back() == '\n') {
        client.pop_back();
      }
    }
    if (client.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
  }
  ASSERT_FALSE(client.empty()) << "the terminal client never attached";

  const std::vector<std::vector<std::string>> spellings{
      {"refresh-client", "-t", client, "-l"},
      {"refresh", "-l", "-t", client},
      {"refresh-c", "-lt", client},
      {"list-sessions", ";", "refresh-client", "-t", client, "-l"},
  };
  const bool gated = lane.running < Version{.major = 3, .minor = 6, .revision = 1};
  for (const auto& spelling : spellings) {
    const auto raw = lane.server.run(spelling);
    if (!gated) {
      // Other releases are not this library's to refuse.
      if (!raw.has_value()) {
        EXPECT_EQ(raw.error().diagnostic.find("needs tmux 3.6a"), std::string::npos);
      }
      return;
    }
    ASSERT_FALSE(raw.has_value()) << spelling.front();
    EXPECT_EQ(raw.error().kind, FailureKind::unsupported) << spelling.front();
    EXPECT_EQ(raw.error().delivery, DeliveryStatus::not_started) << spelling.front();
    EXPECT_NE(raw.error().diagnostic.find("3.6a"), std::string::npos);
    terminal.drain();
    EXPECT_TRUE(lane.server.sessions().has_value())
        << spelling.front() << ": the server must survive";
  }
  // A refresh that does not ask for the clipboard is still sent.
  EXPECT_TRUE(lane.server.run({"refresh-client", "-t", client}).has_value());
}

// What a held-open control client was sent, as one request of single commands.
libtmux::ControlRequestResult
send_over(libtmux::Connection& connection,
          std::vector<std::vector<std::string>> commands) {
  libtmux::ControlRequest request;
  for (auto& argv : commands) {
    request.group.push_back(libtmux::ControlCommand{.argv = std::move(argv)});
  }
  return connection.execute(std::move(request),
                            std::chrono::steady_clock::now() + std::chrono::seconds{5});
}

// A control connection is a raw-command channel, and 3.7 is the release where
// it first exists and the one where an unnamed break-pane ends the server. Each
// spelling must be refused before it is written, and the same connection must
// still answer afterwards.
TEST(OptionalFeatures, AnUnnamedBreakPaneOverAControlConnectionIsRefusedOnExactly37) {
  Lane lane = start_lane();
  if (lane.running < Version{.major = 3, .minor = 7}) {
    GTEST_SKIP() << "control mode is refused before 3.7";
  }
  ASSERT_TRUE(lane.server.run({"split-window", "-d"}).has_value());
  auto pane = lane.server.pane(lane.fixture.session_name());
  ASSERT_TRUE(pane.has_value());
  const std::string source{pane->id().value()};
  auto connection = lane.server.control(lane.fixture.session_name());
  ASSERT_TRUE(connection.has_value()) << connection.error().message;

  const std::vector<std::vector<std::vector<std::string>>> spellings{
      {{"break-pane", "-d", "-s", source}},
      {{"breakp", "-d", "-s", source}},
      {{"break-p", "-d", "-s", source}},
      {{"split-window", "-d"}, {"break-p", "-d", "-s", source}},
  };
  const bool exact_37 = lane.running == Version{.major = 3, .minor = 7};
  const auto windows = lane.server.windows()->size();
  for (const auto& spelling : spellings) {
    const auto sent = send_over(*connection, spelling);
    if (!exact_37) {
      // Other releases are not this library's to refuse.
      EXPECT_FALSE(sent.connection_error.has_value()) << spelling.back().front();
      return;
    }
    ASSERT_TRUE(sent.connection_error.has_value()) << spelling.back().front();
    EXPECT_EQ(sent.connection_error->delivery, DeliveryStatus::not_started)
        << spelling.back().front();
    EXPECT_NE(sent.connection_error->message.find("3.7"), std::string::npos);
    EXPECT_TRUE(sent.blocks.empty());
    const auto after = lane.server.windows();
    ASSERT_TRUE(after.has_value())
        << spelling.back().front() << ": server must survive";
    EXPECT_EQ(after->size(), windows) << "nothing may be broken out or split";
  }
  // The refusal is not a fault of the connection: it answers the next request.
  const auto listed = send_over(*connection, {{"list-sessions"}});
  EXPECT_FALSE(listed.connection_error.has_value());
  EXPECT_EQ(listed.blocks.size(), 1U);
}

TEST(OptionalFeatures, RawLayoutsAndNamesOverAControlConnectionAreRefused) {
  Lane lane = start_lane();
  if (lane.running < Version{.major = 3, .minor = 7}) {
    GTEST_SKIP() << "control mode is refused before 3.7";
  }
  auto connection = lane.server.control(lane.fixture.session_name());
  ASSERT_TRUE(connection.has_value()) << connection.error().message;
  const std::string target{lane.fixture.session_name()};

  const auto layout =
      send_over(*connection, {{"select-layout", "-t", target, "--", "no-such-layout"}});
  ASSERT_TRUE(layout.connection_error.has_value());
  EXPECT_EQ(layout.connection_error->delivery, DeliveryStatus::not_started);

  const auto name =
      send_over(*connection, {{"list-windows"}, {"neww", "-t", target, "-n", "a.b"}});
  if (lane.running == Version{.major = 3, .minor = 7}) {
    ASSERT_TRUE(name.connection_error.has_value());
    EXPECT_EQ(name.connection_error->delivery, DeliveryStatus::not_started);
    EXPECT_EQ(lane.server.windows()->size(), 1U);
  } else {
    EXPECT_FALSE(name.connection_error.has_value());
  }
  // A semicolon that is only an argument does not start a second command.
  const auto data = send_over(*connection, {{"display-message", "-p", ";", "break-p"}});
  EXPECT_FALSE(data.connection_error.has_value());
}

// Every other way a caller can hand the library a command line reaches the same
// gate; on exact 3.7 each one is refused with the server still answering.
TEST(OptionalFeatures, EveryRawEntryPointRefusesAnUnnamedBreakPaneOnExactly37) {
  Lane lane = start_lane();
  if (lane.running != Version{.major = 3, .minor = 7}) {
    GTEST_SKIP() << "the crash is specific to exactly 3.7";
  }
  ASSERT_TRUE(lane.server.run({"split-window", "-d"}).has_value());
  auto pane = lane.server.pane(lane.fixture.session_name());
  ASSERT_TRUE(pane.has_value());
  const std::string source{pane->id().value()};
  const libtmux::CommandRequest unnamed{"break-p", "-d", "-s", source};
  const auto windows = lane.server.windows()->size();
  const auto survived = [&](std::string_view route) {
    const auto after = lane.server.windows();
    ASSERT_TRUE(after.has_value()) << route << ": the server must survive";
    EXPECT_EQ(after->size(), windows) << route;
  };
  const auto refused = [&](std::string_view route, const auto& answer) {
    ASSERT_FALSE(answer.has_value()) << route;
    EXPECT_EQ(answer.error().kind, FailureKind::validation) << route;
    EXPECT_EQ(answer.error().delivery, DeliveryStatus::not_started) << route;
    survived(route);
  };

  libtmux::CommandBatch batch;
  ASSERT_TRUE(batch.add({"list-sessions"}));
  ASSERT_TRUE(batch.add(unnamed));
  refused("run_batch", lane.server.run_batch(batch));

  libtmux::Chain chain;
  chain.command({"breakp", "-d", "-s", source});
  refused("run_chain", lane.server.run_chain(chain));

  auto runtime = libtmux::CommandRuntime::start();
  ASSERT_TRUE(runtime.has_value());
  auto submitted = lane.server.try_submit(*runtime, unnamed);
  if (submitted.has_value()) {
    refused("try_submit", std::move(*submitted).wait());
  } else {
    refused("try_submit", submitted);
  }

  auto controlled = lane.server.over_control(lane.fixture.session_name());
  ASSERT_TRUE(controlled.has_value()) << controlled.error().message;
  refused("over_control", controlled->run(unnamed));
  refused("over_control batch", controlled->run_batch(batch));
}

// `-C` selects control mode for the client, not for a command, so a literal
// command cannot acquire a control client; the call is refused as a client flag.
TEST(OptionalFeatures, ARawControlClientFlagIsRefusedAsAClientFlag) {
  Lane lane = start_lane();
  for (const char* flag : {"-C", "-CC"}) {
    const auto raw = lane.server.run({flag, "attach-session"});
    ASSERT_FALSE(raw.has_value()) << flag;
    EXPECT_EQ(raw.error().kind, FailureKind::validation) << flag;
    EXPECT_EQ(raw.error().delivery, DeliveryStatus::not_started) << flag;
    EXPECT_TRUE(lane.server.clients()->empty()) << flag;
  }
}

} // namespace
