#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace libtmux::mcp::detail {

struct PaneServerIdentity {
  std::string socket_path;
  std::uint64_t server_pid{};
  std::uint64_t server_start_time{};

  bool operator==(const PaneServerIdentity&) const = default;
};

// Record before dispatch; roll back unless commit confirms possible delivery.
class PaneEchoEdit {
public:
  PaneEchoEdit(PaneEchoEdit&&) noexcept;
  PaneEchoEdit& operator=(PaneEchoEdit&&) noexcept;
  ~PaneEchoEdit();

  PaneEchoEdit(const PaneEchoEdit&) = delete;
  PaneEchoEdit& operator=(const PaneEchoEdit&) = delete;

  void commit() noexcept;

private:
  class Impl;
  explicit PaneEchoEdit(std::unique_ptr<Impl> implementation);
  std::unique_ptr<Impl> implementation_;

  friend PaneEchoEdit note_key_dispatch(const PaneServerIdentity&, std::string_view,
                                        std::string_view, bool);
  friend PaneEchoEdit note_literal_write(const PaneServerIdentity&, std::string_view,
                                         std::string_view, bool);
};

[[nodiscard]] PaneEchoEdit note_key_dispatch(const PaneServerIdentity& identity,
                                             std::string_view pane_id,
                                             std::string_view key, bool enter);

[[nodiscard]] PaneEchoEdit note_literal_write(const PaneServerIdentity& identity,
                                              std::string_view pane_id,
                                              std::string_view text, bool enter);

// Re-read pending input; retain recent echoes for the lifetime of a wait.
struct LiveEcho {
  bool input_pending{};
  std::string pending;
  std::vector<std::string> recent;
};

[[nodiscard]] LiveEcho
live_echo(const PaneServerIdentity& identity, std::string_view pane_id,
          std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now());

void prune_dead_panes(const PaneServerIdentity& identity,
                      const std::set<std::string>& live_pane_ids);

} // namespace libtmux::mcp::detail
