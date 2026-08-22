#include "libtmux/control.hpp"

#include <utility>

#if !defined(_WIN32)
#error "connection_windows.cpp is Windows-only"
#endif

LIBTMUX_NAMESPACE_BEGIN

namespace {

ProtocolError control_unavailable() {
  return ProtocolError{"persistent control mode is unavailable with psmux on Windows; "
                       "subprocess Server operations remain supported"};
}

} // namespace

struct Connection::State {};

Connection::Connection(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

Connection::~Connection() noexcept = default;
Connection::Connection(Connection&&) noexcept = default;

Connection& Connection::operator=(Connection&& other) noexcept {
  if (this != &other) {
    state_ = std::move(other.state_);
  }
  return *this;
}

expected<Connection, ProtocolError> Connection::connect(ConnectionOptions options) {
  static_cast<void>(options);
  return unexpected(control_unavailable());
}

ControlRequestResult
Connection::execute(ControlRequest request,
                    std::chrono::steady_clock::time_point deadline) {
  const std::size_t expected_operations = request.group.size();
  return execute(std::move(request), expected_operations, deadline);
}

ControlRequestResult
Connection::execute(ControlRequest request, std::size_t expected_operations,
                    std::chrono::steady_clock::time_point deadline) {
  static_cast<void>(request);
  static_cast<void>(deadline);
  ControlRequestResult result;
  result.operations.resize(expected_operations);
  result.connection_error = control_unavailable();
  return result;
}

std::vector<Notification> Connection::take_notifications() { return {}; }

std::vector<Notification>
Connection::wait_for_notifications(std::chrono::steady_clock::time_point deadline) {
  static_cast<void>(deadline);
  return {};
}

expected<void, ProtocolError>
Connection::set_pane_output(std::string_view pane, bool deliver,
                            std::chrono::steady_clock::time_point deadline) {
  static_cast<void>(pane);
  static_cast<void>(deliver);
  static_cast<void>(deadline);
  return unexpected(control_unavailable());
}

NotificationRange Connection::events(std::chrono::steady_clock::time_point deadline) {
  return NotificationRange{*this, deadline};
}

const Notification* NotificationRange::next() {
  for (;;) {
    if (index_ < batch_.size()) {
      return &batch_[index_++];
    }
    if (connection_ == nullptr || std::chrono::steady_clock::now() >= deadline_) {
      return nullptr;
    }
    batch_ = connection_->wait_for_notifications(deadline_);
    index_ = 0;
    if (batch_.empty()) {
      return nullptr;
    }
  }
}

void NotificationRange::iterator::advance() {
  if (range_ == nullptr) {
    return;
  }
  const Notification* notification = range_->next();
  if (notification == nullptr) {
    range_ = nullptr;
    current_ = ParsedNotification{};
    return;
  }
  current_ = parse(*notification);
}

int Connection::notification_fd() const noexcept { return -1; }

std::size_t Connection::dropped_notifications() const noexcept { return 0; }

std::int64_t Connection::native_child_pid() const noexcept { return -1; }

expected<void, ProtocolError>
Connection::shutdown(std::chrono::steady_clock::time_point deadline) {
  static_cast<void>(deadline);
  state_.reset();
  return {};
}

LIBTMUX_NAMESPACE_END
