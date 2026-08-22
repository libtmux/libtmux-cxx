#include "batch_response.hpp"

#include <utility>

namespace libtmux::mcp::server {

BatchResponse::BatchResponse(std::size_t size, Writer& writer, Dispatcher& dispatcher)
    : writer_{writer}, dispatcher_{dispatcher}, responses_(size), settled_(size, false),
      remaining_{size} {}

void BatchResponse::add_ticket(RequestTicket ticket) {
  std::lock_guard lock{mutex_};
  tickets_.push_back(std::move(ticket));
}

void BatchResponse::settle(std::size_t index, std::optional<json> response) {
  {
    std::lock_guard lock{mutex_};
    if (index >= settled_.size() || settled_[index]) {
      return;
    }
    settled_[index] = true;
    responses_[index] = std::move(response);
    --remaining_;
  }
  deliver();
}

void BatchResponse::seal() {
  {
    std::lock_guard lock{mutex_};
    sealed_ = true;
  }
  deliver();
}

std::optional<BatchResponse::Delivery> BatchResponse::take_delivery() {
  std::lock_guard lock{mutex_};
  if (!sealed_ || remaining_ != 0U || delivered_) {
    return std::nullopt;
  }
  delivered_ = true;
  json replies = json::array();
  for (auto& response : responses_) {
    if (response.has_value()) {
      replies.push_back(*std::move(response));
    }
  }
  return Delivery{.responses = std::move(replies), .tickets = std::move(tickets_)};
}

void BatchResponse::deliver() {
  auto delivery = take_delivery();
  if (!delivery.has_value()) {
    return;
  }
  if (!delivery->responses.empty()) {
    writer_.send(delivery->responses);
  }
  for (const RequestTicket& ticket : delivery->tickets) {
    dispatcher_.release(ticket);
  }
}

} // namespace libtmux::mcp::server
