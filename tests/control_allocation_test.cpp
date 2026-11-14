#include "libtmux/notification.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>
#include <string_view>
#include <vector>

namespace {
thread_local bool reject_allocation = false;

void* allocate(std::size_t size) {
  if (reject_allocation) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0U ? 1U : size)) {
    return memory;
  }
  throw std::bad_alloc{};
}
} // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
  for (const std::string_view line :
       {"%output %7 two words\nnext", "%extended-output %7 42 future : a b\nc",
        "%subscription-changed watch $1 @2 0 %7 : a b", "%window-pane-changed @2 %7",
        "%output", "%extended-output %7", "%subscription-changed watch", ""}) {
    const auto* first = reinterpret_cast<const std::byte*>(line.data());
    const libtmux::Notification notification{
        std::vector<std::byte>{first, first + line.size()}};
    reject_allocation = true;
    try {
      const auto parsed = libtmux::parse(notification);
      reject_allocation = false;
      if (parsed.name.empty() != line.empty()) {
        return 2;
      }
    } catch (const std::bad_alloc&) {
      reject_allocation = false;
      return 1;
    }
  }
}
