#pragma once

#include <cstddef>
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

struct PaneInputPreflight {
  std::vector<std::string> configured_pane_ids;
  std::string foreground_command;
};

[[nodiscard]] ShellCommandPayload shell_command_payload(std::string_view command,
                                                        std::string_view nonce);
[[nodiscard]] std::optional<ShellCommandCompletion>
shell_command_completion(std::string_view capture, std::string_view marker,
                         std::size_t search_begin = 0U);

[[nodiscard]] const std::string* argument(const Arguments& arguments,
                                          std::string_view name);
[[nodiscard]] ToolOutput output(StructuredValue::Object structured,
                                std::optional<std::size_t> maximum_response_bytes = {});
[[nodiscard]] ToolError tmux_error(const CommandFailure& error);
[[nodiscard]] libtmux::expected<PaneInputPreflight, ToolError>
parse_pane_input_snapshot(std::string_view source_pane_id, std::string raw,
                          PaneInputScope scope);
[[nodiscard]] libtmux::expected<PaneInputPreflight, ToolError>
preflight_pane_input(const Server& server, std::string_view source_pane_id,
                     PaneInputScope scope);
[[nodiscard]] StructuredValue session_value(const Session& session);
[[nodiscard]] StructuredValue window_value(const Window& window);
[[nodiscard]] StructuredValue pane_value(const Pane& pane);

} // namespace libtmux::mcp::detail
