#include "libtmux/control.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
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

  std::vector<Event> events;
  const auto consume_line =
      [&](std::span<const std::byte> view) -> expected<void, ProtocolError> {
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
        return {};
      }
      // Counted in full, retained up to the bound. Draining the rest is what
      // keeps the next command's reply attributable: a parser that stopped
      // reading here would meet `%end` mid-body and lose the stream.
      const std::size_t arriving = view.size() + 1U;
      block_->body_bytes += arriving;
      if (retained_reply_bytes_ == 0U ||
          block_->body.size() + arriving <= retained_reply_bytes_) {
        block_->body.insert(block_->body.end(), view.begin(), view.end());
        block_->body.push_back(std::byte{0x0a});
      } else if (!block_->body_truncated) {
        block_->body_truncated = true;
        // Nothing more will be added, so the held bytes are all that is ever
        // needed. Without this the vector keeps whatever growth reserved.
        block_->body.shrink_to_fit();
      }
      return {};
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
      return {};
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
    return {};
  };

  const auto reject_oversized_line =
      [&]() -> expected<std::vector<Event>, ProtocolError> {
    failure_ = ProtocolError{"control line exceeded " + std::to_string(line_bytes_) +
                             " bytes"};
    pending_.clear();
    pending_.shrink_to_fit();
    return unexpected(*failure_);
  };

  std::size_t cursor = 0U;
  while (cursor < input.size()) {
    const auto remaining = input.subspan(cursor);
    const auto newline = std::find(remaining.begin(), remaining.end(), std::byte{0x0a});
    const bool complete = newline != remaining.end();
    const std::size_t segment_bytes =
        static_cast<std::size_t>(newline - remaining.begin());

    if (line_bytes_ != 0U && (pending_.size() > line_bytes_ ||
                              segment_bytes > line_bytes_ - pending_.size())) {
      // Nothing can be handed back and there is nowhere to put the rest. A
      // line this long is not a large answer — bodies arrive as many lines —
      // so the stream is wrong and cannot be resynchronised.
      return reject_oversized_line();
    }

    if (!complete) {
      pending_.insert(pending_.end(), remaining.begin(), remaining.end());
      break;
    }

    expected<void, ProtocolError> consumed;
    if (pending_.empty()) {
      consumed = consume_line(remaining.first(segment_bytes));
    } else {
      pending_.insert(pending_.end(), remaining.begin(), newline);
      consumed = consume_line(std::span<const std::byte>{pending_});
      pending_.clear();
    }
    if (!consumed) {
      return unexpected(consumed.error());
    }
    cursor += segment_bytes + 1U;
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

namespace {

struct KindName {
  std::string_view name;
  NotificationKind kind;
};

// Known notification names in the supported tmux range.
constexpr auto kNotificationNames = std::to_array<KindName>({
    {"%output", NotificationKind::output},
    {"%extended-output", NotificationKind::extended_output},
    {"%pause", NotificationKind::paused},
    {"%continue", NotificationKind::resumed},
    {"%sessions-changed", NotificationKind::sessions_changed},
    {"%session-changed", NotificationKind::session_changed},
    {"%session-renamed", NotificationKind::session_renamed},
    {"%session-window-changed", NotificationKind::session_window_changed},
    {"%client-detached", NotificationKind::client_detached},
    {"%client-session-changed", NotificationKind::client_session_changed},
    {"%window-add", NotificationKind::window_add},
    {"%window-close", NotificationKind::window_close},
    {"%window-renamed", NotificationKind::window_renamed},
    {"%window-pane-changed", NotificationKind::window_pane_changed},
    {"%unlinked-window-add", NotificationKind::unlinked_window_add},
    {"%unlinked-window-close", NotificationKind::unlinked_window_close},
    {"%unlinked-window-renamed", NotificationKind::unlinked_window_renamed},
    {"%pane-mode-changed", NotificationKind::pane_mode_changed},
    {"%paste-buffer-changed", NotificationKind::paste_buffer_changed},
    {"%paste-buffer-deleted", NotificationKind::paste_buffer_deleted},
    {"%subscription-changed", NotificationKind::subscription_changed},
    {"%config-error", NotificationKind::config_error},
    {"%exit", NotificationKind::exit},
    {"%layout-change", NotificationKind::layout_change},
    {"%message", NotificationKind::message},
});

// The arguments before any free text or payload, split on single spaces. Only
// the leading region is tokenised: an output payload is already unescaped, so
// it may hold spaces and newlines that are data rather than separators.
std::vector<std::string_view> leading_fields(std::string_view line, std::size_t limit) {
  std::vector<std::string_view> fields;
  std::size_t index = 0;
  while (index < line.size() && fields.size() < limit) {
    const auto space = line.find(' ', index);
    if (space == std::string_view::npos) {
      fields.push_back(line.substr(index));
      break;
    }
    fields.push_back(line.substr(index, space - index));
    index = space + 1U;
  }
  return fields;
}

void place_argument(ParsedNotification& parsed, std::string_view field) {
  if (field.size() > 1U && field.front() == '$') {
    parsed.session = field;
  } else if (field.size() > 1U && field.front() == '@') {
    parsed.window = field;
  } else if (field.size() > 1U && field.front() == '%') {
    parsed.pane = field;
  }
}

} // namespace

std::string_view to_string(NotificationKind kind) noexcept {
  for (const KindName& known : kNotificationNames) {
    if (known.kind == kind) {
      return known.name;
    }
  }
  return "unknown";
}

ParsedNotification parse(const Notification& notification) {
  ParsedNotification parsed;
  const std::string_view line = text(notification.body);
  const auto first_space = line.find(' ');
  parsed.name = line.substr(0, first_space);
  for (const KindName& known : kNotificationNames) {
    if (known.name == parsed.name) {
      parsed.kind = known.kind;
      break;
    }
  }
  if (first_space == std::string_view::npos) {
    return parsed;
  }

  // `%output` has one field before its byte payload.
  if (parsed.kind == NotificationKind::output) {
    constexpr std::size_t argument_count = 2U;
    const auto fields = leading_fields(line, argument_count);
    if (fields.size() < argument_count) {
      return parsed;
    }
    place_argument(parsed, fields[1]);
    std::size_t consumed = 0;
    for (std::size_t index = 0; index < argument_count; ++index) {
      consumed += fields[index].size() + 1U;
    }
    if (consumed <= notification.body.size()) {
      parsed.payload = std::span<const std::byte>{notification.body}.subspan(consumed);
    }
    return parsed;
  }
  // Extended output may grow fields before the delimiter; tmux requires
  // callers to ignore them.
  if (parsed.kind == NotificationKind::extended_output) {
    const auto fields = leading_fields(line, 3U);
    if (fields.size() < 3U) {
      return parsed;
    }
    place_argument(parsed, fields[1]);
    std::uint64_t age = 0;
    const auto& digits = fields[2];
    const auto converted =
        std::from_chars(digits.data(), digits.data() + digits.size(), age);
    if (converted.ec == std::errc{} && converted.ptr == digits.data() + digits.size()) {
      parsed.age = age;
    }
    const auto delimiter = line.find(
        " : ", static_cast<std::size_t>(digits.data() - line.data()) + digits.size());
    if (delimiter != std::string_view::npos) {
      parsed.payload =
          std::span<const std::byte>{notification.body}.subspan(delimiter + 3U);
    }
    return parsed;
  }

  // Everything else is `%name` then space-separated arguments, with any free
  // text — a session or window or buffer name — last.
  const auto fields = leading_fields(line, 8U);
  std::size_t placed = 0;
  for (std::size_t index = 1; index < fields.size(); ++index) {
    const auto before = std::tuple{parsed.session, parsed.window, parsed.pane};
    place_argument(parsed, fields[index]);
    if (std::tuple{parsed.session, parsed.window, parsed.pane} != before) {
      placed = index;
      continue;
    }
    // The first field that is not an id begins the free text, which runs to
    // the end of the line.
    parsed.text =
        line.substr(static_cast<std::size_t>(fields[index].data() - line.data()));
    return parsed;
  }
  static_cast<void>(placed);
  return parsed;
}

LIBTMUX_NAMESPACE_END
