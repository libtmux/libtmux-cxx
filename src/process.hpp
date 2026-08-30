#pragma once

#include "libtmux/abi.hpp"

#include "libtmux/delivery.hpp"
#include "libtmux/expected.hpp"
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "path.hpp"
#include "transport_values.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

enum class StdioPolicy { capture, inherit_terminal };

struct ProcessRequest {
  std::filesystem::path executable;
  std::vector<Argument> arguments;
  std::vector<std::pair<std::string, std::optional<std::string>>> environment;
  std::optional<std::chrono::milliseconds> timeout;
  std::size_t capture_limit{default_capture_limit};
  StdioPolicy stdio{StdioPolicy::capture};
};

struct ProcessReply {
  Termination termination;
  std::vector<std::byte> stdout_bytes;
  std::vector<std::byte> stderr_bytes;
  bool output_truncated;
};

struct ProcessError {
  enum class Kind { validation, spawn, pre_exec, pipe, timeout, cancelled } kind;
  DeliveryStatus delivery;
  std::string diagnostic;
  std::vector<std::byte> stdout_bytes;
  std::vector<std::byte> stderr_bytes;
  bool output_truncated;
};

[[nodiscard]] inline bool process_request_is_valid(const ProcessRequest& request) {
  const auto contains_nul = [](std::string_view value) {
    return value.find('\0') != std::string_view::npos;
  };
  const auto executable = libtmux_path::command_string(request.executable);
  if (executable.empty() || contains_nul(executable)) {
    return false;
  }
  for (const auto& argument : request.arguments) {
    if (contains_nul(argument.value)) {
      return false;
    }
  }
  for (const auto& [name, value] : request.environment) {
    if (name.empty() || name.find('=') != std::string::npos || contains_nul(name) ||
        (value.has_value() && contains_nul(*value))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] expected<ProcessReply, ProcessError>
run_process(const ProcessRequest& request);

#if defined(_WIN32)
using CancellationProbe = std::function<bool()>;

[[nodiscard]] expected<ProcessReply, ProcessError>
run_process(const ProcessRequest& request, const CancellationProbe& cancelled);
#endif

} // namespace detail
LIBTMUX_NAMESPACE_END
