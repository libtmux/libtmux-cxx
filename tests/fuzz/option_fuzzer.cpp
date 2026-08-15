// Option output carries user-chosen values through four quoting forms, and a
// hook body is an arbitrary tmux command line.
//
// The invariant is determinism and boundedness rather than a claim about
// names: tmux accepts a user option whose name ends in the same asterisk it
// uses to mark inheritance, so a name that keeps one is legal, not a bug. This
// harness found that.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "libtmux/options.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text{reinterpret_cast<const char*>(data), size};

  const auto entries = libtmux::parse_options(text);
  const auto again = libtmux::parse_options(text);
  // The listing is the only input, and nothing about reading it is stateful.
  if (entries.size() != again.size()) {
    __builtin_trap();
  }
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (entries[index].name != again[index].name ||
        entries[index].value != again[index].value ||
        entries[index].inherited != again[index].inherited) {
      __builtin_trap();
    }
    // Nothing is invented: a name and a value both come out of the line.
    if (entries[index].name.size() > text.size() ||
        entries[index].value.size() > text.size()) {
      __builtin_trap();
    }
  }

  (void)libtmux::unquote(text);
  return 0;
}
