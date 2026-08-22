#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "protocol.hpp"

namespace libtmux::mcp::server {

class Writer {
public:
  void send(const json& message);

private:
  std::mutex mutex_;
};

struct RequestTicket {
  std::string key;
  std::shared_ptr<std::atomic_bool> cancellation;
};

using Completion = std::function<void(std::optional<json>)>;

class Dispatcher {
public:
  Dispatcher(ProtocolSession& session, Writer& writer);
  ~Dispatcher();

  Dispatcher(const Dispatcher&) = delete;
  Dispatcher& operator=(const Dispatcher&) = delete;

  [[nodiscard]] std::optional<json> reserve(const json& id, bool remember,
                                            RequestTicket& ticket);
  [[nodiscard]] std::optional<json> submit(CallRequest request, RequestTicket ticket,
                                           Completion completion);
  void release(const RequestTicket& ticket);
  void cancel(const json& id);
  void finish();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace libtmux::mcp::server
