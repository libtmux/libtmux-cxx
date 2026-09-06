#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "libtmux/server.hpp"
#include "libtmux_consumers/mcp.hpp"

namespace libtmux::mcp::detail {

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

  [[nodiscard]] static PaneInputCaller detached() {
    return PaneInputCaller{.relation = PaneInputCallerRelation::detached,
                           .pane_id = {},
                           .session_id = {},
                           .server_pid = 0U};
  }
  [[nodiscard]] static PaneInputCaller foreign() {
    return PaneInputCaller{.relation = PaneInputCallerRelation::foreign,
                           .pane_id = {},
                           .session_id = {},
                           .server_pid = 0U};
  }
  [[nodiscard]] static PaneInputCaller
  selected(std::string pane_id, std::string session_id, std::uint64_t server_pid) {
    return PaneInputCaller{.relation = PaneInputCallerRelation::selected,
                           .pane_id = std::move(pane_id),
                           .session_id = std::move(session_id),
                           .server_pid = server_pid};
  }
};

struct PaneInputPreflight {
  std::vector<std::string> configured_pane_ids;
  std::string foreground_command;
};

using ShellNonceFactory = std::function<std::string()>;

[[nodiscard]] libtmux::expected<ShellCommandPayload, ToolError>
shell_command_payload(std::string_view command, std::string_view tmux_executable,
                      std::string_view socket_path, ShellNonceFactory next_nonce);
[[nodiscard]] libtmux::expected<std::string, ToolError>
resolve_executable(std::string_view search_path,
                   const std::filesystem::path& current_directory);
[[nodiscard]] std::optional<ShellCommandCompletion>
shell_command_completion(std::string_view capture, std::string_view marker,
                         std::size_t search_begin = 0U);

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
[[nodiscard]] StructuredValue session_value(const Session& session);
[[nodiscard]] StructuredValue window_value(const Window& window);
[[nodiscard]] StructuredValue pane_value(const Pane& pane);

} // namespace libtmux::mcp::detail
