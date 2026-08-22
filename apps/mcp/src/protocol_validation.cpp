#include "protocol_validation.hpp"

#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <ranges>

#include "schema.hpp"

namespace libtmux::mcp::server {
namespace {

[[nodiscard]] bool alpha(char value) {
  return std::isalpha(static_cast<unsigned char>(value)) != 0;
}

[[nodiscard]] bool alphanumeric(char value) {
  return std::isalnum(static_cast<unsigned char>(value)) != 0;
}

[[nodiscard]] bool valid_label(std::string_view label) {
  if (label.empty() || !alpha(label.front()) || !alphanumeric(label.back())) {
    return false;
  }
  return std::ranges::all_of(
      label, [](char value) { return alphanumeric(value) || value == '-'; });
}

[[nodiscard]] bool valid_meta_name(std::string_view name) {
  if (name.empty()) {
    return true;
  }
  if (!alphanumeric(name.front()) || !alphanumeric(name.back())) {
    return false;
  }
  return std::ranges::all_of(name, [](char value) {
    return alphanumeric(value) || value == '-' || value == '_' || value == '.';
  });
}

[[nodiscard]] bool valid_meta_key(std::string_view key) {
  const auto slash = key.find('/');
  if (slash == std::string_view::npos) {
    return valid_meta_name(key);
  }
  if (key.find('/', slash + 1U) != std::string_view::npos || slash == 0U) {
    return false;
  }
  const std::string_view prefix = key.substr(0U, slash);
  std::size_t offset = 0;
  while (offset < prefix.size()) {
    const auto dot = prefix.find('.', offset);
    const std::string_view label = prefix.substr(
        offset, dot == std::string_view::npos ? std::string_view::npos : dot - offset);
    if (!valid_label(label)) {
      return false;
    }
    if (dot == std::string_view::npos) {
      break;
    }
    offset = dot + 1U;
  }
  return valid_meta_name(key.substr(slash + 1U));
}

[[nodiscard]] bool valid_icon(const json& value) {
  if (!value.is_object() || !only_keys(value, {"src", "mimeType", "sizes", "theme"})) {
    return false;
  }
  const auto source = value.find("src");
  if (source == value.end() || !source->is_string()) {
    return false;
  }
  const auto mime = value.find("mimeType");
  if (mime != value.end() && !mime->is_string()) {
    return false;
  }
  const auto sizes = value.find("sizes");
  if (sizes != value.end() &&
      (!sizes->is_array() || !std::ranges::all_of(*sizes, [](const json& size) {
        return size.is_string();
      }))) {
    return false;
  }
  const auto theme = value.find("theme");
  return theme == value.end() ||
         (theme->is_string() && (*theme == "light" || *theme == "dark"));
}

[[nodiscard]] bool object_values_are_objects(const json& value) {
  return value.is_object() && std::ranges::all_of(value.items(), [](const auto& item) {
           return item.value().is_object();
         });
}

[[nodiscard]] bool valid_client_capabilities(const json& value) {
  if (!value.is_object()) {
    return false;
  }
  const auto experimental = value.find("experimental");
  if (experimental != value.end() && !object_values_are_objects(*experimental)) {
    return false;
  }
  const auto roots = value.find("roots");
  if (roots != value.end() && !roots->is_object()) {
    return false;
  }
  const auto sampling = value.find("sampling");
  if (sampling != value.end() &&
      (!sampling->is_object() || !only_keys(*sampling, {"context", "tools"}))) {
    return false;
  }
  if (sampling != value.end()) {
    for (const std::string_view key : {"context", "tools"}) {
      const auto capability = sampling->find(key);
      if (capability != sampling->end() && !capability->is_object()) {
        return false;
      }
    }
  }
  const auto elicitation = value.find("elicitation");
  if (elicitation != value.end() &&
      (!elicitation->is_object() || !only_keys(*elicitation, {"form", "url"}))) {
    return false;
  }
  if (elicitation != value.end()) {
    for (const std::string_view key : {"form", "url"}) {
      const auto capability = elicitation->find(key);
      if (capability != elicitation->end() && !capability->is_object()) {
        return false;
      }
    }
  }
  const auto extensions = value.find("extensions");
  if (extensions == value.end()) {
    return true;
  }
  if (!object_values_are_objects(*extensions)) {
    return false;
  }
  return std::ranges::all_of(extensions->items(), [](const auto& item) {
    const std::string_view key = item.key();
    return key.find('/') != std::string_view::npos && valid_meta_key(key);
  });
}

} // namespace

bool only_keys(const json& object, std::initializer_list<std::string_view> allowed) {
  for (const auto& [key, value] : object.items()) {
    static_cast<void>(value);
    if (std::ranges::find(allowed, key) == allowed.end()) {
      return false;
    }
  }
  return true;
}

std::optional<std::string>
unexpected_key(const json& object, std::initializer_list<std::string_view> allowed) {
  for (const auto& [key, value] : object.items()) {
    static_cast<void>(value);
    if (std::ranges::find(allowed, key) == allowed.end()) {
      return key;
    }
  }
  return std::nullopt;
}

bool progress_token(const json& value) {
  return value.is_string() || value.is_number();
}

bool metadata_object(const json& params) {
  const auto metadata = params.find("_meta");
  return metadata == params.end() || metadata->is_object();
}

bool valid_legacy_metadata(const json& params) {
  const auto metadata = params.find("_meta");
  if (metadata == params.end()) {
    return true;
  }
  if (!metadata->is_object()) {
    return false;
  }
  const auto token = metadata->find("progressToken");
  return token == metadata->end() || progress_token(*token);
}

bool valid_implementation(const json& value) {
  if (!value.is_object() || !only_keys(value, {"name", "title", "version",
                                               "description", "websiteUrl", "icons"})) {
    return false;
  }
  const auto name = value.find("name");
  const auto version = value.find("version");
  if (name == value.end() || !name->is_string() || name->empty() ||
      version == value.end() || !version->is_string() || version->empty()) {
    return false;
  }
  for (const std::string_view optional : {"title", "description", "websiteUrl"}) {
    const auto found = value.find(optional);
    if (found != value.end() && !found->is_string()) {
      return false;
    }
  }
  const auto icons = value.find("icons");
  return icons == value.end() ||
         (icons->is_array() && std::ranges::all_of(*icons, valid_icon));
}

bool declares_modern_metadata(const json& params) {
  const auto metadata = params.find("_meta");
  if (metadata == params.end() || !metadata->is_object()) {
    return false;
  }
  constexpr std::array reserved{
      std::string_view{"io.modelcontextprotocol/protocolVersion"},
      std::string_view{"io.modelcontextprotocol/clientInfo"},
      std::string_view{"io.modelcontextprotocol/clientCapabilities"},
      std::string_view{"io.modelcontextprotocol/logLevel"}};
  return std::ranges::any_of(
      reserved, [&metadata](std::string_view key) { return metadata->contains(key); });
}

libtmux::expected<std::optional<json>, MetadataError>
validate_modern_metadata(const json& params) {
  const auto metadata = params.find("_meta");
  if (metadata == params.end() || !metadata->is_object()) {
    return libtmux::unexpected(MetadataError{
        .message = "modern MCP requests require an object _meta", .data = {}});
  }
  for (const auto& [key, value] : metadata->items()) {
    static_cast<void>(value);
    if (!valid_meta_key(key)) {
      return libtmux::unexpected(
          MetadataError{.message = "invalid _meta key: " + key, .data = {}});
    }
  }

  const auto version = metadata->find("io.modelcontextprotocol/protocolVersion");
  if (version == metadata->end() || !version->is_string() || version->empty()) {
    return libtmux::unexpected(MetadataError{
        .message = "_meta requires a string MCP protocol version", .data = {}});
  }
  const std::string requested = version->get<std::string>();
  if (requested != kModernProtocolVersion) {
    return libtmux::unexpected(
        MetadataError{.code = kUnsupportedProtocolVersion,
                      .message = "Unsupported protocol version",
                      .data = json{{"supported", modern_protocol_versions()},
                                   {"requested", requested}}});
  }

  const auto capabilities =
      metadata->find("io.modelcontextprotocol/clientCapabilities");
  if (capabilities == metadata->end() || !valid_client_capabilities(*capabilities)) {
    return libtmux::unexpected(MetadataError{
        .message = "_meta requires object client capabilities", .data = {}});
  }
  const auto client = metadata->find("io.modelcontextprotocol/clientInfo");
  if (client != metadata->end() && !valid_implementation(*client)) {
    return libtmux::unexpected(
        MetadataError{.message = "_meta clientInfo is invalid", .data = {}});
  }
  const auto level = metadata->find("io.modelcontextprotocol/logLevel");
  if (level != metadata->end()) {
    constexpr std::array levels{
        std::string_view{"debug"},  std::string_view{"info"},
        std::string_view{"notice"}, std::string_view{"warning"},
        std::string_view{"error"},  std::string_view{"critical"},
        std::string_view{"alert"},  std::string_view{"emergency"}};
    if (!level->is_string() ||
        std::ranges::find(levels, level->get<std::string>()) == levels.end()) {
      return libtmux::unexpected(
          MetadataError{.message = "_meta logLevel is invalid", .data = {}});
    }
  }
  const auto progress = metadata->find("progressToken");
  if (progress != metadata->end() && !progress_token(*progress)) {
    return libtmux::unexpected(MetadataError{
        .message = "progressToken must be a string or number", .data = {}});
  }
  return progress == metadata->end() ? std::optional<json>{}
                                     : std::optional<json>{*progress};
}

libtmux::expected<Arguments, ArgumentError> read_arguments(const json& params,
                                                           const Tool& tool) {
  Arguments arguments;
  const auto found = params.find("arguments");
  if (found == params.end()) {
    return arguments;
  }
  if (!found->is_object()) {
    return libtmux::unexpected(ArgumentError{false, "arguments must be an object"});
  }
  for (const auto& [key, value] : found->items()) {
    const auto parameter = std::ranges::find(tool.parameters, key, &Parameter::name);
    if (parameter == tool.parameters.end()) {
      return libtmux::unexpected(ArgumentError{true, "unknown argument: " + key});
    }
    if (parameter->type == ArgumentType::string) {
      if (!value.is_string()) {
        return libtmux::unexpected(
            ArgumentError{true, "argument " + key + " must be a string"});
      }
      arguments.emplace(key, value.get<std::string>());
      continue;
    }
    if (!value.is_number_integer() || value.is_boolean()) {
      return libtmux::unexpected(
          ArgumentError{true, "argument " + key + " must be an integer"});
    }
    if (value.is_number_unsigned()) {
      const auto item = value.get<std::uint64_t>();
      if (item > static_cast<std::uint64_t>(std::numeric_limits<long long>::max())) {
        return libtmux::unexpected(
            ArgumentError{true, "argument " + key + " is out of range"});
      }
      arguments.emplace(key, std::to_string(item));
    } else {
      arguments.emplace(key, std::to_string(value.get<long long>()));
    }
  }
  return arguments;
}

std::optional<json> legacy_progress_token(const json& params) {
  const auto metadata = params.find("_meta");
  if (metadata == params.end() || !metadata->is_object()) {
    return std::nullopt;
  }
  const auto token = metadata->find("progressToken");
  return token == metadata->end() ? std::optional<json>{} : std::optional<json>{*token};
}

} // namespace libtmux::mcp::server
