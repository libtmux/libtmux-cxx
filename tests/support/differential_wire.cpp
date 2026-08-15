#include "support/differential_wire.hpp"
#include "libtmux/expected.hpp"

#include <array>
#include <limits>

namespace libtmux::test::differential {
namespace {

libtmux::expected<std::size_t, std::string> field_count(WireTag tag) {
  switch (tag) {
  case WireTag::ListSessionsRequest:
  case WireTag::EndObservation:
    return 0U;
  case WireTag::SessionObservation:
    return 2U;
  case WireTag::ErrorObservation:
    return 3U;
  }
  return libtmux::unexpected("unknown wire tag");
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
  output.push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
  output.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
  output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
  output.push_back(static_cast<std::byte>(value & 0xffU));
}

std::uint32_t read_u32(std::span<const std::byte> input, std::size_t offset) {
  auto value = std::uint32_t{0};
  for (auto index = std::size_t{0}; index < 4U; ++index) {
    value = static_cast<std::uint32_t>(
        (value << 8U) | std::to_integer<std::uint8_t>(input[offset + index]));
  }
  return value;
}

} // namespace

libtmux::expected<std::vector<std::byte>, std::string>
encode_wire_frame(const WireFrame& frame) {
  const auto expected_fields = field_count(frame.tag);
  if (!expected_fields.has_value()) {
    return libtmux::unexpected(expected_fields.error());
  }
  if (frame.fields.size() != *expected_fields) {
    return libtmux::unexpected("wire tag has wrong field count");
  }

  auto payload_size = std::size_t{1};
  for (const auto& field : frame.fields) {
    if (field.size() > std::numeric_limits<std::uint32_t>::max() ||
        payload_size > std::numeric_limits<std::size_t>::max() - 4U - field.size()) {
      return libtmux::unexpected("wire payload length overflow");
    }
    payload_size += 4U + field.size();
  }
  if (payload_size > max_wire_frame_size ||
      payload_size > std::numeric_limits<std::uint32_t>::max()) {
    return libtmux::unexpected("wire payload exceeds maximum frame size");
  }

  std::vector<std::byte> encoded;
  encoded.reserve(4U + payload_size);
  append_u32(encoded, static_cast<std::uint32_t>(payload_size));
  encoded.push_back(static_cast<std::byte>(frame.tag));
  for (const auto& field : frame.fields) {
    append_u32(encoded, static_cast<std::uint32_t>(field.size()));
    encoded.insert(encoded.end(), field.begin(), field.end());
  }
  return encoded;
}

libtmux::expected<WireFrame, std::string>
decode_wire_frame(std::span<const std::byte> encoded) {
  if (encoded.size() < 5U) {
    return libtmux::unexpected("truncated wire frame");
  }
  const auto payload_size = static_cast<std::size_t>(read_u32(encoded, 0U));
  if (payload_size > max_wire_frame_size) {
    return libtmux::unexpected("wire payload exceeds maximum frame size");
  }
  if (payload_size > std::numeric_limits<std::size_t>::max() - 4U) {
    return libtmux::unexpected("wire payload length overflow");
  }
  const auto total_size = 4U + payload_size;
  if (encoded.size() < total_size) {
    return libtmux::unexpected("truncated wire frame");
  }
  if (encoded.size() > total_size) {
    return libtmux::unexpected("trailing wire data");
  }

  const auto tag = static_cast<WireTag>(std::to_integer<std::uint8_t>(encoded[4U]));
  const auto expected_fields = field_count(tag);
  if (!expected_fields.has_value()) {
    return libtmux::unexpected(expected_fields.error());
  }
  WireFrame frame{.tag = tag, .fields = {}};
  frame.fields.reserve(*expected_fields);
  auto cursor = std::size_t{5};
  for (auto index = std::size_t{0}; index < *expected_fields; ++index) {
    if (cursor > total_size || total_size - cursor < 4U) {
      return libtmux::unexpected("truncated wire field length");
    }
    const auto size = static_cast<std::size_t>(read_u32(encoded, cursor));
    cursor += 4U;
    if (size > total_size - cursor) {
      return libtmux::unexpected("truncated wire field");
    }
    frame.fields.emplace_back(encoded.begin() + static_cast<std::ptrdiff_t>(cursor),
                              encoded.begin() +
                                  static_cast<std::ptrdiff_t>(cursor + size));
    cursor += size;
  }
  if (cursor != total_size) {
    return libtmux::unexpected("trailing wire payload data");
  }
  return frame;
}

} // namespace libtmux::test::differential
