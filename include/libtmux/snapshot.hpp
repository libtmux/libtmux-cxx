#pragma once

// tmux format requests and the snapshots their output becomes.
//
// A snapshot is everything one tmux listing returned: the bytes, the rows
// parsed out of them, and the connection that produced them. Entities are a
// shared pointer to one of these plus a row index, which is why an entity can
// be copied, stored and returned with no owner to keep alive, and why
// iterating a filtered range never spawns tmux — the process ran once, when
// the snapshot was taken.
//
// A snapshot is a moment. Nothing in it changes when tmux does; asking for
// current state means taking another one.

#include "libtmux/abi.hpp"
#include "libtmux/command.hpp"
#include "libtmux/expected.hpp"
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN

namespace detail {
// Which tmux server to talk to, and how to run a command against it. Opaque
// here on purpose: no transport type appears in an installed header.
class Backend;
} // namespace detail

// tmux joins requested formats with a separator that cannot appear in a format
// name. U+241E matches the Python implementation's default so both can read the
// same recorded output.
//
// It is multi-byte, so every tmux this library starts is passed `-u`: a tmux
// that believes the terminal is not UTF-8 substitutes an underscore and no row
// splits at all.
inline constexpr std::string_view kFormatSeparator = "␞";

// Build the format argument for one entity's fields, terminating every field so
// a trailing empty value is still a value rather than a missing column.
[[nodiscard]] inline std::string
format_request(std::span<const std::string_view> fields) {
  std::string request;
  for (const std::string_view field : fields) {
    request += "#{";
    request += field;
    request += '}';
    request += kFormatSeparator;
  }
  return request;
}

// Split one tmux output line into its field values.
//
// The trailing separator emitted by `format_request` produces one empty tail
// element, which is dropped; a short or long row is reported rather than padded
// so a format-name typo cannot masquerade as an empty field.
[[nodiscard]] inline bool split_row(std::string_view line, std::size_t fields,
                                    std::vector<std::string_view>& values) {
  values.clear();
  std::size_t position = 0;
  while (position <= line.size()) {
    const std::size_t next = line.find(kFormatSeparator, position);
    if (next == std::string_view::npos) {
      break;
    }
    values.push_back(line.substr(position, next - position));
    position = next + kFormatSeparator.size();
  }
  return values.size() == fields && position == line.size();
}

// Where a tmux subcommand wants its format string. Most take `-F`;
// `display-message` takes the format as its message argument instead, and
// passing `-F` to it addresses a different thing entirely.
enum class FormatArgument { flag, message };

// Owning row storage, shared by every entity taken from it.
class Snapshot {
public:
  // Run `request` with this entity's fields appended and parse what came back.
  // Rows are parsed once, here: entities hold views into them, so a second
  // parse would invalidate every entity already handed out.
  [[nodiscard]] static expected<std::shared_ptr<const Snapshot>, CommandFailure>
  take(std::shared_ptr<const detail::Backend> backend,
       std::span<const std::string_view> fields, std::vector<std::string> request,
       FormatArgument placement = FormatArgument::flag);

  // Output that did not come from a live server: a recording, a fixture, a
  // test. Entities read and filter exactly as they would from a listing, and
  // anything that would run a command reports that there is no connection.
  // Returns null if a row does not match the fields it claims to have.
  [[nodiscard]] static std::shared_ptr<const Snapshot>
  from_recording(std::span<const std::string_view> fields, std::string output);

  Snapshot(const Snapshot&) = delete;
  Snapshot& operator=(const Snapshot&) = delete;
  Snapshot(Snapshot&&) = delete;
  Snapshot& operator=(Snapshot&&) = delete;
  ~Snapshot();

  [[nodiscard]] std::size_t index_of(std::string_view field) const noexcept;

  [[nodiscard]] const std::vector<std::vector<std::string_view>>&
  rows() const noexcept {
    return rows_;
  }

  // The connection this came from, so an entity can act on what it describes.
  [[nodiscard]] const std::shared_ptr<const detail::Backend>& backend() const noexcept {
    return backend_;
  }

private:
  Snapshot(std::shared_ptr<const detail::Backend> backend,
           std::vector<std::string_view> fields, std::string output);

  [[nodiscard]] bool parse();

  std::shared_ptr<const detail::Backend> backend_;
  std::vector<std::string_view> fields_;
  std::string output_;
  std::vector<std::vector<std::string_view>> rows_;
};

LIBTMUX_NAMESPACE_END
