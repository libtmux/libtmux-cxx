#pragma once

// Capture a selected endpoint and retain a hard-link route to its daemon.
// Windows psmux uses its native logical name instead of a Unix socket path.

#include "libtmux/abi.hpp"
#include "libtmux/expected.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

// Empty when the selector is not understood or its Unix path cannot resolve.
[[nodiscard]] std::optional<std::string>
resolved_socket_path(const std::vector<std::string>& selector,
                     const std::vector<std::string>& environment = {});

[[nodiscard]] expected<void, std::string>
prepare_socket_directory(const std::string& directory, bool create = true);

// Owns a private hard link to one socket inode.
class SocketAlias final {
public:
  SocketAlias(std::string path, std::string directory) noexcept;
  SocketAlias(const SocketAlias&) = delete;
  SocketAlias& operator=(const SocketAlias&) = delete;
  ~SocketAlias() noexcept;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
  std::string path_;
  std::string directory_;
  std::uint64_t creator_pid_{};
};

struct SocketEndpoint {
  std::vector<std::string> connection;
  std::string socket_path;
  std::string identity;
  std::shared_ptr<const SocketAlias> alias;
  bool missing{};
};

// A live POSIX endpoint uses an owned hard-link route. A missing endpoint
// remains permanently unbound.
[[nodiscard]] expected<SocketEndpoint, std::string>
bind_socket_endpoint(const std::vector<std::string>& selector);

} // namespace detail
LIBTMUX_NAMESPACE_END
