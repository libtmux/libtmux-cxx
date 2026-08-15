#pragma once

#include "libtmux/expected.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace libtmux::test::differential {

inline constexpr std::size_t max_wire_frame_size = 1024U * 1024U;

enum class WireTag : std::uint8_t {
  ListSessionsRequest = 0x01,
  SessionObservation = 0x02,
  ErrorObservation = 0x03,
  EndObservation = 0x04,
};

struct WireFrame {
  WireTag tag;
  std::vector<std::vector<std::byte>> fields;

  friend bool operator==(const WireFrame&, const WireFrame&) = default;
};

libtmux::expected<std::vector<std::byte>, std::string>
encode_wire_frame(const WireFrame& frame);

libtmux::expected<WireFrame, std::string>
decode_wire_frame(std::span<const std::byte> encoded);

} // namespace libtmux::test::differential
