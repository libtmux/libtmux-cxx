#pragma once

#include "libtmux/abi.hpp"

#include "libtmux/delivery.hpp"
#include "libtmux/expected.hpp"
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

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
  enum class Kind { validation, spawn, pre_exec, pipe, timeout } kind;
  DeliveryStatus delivery;
  std::error_code cause;
  std::string diagnostic;
  std::vector<std::byte> stdout_bytes;
  std::vector<std::byte> stderr_bytes;
  bool output_truncated;
};

[[nodiscard]] expected<ProcessReply, ProcessError>
run_process(const ProcessRequest& request);

} // namespace detail
LIBTMUX_NAMESPACE_END
