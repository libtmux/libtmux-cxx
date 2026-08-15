// `tmux -V` output, which a distribution is free to have patched.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "libtmux/version.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text{reinterpret_cast<const char*>(data), size};
  const auto version = libtmux::parse_version(text);
  if (version.has_value()) {
    // Ordering must be usable on anything that parsed: a version compares
    // equal to itself, and comparison must not depend on being asked twice.
    const auto again = libtmux::parse_version(text);
    if (!again.has_value() || !(*version == *again) || (*version < *version)) {
      __builtin_trap();
    }
    (void)libtmux::is_supported(*version);
  }
  return 0;
}
