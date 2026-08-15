#include "support/differential_wire.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace {

using libtmux::test::differential::decode_wire_frame;
using libtmux::test::differential::encode_wire_frame;
using libtmux::test::differential::max_wire_frame_size;
using libtmux::test::differential::WireFrame;
using libtmux::test::differential::WireTag;

TEST(DifferentialWire, PreservesTagDefinedByteFields) {
  const WireFrame frame{
      .tag = WireTag::SessionObservation,
      .fields = {{std::byte{'$'}, std::byte{0}}, {std::byte{0xff}}},
  };
  const std::vector<std::byte> expected{
      std::byte{0},    std::byte{0},   std::byte{0}, std::byte{12},
      std::byte{0x02}, std::byte{0},   std::byte{0}, std::byte{0},
      std::byte{2},    std::byte{'$'}, std::byte{0}, std::byte{0},
      std::byte{0},    std::byte{0},   std::byte{1}, std::byte{0xff},
  };

  const auto encoded = encode_wire_frame(frame);
  ASSERT_TRUE(encoded.has_value()) << encoded.error();
  EXPECT_EQ(*encoded, expected);
  const auto decoded = decode_wire_frame(*encoded);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  EXPECT_EQ(*decoded, frame);
}

TEST(DifferentialWire, RoundTripsEveryKnownTagShape) {
  const std::vector<WireFrame> frames{
      {.tag = WireTag::ListSessionsRequest, .fields = {}},
      {.tag = WireTag::SessionObservation,
       .fields = {{std::byte{'$'}}, {std::byte{'n'}}}},
      {.tag = WireTag::ErrorObservation,
       .fields = {{std::byte{'c'}}, {std::byte{'m'}}, {std::byte{'e'}}}},
      {.tag = WireTag::EndObservation, .fields = {}},
  };

  for (const auto& frame : frames) {
    const auto encoded = encode_wire_frame(frame);
    ASSERT_TRUE(encoded.has_value()) << encoded.error();
    const auto decoded = decode_wire_frame(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error();
    EXPECT_EQ(*decoded, frame);
  }
}

TEST(DifferentialWire, RejectsUnknownTagAndWrongFieldCount) {
  const WireFrame unknown{.tag = static_cast<WireTag>(0x7f), .fields = {}};
  const auto unknown_result = encode_wire_frame(unknown);
  ASSERT_FALSE(unknown_result.has_value());
  EXPECT_NE(unknown_result.error().find("unknown"), std::string::npos);

  const WireFrame wrong_count{.tag = WireTag::ListSessionsRequest,
                              .fields = {{std::byte{1}}}};
  const auto count_result = encode_wire_frame(wrong_count);
  ASSERT_FALSE(count_result.has_value());
  EXPECT_NE(count_result.error().find("field count"), std::string::npos);
}

TEST(DifferentialWire, RejectsTruncationAndTrailingData) {
  const std::vector<std::byte> truncated_length{std::byte{0}, std::byte{0}};
  EXPECT_FALSE(decode_wire_frame(truncated_length).has_value());

  const std::vector<std::byte> truncated_payload{
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{2}, std::byte{0x01}};
  EXPECT_FALSE(decode_wire_frame(truncated_payload).has_value());

  const std::vector<std::byte> truncated_field{
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{6}, std::byte{0x02},
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{2}, std::byte{'x'}};
  EXPECT_FALSE(decode_wire_frame(truncated_field).has_value());

  const std::vector<std::byte> outer_trailing{std::byte{0},    std::byte{0},
                                              std::byte{0},    std::byte{1},
                                              std::byte{0x01}, std::byte{0}};
  const auto outer_result = decode_wire_frame(outer_trailing);
  ASSERT_FALSE(outer_result.has_value());
  EXPECT_NE(outer_result.error().find("trailing"), std::string::npos);

  const std::vector<std::byte> payload_trailing{std::byte{0},    std::byte{0},
                                                std::byte{0},    std::byte{2},
                                                std::byte{0x01}, std::byte{0}};
  const auto payload_result = decode_wire_frame(payload_trailing);
  ASSERT_FALSE(payload_result.has_value());
  EXPECT_NE(payload_result.error().find("trailing"), std::string::npos);
}

TEST(DifferentialWire, RejectsOversizeAndUint32Lengths) {
  std::vector<std::byte> large(max_wire_frame_size, std::byte{'x'});
  const WireFrame oversize{.tag = WireTag::SessionObservation,
                           .fields = {std::move(large), {}}};
  const auto encode_result = encode_wire_frame(oversize);
  ASSERT_FALSE(encode_result.has_value());
  EXPECT_NE(encode_result.error().find("maximum"), std::string::npos);

  const std::vector<std::byte> declared_uint32{std::byte{0xff}, std::byte{0xff},
                                               std::byte{0xff}, std::byte{0xff},
                                               std::byte{0x01}};
  const auto decode_result = decode_wire_frame(declared_uint32);
  ASSERT_FALSE(decode_result.has_value());
  EXPECT_NE(decode_result.error().find("maximum"), std::string::npos);
}

} // namespace
