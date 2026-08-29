#include "libtmux/control.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {

using libtmux::ControlBlock;
using libtmux::ControlTerminal;
using libtmux::Event;
using libtmux::Notification;
using libtmux::Parser;

using Bytes = std::vector<std::byte>;

Bytes bytes(std::string_view text) {
  Bytes result;
  result.reserve(text.size());
  for (const auto character : text) {
    result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  return result;
}

void append(Bytes& destination, std::string_view text) {
  auto suffix = bytes(text);
  destination.insert(destination.end(), suffix.begin(), suffix.end());
}

std::string hex(const Bytes& value) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2U);
  for (const auto byte : value) {
    const auto raw = std::to_integer<unsigned int>(byte);
    result.push_back(digits[(raw >> 4U) & 0x0fU]);
    result.push_back(digits[raw & 0x0fU]);
  }
  return result;
}

ControlBlock block(std::uint64_t sequence, std::uint64_t command_number,
                   ControlTerminal terminal, std::string_view metadata, Bytes body) {
  return {.sequence = sequence,
          .command_number = command_number,
          .terminal = terminal,
          .begin_metadata = bytes(metadata),
          .terminal_metadata = bytes(metadata),
          .body = std::move(body)};
}

void expect_events(const std::vector<Event>& actual,
                   const std::vector<Event>& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t index = 0; index < actual.size(); ++index) {
    SCOPED_TRACE(index);
    ASSERT_EQ(actual[index].index(), expected[index].index());
    if (const auto* actual_block = std::get_if<ControlBlock>(&actual[index])) {
      const auto& expected_block = std::get<ControlBlock>(expected[index]);
      EXPECT_EQ(actual_block->sequence, expected_block.sequence);
      EXPECT_EQ(actual_block->command_number, expected_block.command_number);
      EXPECT_EQ(actual_block->terminal, expected_block.terminal);
      EXPECT_EQ(hex(actual_block->begin_metadata), hex(expected_block.begin_metadata));
      EXPECT_EQ(hex(actual_block->terminal_metadata),
                hex(expected_block.terminal_metadata));
      EXPECT_EQ(hex(actual_block->body), hex(expected_block.body));
    } else {
      EXPECT_EQ(hex(std::get<Notification>(actual[index]).body),
                hex(std::get<Notification>(expected[index]).body));
    }
  }
}

bool feed(Parser& parser, std::span<const std::byte> input,
          std::vector<Event>& events) {
  auto result = parser.feed(input);
  if (!result.has_value()) {
    ADD_FAILURE() << result.error().message;
    return false;
  }
  events.insert(events.end(), std::make_move_iterator(result->begin()),
                std::make_move_iterator(result->end()));
  return true;
}

struct Golden {
  std::string_view id;
  Bytes wire;
  std::vector<Event> events;
};

std::vector<Golden> goldens() {
  std::vector<Golden> values;

  values.push_back({.id = "cm-guard-end-v1",
                    .wire = bytes("%begin 1700000000 42 1\nalpha\n%end "
                                  "1700000000 42 1\n"),
                    .events = {block(1700000000U, 42U, ControlTerminal::end,
                                     "1700000000 42 1", bytes("alpha\n"))}});

  values.push_back({.id = "cm-guard-error-v1",
                    .wire = bytes("%begin 9 4294967295 1\nfailed\n%error 9 "
                                  "4294967295 1\n"),
                    .events = {block(9U, 4294967295U, ControlTerminal::error,
                                     "9 4294967295 1", bytes("failed\n"))}});

  values.push_back({.id = "cm-body-percent-v1",
                    .wire = bytes("%begin 8 11 1\n%message direct\n%output %7 shaped\n"
                                  "%end 8 12 1\n%error 8 11 1\n"),
                    .events = {block(8U, 11U, ControlTerminal::error, "8 11 1",
                                     bytes("%message direct\n%output %7 shaped\n"
                                           "%end 8 12 1\n"))}});

  values.push_back(
      {.id = "cm-notification-interleave-v1",
       .wire = bytes("%sessions-changed\n%begin 7 10 1\ninside\n%end 7 10 "
                     "1\n%future-notification raw value\nasync bare line\n"),
       .events = {Notification{bytes("%sessions-changed")},
                  block(7U, 10U, ControlTerminal::end, "7 10 1", bytes("inside\n")),
                  Notification{bytes("%future-notification raw value")},
                  Notification{bytes("async bare line")}}});

  auto output_wire = bytes("%output %7 A\\000\\011\\012\\033\\134");
  output_wire.push_back(std::byte{0x7f});
  output_wire.push_back(std::byte{0x80});
  output_wire.push_back(std::byte{0xff});
  output_wire.push_back(std::byte{'\n'});
  auto output_body = bytes("%output %7 A");
  for (const auto value : {0x00U, 0x09U, 0x0aU, 0x1bU, 0x5cU, 0x7fU, 0x80U, 0xffU}) {
    output_body.push_back(static_cast<std::byte>(value));
  }
  values.push_back({.id = "cm-output-octal-v1",
                    .wire = std::move(output_wire),
                    .events = {Notification{std::move(output_body)}}});

  values.push_back({.id = "cm-extended-output-octal-v1",
                    .wire = bytes("%extended-output %3 27 reserved : x\\001\\134\n"),
                    .events = {Notification{[] {
                      auto value = bytes("%extended-output %3 27 reserved : x");
                      value.push_back(std::byte{0x01});
                      value.push_back(std::byte{0x5c});
                      return value;
                    }()}}});

  auto raw_wire = bytes("%begin 6 9 1\nraw:");
  raw_wire.push_back(std::byte{0x00});
  raw_wire.push_back(std::byte{0x80});
  append(raw_wire, "\r\n%end 6 9 1\n");
  auto raw_body = bytes("raw:");
  raw_body.push_back(std::byte{0x00});
  raw_body.push_back(std::byte{0x80});
  append(raw_body, "\r\n");
  values.push_back(
      {.id = "cm-raw-bytes-v1",
       .wire = std::move(raw_wire),
       .events = {block(6U, 9U, ControlTerminal::end, "6 9 1", std::move(raw_body))}});

  return values;
}

TEST(ControlModeParser, ParsesEveryGoldenAtEveryByteBoundary) {
  for (const auto& golden : goldens()) {
    for (std::size_t split = 0; split <= golden.wire.size(); ++split) {
      SCOPED_TRACE(golden.id);
      SCOPED_TRACE(split);
      Parser parser;
      std::vector<Event> actual;
      ASSERT_TRUE(feed(parser, std::span{golden.wire}.first(split), actual));
      ASSERT_TRUE(feed(parser, std::span{golden.wire}.subspan(split), actual));
      const auto finished = parser.finish();
      ASSERT_TRUE(finished.has_value())
          << (finished.has_value() ? "" : finished.error().message);
      expect_events(actual, golden.events);
    }
  }
}

TEST(ControlModeParser, ParsesEveryGoldenOneByteAtATime) {
  for (const auto& golden : goldens()) {
    SCOPED_TRACE(golden.id);
    Parser parser;
    std::vector<Event> actual;
    for (const auto byte : golden.wire) {
      ASSERT_TRUE(feed(parser, std::span{&byte, 1U}, actual));
    }
    ASSERT_TRUE(parser.finish().has_value());
    expect_events(actual, golden.events);
  }
}

TEST(ControlModeParser, RejectsMalformedBoundariesAndPaneEscapes) {
  const std::vector<std::string> malformed{
      "%begin\n",
      "%begin 1 2\n",
      "%begin -1 2 1\n",
      "%begin 1 2 1 extra\n",
      "%begin 18446744073709551616 2 1\n",
      "%end 1 2 1\n",
      "%error 1 2 1\n",
      "%output %1 bad\\12\n",
      "%output %1 bad\\128\n",
      "%output %1 bad\\400\n",
      "%extended-output %1 3 missing-delimiter\\001\n",
  };
  for (const auto& wire : malformed) {
    SCOPED_TRACE(wire);
    Parser parser;
    const auto result = parser.feed(bytes(wire));
    EXPECT_FALSE(result.has_value());
  }
}

TEST(ControlModeParser, RejectsIncompleteEofAndRemainsPoisoned) {
  Parser partial_line;
  ASSERT_TRUE(partial_line.feed(bytes("%sessions-changed")).has_value());
  EXPECT_FALSE(partial_line.finish().has_value());

  Parser open_block;
  ASSERT_TRUE(open_block.feed(bytes("%begin 1 2 1\nbody\n")).has_value());
  EXPECT_FALSE(open_block.finish().has_value());

  Parser poisoned;
  const auto first = poisoned.feed(bytes("%begin invalid\n"));
  ASSERT_FALSE(first.has_value());
  const auto second = poisoned.feed(bytes("%sessions-changed\n"));
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().message, first.error().message);
  EXPECT_FALSE(poisoned.finish().has_value());
}

TEST(ControlModeParser, FinishesIdempotentlyAndRejectsLaterInput) {
  Parser parser;
  ASSERT_TRUE(parser.feed({}).has_value());
  ASSERT_TRUE(parser.finish().has_value());
  EXPECT_TRUE(parser.finish().has_value());
  EXPECT_FALSE(parser.feed(bytes("%sessions-changed\n")).has_value());
}

// The bound is on what the parser holds, not on what it reports afterwards.
// Checking the size of an assembled reply says nothing about the memory the
// process spent assembling it, which is the whole reason the bound moved here.
TEST(ControlModeParser, RetainsBoundedBodyAndKeepsTheStreamAttributable) {
  Parser parser{16U, 4096U};
  Bytes stream;
  append(stream, "%begin 1 2 1\n");
  for (int line = 0; line < 64; ++line) {
    append(stream, "0123456789abcdef\n");
  }
  append(stream, "%end 1 2 1\n");
  // A second command, so the test proves the stream survived the first.
  append(stream, "%begin 3 4 1\nkept\n%end 3 4 1\n");

  const auto events = parser.feed(stream);
  ASSERT_TRUE(events.has_value());
  ASSERT_EQ(events->size(), 2U);

  const auto& oversized = std::get<ControlBlock>(events->front());
  EXPECT_TRUE(oversized.body_truncated);
  EXPECT_EQ(oversized.body_bytes, 64U * 17U);
  EXPECT_LE(oversized.body.size(), 16U);

  const auto& following = std::get<ControlBlock>(events->back());
  EXPECT_FALSE(following.body_truncated);
  EXPECT_EQ(following.body_bytes, 5U);
  EXPECT_EQ(hex(following.body), hex(bytes("kept\n")));
}

// A line is bounded separately, because a body arrives as many lines and no
// legitimate one grows without end. Terminal, since a control stream offers
// nothing to resynchronise against.
TEST(ControlModeParser, FailsAndReleasesOnALineThatNeverEnds) {
  Parser parser{4096U, 64U};
  const auto first = parser.feed(bytes(std::string(128U, 'x')));
  ASSERT_FALSE(first.has_value());
  EXPECT_NE(first.error().message.find("control line exceeded"), std::string::npos);
  EXPECT_FALSE(parser.feed(bytes("\n%sessions-changed\n")).has_value());
  EXPECT_FALSE(parser.finish().has_value());
}

// Zero is the escape hatch, and a test that owns both ends of the stream is
// the only caller entitled to it.
TEST(ControlModeParser, TreatsZeroAsUnbounded) {
  Parser parser{0U, 0U};
  Bytes stream;
  append(stream, "%begin 1 2 1\n");
  append(stream, std::string(8192U, 'y'));
  append(stream, "\n%end 1 2 1\n");

  const auto events = parser.feed(stream);
  ASSERT_TRUE(events.has_value());
  ASSERT_EQ(events->size(), 1U);
  const auto& block = std::get<ControlBlock>(events->front());
  EXPECT_FALSE(block.body_truncated);
  EXPECT_EQ(block.body.size(), 8193U);
}

} // namespace

// Reading a notification's name and arguments.
//
// tmux types its arguments by prefix, so the grammar is the same for every
// notification: `%name` then `$session`, `@window`, `%pane` in whatever
// combination that one carries, then any free text. The shapes below are the
// ones tmux writes, taken from its source across 3.2a to master.

namespace {

libtmux::Notification notification_of(std::string_view line) {
  const auto* first = reinterpret_cast<const std::byte*>(line.data());
  return libtmux::Notification{std::vector<std::byte>{first, first + line.size()}};
}

std::string as_text(std::span<const std::byte> bytes) {
  return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace

TEST(NotificationParse, KnowsEveryNotificationInTheSupportedProtocol) {
  struct Case {
    std::string_view line;
    libtmux::NotificationKind kind;
  };
  constexpr auto cases = std::to_array<Case>({
      {"%output %1 value", libtmux::NotificationKind::output},
      {"%extended-output %1 0 : value", libtmux::NotificationKind::extended_output},
      {"%pause %1", libtmux::NotificationKind::paused},
      {"%continue %1", libtmux::NotificationKind::resumed},
      {"%sessions-changed", libtmux::NotificationKind::sessions_changed},
      {"%session-changed $0 work", libtmux::NotificationKind::session_changed},
      {"%session-renamed $0 work", libtmux::NotificationKind::session_renamed},
      {"%session-window-changed $0 @1",
       libtmux::NotificationKind::session_window_changed},
      {"%client-detached tty", libtmux::NotificationKind::client_detached},
      {"%client-session-changed tty $0 work",
       libtmux::NotificationKind::client_session_changed},
      {"%window-add @1", libtmux::NotificationKind::window_add},
      {"%window-close @2", libtmux::NotificationKind::window_close},
      {"%window-renamed @1 work", libtmux::NotificationKind::window_renamed},
      {"%window-pane-changed @1 %4", libtmux::NotificationKind::window_pane_changed},
      {"%unlinked-window-add @1", libtmux::NotificationKind::unlinked_window_add},
      {"%unlinked-window-close @1", libtmux::NotificationKind::unlinked_window_close},
      {"%unlinked-window-renamed @1 work",
       libtmux::NotificationKind::unlinked_window_renamed},
      {"%pane-mode-changed %3", libtmux::NotificationKind::pane_mode_changed},
      {"%paste-buffer-changed buffer0",
       libtmux::NotificationKind::paste_buffer_changed},
      {"%paste-buffer-deleted buffer0",
       libtmux::NotificationKind::paste_buffer_deleted},
      {"%subscription-changed sub $0 @1 0 %2 : value",
       libtmux::NotificationKind::subscription_changed},
      {"%config-error invalid option", libtmux::NotificationKind::config_error},
      {"%exit server exited", libtmux::NotificationKind::exit},
      {"%layout-change @1 layout visible flags",
       libtmux::NotificationKind::layout_change},
      {"%message hello", libtmux::NotificationKind::message},
  });
  for (const Case& one : cases) {
    const auto held = notification_of(one.line);
    const auto parsed = libtmux::parse(held);
    EXPECT_EQ(parsed.kind, one.kind) << one.line;
    EXPECT_EQ(libtmux::to_string(parsed.kind), parsed.name) << one.line;
    EXPECT_EQ(parsed.name, one.line.substr(0, one.line.find(' '))) << one.line;
  }
}

TEST(NotificationParse, PutsEachIdInTheFieldItsPrefixNames) {
  const auto session = notification_of("%session-window-changed $2 @7");
  const auto parsed = libtmux::parse(session);
  EXPECT_EQ(parsed.session, "$2");
  EXPECT_EQ(parsed.window, "@7");
  EXPECT_TRUE(parsed.pane.empty());

  const auto pane = notification_of("%window-pane-changed @1 %4");
  const auto pane_parsed = libtmux::parse(pane);
  EXPECT_EQ(pane_parsed.window, "@1");
  EXPECT_EQ(pane_parsed.pane, "%4");
  EXPECT_TRUE(pane_parsed.session.empty());
}

TEST(NotificationParse, KeepsFreeTextWholeIncludingItsSpaces) {
  const auto held = notification_of("%window-renamed @3 a name with spaces");
  const auto parsed = libtmux::parse(held);
  EXPECT_EQ(parsed.kind, libtmux::NotificationKind::window_renamed);
  EXPECT_EQ(parsed.window, "@3");
  EXPECT_EQ(parsed.text, "a name with spaces");
}

// The payload is already unescaped by the time it is a notification, so it may
// hold the spaces and newlines that would otherwise separate fields. Only the
// prefix is tokenised.
TEST(NotificationParse, TakesOutputPayloadWholeAfterItsPane) {
  const auto held = notification_of("%output %2 two words\nand a newline");
  const auto parsed = libtmux::parse(held);
  EXPECT_EQ(parsed.kind, libtmux::NotificationKind::output);
  EXPECT_EQ(parsed.pane, "%2");
  EXPECT_EQ(as_text(parsed.payload), "two words\nand a newline");
}

TEST(NotificationParse, ReadsTheAgeOfExtendedOutput) {
  const auto held = notification_of("%extended-output %5 1234 : late bytes");
  const auto parsed = libtmux::parse(held);
  EXPECT_EQ(parsed.kind, libtmux::NotificationKind::extended_output);
  EXPECT_EQ(parsed.pane, "%5");
  ASSERT_TRUE(parsed.age.has_value());
  EXPECT_EQ(*parsed.age, 1234U);
  EXPECT_EQ(as_text(parsed.payload), "late bytes");
}

TEST(NotificationParse, IgnoresFutureExtendedOutputFieldsBeforeTheDelimiter) {
  const auto held =
      notification_of("%extended-output %5 1234 future fields : late bytes");
  const auto parsed = libtmux::parse(held);
  EXPECT_EQ(parsed.kind, libtmux::NotificationKind::extended_output);
  EXPECT_EQ(parsed.pane, "%5");
  ASSERT_TRUE(parsed.age.has_value());
  EXPECT_EQ(*parsed.age, 1234U);
  EXPECT_EQ(as_text(parsed.payload), "late bytes");
}

TEST(NotificationParse, RejectsAnExtendedOutputAgeWithTrailingText) {
  const auto held = notification_of("%extended-output %5 1234ms : late bytes");
  const auto parsed = libtmux::parse(held);
  EXPECT_EQ(parsed.kind, libtmux::NotificationKind::extended_output);
  EXPECT_FALSE(parsed.age.has_value());
  EXPECT_EQ(as_text(parsed.payload), "late bytes");
}

// A name this build does not know is a newer tmux, not a failure: the set has
// only ever grown across the supported range.
TEST(NotificationParse, ReportsAnUnknownNameWithoutLosingIt) {
  const auto held = notification_of("%something-tmux-added-later @9 detail");
  const auto parsed = libtmux::parse(held);
  EXPECT_EQ(parsed.kind, libtmux::NotificationKind::unknown);
  EXPECT_EQ(parsed.name, "%something-tmux-added-later");
  EXPECT_EQ(parsed.window, "@9");
  EXPECT_EQ(libtmux::to_string(libtmux::NotificationKind::unknown), "unknown");
}
