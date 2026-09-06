#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
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
  void release();
  void abandon() noexcept;

private:
  class Impl;
  explicit PaneInputLease(std::unique_ptr<Impl> implementation);

  std::unique_ptr<Impl> implementation_;

  friend libtmux::expected<PaneInputLease, ToolError>
      reserve_pane_input(std::string, std::uint64_t, std::uint64_t,
                         std::vector<std::string>, PaneInputReservationKind,
                         std::string_view);
};

[[nodiscard]] libtmux::expected<PaneInputLease, ToolError>
reserve_pane_input(std::string endpoint, std::uint64_t server_pid,
                   std::uint64_t server_start_time, std::vector<std::string> pane_ids,
                   PaneInputReservationKind kind, std::string_view tool_name);

} // namespace libtmux::mcp::detail
