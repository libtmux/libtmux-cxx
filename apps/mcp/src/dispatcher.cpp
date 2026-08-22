#include "dispatcher.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace libtmux::mcp::server {

void Writer::send(const json& message) {
  std::lock_guard lock{mutex_};
  std::cout << message.dump() << '\n' << std::flush;
}

class Dispatcher::Impl {
public:
  Impl(ProtocolSession& session, Writer& writer) : session_{session}, writer_{writer} {
    workers_.reserve(kWorkerCount);
    for (std::size_t index = 0; index < kWorkerCount; ++index) {
      static_cast<void>(index);
      workers_.emplace_back([this] { worker(); });
    }
  }

  ~Impl() { finish(); }

  [[nodiscard]] std::optional<json> reserve(const json& id, bool remember,
                                            RequestTicket& ticket) {
    const std::string key = request_key(id);
    std::lock_guard lock{mutex_};
    if (closing_) {
      return failure(id, kServerBusy, "server is shutting down");
    }
    if (active_.contains(key)) {
      return failure(id, kInvalidRequest, "request id is already in flight");
    }
    if (remember && seen_.contains(key)) {
      return failure(id, kInvalidRequest, "request id was already used");
    }
    if (active_.size() >= kMaximumInFlight) {
      return failure(id, kServerBusy, "too many in-flight requests");
    }
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    active_.emplace(key, cancellation);
    if (remember) {
      seen_.insert(key);
    }
    ticket = RequestTicket{.key = key, .cancellation = std::move(cancellation)};
    return std::nullopt;
  }

  [[nodiscard]] std::optional<json> submit(CallRequest request, RequestTicket ticket,
                                           Completion completion) {
    std::lock_guard lock{mutex_};
    if (closing_) {
      return failure(request.id, kServerBusy, "server is shutting down");
    }
    const auto reserved = active_.find(ticket.key);
    if (reserved == active_.end() || reserved->second != ticket.cancellation) {
      return failure(request.id, kInternalError, "request reservation was lost");
    }
    queue_.push_back(Job{.request = std::move(request),
                         .ticket = std::move(ticket),
                         .completion = std::move(completion)});
    ready_.notify_one();
    return std::nullopt;
  }

  void release(const RequestTicket& ticket) {
    std::lock_guard lock{mutex_};
    const auto found = active_.find(ticket.key);
    if (found != active_.end() && found->second == ticket.cancellation) {
      active_.erase(found);
    }
  }

  void cancel(const json& id) {
    std::lock_guard lock{mutex_};
    const auto found = active_.find(request_key(id));
    if (found != active_.end()) {
      found->second->store(true);
    }
  }

  void finish() {
    {
      std::lock_guard lock{mutex_};
      if (workers_.empty()) {
        return;
      }
      closing_ = true;
      for (const auto& [key, cancellation] : active_) {
        static_cast<void>(key);
        cancellation->store(true);
      }
    }
    ready_.notify_all();
    for (std::thread& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
  }

private:
  struct Job {
    CallRequest request;
    RequestTicket ticket;
    Completion completion;
  };

  void worker() noexcept {
    while (true) {
      std::optional<Job> job;
      {
        std::unique_lock lock{mutex_};
        ready_.wait(lock, [this] { return closing_ || !queue_.empty(); });
        if (queue_.empty()) {
          if (closing_) {
            return;
          }
          continue;
        }
        job = std::move(queue_.front());
        queue_.pop_front();
      }

      if (job->ticket.cancellation->load()) {
        job->completion(std::nullopt);
        continue;
      }
      const auto progress = [this, token = job->request.progress_token,
                             cancellation = job->ticket.cancellation](
                                double completed, std::optional<double> total,
                                std::string message) {
        if (!token.has_value() || cancellation->load()) {
          return;
        }
        json params{{"progressToken", *token},
                    {"progress", completed},
                    {"message", std::move(message)}};
        if (total.has_value()) {
          params["total"] = *total;
        }
        writer_.send({{"jsonrpc", "2.0"},
                      {"method", "notifications/progress"},
                      {"params", std::move(params)}});
      };
      json reply;
      try {
        reply = session_.execute(
            job->request, CallContext{.is_cancelled =
                                          [cancellation = job->ticket.cancellation] {
                                            return cancellation->load();
                                          },
                                      .progress = progress});
      } catch (const std::exception& error) {
        std::fprintf(stderr, "libtmux-mcp: tool request failed: %s\n", error.what());
        reply =
            failure(job->request.id, kInternalError, "internal tool execution failure");
      } catch (...) {
        std::fprintf(stderr, "libtmux-mcp: tool request failed\n");
        reply =
            failure(job->request.id, kInternalError, "internal tool execution failure");
      }

      job->completion(job->ticket.cancellation->load()
                          ? std::optional<json>{}
                          : std::optional<json>{std::move(reply)});
    }
  }

  static constexpr std::size_t kWorkerCount = 4U;
  static constexpr std::size_t kMaximumInFlight = 64U;
  ProtocolSession& session_;
  Writer& writer_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Job> queue_;
  std::unordered_map<std::string, std::shared_ptr<std::atomic_bool>> active_;
  std::unordered_set<std::string> seen_;
  std::vector<std::thread> workers_;
  bool closing_{false};
};

Dispatcher::Dispatcher(ProtocolSession& session, Writer& writer)
    : impl_{std::make_unique<Impl>(session, writer)} {}

Dispatcher::~Dispatcher() = default;

std::optional<json> Dispatcher::reserve(const json& id, bool remember,
                                        RequestTicket& ticket) {
  return impl_->reserve(id, remember, ticket);
}

std::optional<json> Dispatcher::submit(CallRequest request, RequestTicket ticket,
                                       Completion completion) {
  return impl_->submit(std::move(request), std::move(ticket), std::move(completion));
}

void Dispatcher::release(const RequestTicket& ticket) { impl_->release(ticket); }

void Dispatcher::cancel(const json& id) { impl_->cancel(id); }

void Dispatcher::finish() { impl_->finish(); }

} // namespace libtmux::mcp::server
