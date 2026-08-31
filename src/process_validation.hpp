#pragma once

#include "process.hpp"

#include <cstddef>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

enum class ProcessValidationMode { posix, windows };

enum class ProcessValidationFailure {
  none,
  structure,
  malformed_utf8,
  command_line_too_long,
};

struct ProcessValidationResult final {
  ProcessValidationFailure failure{ProcessValidationFailure::none};
  std::size_t windows_command_line_size{};
};

[[nodiscard]] ProcessValidationResult
validate_process_request(const ProcessRequest& request, ProcessValidationMode mode);

} // namespace detail
LIBTMUX_NAMESPACE_END
