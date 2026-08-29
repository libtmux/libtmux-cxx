#pragma once

#include "libtmux/abi.hpp"
#include "libtmux/command.hpp"

#include <cstddef>
#include <string>
#include <variant>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

using Sensitivity = ArgumentSensitivity;

inline constexpr std::size_t default_capture_limit = 1024U * 1024U;

struct Argument {
  std::string value;
  Sensitivity sensitivity{Sensitivity::public_value};
};

struct Exited {
  int code;
};

struct Signaled {
  int signal;
};

using Termination = std::variant<Exited, Signaled>;

} // namespace detail
LIBTMUX_NAMESPACE_END
