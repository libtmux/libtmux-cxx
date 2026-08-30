#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <semaphore>
#include <string>
#include <vector>

#include "backend.hpp"
#include "libtmux/server.hpp"

namespace {

class BlockingBackend final : public libtmux::detail::Backend {
public:
  BlockingBackend() : Backend{{}, {}} {}

  libtmux::expected<std::string, libtmux::CommandFailure>
  run(const libtmux::CommandRequest&, std::optional<std::chrono::milliseconds>,
      std::optional<std::size_t>) const override {
    started.release();
    never_returns.acquire();
    return std::string{};
  }

  const std::vector<std::string>& connection() const noexcept override {
    return connection_;
  }

  libtmux::expected<libtmux::Version, libtmux::CommandFailure>
  version() const override {
    return libtmux::Version{.major = 3, .minor = 7};
  }

  mutable std::binary_semaphore started{0};

private:
  mutable std::binary_semaphore never_returns{0};
  std::vector<std::string> connection_;
};

} // namespace

int main() {
  using namespace std::chrono_literals;
  auto backend = std::make_shared<BlockingBackend>();
  const auto server = libtmux::detail::server_over(backend);
  {
    auto submitted = server.submit({"display-message", "-p", "detached"});
    if (!submitted.has_value() || !backend->started.try_acquire_for(1s)) {
      return 1;
    }
  }
  return 0;
}
