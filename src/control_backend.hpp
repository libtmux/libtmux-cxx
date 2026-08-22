#pragma once

// The second implementation of the transport, and the reason the seam exists.
//
// A subprocess backend launches tmux once per command. A control-mode backend
// keeps one tmux client open and writes commands to it, so the cost of a
// command stops being the cost of a process. Every entity works over it
// unchanged, because nothing above this line knows which one it has.
//
// One connection carries one conversation, so commands are serialized here.
// That is a property of the protocol rather than a limitation of this class:
// replies are matched to commands by order.

#include "libtmux/abi.hpp"
#include "libtmux/command.hpp"
#include "libtmux/control.hpp"
#include "libtmux/expected.hpp"
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "backend.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

// A Server owns routing; the remaining connection policy stays caller-owned.
[[nodiscard]] ConnectionOptions routed_control_options(ConnectionOptions options,
                                                       std::string socket_path,
                                                       std::string session);
[[nodiscard]] ControlRequest batch_request(const CommandBatch& batch);

class ControlBackend final : public Backend {
public:
  // `socket_path` is where the client is launched; `identity` is which server
  // that is. They are the same string today and are still two parameters,
  // because a connection opened over a resolved path must keep identifying as
  // the server the caller selected — otherwise a `-L` server and its own
  // control connection would compare as two.
  [[nodiscard]] static expected<std::shared_ptr<const ControlBackend>, ProtocolError>
  open(std::vector<std::string> selector, std::string socket_path, std::string identity,
       std::string session, ConnectionOptions options, CommandObserver observer,
       ExecutionPolicy policy = {});

  using Backend::run;

  [[nodiscard]] expected<std::string, CommandFailure>
  run(const std::vector<std::string>& command,
      std::optional<std::chrono::milliseconds> timeout,
      std::optional<std::size_t> output_limit) const override;

  [[nodiscard]] const std::vector<std::string>& connection() const noexcept override {
    return selector_;
  }

  [[nodiscard]] std::string_view identity() const noexcept override {
    return identity_;
  }

  [[nodiscard]] ServerCapabilities capabilities() const noexcept override {
    return {.implementation = ServerImplementation::tmux,
            .backend = BackendKind::control_mode};
  }

  // `tmux -V` is a flag of the binary, not a command a connection can carry,
  // so the running version is asked for as a format instead.
  [[nodiscard]] expected<Version, CommandFailure> version() const override;

  [[nodiscard]] std::vector<Notification> take_notifications() const override;
  // Each command in the batch becomes one operation in the request, so the
  // separator is the protocol's rather than a literal argument.
  [[nodiscard]] expected<std::string, CommandFailure>
  run_batch(const CommandBatch& batch, std::optional<std::chrono::milliseconds> timeout,
            std::optional<std::size_t> output_limit) const override;

  [[nodiscard]] std::size_t dropped_notifications() const noexcept override;

  ControlBackend(Connection connection, std::vector<std::string> selector,
                 std::string identity, CommandObserver observer,
                 ExecutionPolicy policy);

private:
  mutable std::mutex mutex_;
  mutable Connection connection_;
  std::vector<std::string> selector_;
  std::string identity_;
};

} // namespace detail
LIBTMUX_NAMESPACE_END
