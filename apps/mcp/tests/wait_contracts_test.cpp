// What the wait_for_text tool promises when the pane misbehaves: the same
// answers as the library wait, through the tool surface.

#include "libtmux_consumers/mcp.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

#include <signal.h>

#include <gtest/gtest.h>

#include "libtmux/server.hpp"
#include "libtmux/testing/scoped_server.hpp"
#include "libtmux/version.hpp"

namespace {

using libtmux::Server;
using libtmux::mcp::Arguments;
using libtmux::mcp::ToolRegistry;
using namespace std::chrono_literals;

constexpr libtmux::Version k37{.major = 3, .minor = 7};

struct Lane {
  libtmux::test::ScopedTmuxServer fixture;
  Server server;
  libtmux::Version running;
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

ToolRegistry all_tools() {
  auto built = libtmux::mcp::default_tools();
  EXPECT_TRUE(built.has_value()) << built.error();
  return std::move(*built);
}

std::string idle_pane(const Server& server) {
  const auto made = server.run({"split-window", "-d", "-P", "-F", "#{pane_id}", "sh",
                                "-c", "echo before-death; sleep 60"});
  EXPECT_TRUE(made.has_value());
  std::string id = made.value_or("");
  while (!id.empty() && (id.back() == '\n' || id.back() == '\r')) {
    id.pop_back();
  }
  return id;
}

// The tool cannot say when its wait has begun, so wait for the pane's output
// to be visible and leave a margin for the call to start.
void let_the_wait_begin(const Server& server, const std::string& pane) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    const auto screen = server.run({"capture-pane", "-p", "-t", pane});
    if (screen.has_value() && screen->find("before-death") != std::string::npos) {
      break;
    }
    std::this_thread::sleep_for(25ms);
  }
  std::this_thread::sleep_for(1000ms);
}

bool failed_with_output(const libtmux::mcp::ToolResult& result) {
  return !result.has_value() && !result.error().caller_error &&
         !result.error().message.empty() &&
         result.error().message.find("output so far") != std::string::npos &&
         result.error().message.find("before-death") != std::string::npos;
}

TEST(McpToolsTmux, WaitForTextFindsAMatchSplitAcrossAWrappedRow) {
  Lane lane = start_lane();
  const auto tools = all_tools();
  const std::string head(60, 'a');
  const std::string tail(60, 'b');
  const std::string pane{lane.server.pane(lane.fixture.session_name())->id().value()};
  std::thread typist{[&] {
    std::this_thread::sleep_for(800ms);
    EXPECT_TRUE(lane.server
                    .run({"send-keys", "-t", pane, "-l",
                          "printf '%s%s\\n' " + head + " " + tail})
                    .has_value());
    EXPECT_TRUE(lane.server.run({"send-keys", "-t", pane, "Enter"}).has_value());
  }};
  const auto waited = tools.call(
      lane.server, "wait_for_text",
      Arguments{{"target", pane}, {"text", head + tail}, {"timeout_ms", "8000"}});
  typist.join();
  ASSERT_TRUE(waited.has_value()) << waited.error().message;
  EXPECT_TRUE(std::get<bool>(waited->structured.at("matched").value));
  EXPECT_FALSE(std::get<bool>(waited->structured.at("matched_at_entry").value));
}

TEST(McpToolsTmux, WaitForTextFailsWithTheOutputSoFarWhenThePaneDies) {
  Lane lane = start_lane();
  const auto tools = all_tools();
  const std::string pane = idle_pane(lane.server);
  std::thread killer{[&] {
    let_the_wait_begin(lane.server, pane);
    EXPECT_TRUE(lane.server.run({"kill-pane", "-t", pane}).has_value());
  }};
  const auto waited = tools.call(
      lane.server, "wait_for_text",
      Arguments{{"target", pane}, {"text", "never-printed"}, {"timeout_ms", "10000"}});
  killer.join();
  EXPECT_TRUE(failed_with_output(waited))
      << (waited ? "the wait did not fail" : waited.error().message);
}

TEST(McpToolsTmux, WaitForTextFailsTheSameWayWhenTheServerIsKilled) {
  Lane lane = start_lane();
  const auto tools = all_tools();
  const std::string pane = idle_pane(lane.server);
  std::thread killer{[&] {
    let_the_wait_begin(lane.server, pane);
    static_cast<void>(lane.server.run({"kill-server"}));
  }};
  const auto waited = tools.call(
      lane.server, "wait_for_text",
      Arguments{{"target", pane}, {"text", "never-printed"}, {"timeout_ms", "10000"}});
  killer.join();
  EXPECT_TRUE(failed_with_output(waited))
      << (waited ? "the wait did not fail" : waited.error().message);
}

TEST(McpToolsTmux, WaitForTextKeepsWaitingWhenOnlyTheStreamDrops) {
  Lane lane = start_lane();
  if (lane.running < k37) {
    GTEST_SKIP() << "library-managed control mode starts at tmux 3.7";
  }
  const auto tools = all_tools();
  const std::string pane{lane.server.pane(lane.fixture.session_name())->id().value()};
  ASSERT_TRUE(
      lane.server
          .run({"send-keys", "-t", pane, "-l", "sleep 3; echo after-drop-$((40+2))"})
          .has_value());
  ASSERT_TRUE(lane.server.run({"send-keys", "-t", pane, "Enter"}).has_value());
  std::atomic<bool> dropped{false};
  std::thread dropper{[&] {
    for (int attempt = 0; attempt < 100 && !dropped; ++attempt) {
      std::this_thread::sleep_for(50ms);
      const auto clients = lane.server.clients();
      if (clients.has_value() && !clients->empty()) {
        std::this_thread::sleep_for(300ms);
        ::kill(static_cast<pid_t>(clients->front().pid()), SIGKILL);
        dropped = true;
      }
    }
  }};
  const auto waited = tools.call(
      lane.server, "wait_for_text",
      Arguments{{"target", pane}, {"text", "after-drop-42"}, {"timeout_ms", "10000"}});
  dropper.join();
  ASSERT_TRUE(dropped) << "the tool never opened a control client";
  ASSERT_TRUE(waited.has_value()) << waited.error().message;
  EXPECT_TRUE(std::get<bool>(waited->structured.at("matched").value));
  EXPECT_EQ(std::get<std::string>(waited->structured.at("mode").value),
            "capture-polling");
}

} // namespace
