#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace libtmux::mcp::server {

using json = nlohmann::json;

inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
inline constexpr int kNotInitialized = -32002;
inline constexpr int kServerBusy = -32003;
inline constexpr int kUnsupportedProtocolVersion = -32022;
inline constexpr std::string_view kModernProtocolVersion = "2026-07-28";
inline constexpr std::string_view kLatestLegacyProtocolVersion = "2025-11-25";
inline constexpr std::string_view kBatchLegacyProtocolVersion = "2025-03-26";
inline constexpr std::array<std::string_view, 4> kLegacyProtocolVersions{
    kLatestLegacyProtocolVersion, "2025-06-18", kBatchLegacyProtocolVersion,
    "2024-11-05"};
inline constexpr std::size_t kMaximumLineBytes = 8U * 1024U * 1024U;

enum class ProtocolEra : std::uint8_t { undecided, legacy, modern };

[[nodiscard]] json failure(const json& id, int code, std::string message,
                           std::optional<json> data = {});
[[nodiscard]] json success(const json& id, json result);
[[nodiscard]] std::string request_key(const json& id);
[[nodiscard]] bool request_id(const json& value);
[[nodiscard]] bool is_legacy_protocol_version(std::string_view version);

} // namespace libtmux::mcp::server
