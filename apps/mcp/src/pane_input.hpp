#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "libtmux/expected.hpp"
#include "libtmux_consumers/mcp.hpp"

namespace libtmux::mcp::detail {

enum class PaneInputReservationKind { input, run };

class PaneInputLease {
public:
  PaneInputLease(PaneInputLease&&) noexcept;
  PaneInputLease& operator=(PaneInputLease&&) noexcept;
  ~PaneInputLease();

  PaneInputLease(const PaneInputLease&) = delete;
  PaneInputLease& operator=(const PaneInputLease&) = delete;

  [[nodiscard]] bool covers(std::string_view endpoint, std::uint64_t server_pid,
                            const std::vector<std::string>& pane_ids) const;
  void release();
  void abandon() noexcept;

private:
  class Impl;
  explicit PaneInputLease(std::unique_ptr<Impl> implementation);

  std::unique_ptr<Impl> implementation_;

  friend libtmux::expected<PaneInputLease, ToolError>
      reserve_pane_input(std::string, std::uint64_t, std::vector<std::string>,
                         PaneInputReservationKind, std::string_view);
};

[[nodiscard]] libtmux::expected<PaneInputLease, ToolError>
reserve_pane_input(std::string endpoint, std::uint64_t server_pid,
                   std::vector<std::string> pane_ids, PaneInputReservationKind kind,
                   std::string_view tool_name);

} // namespace libtmux::mcp::detail
