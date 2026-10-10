#include "socket_identity.hpp"
#include "environment.hpp"

#include "libtmux/socket.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

SocketAlias::SocketAlias(std::string path, std::string directory) noexcept
    : path_{std::move(path)}, directory_{std::move(directory)} {
#if !defined(_WIN32)
  creator_pid_ = static_cast<std::uint64_t>(::getpid());
#endif
}

SocketAlias::~SocketAlias() noexcept {
#if !defined(_WIN32)
  if (creator_pid_ == static_cast<std::uint64_t>(::getpid())) {
    static_cast<void>(::unlink(path_.c_str()));
    static_cast<void>(::rmdir(directory_.c_str()));
  }
#endif
}

#if defined(_WIN32)

std::optional<std::string>
resolved_socket_path(const std::vector<std::string>& selector,
                     const std::vector<std::string>& environment) {
  static_cast<void>(environment);
  if (selector.empty()) {
    return std::string{"psmux:default"};
  }
  if (selector.size() == 2U && selector.front() == "-L") {
    return std::string{"psmux:-L:"} + selector.back();
  }
  return std::nullopt;
}

expected<SocketEndpoint, std::string>
bind_socket_endpoint(const std::vector<std::string>& selector) {
  const std::string identity = resolved_socket_path(selector).value_or(std::string{});
  return SocketEndpoint{.connection = selector,
                        .socket_path = identity,
                        .identity = identity,
                        .alias = {},
                        .missing = false};
}

#else

namespace {

std::optional<std::filesystem::path>
socket_directory(const std::vector<std::string>& environment) {
  const auto configured =
      libtmux_env::value(environment, "TMUX_TMPDIR").value_or("/tmp");
  if (!std::filesystem::path{configured}.is_absolute() ||
      configured.find('\0') != std::string::npos) {
    return std::nullopt;
  }
  std::error_code failed;
  auto resolved = std::filesystem::canonical(configured, failed);
  if (failed || !std::filesystem::is_directory(resolved, failed)) {
    return std::nullopt;
  }
  return resolved;
}

enum class PinStatus { pinned, missing, retry_elsewhere, failed };

class DirectoryGuard final {
public:
  explicit DirectoryGuard(const std::string& path) noexcept : path_{path} {}
  DirectoryGuard(const DirectoryGuard&) = delete;
  DirectoryGuard& operator=(const DirectoryGuard&) = delete;
  ~DirectoryGuard() noexcept {
    if (armed_) {
      static_cast<void>(::rmdir(path_.c_str()));
    }
  }

  void release() noexcept { armed_ = false; }

private:
  const std::string& path_;
  bool armed_{true};
};

struct PinAttempt {
  explicit PinAttempt(PinStatus state, std::string failure = {})
      : status{state}, diagnostic{std::move(failure)} {}
  PinAttempt(const struct stat& captured,
             std::shared_ptr<const SocketAlias> alias_lifetime)
      : status{PinStatus::pinned}, metadata{captured},
        lifetime{std::move(alias_lifetime)} {}
  PinAttempt(PinStatus state, std::shared_ptr<const SocketAlias> alias_lifetime)
      : status{state}, lifetime{std::move(alias_lifetime)} {}

  PinStatus status;
  struct stat metadata {};
  std::string diagnostic;
  std::shared_ptr<const SocketAlias> lifetime;
};

PinAttempt pin_under(const std::filesystem::path& root,
                     const std::filesystem::path& socket) {
  std::string directory_text = (root / ".libtmux-XXXXXX").string();
  if (directory_text.size() + 2U > kSocketPathLimit) {
    return PinAttempt{PinStatus::retry_elsewhere,
                      "the pinned tmux socket path does not fit a unix domain "
                      "address"};
  }
  if (::mkdtemp(directory_text.data()) == nullptr) {
    return PinAttempt{PinStatus::retry_elsewhere,
                      "could not create a private tmux socket alias directory: " +
                          std::string{std::strerror(errno)}};
  }
  DirectoryGuard directory{directory_text};
  std::string alias_text = directory_text + "/s";
  auto lifetime = std::make_shared<SocketAlias>(alias_text, directory_text);
  directory.release();
  const std::filesystem::path alias{lifetime->path()};

  if (::linkat(AT_FDCWD, socket.c_str(), AT_FDCWD, alias.c_str(), AT_SYMLINK_FOLLOW) !=
      0) {
    const int failed = errno;
    if (failed == ENOENT || failed == ENOTDIR) {
      return PinAttempt{PinStatus::missing, std::move(lifetime)};
    }
    if (failed == EXDEV) {
      return PinAttempt{PinStatus::retry_elsewhere,
                        "the private tmux socket alias is on another filesystem"};
    }
    return PinAttempt{PinStatus::failed, "could not pin the tmux socket: " +
                                             std::string{std::strerror(failed)}};
  }

  struct stat metadata {};
  if (::lstat(alias.c_str(), &metadata) != 0) {
    const int failed = errno;
    return PinAttempt{PinStatus::failed, "could not inspect the pinned tmux socket: " +
                                             std::string{std::strerror(failed)}};
  }
  if (!S_ISSOCK(metadata.st_mode)) {
    return PinAttempt{PinStatus::failed,
                      "the resolved tmux endpoint is not a unix socket"};
  }
  return PinAttempt{metadata, std::move(lifetime)};
}

std::string inode_identity(const struct stat& metadata) {
  return "unix:" + std::to_string(static_cast<std::uintmax_t>(metadata.st_dev)) + ":" +
         std::to_string(static_cast<std::uintmax_t>(metadata.st_ino));
}

} // namespace

std::optional<std::string>
resolved_socket_path(const std::vector<std::string>& selector,
                     const std::vector<std::string>& environment) {
  const bool pair = selector.size() == 2U;
  if (pair && selector.front() == "-S") {
    return selector.back();
  }

  std::string label{"default"};
  if (pair && selector.front() == "-L") {
    label = selector.back();
  } else if (!selector.empty()) {
    return std::nullopt;
  }

  const auto directory = socket_directory(environment);
  if (!directory.has_value()) {
    return std::nullopt;
  }
  return (*directory / ("tmux-" + std::to_string(::getuid())) / label).string();
}

expected<void, std::string> prepare_socket_directory(const std::string& directory,
                                                     bool create) {
  if (create && ::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
    return unexpected("could not create the tmux socket directory: " +
                      std::string{std::strerror(errno)});
  }
  struct stat metadata {};
  if (::lstat(directory.c_str(), &metadata) != 0) {
    if (!create && errno == ENOENT) {
      return {};
    }
    return unexpected("could not inspect the tmux socket directory: " +
                      std::string{std::strerror(errno)});
  }
  if (!S_ISDIR(metadata.st_mode) || metadata.st_uid != ::getuid() ||
      (metadata.st_mode & 0007) != 0) {
    return unexpected(std::string{"the tmux socket directory must be a real directory "
                                  "owned by this uid without other-user permissions"});
  }
  return {};
}

expected<SocketEndpoint, std::string>
bind_socket_endpoint(const std::vector<std::string>& selector) {
  const auto resolved = resolved_socket_path(selector);
  if (!resolved.has_value()) {
    return SocketEndpoint{.connection = selector,
                          .socket_path = {},
                          .identity = {},
                          .alias = {},
                          .missing = false};
  }

  const std::filesystem::path socket{*resolved};
  PinAttempt pin = pin_under("/tmp", socket);
  if (pin.status == PinStatus::retry_elsewhere) {
    pin = pin_under(socket.parent_path(), socket);
  }
  if (pin.status == PinStatus::missing) {
    const std::string alias_path = pin.lifetime->path();
    return SocketEndpoint{.connection = {"-S", alias_path},
                          .socket_path = alias_path,
                          .identity = "missing:" + alias_path,
                          .alias = std::move(pin.lifetime),
                          .missing = true};
  }
  if (pin.status != PinStatus::pinned) {
    if (pin.diagnostic.empty()) {
      pin.diagnostic = "could not pin the tmux socket";
    }
    return unexpected(std::move(pin.diagnostic));
  }

  const std::string alias_path = pin.lifetime->path();
  return SocketEndpoint{
      .connection = {"-S", alias_path},
      .socket_path = alias_path,
      .identity = inode_identity(pin.metadata),
      .alias = std::move(pin.lifetime),
      .missing = false,
  };
}

#endif

} // namespace detail
LIBTMUX_NAMESPACE_END
