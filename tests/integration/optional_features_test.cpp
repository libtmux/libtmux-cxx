// Optional tmux features and the releases that cannot do them safely.
//
// The core floor is 3.3a; these operations need more, and each is refused
// before it has an effect when the daemon is older. Every test runs against
// whichever tmux is on PATH, so a lane exercises the refusal below the
// threshold and the supported behaviour at or above it; the sentinel state
// proves that a refusal changed nothing.

#include <chrono>
#include <optional>
#include <memory>
#include <string>
#include <vector>

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
    EXPECT_TRUE(connection->shutdown(std::chrono::steady_clock::now() +
                                     std::chrono::seconds{2})
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
    const auto window = lane.server.sessions()->front().new_window(libtmux::NewWindowOptions{.name = "a:b"});
    ASSERT_FALSE(window.has_value());
    EXPECT_EQ(window.error().kind, FailureKind::validation);
  } else if (!created.has_value()) {
    // tmux may itself decline on another release; this library must not.
    EXPECT_EQ(created.error().diagnostic.find("tmux 3.7 refuses"), std::string::npos);
  }
  // A plain name is accepted on every release.
  EXPECT_TRUE(lane.server.new_session("plain-name").has_value());
}

} // namespace
