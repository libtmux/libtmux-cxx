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

// A separator absent from every format *name* is not absent from every format
// *value*, which is the whole difficulty. `tmux rename-window 'a␞b'` is
// accepted, and one such name used to make every window and pane listing on
// that server fail to split — data the caller never chose breaking reads of
// everything else.
//
// So tmux escapes the separator before it can be mistaken for one. U+241B pairs
// with it and is escaped in turn, which is what makes the transform reversible:
// `␛S` is a separator that was in the value and `␛E` is an escape marker that
// was, and neither has a second reading.
inline constexpr std::string_view kFormatEscape = "␛";

// Build the format argument for one entity's fields, terminating every field so
// a trailing empty value is still a value rather than a missing column.
//
// The two substitutions nest rather than run in sequence, because tmux applies
// the inner one to the raw value and the outer one to its result. In that order
// a value already holding the escape marker is neutralised before the separator
// pass can produce one; reversed, the two become indistinguishable.
//
// `#{s/…/…/:…}` predates every tmux this library supports, and neither
// character is a regular-expression metacharacter.
//
// Unconditional, rather than applied only to the fields that could carry a
// separator. Expanding the substitutions costs about 0.32us per row — a 61-row
// listing pays 19us, against a process launch of some milliseconds — and a
// per-field exemption list is a thing to get wrong later, once, silently.
[[nodiscard]] inline std::string
format_request(std::span<const std::string_view> fields) {
  std::string request;
  for (const std::string_view field : fields) {
    request += "#{s/";
    request += kFormatSeparator;
    request += '/';
    request += kFormatEscape;
    request += "S/:#{s/";
    request += kFormatEscape;
    request += '/';
    request += kFormatEscape;
    request += "E/:";
    request += field;
    request += "}}";
    request += kFormatSeparator;
  }
  return request;
}

// Undo that escaping, in place.
//
// Escaping only ever lengthens, so the decoded bytes fit where the encoded ones
// were: the write cursor never overtakes the read cursor, nothing moves, and
// nothing is allocated. Answers the decoded length.
//
// An escape marker followed by anything else is left as written. Output this
// library asked for contains no such sequence, and failing on one would mean a
// recording could not carry a literal `␛`.
[[nodiscard]] inline std::size_t decode_value(char* begin, std::size_t size) noexcept {
  const std::string_view escape = kFormatEscape;
  std::size_t read = 0;
  std::size_t write = 0;
  while (read < size) {
    const bool marked = size - read > escape.size() &&
                        std::string_view{begin + read, escape.size()} == escape;
    const char tag = marked ? begin[read + escape.size()] : '\0';
    const std::string_view decoded = tag == 'S'   ? kFormatSeparator
                                     : tag == 'E' ? kFormatEscape
                                                  : std::string_view{};
    if (decoded.empty()) {
      begin[write++] = begin[read++];
      continue;
    }
    for (const char byte : decoded) {
      begin[write++] = byte;
    }
    read += escape.size() + 1U;
  }
  return write;
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
           std::vector<std::string> fields, std::string output);

  [[nodiscard]] bool parse();

  std::shared_ptr<const detail::Backend> backend_;
  // Owned, not viewed. The entity field arrays outlive everything, but
  // `from_recording` is public and takes whatever a caller passes: a snapshot
  // that outlived the names it was built from would report field indices
  // against freed memory. Every tmux format name is short enough to sit in the
  // string itself, so owning them allocates nothing.
  std::vector<std::string> fields_;
  std::string output_;
  std::vector<std::vector<std::string_view>> rows_;
};

LIBTMUX_NAMESPACE_END
