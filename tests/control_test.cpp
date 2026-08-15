#include "libtmux/control.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/server.hpp"
#include "support/scoped_tmux_server.hpp"

namespace {

using libtmux::ControlBlock;
using libtmux::ControlTerminal;
using libtmux::Event;
using libtmux::Notification;
using libtmux::Parser;

std::span<const std::byte> bytes_of(const std::string& text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::string text_of(const std::vector<std::byte>& body) {
  std::string out;
  for (const std::byte byte : body) {
    out.push_back(static_cast<char>(byte));
  }
  return out;
}

std::vector<Event> feed(Parser& parser, const std::string& text) {
  auto events = parser.feed(bytes_of(text));
  EXPECT_TRUE(events.has_value());
  return events.has_value() ? *events : std::vector<Event>{};
}

TEST(ControlParser, DeliversOneBlockPerGuardedReply) {
  Parser parser;
  const auto events = feed(parser, "%begin 1 2 0\nhello\n%end 1 2 0\n");
  ASSERT_EQ(events.size(), 1U);
  const auto* block = std::get_if<ControlBlock>(&events[0]);
  ASSERT_NE(block, nullptr);
  EXPECT_EQ(block->terminal, ControlTerminal::end);
  EXPECT_EQ(text_of(block->body), "hello\n");
}

TEST(ControlParser, SeparatesAFailedReplyFromASuccessfulOne) {
  Parser parser;
  const auto events = feed(parser, "%begin 1 2 0\noops\n%error 1 2 0\n");
  ASSERT_EQ(events.size(), 1U);
  const auto* block = std::get_if<ControlBlock>(&events[0]);
  ASSERT_NE(block, nullptr);
  EXPECT_EQ(block->terminal, ControlTerminal::error);
}

TEST(ControlParser, KeepsANotificationShapedLineInsideABlockAsBody) {
  Parser parser;
  const auto events = feed(parser, "%begin 1 2 0\n%output %1 hi\n%end 1 2 0\n");
  ASSERT_EQ(events.size(), 1U);
  const auto* block = std::get_if<ControlBlock>(&events[0]);
  ASSERT_NE(block, nullptr);
  // Control-mode framing does not make it independently attributable.
  EXPECT_EQ(text_of(block->body), "%output %1 hi\n");
}

TEST(ControlParser, ReportsANotificationOutsideAnyBlock) {
  Parser parser;
  const auto events = feed(parser, "%output %1 hi\n");
  ASSERT_EQ(events.size(), 1U);
  EXPECT_NE(std::get_if<Notification>(&events[0]), nullptr);
}

TEST(ControlParser, FramesTheSameStreamOneByteAtATime) {
  const std::string stream = "%begin 1 2 0\nhello\n%end 1 2 0\n";
  Parser whole;
  const auto once = feed(whole, stream);

  Parser split;
  std::vector<Event> drip;
  for (const char character : stream) {
    for (auto& event : feed(split, std::string{character})) {
      drip.push_back(std::move(event));
    }
  }
  ASSERT_EQ(once.size(), drip.size());
  EXPECT_EQ(text_of(std::get<ControlBlock>(drip[0]).body),
            text_of(std::get<ControlBlock>(once[0]).body));
}

TEST(ControlParser, RefusesToEndInsideAnUnterminatedBlock) {
  Parser parser;
  feed(parser, "%begin 1 2 0\npartial\n");
  EXPECT_FALSE(parser.finish().has_value());
}

TEST(ControlConnection, GivesEachCommandItsOwnReplyBlock) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto server = libtmux::Server::at_socket_path(fixture->socket_path().string());
  ASSERT_TRUE(server.has_value());

  auto connected = server->control(fixture->session_name());
  ASSERT_TRUE(connected.has_value()) << connected.error().message;
  auto connection = std::move(*connected);

  libtmux::ControlRequest request;
  request.group.push_back(
      libtmux::ControlCommand{.argv = {"display-message", "-p", "streamed"}});
  const auto result = connection.execute(
      std::move(request), std::chrono::steady_clock::now() + std::chrono::seconds{5});
  ASSERT_FALSE(result.connection_error.has_value()) << result.connection_error->message;
  ASSERT_EQ(result.operations.size(), 1U);
  // Attribution is exact because control mode framed a reply for this command
  // rather than leaving one exit status to cover a group.
  EXPECT_EQ(result.operations[0].attribution, libtmux::Attribution::exact);
  ASSERT_TRUE(result.operations[0].block.has_value());
  EXPECT_NE(text_of(result.operations[0].block->body).find("streamed"),
            std::string::npos);

  const auto closed =
      connection.shutdown(std::chrono::steady_clock::now() + std::chrono::seconds{5});
  EXPECT_TRUE(closed.has_value());
}

} // namespace
