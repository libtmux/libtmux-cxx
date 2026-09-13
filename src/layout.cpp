#include "libtmux/layout.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
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

bool layout_space(char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
         value == '\f' || value == '\v';
}

bool json_layout(std::string_view layout) {
  while (!layout.empty() && layout_space(layout.front()))
    layout.remove_prefix(1);
  return layout.starts_with('{');
}

bool layout_needs_version(std::string_view layout) {
  return json_layout(layout) ||
         named_layout(layout, false) != named_layout(layout, true);
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

// tmux's v2 layout reader accepts a JSON subset: object-only arrays, Int64
// numbers, nonempty strings, no null, and validated but undecoded escapes.
// Keep only field metadata; the caller dispatches the original saved layout.
struct JsonLayoutSyntax {
  enum class Type { absent, string, number, boolean, object, array };
  enum class Role { root, cell, ignored };
  struct Value {
    Type type{Type::absent};
    std::string_view text;
    std::int64_t number{};
    std::size_t count{};
    bool boolean{};
  };
  using Fields = std::map<std::string_view, Value>;

  std::string_view input;
  std::size_t offset{};
  std::size_t leaves{};
  unsigned active{};
  std::set<std::int64_t> indexes{}, floating{}, last{};
  std::string_view error{"malformed JSON saved layout (maximum object depth is 200)"};

  bool fail(std::string_view reason) {
    error = reason;
    return false;
  }
  void space() {
    while (offset < input.size() &&
           std::string_view{" \t\r\n"}.find(input[offset]) != std::string_view::npos)
      ++offset;
  }
  bool take(char value) {
    space();
    if (offset == input.size() || input[offset] != value)
      return false;
    ++offset;
    return true;
  }
  static bool hex(char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
  }
  bool string(std::string_view& result) {
    if (!take('"'))
      return false;
    const auto start = offset;
    while (offset < input.size()) {
      const char value = input[offset++];
      if (value == '"') {
        result = input.substr(start, offset - start - 1);
        return !result.empty();
      }
      if (static_cast<unsigned char>(value) < 0x20U)
        return false;
      if (value != '\\')
        continue;
      if (offset == input.size())
        return false;
      const char escaped = input[offset++];
      if (escaped == 'u') {
        for (unsigned digit = 0; digit < 4; ++digit)
          if (offset == input.size() || !hex(input[offset++]))
            return false;
      } else if (std::string_view{"\"\\/bfnrt"}.find(escaped) == std::string_view::npos)
        return false;
    }
    return false;
  }
  bool value(Value& result, Role role, unsigned depth) {
    space();
    if (offset == input.size())
      return false;
    if (input[offset] == '{') {
      result.type = Type::object;
      return object(role, depth + 1);
    }
    if (take('[')) {
      result.type = Type::array;
      if (take(']'))
        return true;
      do {
        if (!object(role, depth + 1))
          return false;
        ++result.count;
      } while (take(','));
      return take(']');
    }
    if (input[offset] == '"') {
      result.type = Type::string;
      return string(result.text);
    }
    for (const auto literal : {std::string_view{"true"}, std::string_view{"false"}}) {
      if (input.substr(offset).starts_with(literal)) {
        offset += literal.size();
        result.type = Type::boolean;
        result.boolean = literal == "true";
        return true;
      }
    }
    const auto start = offset;
    if (input[offset] == '-')
      ++offset;
    const auto digits = offset;
    while (offset < input.size() && input[offset] >= '0' && input[offset] <= '9')
      ++offset;
    if (digits == offset || (input[digits] == '0' && offset - digits != 1))
      return false;
    result.type = Type::number;
    const auto parsed =
        std::from_chars(input.data() + start, input.data() + offset, result.number);
    return parsed.ec == std::errc{} && parsed.ptr == input.data() + offset;
  }
  static const Value& field(const Fields& fields, std::string_view key) {
    static const Value absent{};
    const auto found = fields.find(key);
    return found == fields.end() ? absent : found->second;
  }
  static bool range(const Value& value, std::int64_t low, std::int64_t high) {
    return value.type == Type::number && value.number >= low && value.number <= high;
  }
  bool cell(const Fields& fields) {
    const auto& type = field(fields, "t");
    if (type.type != Type::string ||
        (type.text != "p" && type.text != "h" && type.text != "v"))
      return fail("JSON layout cell type must be p, h or v");
    if (!range(field(fields, "w"), 1, 10000) || !range(field(fields, "h"), 1, 10000) ||
        !range(field(fields, "x"), -10000, 10000) ||
        !range(field(fields, "y"), -10000, 10000))
      return fail("JSON layout cell dimensions or offsets are outside tmux's bounds");
    const auto& children = field(fields, "c");
    if (type.text != "p")
      return (children.type == Type::array && children.count >= 2) ||
             fail("JSON layout nodes require at least two children");
    if (children.type != Type::absent)
      return fail("JSON layout panes cannot have children");
    constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
    const auto& index = field(fields, "i");
    if (!range(index, 0, maximum) || !indexes.insert(index.number).second)
      return fail("JSON layout pane indexes must be unique nonnegative Int32 values");
    const auto& selected = field(fields, "a");
    const auto& previous = field(fields, "l");
    // Native tmux ignores l whenever a is present, including a:false.
    if (selected.type != Type::absent) {
      if (selected.type != Type::boolean || (selected.boolean && ++active > 1))
        return fail("JSON layout permits only one active pane, marked by a boolean");
    } else if (previous.type != Type::absent &&
               (!range(previous, 0, maximum) || !last.insert(previous.number).second))
      return fail(
          "JSON layout last-pane indexes must be unique nonnegative Int32 values");
    const auto& z = field(fields, "z");
    if (z.type != Type::absent &&
        (!range(z, 0, maximum - 1) || !floating.insert(z.number).second))
      return fail("JSON layout floating indexes must be unique and below INT32_MAX");
    ++leaves;
    return true;
  }
  bool object(Role role, unsigned depth) {
    if (depth > 200 || !take('{'))
      return false;
    Fields fields;
    if (!take('}')) {
      do {
        std::string_view key;
        Value parsed;
        if (!string(key) || fields.contains(key) || !take(':'))
          return false;
        const auto child =
            (role == Role::root && key == "L") || (role == Role::cell && key == "c")
                ? Role::cell
                : Role::ignored;
        if (!value(parsed, child, depth))
          return false;
        fields.emplace(key, parsed);
      } while (take(','));
      if (!take('}'))
        return false;
    }
    if (role == Role::root)
      return (range(field(fields, "V"), 2, 2) &&
              field(fields, "L").type == Type::object) ||
             fail("JSON saved layout requires V:2 and an L cell object");
    return role != Role::cell || cell(fields);
  }
  bool parse() {
    // layout_construct skips C whitespace before passing JSON to its tokenizer.
    while (offset < input.size() && layout_space(input[offset]))
      ++offset;
    if (!object(Role::root, 1))
      return false;
    space();
    return offset == input.size() && leaves != 0;
  }
};

std::optional<std::string> layout_error(std::string_view layout, std::size_t panes,
                                        std::optional<bool> mirrored = std::nullopt) {
  if (layout.empty())
    return "layout is empty";
  if (json_layout(layout)) {
    JsonLayoutSyntax parser{.input = layout};
    if (!parser.parse())
      return std::string{parser.error};
    if (parser.leaves < panes)
      return "custom layout has fewer cells than requested panes";
    return std::nullopt;
  }
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
    if (json_layout(request.layout) &&
        *version < Version{.major = 3, .minor = 9, .prerelease = true})
      return unexpected(LayoutFailure{
          index, invalid_layout("JSON saved layouts require tmux 3.9 or newer")});
    if (layout_needs_version(request.layout))
      if (auto error = layout_error(request.layout, request.minimum_panes, mirrored))
        return unexpected(LayoutFailure{index, invalid_layout(std::move(*error))});
  }
  return {};
}

LIBTMUX_NAMESPACE_END
