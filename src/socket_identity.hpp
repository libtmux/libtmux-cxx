#pragma once

// A stable identity for the server a selector names.
//
// tmux's own rule, from `make_label` in tmux.c: take the first of
// `$TMUX_TMPDIR` and `/tmp` that resolves, append `tmux-<uid>`, and put the
// label under that — "default" when the selector names none. The directory is
// resolved the way tmux resolves it, with realpath, so two selectors differing
// only by a symlink name one server.
//
// On Windows, psmux has no public socket path; the selector itself supplies a
// logical identity instead.
//
// On Unix, a control client is launched against a path, so a server selected by `-L` or
// by nothing at all had no path to hand it and could not use the faster
// transport at all.
//
// And a command combining two entities has to know whether they came from one
// tmux. Ids are numbered per server: `%1` on one socket names a different pane
// on another, so `pane_a.swap_with(pane_b)` across two servers ran against
// `pane_a`'s and found some unrelated pane there. Comparing the argv instead
// would call `-L work` and `-S <the path that resolves to>` different servers
// when they are the same one.

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
resolved_socket_path(const std::vector<std::string>& selector);

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
