#include "libtmux/control.hpp"

#include <algorithm>
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

} // namespace
