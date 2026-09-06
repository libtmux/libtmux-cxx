#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtmux/server.hpp"
#include "libtmux_consumers/mcp.hpp"
#include "pane_input.hpp"

namespace libtmux::mcp::detail {

// The sentence every tool description opens with, naming the authority the
// caller is granting before the tool says what it does.
//
// One definition on purpose: the catalog prepends this when it builds a tool
// and the registry validator re-derives it to check the tool kept it, so two
// copies would be two predicates enforcing one invariant. They had already
// drifted — the builder keyed the change sentence off the toolset while the
// validator keyed it off the declared effect, which agree only for as long as
// no tool pairs `inspect` with `Effect::change`.
[[nodiscard]] inline std::string_view
controlled_opener(Toolset toolset, ProcessReach reach,
                  const std::set<OutputClass>& outputs,
                  const std::set<Effect>& effects) {
  switch (reach) {
  case ProcessReach::configured_process:
    return "Start a pane's configured process; accepts no command payload.";
  case ProcessReach::pane_input:
    return "Send input to a pane's program; a shell that receives it runs it with "
           "your user's permissions.";
  case ProcessReach::pane_command:
    return "Run a shell command in a pane with your user's permissions.";
  case ProcessReach::none:
    break;
  }
  if (toolset == Toolset::teardown) {
    return "Delete tmux state; accepts no command payload.";
  }
  if (outputs.contains(OutputClass::terminal_content)) {
    return "Read pane output; accepts no client-supplied executable input. Returned "
           "content may be sensitive or untrusted.";
  }
  if (outputs.contains(OutputClass::process_environment)) {
    return "Read the tmux environment; accepts no client-supplied executable input. "
           "Returned values may contain secrets.";
  }
  if (outputs.contains(OutputClass::configured_command)) {
    return "Read configured tmux commands; accepts no client-supplied executable "
           "input. Returned values may contain executable configuration.";
  }
  if (effects.contains(Effect::change) || effects.contains(Effect::delete_)) {
    return "Change tmux state; no client-supplied executable input.";
  }
  return "Inspect tmux metadata; accepts no client-supplied executable input.";
}

inline constexpr std::size_t kTargetCharacters = 512U;
inline constexpr std::size_t kSearchCharacters = 4096U;
inline constexpr std::size_t kRegexWorkUnits = 8U * 1024U * 1024U;
inline constexpr std::size_t kSearchMatchLimit = 1024U;
inline constexpr std::size_t kReadBatchResponseBytes = 1'000'000U;

class BoundedRegex {
public:
  [[nodiscard]] static libtmux::expected<BoundedRegex, std::string>
  compile(std::string_view pattern);
  [[nodiscard]] libtmux::expected<bool, std::string>
  search(std::string_view text, std::size_t& remaining_work) const;

private:
  struct Atom {
    char value{};
    bool any{};
    bool repeated{};
  };

  explicit BoundedRegex(std::vector<Atom> atoms) : atoms_{std::move(atoms)} {}

  std::vector<Atom> atoms_;
};

struct ShellCommandPayload {
  std::string marker;
  std::string text;
};

struct ShellCommandCompletion {
  int exit_code{};
  std::size_t text_begin{};
  std::size_t record_begin{};
};

enum class PaneInputScope { effective_cohort, target_only, singular_posix_shell };

enum class PaneInputCallerRelation { detached, foreign, selected };

struct PaneInputCaller {
  PaneInputCallerRelation relation{};
  std::string pane_id;
  std::string session_id;
  std::uint64_t server_pid{};
  PaneInputEndpointIdentity endpoint;

  [[nodiscard]] static PaneInputCaller detached() {
    return PaneInputCaller{.relation = PaneInputCallerRelation::detached,
                           .pane_id = {},
                           .session_id = {},
                           .server_pid = 0U,
                           .endpoint = {}};
  }
  [[nodiscard]] static PaneInputCaller foreign(std::string pane_id,
                                               std::string session_id,
                                               std::uint64_t server_pid,
                                               PaneInputEndpointIdentity endpoint) {
    return PaneInputCaller{.relation = PaneInputCallerRelation::foreign,
                           .pane_id = std::move(pane_id),
                           .session_id = std::move(session_id),
                           .server_pid = server_pid,
                           .endpoint = endpoint};
  }
  [[nodiscard]] static PaneInputCaller selected(std::string pane_id,
                                                std::string session_id,
                                                std::uint64_t server_pid,
                                                PaneInputEndpointIdentity endpoint) {
    return PaneInputCaller{.relation = PaneInputCallerRelation::selected,
                           .pane_id = std::move(pane_id),
                           .session_id = std::move(session_id),
                           .server_pid = server_pid,
                           .endpoint = endpoint};
  }

  bool operator==(const PaneInputCaller&) const = default;
};

struct PaneInputMemberState {
  std::string pane_id;
  std::string window_id;
  std::uint32_t window_index{};
  std::string session_id;
  bool synchronized{};
  std::uint64_t mode{};
  bool dead{};
  bool input_off{};

  auto operator<=>(const PaneInputMemberState&) const = default;
};

struct PaneInputClientState {
  std::string session_id;
  std::string window_id;
  std::uint32_t window_index{};
  std::string pane_id;
  bool zoomed{};

  auto operator<=>(const PaneInputClientState&) const = default;
};

struct PaneInputPreflight {
  std::vector<std::string> configured_pane_ids;
  std::vector<PaneInputMemberState> configured_state;
  std::vector<PaneInputClientState> terminal_clients;
  PaneInputCaller caller;
  std::string endpoint_path;
  std::string window_id;
  std::uint64_t server_pid{};
  std::uint64_t server_start_time{};
  std::string server_process_generation;
  std::string foreground_command;
};

using ShellNonceFactory = std::function<std::string()>;

[[nodiscard]] libtmux::expected<ShellCommandPayload, ToolError>
shell_command_payload(std::string_view command, std::string_view tmux_executable,
                      std::string_view socket_path, std::string_view current_shell,
                      ShellNonceFactory next_nonce);
[[nodiscard]] libtmux::expected<std::string, ToolError>
resolve_executable(std::string_view search_path,
                   const std::filesystem::path& current_directory);
[[nodiscard]] std::optional<ShellCommandCompletion>
shell_command_completion(std::string_view capture, std::string_view marker,
                         std::size_t search_begin = 0U);
[[nodiscard]] std::optional<std::string>
parse_linux_process_generation(std::string_view stat);

[[nodiscard]] const std::string* argument(const Arguments& arguments,
                                          std::string_view name);
[[nodiscard]] ToolOutput output(StructuredValue::Object structured,
                                std::optional<std::size_t> maximum_response_bytes = {});
[[nodiscard]] ToolError tmux_error(const CommandFailure& error);
[[nodiscard]] libtmux::expected<PaneInputCaller, ToolError>
parse_pane_input_caller(std::optional<std::string> tmux,
                        std::optional<std::string> tmux_pane,
                        const std::filesystem::path& selected_socket_path);
[[nodiscard]] libtmux::expected<PaneInputPreflight, ToolError>
parse_pane_input_snapshot(std::string_view source_pane_id, std::string raw,
                          PaneInputScope scope, std::string clients = {},
                          PaneInputCaller caller = PaneInputCaller::detached());
[[nodiscard]] libtmux::expected<PaneInputPreflight, ToolError>
preflight_pane_input(const Server& server, std::string_view source_pane_id,
                     PaneInputScope scope);
[[nodiscard]] bool same_pane_input_route(const PaneInputPreflight& initial,
                                         const PaneInputPreflight& final);
[[nodiscard]] StructuredValue session_value(const Session& session);
[[nodiscard]] StructuredValue window_value(const Window& window);
[[nodiscard]] StructuredValue pane_value(const Pane& pane);

} // namespace libtmux::mcp::detail
