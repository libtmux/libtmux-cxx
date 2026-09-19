#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "libtmux/expected.hpp"
#include "libtmux_consumers/mcp.hpp"

namespace libtmux::mcp::detail {

inline constexpr auto kPaneInputSettlementProofTimeout = std::chrono::milliseconds{250};
inline constexpr std::array kPaneInputSettlementAttemptDelays{
    std::chrono::milliseconds{0},   std::chrono::milliseconds{20},
    std::chrono::milliseconds{40},  std::chrono::milliseconds{80},
    std::chrono::milliseconds{160}, std::chrono::milliseconds{250},
    std::chrono::milliseconds{250}, std::chrono::milliseconds{250}};

template <typename Stopped, typename Prove, typename Wait>
[[nodiscard]] bool prove_pane_input_settlement(Stopped stopped, Prove prove,
                                               Wait wait) {
  for (const auto delay : kPaneInputSettlementAttemptDelays) {
    if (stopped()) {
      return false;
    }
    wait(delay);
    if (stopped()) {
      return false;
    }
    if (prove()) {
      return true;
    }
  }
  return false;
}

enum class PaneInputReservationKind { input, run };

struct PaneInputEndpointIdentity {
  std::uintmax_t device{};
  std::uintmax_t inode{};

  auto operator<=>(const PaneInputEndpointIdentity&) const = default;
};

[[nodiscard]] std::optional<PaneInputEndpointIdentity>
pane_input_endpoint_identity(std::string_view endpoint);

class PaneInputLease {
public:
  PaneInputLease(PaneInputLease&&) noexcept;
  PaneInputLease& operator=(PaneInputLease&&) noexcept;
  ~PaneInputLease();

  PaneInputLease(const PaneInputLease&) = delete;
  PaneInputLease& operator=(const PaneInputLease&) = delete;

  [[nodiscard]] bool covers(std::string_view endpoint, std::uint64_t server_pid,
                            std::uint64_t server_start_time,
                            const std::vector<std::string>& pane_ids) const;
  // How another request can tell that the run holding these panes has finished.
  // Asked when that request finds a pane reserved, so a run that outlived its
  // answer releases the pane the moment somebody needs it — not only if a
  // background watcher happened to see it finish before giving up.
  void prove_completion_with(std::function<bool()> proves_complete);
  void release();
  void abandon() noexcept;

private:
  class Impl;
  explicit PaneInputLease(std::unique_ptr<Impl> implementation);

  std::unique_ptr<Impl> implementation_;

  friend libtmux::expected<PaneInputLease, ToolError>
      reserve_pane_input(std::string_view, std::uint64_t, std::uint64_t,
                         std::vector<std::string>, PaneInputReservationKind,
                         std::string_view);
};

[[nodiscard]] libtmux::expected<PaneInputLease, ToolError>
reserve_pane_input(std::string_view endpoint, std::uint64_t server_pid,
                   std::uint64_t server_start_time, std::vector<std::string> pane_ids,
                   PaneInputReservationKind kind, std::string_view tool_name);

// A bounded record of literal text this process has itself written to a
// pane - the exact bytes `wait_for_text` must never credit to the pane's own
// output, however a shell's later redraw has since rearranged them on
// screen, and however many rounds of kernel echo and line-editor redisplay
// put more than one copy of them on screen at once. This is a same-process
// matching hint, not `reserve_pane_input`'s cross-process mutual exclusion:
// it is keyed loosely, by socket path and pane ID, rather than the physical
// endpoint identity a lease needs. Entries are never removed on a later
// Enter - the echo of a now-submitted command is still not output the pane
// produced, only older entries past the bound fall off.
void remember_pane_input(std::string_view socket_path, std::string_view pane_id,
                         std::string text);

// Every literal text this process has recently written to a pane, oldest
// first.
[[nodiscard]] std::vector<std::string>
remembered_pane_input(std::string_view socket_path, std::string_view pane_id);

} // namespace libtmux::mcp::detail
