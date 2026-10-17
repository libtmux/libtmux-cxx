#pragma once

// Which tmux an endpoint runs, asked without side effects, and the fence that
// keeps a later command on the daemon that was asked.
//
// A client executable and the daemon it reaches can be different releases, so
// the daemon answers. `display-message` would fire a user's
// `after-display-message` hook on every release; `run-shell -C` has no after
// hook and fails while parsing its own output, so it fires neither that nor
// `command-error`. The failure message carries the version.

#include "libtmux/abi.hpp"
#include "libtmux/command.hpp"
#include "libtmux/expected.hpp"
#include "libtmux/version.hpp"
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "transport_values.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace support::versions {
using namespace detail;

/// What an endpoint reports: the daemon when one answers, otherwise the
/// executable that would start one.
struct EndpointVersion {
  bool live{false};
  Version version{};
  /// Exactly what the daemon printed for `#{version}`, which is what a fence
  /// compares against byte for byte.
  std::string token;
};

/// `libtmux-version-<random hex>`: a command name no tmux has, so a command
/// that reaches it fails with `unknown command: <marker>`.
[[nodiscard]] std::string version_marker();

/// Read the daemon at `selectors`, or the executable when none answers.
///
/// Anything but a version report or an absent endpoint is an error rather than
/// an absent server.
[[nodiscard]] expected<EndpointVersion, CommandFailure>
query_endpoint(const std::filesystem::path& binary,
               const std::vector<std::string>& selectors,
               std::optional<std::chrono::milliseconds> timeout);

/// Parse the failure text of the probe, `unknown command: <marker><version>`.
/// Nothing when `text` carries no such line.
[[nodiscard]] std::optional<std::string> probe_report(std::string_view text,
                                                      std::string_view marker);

/// Whether `text` is how a tmux older than 3.2 answers the probe: a 3.0a
/// daemon refusing the client, a 3.1c `run-shell` that has no `-C`, or a
/// client that rejects `-N`.
[[nodiscard]] bool answers_like_before_three_two(std::string_view text);

/// The refusal for such an answer, with the remedy.
[[nodiscard]] CommandFailure before_three_two_failure();

/// Whether `text` says no server answers on the endpoint.
[[nodiscard]] bool endpoint_is_absent(std::string_view text);

/// Parse `<executable> -V`: `tmux 3.3a`, or `<basename> 3.3a`, which is what
/// tmux 3.3a prints when it is not called `tmux`.
[[nodiscard]] std::optional<std::string>
executable_report(std::string_view output, const std::filesystem::path& binary);

/// Parse a version token the way a daemon or executable prints it.
[[nodiscard]] expected<EndpointVersion, CommandFailure>
read_report(std::string_view token, bool live);

/// Refuse a release below the floor, a development build that cannot be
/// ordered, and a version this library cannot read. Names whom it asked.
[[nodiscard]] expected<void, CommandFailure>
require_supported(const EndpointVersion& endpoint,
                  const Version& floor = kMinimumSupported);

/// The arguments that make the rest of a command list fail, with `marker` as
/// the unknown command, unless the daemon still reports `token`.
[[nodiscard]] std::vector<Argument> version_fence(std::string_view token,
                                                  std::string_view marker);

/// Whether a failed run stopped at the fence.
[[nodiscard]] bool fence_fired(std::string_view stderr_text, std::string_view marker);

[[nodiscard]] CommandFailure replaced_daemon();

/// Whether a command list holds a `break-pane` that names no window, however
/// the list spells the command: its alias, a unique prefix, or after a `;`. tmux
/// 3.7 crashes on that call and ignores an explicit name there, so only the
/// typed repair, which names the window and then corrects it, may send one.
[[nodiscard]] bool has_unnamed_break_pane(const std::vector<std::string>& argv);

/// A failure that stopped before any command was dispatched.
[[nodiscard]] CommandFailure version_check_failure(FailureKind kind,
                                                   std::string diagnostic);

} // namespace support::versions
LIBTMUX_NAMESPACE_END
