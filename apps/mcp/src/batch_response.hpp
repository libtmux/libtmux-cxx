#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

#include "dispatcher.hpp"

namespace libtmux::mcp::server {

class BatchResponse {
public:
  BatchResponse(std::size_t size, Writer& writer, Dispatcher& dispatcher);

  void add_ticket(RequestTicket ticket);
  void settle(std::size_t index, std::optional<json> response);
  void seal();

private:
  struct Delivery {
    json responses;
    std::vector<RequestTicket> tickets;
  };

  [[nodiscard]] std::optional<Delivery> take_delivery();
  void deliver();

  Writer& writer_;
  Dispatcher& dispatcher_;
  std::mutex mutex_;
  std::vector<std::optional<json>> responses_;
  std::vector<bool> settled_;
  std::vector<RequestTicket> tickets_;
  std::size_t remaining_;
  bool sealed_{false};
  bool delivered_{false};
};

} // namespace libtmux::mcp::server
