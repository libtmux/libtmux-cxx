#pragma once

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

#include "libtmux/expected.hpp"
#include "libtmux_consumers/mcp.hpp"
#include "protocol_types.hpp"

namespace libtmux::mcp::server {

struct MetadataError {
  int code{kInvalidParams};
  std::string message;
  std::optional<json> data;
};

struct ArgumentError {
  bool tool_input{};
  std::string message;
};

[[nodiscard]] bool only_keys(const json& object,
                             std::initializer_list<std::string_view> allowed);
[[nodiscard]] std::optional<std::string>
unexpected_key(const json& object, std::initializer_list<std::string_view> allowed);
[[nodiscard]] bool progress_token(const json& value);
[[nodiscard]] bool metadata_object(const json& params);
[[nodiscard]] bool valid_legacy_metadata(const json& params);
[[nodiscard]] bool valid_implementation(const json& value);
[[nodiscard]] bool declares_modern_metadata(const json& params);
[[nodiscard]] libtmux::expected<std::optional<json>, MetadataError>
validate_modern_metadata(const json& params);
[[nodiscard]] libtmux::expected<Arguments, ArgumentError>
read_arguments(const json& params, const Tool& tool);
[[nodiscard]] std::optional<json> legacy_progress_token(const json& params);

} // namespace libtmux::mcp::server
