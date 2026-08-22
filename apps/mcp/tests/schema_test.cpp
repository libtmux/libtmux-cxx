#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "libtmux_consumers/mcp.hpp"
#include "schema.hpp"

namespace {

using json = nlohmann::json;
using libtmux::mcp::StructuredValue;

TEST(McpProtocolSchema, PreservesStructuredScalarTypes) {
  const libtmux::mcp::ToolOutput answer{
      .structured = {
          {"array", StructuredValue::Array{StructuredValue{}, true, 7, "mcp"}},
          {"boolean", true},
          {"integer", 7},
          {"null", StructuredValue{}},
          {"object", StructuredValue::Object{{"nested", "value"}}},
          {"string", "mcp"}}};
  const json result = libtmux::mcp::server::tool_success(
      answer, libtmux::mcp::server::ProtocolEra::legacy);
  const json& structured = result["structuredContent"];

  ASSERT_TRUE(structured["array"].is_array());
  EXPECT_TRUE(structured["array"][0].is_null());
  EXPECT_TRUE(structured["array"][1].is_boolean());
  EXPECT_TRUE(structured["array"][2].is_number_integer());
  EXPECT_TRUE(structured["array"][3].is_string());
  EXPECT_TRUE(structured["boolean"].is_boolean());
  EXPECT_TRUE(structured["integer"].is_number_integer());
  EXPECT_TRUE(structured["null"].is_null());
  EXPECT_TRUE(structured["object"].is_object());
  EXPECT_TRUE(structured["string"].is_string());
  EXPECT_EQ(structured["array"], json::array({nullptr, true, 7, "mcp"}));
  EXPECT_EQ(structured["object"], json({{"nested", "value"}}));
  EXPECT_EQ(result["content"][0]["text"], structured.dump());
}

} // namespace
