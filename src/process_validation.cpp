#include "process_validation.hpp"

#include "path.hpp"

#include <limits>
#include <new>
#include <string_view>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {
namespace {

struct Utf8Measure final {
  bool valid{};
  std::size_t quoted_utf16_size{};
};

[[nodiscard]] bool add(std::size_t& total, std::size_t amount) noexcept {
  if (amount > std::numeric_limits<std::size_t>::max() - total) {
    total = std::numeric_limits<std::size_t>::max();
    return false;
  }
  total += amount;
  return true;
}

[[nodiscard]] bool continuation(unsigned char byte) noexcept {
  return byte >= 0x80U && byte <= 0xbfU;
}

[[nodiscard]] Utf8Measure measure_quoted_utf8(std::string_view value) noexcept {
  std::size_t quoted_size = 1U;
  std::size_t backslashes = 0U;
  for (std::size_t index = 0U; index < value.size();) {
    const auto first = static_cast<unsigned char>(value[index]);
    std::size_t encoded_size = 0U;
    std::size_t utf16_size = 1U;
    if (first <= 0x7fU) {
      encoded_size = 1U;
    } else if (first >= 0xc2U && first <= 0xdfU && index + 1U < value.size() &&
               continuation(static_cast<unsigned char>(value[index + 1U]))) {
      encoded_size = 2U;
    } else if (first >= 0xe0U && first <= 0xefU && index + 2U < value.size()) {
      const auto second = static_cast<unsigned char>(value[index + 1U]);
      const auto third = static_cast<unsigned char>(value[index + 2U]);
      const bool second_valid =
          (first == 0xe0U && second >= 0xa0U && second <= 0xbfU) ||
          (first == 0xedU && second >= 0x80U && second <= 0x9fU) ||
          (((first >= 0xe1U && first <= 0xecU) || (first >= 0xeeU && first <= 0xefU)) &&
           continuation(second));
      if (!second_valid || !continuation(third)) {
        return {};
      }
      encoded_size = 3U;
    } else if (first >= 0xf0U && first <= 0xf4U && index + 3U < value.size()) {
      const auto second = static_cast<unsigned char>(value[index + 1U]);
      const auto third = static_cast<unsigned char>(value[index + 2U]);
      const auto fourth = static_cast<unsigned char>(value[index + 3U]);
      const bool second_valid =
          (first == 0xf0U && second >= 0x90U && second <= 0xbfU) ||
          (first >= 0xf1U && first <= 0xf3U && continuation(second)) ||
          (first == 0xf4U && second >= 0x80U && second <= 0x8fU);
      if (!second_valid || !continuation(third) || !continuation(fourth)) {
        return {};
      }
      encoded_size = 4U;
      utf16_size = 2U;
    } else {
      return {};
    }

    if (encoded_size == 1U && first == static_cast<unsigned char>('\\')) {
      ++backslashes;
    } else if (encoded_size == 1U && first == static_cast<unsigned char>('"')) {
      if (backslashes > (std::numeric_limits<std::size_t>::max() - 2U) / 2U ||
          !add(quoted_size, backslashes * 2U + 2U)) {
        return Utf8Measure{.valid = true,
                           .quoted_utf16_size =
                               std::numeric_limits<std::size_t>::max()};
      }
      backslashes = 0U;
    } else {
      if (!add(quoted_size, backslashes) || !add(quoted_size, utf16_size)) {
        return Utf8Measure{.valid = true,
                           .quoted_utf16_size =
                               std::numeric_limits<std::size_t>::max()};
      }
      backslashes = 0U;
    }
    index += encoded_size;
  }

  if (backslashes > (std::numeric_limits<std::size_t>::max() - 1U) / 2U ||
      !add(quoted_size, backslashes * 2U + 1U)) {
    quoted_size = std::numeric_limits<std::size_t>::max();
  }
  return Utf8Measure{.valid = true, .quoted_utf16_size = quoted_size};
}

[[nodiscard]] bool structurally_valid(const ProcessRequest& request,
                                      std::string_view executable) noexcept {
  const auto contains_nul = [](std::string_view value) {
    return value.find('\0') != std::string_view::npos;
  };
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

} // namespace

ProcessValidationResult validate_process_request(const ProcessRequest& request,
                                                 ProcessValidationMode mode) {
  std::string executable;
  try {
    executable = libtmux_path::command_string(request.executable);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    return {.failure = ProcessValidationFailure::malformed_utf8};
  }
  if (!structurally_valid(request, executable)) {
    return {.failure = ProcessValidationFailure::structure};
  }
  if (mode == ProcessValidationMode::posix) {
    return {};
  }

  const auto executable_measure = measure_quoted_utf8(executable);
  if (!executable_measure.valid) {
    return {.failure = ProcessValidationFailure::malformed_utf8};
  }
  std::size_t command_line_size = executable_measure.quoted_utf16_size;
  for (const auto& argument : request.arguments) {
    const auto measured = measure_quoted_utf8(argument.value);
    if (!measured.valid) {
      return {.failure = ProcessValidationFailure::malformed_utf8};
    }
    static_cast<void>(add(command_line_size, 1U));
    static_cast<void>(add(command_line_size, measured.quoted_utf16_size));
  }
  for (const auto& [name, value] : request.environment) {
    if (!measure_quoted_utf8(name).valid ||
        (value.has_value() && !measure_quoted_utf8(*value).valid)) {
      return {.failure = ProcessValidationFailure::malformed_utf8};
    }
  }

  return {
      .failure = command_line_size >= 32767U
                     ? ProcessValidationFailure::command_line_too_long
                     : ProcessValidationFailure::none,
      .windows_command_line_size = command_line_size,
  };
}

} // namespace detail
LIBTMUX_NAMESPACE_END
