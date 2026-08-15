// The control-mode protocol decoder, fed one arbitrary byte stream in
// arbitrary chunks — which is what a socket delivers.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "libtmux/control.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  libtmux::Parser parser;
  // The first byte chooses the chunk size, so the decoder is exercised across
  // every boundary rather than only on whole buffers.
  const std::size_t chunk =
      size == 0 ? 1U : (static_cast<std::size_t>(data[0]) % 17U) + 1U;

  const auto* bytes = reinterpret_cast<const std::byte*>(data);
  for (std::size_t offset = 0; offset < size;) {
    const std::size_t take = std::min(chunk, size - offset);
    const auto events = parser.feed(std::span{bytes + offset, take});
    if (!events.has_value()) {
      return 0;
    }
    offset += take;
  }
  (void)parser.finish();
  return 0;
}
