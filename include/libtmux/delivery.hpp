#pragma once

// How far a tmux command is known to have progressed before a failure.
//
// Only `not_started` is safe to retry without inspecting the operation.
// `written` means the complete request reached the transport but no terminal
// reply did. `replied` means tmux produced a terminal reply. `indeterminate`
// means the transport cannot prove whether tmux saw or completed the request.

#include <cstdint>
#include <string_view>

#include "libtmux/abi.hpp"

LIBTMUX_NAMESPACE_BEGIN

enum class DeliveryStatus : std::uint8_t {
  not_started,
  written,
  replied,
  indeterminate,
};

[[nodiscard]] constexpr std::string_view to_string(DeliveryStatus status) noexcept {
  switch (status) {
  case DeliveryStatus::not_started:
    return "not_started";
  case DeliveryStatus::written:
    return "written";
  case DeliveryStatus::replied:
    return "replied";
  case DeliveryStatus::indeterminate:
    return "indeterminate";
  }
  return "unknown";
}

LIBTMUX_NAMESPACE_END
