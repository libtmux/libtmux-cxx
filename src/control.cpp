#include "libtmux/control.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN
namespace {

using Bytes = std::vector<std::byte>;

bool starts_with(std::span<const std::byte> value, std::string_view prefix) {
  if (value.size() < prefix.size()) {
    return false;
  }
  for (std::size_t index = 0; index < prefix.size(); ++index) {
    if (value[index] !=
        static_cast<std::byte>(static_cast<unsigned char>(prefix[index]))) {
      return false;
    }
  }
  return true;
}

std::string_view text(std::span<const std::byte> value) {
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

Bytes bytes(std::string_view value) {
  Bytes result;
  result.reserve(value.size());
  for (const auto character : value) {
    result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  return result;
}

std::optional<std::uint64_t> decimal(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  std::uint64_t parsed = 0;
  const auto converted =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return parsed;
}

struct ParsedGuard {
  std::uint64_t sequence;
  std::uint64_t command_number;
  std::uint64_t flags;
  Bytes metadata;
};

expected<ParsedGuard, ProtocolError> parse_begin(std::span<const std::byte> line) {
  constexpr std::string_view prefix = "%begin ";
  if (!starts_with(line, prefix)) {
    return unexpected(ProtocolError{"malformed begin guard"});
  }
  const auto metadata = text(line.subspan(prefix.size()));
  const auto first_space = metadata.find(' ');
  if (first_space == std::string_view::npos || first_space == 0U) {
    return unexpected(ProtocolError{"malformed begin guard metadata"});
  }
  const auto second_space = metadata.find(' ', first_space + 1U);
  if (second_space == std::string_view::npos || second_space == first_space + 1U ||
      metadata.find(' ', second_space + 1U) != std::string_view::npos ||
      second_space + 1U == metadata.size()) {
    return unexpected(ProtocolError{"malformed begin guard metadata"});
  }
  const auto sequence = decimal(metadata.substr(0U, first_space));
  const auto command_number =
      decimal(metadata.substr(first_space + 1U, second_space - first_space - 1U));
  const auto flags = decimal(metadata.substr(second_space + 1U));
  if (!sequence || !command_number || !flags) {
    return unexpected(ProtocolError{"non-decimal begin guard metadata"});
  }
  return ParsedGuard{.sequence = *sequence,
                     .command_number = *command_number,
                     .flags = *flags,
                     .metadata = bytes(metadata)};
}

expected<Bytes, ProtocolError> decode_octal_payload(std::span<const std::byte> line,
                                                    std::size_t payload_start) {
  Bytes result;
  result.reserve(line.size());
  result.insert(result.end(), line.begin(),
                line.begin() + static_cast<std::ptrdiff_t>(payload_start));
  for (std::size_t index = payload_start; index < line.size();) {
    const auto current = std::to_integer<unsigned int>(line[index]);
    if (current != static_cast<unsigned int>('\\')) {
      result.push_back(line[index]);
      ++index;
      continue;
    }
    if (line.size() - index < 4U) {
      return unexpected(ProtocolError{"short pane-output octal escape"});
    }
    unsigned int decoded = 0U;
    for (std::size_t offset = 1U; offset <= 3U; ++offset) {
      const auto digit = std::to_integer<unsigned int>(line[index + offset]);
      if (digit < static_cast<unsigned int>('0') ||
          digit > static_cast<unsigned int>('7')) {
        return unexpected(ProtocolError{"invalid pane-output octal escape"});
      }
      decoded = decoded * 8U + (digit - static_cast<unsigned int>('0'));
    }
    if (decoded > 0xffU) {
      return unexpected(ProtocolError{"pane-output octal escape exceeds one byte"});
    }
    result.push_back(static_cast<std::byte>(decoded));
    index += 4U;
  }
  return result;
}

expected<Bytes, ProtocolError> notification_body(std::span<const std::byte> line) {
  constexpr std::string_view output_prefix = "%output ";
  constexpr std::string_view extended_prefix = "%extended-output ";
  if (starts_with(line, output_prefix)) {
    const auto value = text(line);
    const auto payload = value.find(' ', output_prefix.size());
    if (payload == std::string_view::npos || payload == output_prefix.size()) {
      return unexpected(ProtocolError{"malformed pane-output notification"});
    }
    return decode_octal_payload(line, payload + 1U);
  }
  if (starts_with(line, extended_prefix)) {
    const auto delimiter = text(line).find(" : ", extended_prefix.size());
    if (delimiter == std::string_view::npos) {
      return unexpected(ProtocolError{"malformed extended pane-output notification"});
    }
    return decode_octal_payload(line, delimiter + 3U);
  }
  return Bytes{line.begin(), line.end()};
}

bool marker(std::span<const std::byte> line, std::string_view name) {
  return text(line) == name || (starts_with(line, name) && line.size() > name.size() &&
                                line[name.size()] == std::byte{' '});
}

} // namespace

expected<std::vector<Event>, ProtocolError>
Parser::feed(std::span<const std::byte> input) {
  if (failure_) {
    return unexpected(*failure_);
  }
  if (finished_) {
    failure_ = ProtocolError{"control stream already finished"};
    return unexpected(*failure_);
  }

  pending_.insert(pending_.end(), input.begin(), input.end());
  std::vector<Event> events;
  for (;;) {
    const auto newline = std::find(pending_.begin(), pending_.end(), std::byte{0x0a});
    if (newline == pending_.end()) {
      // Nothing to hand back and nowhere to put the rest. A line this long is
      // not a large answer — bodies arrive as many lines — so it is the stream
      // that is wrong, and a control stream cannot be resynchronised.
      if (line_bytes_ != 0U && pending_.size() > line_bytes_) {
        failure_ = ProtocolError{"control line exceeded " +
                                 std::to_string(line_bytes_) + " bytes"};
        pending_.clear();
        pending_.shrink_to_fit();
        return unexpected(*failure_);
      }
      break;
    }
    Bytes line{pending_.begin(), newline};
    pending_.erase(pending_.begin(), newline + 1);
    const auto view = std::span<const std::byte>{line};

    if (block_) {
      const auto close = [&](std::string_view prefix,
                             ControlTerminal terminal) -> bool {
        if (!starts_with(view, prefix)) {
          return false;
        }
        const auto metadata = view.subspan(prefix.size());
        if (!std::ranges::equal(metadata, block_->begin_metadata)) {
          return false;
        }
        block_->terminal = terminal;
        block_->terminal_metadata.assign(metadata.begin(), metadata.end());
        events.emplace_back(std::move(*block_));
        block_.reset();
        return true;
      };
      if (close("%end ", ControlTerminal::end) ||
          close("%error ", ControlTerminal::error)) {
        continue;
      }
      // Counted in full, retained up to the bound. Draining the rest is what
      // keeps the next command's reply attributable: a parser that stopped
      // reading here would meet `%end` mid-body and lose the stream.
      const std::size_t arriving = line.size() + 1U;
      block_->body_bytes += arriving;
      if (retained_reply_bytes_ == 0U || block_->body.size() + arriving <=
                                             retained_reply_bytes_) {
        block_->body.insert(block_->body.end(), line.begin(), line.end());
        block_->body.push_back(std::byte{0x0a});
      } else if (!block_->body_truncated) {
        block_->body_truncated = true;
        // Nothing more will be added, so the held bytes are all that is ever
        // needed. Without this the vector keeps whatever growth reserved.
        block_->body.shrink_to_fit();
      }
      continue;
    }

    if (marker(view, "%begin")) {
      auto parsed = parse_begin(view);
      if (!parsed) {
        failure_ = std::move(parsed.error());
        return unexpected(*failure_);
      }
      block_ = ControlBlock{.sequence = parsed->sequence,
                            .command_number = parsed->command_number,
                            .terminal = ControlTerminal::end,
                            .begin_metadata = std::move(parsed->metadata),
                            .terminal_metadata = {},
                            .body = {}};
      continue;
    }
    if (marker(view, "%end") || marker(view, "%error")) {
      failure_ = ProtocolError{"terminal guard without an open block"};
      return unexpected(*failure_);
    }
    auto body = notification_body(view);
    if (!body) {
      failure_ = std::move(body.error());
      return unexpected(*failure_);
    }
    events.emplace_back(Notification{std::move(*body)});
  }
  return events;
}

expected<void, ProtocolError> Parser::finish() {
  if (failure_) {
    return unexpected(*failure_);
  }
  if (finished_) {
    return {};
  }
  if (!pending_.empty()) {
    failure_ = ProtocolError{"control stream ended with a partial line"};
    return unexpected(*failure_);
  }
  if (block_) {
    failure_ = ProtocolError{"control stream ended inside a command block"};
    return unexpected(*failure_);
  }
  finished_ = true;
  return {};
}

LIBTMUX_NAMESPACE_END
