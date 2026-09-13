#include "libtmux/layout.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "backend.hpp"

LIBTMUX_NAMESPACE_BEGIN

namespace {

bool named_layout(std::string_view layout, bool mirrored) {
  constexpr std::array<std::string_view, 7> names{
      "even-horizontal",       "even-vertical", "main-horizontal",
      "main-vertical",         "tiled",         "main-horizontal-mirrored",
      "main-vertical-mirrored"};
  const std::size_t count = mirrored ? names.size() : 5U;
  std::size_t matches{};
  for (std::size_t index = 0; index < count; ++index) {
    if (layout == names[index])
      return true;
    if (names[index].starts_with(layout))
      ++matches;
  }
  return matches == 1;
}

bool layout_needs_version(std::string_view layout) {
  return named_layout(layout, false) != named_layout(layout, true);
}

// Validate the saved-layout grammar without arranging or resizing any panes.
// tmux 3.3a crashes on unknown names; 3.7c also crashes on empty custom nodes.
struct LayoutSyntax {
  std::string_view input;
  std::size_t offset{};
  std::size_t leaves{};

  bool take(char value) {
    if (offset == input.size() || input[offset] != value)
      return false;
    ++offset;
    return true;
  }

  bool number(std::uint32_t& value) {
    const auto start = offset;
    value = 0;
    while (offset < input.size() && input[offset] >= '0' && input[offset] <= '9') {
      const auto digit = static_cast<std::uint32_t>(input[offset] - '0');
      if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U)
        return false;
      value = value * 10U + digit;
      ++offset;
    }
    return offset > start;
  }

  bool number() {
    std::uint32_t ignored{};
    return number(ignored);
  }

  bool cell(unsigned depth) {
    if (depth > 256 || !number() || !take('x') || !number() || !take(',') ||
        !number() || !take(',') || !number())
      return false;
    if (offset < input.size() && input[offset] == ',') {
      // A pane ID is optional; a following width belongs to the next cell.
      const auto saved = offset++;
      if (!number() || (offset < input.size() && input[offset] == 'x'))
        offset = saved;
    }
    if (offset == input.size() || (input[offset] != '[' && input[offset] != '{')) {
      ++leaves;
      return true;
    }
    const char close = input[offset++] == '[' ? ']' : '}';
    do {
      if (!cell(depth + 1))
        return false;
    } while (take(','));
    return take(close);
  }
};

std::optional<std::string> layout_error(std::string_view layout, std::size_t panes,
                                        std::optional<bool> mirrored = std::nullopt) {
  if (layout.empty())
    return "layout is empty";
  if (mirrored ? named_layout(layout, *mirrored)
               : named_layout(layout, false) || named_layout(layout, true))
    return std::nullopt;
  if (layout.size() < 6 || layout[4] != ',')
    return "unknown or ambiguous layout name";
  std::uint32_t expected{};
  for (const char digit : layout.substr(0, 4)) {
    const auto value = digit >= '0' && digit <= '9'   ? digit - '0'
                       : digit >= 'a' && digit <= 'f' ? digit - 'a' + 10
                       : digit >= 'A' && digit <= 'F' ? digit - 'A' + 10
                                                      : -1;
    if (value < 0)
      return "a custom layout starts with four hexadecimal checksum digits";
    expected = expected * 16U + static_cast<std::uint32_t>(value);
  }
  layout.remove_prefix(5);
  std::uint32_t checksum{};
  for (const char byte : layout) {
    checksum = (checksum >> 1U) | ((checksum & 1U) << 15U);
    checksum = (checksum + static_cast<unsigned char>(byte)) & 0xffffU;
  }
  if (checksum != expected)
    return "custom layout checksum does not match";
  LayoutSyntax parser{layout};
  if (!parser.cell(0) || parser.offset != layout.size())
    return "malformed custom layout tree (maximum depth is 256)";
  if (parser.leaves < panes)
    return "custom layout has fewer cells than requested panes";
  return std::nullopt;
}

CommandFailure invalid_layout(std::string reason) {
  return {.kind = FailureKind::validation,
          .delivery = DeliveryStatus::not_started,
          .diagnostic = std::move(reason)};
}

} // namespace

expected<void, CommandFailure> validate_layout(std::string_view layout,
                                               std::size_t minimum_panes) {
  if (auto error = layout_error(layout, minimum_panes))
    return unexpected(invalid_layout(std::move(*error)));
  return {};
}

expected<void, LayoutFailure>
detail::validate_layouts(const Backend& backend, std::span<const LayoutRequest> layouts,
                         bool allow_cold) {
  std::optional<std::size_t> sensitive;
  for (std::size_t index = 0; index < layouts.size(); ++index) {
    const auto& request = layouts[index];
    if (auto checked = validate_layout(request.layout, request.minimum_panes); !checked)
      return unexpected(LayoutFailure{index, std::move(checked.error())});
    if (!sensitive && layout_needs_version(request.layout))
      sensitive = index;
  }
  if (!sensitive)
    return {};

  const auto& policy = backend.policy();
  const auto running = backend.run({"display-message", "-p", "#{version}"},
                                   policy.timeout, policy.output_limit);
  std::optional<Version> version;
  if (running) {
    const auto parsed = parse_version("tmux " + *running);
    if (!parsed)
      return unexpected(LayoutFailure{
          *sensitive,
          invalid_layout("cannot determine tmux daemon version for layout")});
    version = *parsed;
  } else {
    const auto& failure = running.error();
    if (!allow_cold || !backend.allows_layout_client_version() ||
        failure.kind != FailureKind::missing ||
        failure.delivery != DeliveryStatus::not_started)
      return unexpected(LayoutFailure{*sensitive, failure});
    const auto client = backend.version();
    if (!client)
      return unexpected(LayoutFailure{*sensitive, client.error()});
    version = *client;
  }
  const bool mirrored = *version >= Version{.major = 3, .minor = 5};
  for (std::size_t index = 0; index < layouts.size(); ++index) {
    const auto& request = layouts[index];
    if (layout_needs_version(request.layout))
      if (auto error = layout_error(request.layout, request.minimum_panes, mirrored))
        return unexpected(LayoutFailure{index, invalid_layout(std::move(*error))});
  }
  return {};
}

LIBTMUX_NAMESPACE_END
