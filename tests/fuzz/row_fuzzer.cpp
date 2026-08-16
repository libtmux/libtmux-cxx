// The row splitter reads whatever tmux printed, and tmux prints whatever the
// programs inside the panes put in a title, a path or a command line.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "libtmux/snapshot.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view line{reinterpret_cast<const char*>(data), size};
  std::vector<std::string_view> values;
  // Every field count the entities actually ask for, plus the degenerate ones.
  for (const std::size_t fields : {std::size_t{0}, std::size_t{1}, std::size_t{8},
                                   std::size_t{12}, std::size_t{18}}) {
    if (libtmux::split_row(line, fields, values)) {
      // A row that split must have said so truthfully.
      if (values.size() != fields) {
        __builtin_trap();
      }
    }
  }

  // Decoding walks a mutable buffer with two cursors and a multi-byte marker
  // that can be cut short by the end of the input. It runs on every value of
  // every row, so a read past the end here is a read past the end everywhere.
  std::string buffer{line};
  const std::size_t decoded =
      buffer.empty() ? 0U : libtmux::decode_value(buffer.data(), buffer.size());
  // Decoding only ever shortens: an escape is longer than what it stands for.
  if (decoded > buffer.size()) {
    __builtin_trap();
  }
  return 0;
}
