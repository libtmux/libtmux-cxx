#pragma once

// What a tmux daemon has to be for each optional feature, in one table.
//
// The gates that refuse or repair read their releases from here, and the root
// `capabilities.json` that the documentation site publishes is written from
// the same table, so what the library does and what the site says cannot
// differ. A capability is something a caller can do from a release on; a
// workaround is a defect that exact releases have. Defects the library never
// meets are not recorded.

#include <span>
#include <string>
#include <string_view>

#include "libtmux/abi.hpp"
#include "libtmux/version.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace support::capabilities {

/// How the library answers where a capability is missing.
enum class Handling { refuse, fallback, repair };

/// A release as the gate compares it and as tmux prints it.
struct Release {
  Version version;
  std::string_view text;
};

/// A tmux command a record concerns, with the flags that matter. A literal
/// command is held to a record's gate only if the record lists it here.
struct CommandUse {
  std::string_view command;
  std::span<const std::string_view> flags;
};

enum class CapabilityId {
  control_mode,
  buffer_delete_named,
  layout_mirrored_main,
  layout_json,
  clipboard_query,
};

enum class WorkaroundId {
  break_pane_misnames_windows,
  name_separators_rejected,
};

/// Something a caller can do from `since` on.
struct Capability {
  CapabilityId key;
  std::string_view id;
  std::string_view name;
  std::string_view summary;
  Release since;
  Handling handling;
  /// The public type a refusal arrives as: what the listed APIs return as the
  /// error of their `expected`, or a struct they return that holds one, as
  /// `LayoutFailure` holds a `CommandFailure`. Empty unless `handling` is
  /// `refuse`.
  std::string_view error;
  std::span<const std::string_view> apis;
  std::span<const CommandUse> commands;
};

/// A defect that only the listed releases have.
struct Workaround {
  WorkaroundId key;
  std::string_view id;
  std::string_view name;
  std::string_view summary;
  std::span<const Release> releases;
  Handling handling;
  std::span<const std::string_view> apis;
  std::span<const CommandUse> commands;
};

[[nodiscard]] std::span<const Capability> all_capabilities() noexcept;
[[nodiscard]] std::span<const Workaround> all_workarounds() noexcept;

[[nodiscard]] const Capability& capability(CapabilityId id) noexcept;
[[nodiscard]] const Workaround& workaround(WorkaroundId id) noexcept;

/// Whether `running` is a release on which the defect exists.
[[nodiscard]] bool affects(WorkaroundId id, const Version& running) noexcept;

/// The manifest `libtmux-capabilities/1`, byte for byte what the root
/// `capabilities.json` holds.
[[nodiscard]] std::string manifest_json();

} // namespace support::capabilities
LIBTMUX_NAMESPACE_END
