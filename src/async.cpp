#include "libtmux/async.hpp"

#include "backend.hpp"
#include "command_engine.hpp"
#include "completion_queue.hpp"
#include "operation_state.hpp"
#include "process.hpp"
#if !defined(_WIN32)
#include "process_engine.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "libtmux/server.hpp"

LIBTMUX_NAMESPACE_BEGIN

namespace {

[[nodiscard]] CommandFailure immediate_failure(FailureKind kind,
                                               std::string diagnostic) {
  return CommandFailure{.kind = kind,
                        .delivery = DeliveryStatus::not_started,
                        .exit_code = 0,
                        .diagnostic = std::move(diagnostic)};
}

[[nodiscard]] CommandFailure accepted_internal_failure(std::string diagnostic) {
  return CommandFailure{.kind = FailureKind::pipe,
                        .delivery = DeliveryStatus::indeterminate,
                        .exit_code = 0,
                        .diagnostic = std::move(diagnostic)};
}

#if !defined(_WIN32)
[[nodiscard]] FailureKind process_failure_kind(detail::ProcessError::Kind kind) {
  switch (kind) {
  case detail::ProcessError::Kind::validation:
    return FailureKind::validation;
  case detail::ProcessError::Kind::spawn:
    return FailureKind::spawn;
  case detail::ProcessError::Kind::pre_exec:
    return FailureKind::pre_exec;
  case detail::ProcessError::Kind::pipe:
    return FailureKind::pipe;
  case detail::ProcessError::Kind::timeout:
    return FailureKind::timeout;
  case detail::ProcessError::Kind::cancelled:
    return FailureKind::cancelled;
  }
  return FailureKind::pipe;
}
#endif

class RuntimeLedger final {
public:
  explicit RuntimeLedger(std::size_t capacity) noexcept : capacity_{capacity} {}

  [[nodiscard]] bool accepting() const noexcept {
    std::lock_guard lock{mutex_};
    return accepting_;
  }

  void stop() noexcept {
    std::lock_guard lock{mutex_};
    accepting_ = false;
  }

  [[nodiscard]] bool full() const noexcept {
    std::lock_guard lock{mutex_};
    return in_flight_ >= capacity_;
  }

  void refuse() noexcept {
    std::lock_guard lock{mutex_};
    ++refused_;
  }

  [[nodiscard]] CommandRuntimeSnapshot snapshot() const noexcept {
    std::lock_guard lock{mutex_};
    return CommandRuntimeSnapshot{.capacity = capacity_,
                                  .in_flight = in_flight_,
                                  .pending_results = pending_results_,
                                  .pending_observers = pending_observers_,
                                  .accepted = accepted_,
                                  .refused = refused_,
                                  .completed = completed_,
                                  .accepting = accepting_};
  }

private:
  friend class RuntimeLease;

  mutable std::mutex mutex_;
  std::size_t capacity_{};
  std::size_t in_flight_{};
  std::size_t pending_results_{};
  std::size_t pending_observers_{};
  std::uint64_t accepted_{};
  std::uint64_t refused_{};
  std::uint64_t completed_{};
  bool accepting_{true};
};

class RuntimeLease final {
public:
  explicit RuntimeLease(std::shared_ptr<RuntimeLedger> ledger) noexcept
      : ledger_{std::move(ledger)} {}

  void admit(bool has_observer) noexcept {
    std::lock_guard lock{ledger_->mutex_};
    admitted_ = true;
    observer_finished_ = !has_observer;
    ++ledger_->in_flight_;
    ++ledger_->pending_results_;
    if (has_observer) {
      ++ledger_->pending_observers_;
    }
    ++ledger_->accepted_;
  }

  void complete() noexcept {
    std::lock_guard lock{ledger_->mutex_};
    if (admitted_ && !completed_) {
      completed_ = true;
      ++ledger_->completed_;
    }
  }

  void finish_transport() noexcept { finish(Leg::transport); }
  void finish_result() noexcept { finish(Leg::result); }
  void finish_observer() noexcept { finish(Leg::observer); }

private:
  enum class Leg { transport, result, observer };

  void finish(Leg leg) noexcept {
    std::lock_guard lock{ledger_->mutex_};
    if (!admitted_) {
      return;
    }
    switch (leg) {
    case Leg::transport:
      transport_finished_ = true;
      break;
    case Leg::result:
      if (!result_finished_) {
        result_finished_ = true;
        --ledger_->pending_results_;
      }
      break;
    case Leg::observer:
      if (!observer_finished_) {
        observer_finished_ = true;
        --ledger_->pending_observers_;
      }
      break;
    }
    if (!released_ && transport_finished_ && result_finished_ && observer_finished_) {
      released_ = true;
      --ledger_->in_flight_;
    }
  }

  std::shared_ptr<RuntimeLedger> ledger_;
  bool admitted_{};
  bool completed_{};
  bool transport_finished_{};
  bool result_finished_{};
  bool observer_finished_{};
  bool released_{};
};

class ResultHooks final : public detail::OperationHooks {
public:
  explicit ResultHooks(std::shared_ptr<RuntimeLease> lease) noexcept
      : lease_{std::move(lease)} {}

  void wake_reactor() noexcept override {}

  void release_admission() noexcept override {
    if (lease_) {
      lease_->finish_result();
      lease_.reset();
    }
  }

private:
  std::shared_ptr<RuntimeLease> lease_;
};

class ObserverLeg final {
public:
  explicit ObserverLeg(std::shared_ptr<RuntimeLease> lease) noexcept
      : lease_{std::move(lease)} {}
  ~ObserverLeg() { release(); }
  ObserverLeg(const ObserverLeg&) = delete;
  ObserverLeg& operator=(const ObserverLeg&) = delete;
  ObserverLeg(ObserverLeg&& other) noexcept : lease_{std::move(other.lease_)} {}
  ObserverLeg& operator=(ObserverLeg&&) = delete;

  void release() noexcept {
    if (lease_) {
      lease_->finish_observer();
      lease_.reset();
    }
  }

private:
  std::shared_ptr<RuntimeLease> lease_;
};

struct Observation final {
  CommandObserver callback;
  std::string command;
  CommandFailure failure;
  bool failed{};

  void dispatch() const { callback(command, failed ? &failure : nullptr); }
};

class ObserverRecord final {
public:
  ObserverRecord(std::shared_ptr<Observation> observation,
                 std::shared_ptr<RuntimeLease> lease) noexcept
      : observation_{std::move(observation)}, leg_{std::move(lease)} {}
  ~ObserverRecord() {
    leg_.release();
    observation_.reset();
  }
  ObserverRecord(const ObserverRecord&) = delete;
  ObserverRecord& operator=(const ObserverRecord&) = delete;
  ObserverRecord(ObserverRecord&&) noexcept = default;
  ObserverRecord& operator=(ObserverRecord&&) = delete;

  void operator()() {
    leg_.release();
    observation_->dispatch();
  }

private:
  std::shared_ptr<Observation> observation_;
  ObserverLeg leg_;
};

class ActiveObserverDisposition final {
public:
  explicit ActiveObserverDisposition(std::atomic_size_t& active) noexcept
      : active_{active} {
    active_.fetch_add(1U);
  }
  ~ActiveObserverDisposition() { active_.fetch_sub(1U); }
  ActiveObserverDisposition(const ActiveObserverDisposition&) = delete;
  ActiveObserverDisposition& operator=(const ActiveObserverDisposition&) = delete;

private:
  std::atomic_size_t& active_;
};

#if !defined(_WIN32)
std::mutex launch_observer_mutex;
std::function<void(const detail::ProcessRequest&)> runtime_launch_observer;
std::function<void()> runtime_completion_observer;
std::optional<detail::RuntimeFailurePoint> runtime_action_failure;
bool fail_runtime_start{};
bool fail_runtime_subscription{};

[[nodiscard]] std::function<void(const detail::ProcessRequest&)>
copy_runtime_launch_observer() {
  std::lock_guard lock{launch_observer_mutex};
  return runtime_launch_observer;
}

[[nodiscard]] bool consume_runtime_start_failure() {
  std::lock_guard lock{launch_observer_mutex};
  return std::exchange(fail_runtime_start, false);
}

[[nodiscard]] bool consume_runtime_subscription_failure() {
  std::lock_guard lock{launch_observer_mutex};
  return std::exchange(fail_runtime_subscription, false);
}

[[nodiscard]] bool consume_runtime_action_failure(detail::RuntimeFailurePoint point) {
  std::lock_guard lock{launch_observer_mutex};
  if (runtime_action_failure != point) {
    return false;
  }
  runtime_action_failure.reset();
  return true;
}

void notify_runtime_completion_observer() noexcept {
  std::function<void()> observer;
  try {
    {
      std::lock_guard lock{launch_observer_mutex};
      observer = runtime_completion_observer;
    }
    if (observer) {
      observer();
    }
  } catch (...) {
  }
}
#endif

} // namespace

#if !defined(_WIN32)
namespace detail {

void set_runtime_launch_observer_for_test(
    std::function<void(const ProcessRequest&)> observer) {
  std::lock_guard lock{launch_observer_mutex};
  runtime_launch_observer = std::move(observer);
}

void fail_next_runtime_action_for_test(RuntimeFailurePoint point) {
  std::lock_guard lock{launch_observer_mutex};
  runtime_action_failure = point;
}

void set_runtime_completion_observer_for_test(std::function<void()> observer) {
  std::lock_guard lock{launch_observer_mutex};
  runtime_completion_observer = std::move(observer);
}

void fail_next_runtime_start_for_test() {
  std::lock_guard lock{launch_observer_mutex};
  fail_runtime_start = true;
}

void fail_next_runtime_subscription_for_test() {
  std::lock_guard lock{launch_observer_mutex};
  fail_runtime_subscription = true;
}

} // namespace detail
#endif

#if !defined(_WIN32)
using RuntimeRawReply = detail::ProcessReply;
#else
using RuntimeRawReply = std::string;
#endif

struct CommandOperation::State final {
  detail::Operation<std::string> result;
  detail::OperationCancellation<RuntimeRawReply> cancellation;
  std::shared_ptr<RuntimeLease> result_lease;

  ~State() {
    if (result_lease) {
      result_lease->finish_result();
    }
  }
};

struct CommandRuntime::State final {
  struct Task final {
    std::uint64_t id{};
    detail::Subscription<RuntimeRawReply> subscription;
  };

#if !defined(_WIN32)
  State(std::shared_ptr<RuntimeLedger> ledger,
        std::shared_ptr<detail::ProcessEngine> engine, std::size_t capacity)
      : ledger_{std::move(ledger)}, engine_{std::move(engine)} {
#else
  State(std::shared_ptr<RuntimeLedger> ledger,
        std::shared_ptr<detail::CommandEngine> engine, std::size_t capacity)
      : ledger_{std::move(ledger)}, engine_{std::move(engine)} {
#endif
    tasks_.reserve(capacity);
  }

  ~State() noexcept {
    try {
      static_cast<void>(close());
    } catch (...) {
      request_stop();
#if !defined(_WIN32)
      static_cast<void>(engine_->close());
#else
      engine_->close();
#endif
      {
        std::lock_guard lock{completion_mutex_};
        finish_completion_thread_ = true;
        completion_wake_ = true;
      }
      completion_ready_.notify_all();
      if (completion_thread_.joinable()) {
        completion_thread_.join();
      }
    }
  }

  void start_completion_thread() {
    completion_thread_ = std::thread{[this] { completion_loop(); }};
  }

  [[nodiscard]] expected<CommandOperation, CommandFailure>
  submit(std::shared_ptr<const detail::Backend> backend, CommandRequest command,
         std::optional<std::chrono::milliseconds> timeout,
         std::optional<std::size_t> output_limit) {
    std::lock_guard submission_lock{submission_mutex_};
    const auto refuse =
        [this](CommandFailure failure) -> expected<CommandOperation, CommandFailure> {
      ledger_->refuse();
      return unexpected(std::move(failure));
    };

    if (!ledger_->accepting()) {
      return refuse(
          immediate_failure(FailureKind::cancelled,
                            "the command runtime stopped before this was admitted"));
    }
    if (command.empty()) {
      return refuse(immediate_failure(FailureKind::validation,
                                      "an asynchronous command cannot be empty"));
    }
    const auto subprocess =
        std::dynamic_pointer_cast<const detail::SubprocessBackend>(backend);
    if (!subprocess) {
      return refuse(
          immediate_failure(FailureKind::unsupported,
                            "this backend has no cooperative asynchronous transport"));
    }
    auto preflight = subprocess->async_preflight();
    if (!preflight.has_value()) {
      return refuse(std::move(preflight.error()));
    }

    const ExecutionPolicy& policy = subprocess->policy();
    const auto deadline = timeout.has_value() ? timeout : policy.timeout;
    const auto allowed = output_limit.has_value() ? output_limit : policy.output_limit;
    detail::ProcessRequest request =
        subprocess->build_request(command, std::nullopt, deadline, allowed);
    if (!detail::process_request_is_valid(request)) {
      return refuse(immediate_failure(
          FailureKind::validation,
          "an asynchronous command contains a value that cannot be executed"));
    }
    if (ledger_->full()) {
      return refuse(immediate_failure(
          FailureKind::overloaded,
          "the command runtime has more work in flight than it accepts"));
    }
    auto lease = std::make_shared<RuntimeLease>(ledger_);
    auto result =
        detail::make_operation<std::string>(std::make_shared<ResultHooks>(lease));
    // If setup fails after admission, losing internal bookkeeping cannot prove
    // that the raw transport did not execute.
    result.source.mark_dispatching();
    auto operation_state = std::make_unique<CommandOperation::State>();
    operation_state->result = std::move(result.operation);
    operation_state->result_lease = lease;
    auto result_source = std::make_shared<detail::OperationSource<std::string>>(
        std::move(result.source));

#if !defined(_WIN32)
    const std::size_t allowed_bytes = request.capture_limit;
#endif

    auto observer = subprocess->command_observer();
    std::shared_ptr<Observation> observation;
    std::optional<detail::CompletionToken> observer_token;
    detail::WeakCompletionMailbox observer_mailbox;
    if (observer.has_value()) {
      observation = std::make_shared<Observation>(
          Observation{.callback = std::move(*observer),
                      .command = detail::rendered_command(command),
                      .failure = accepted_internal_failure(
                          "the runtime could not translate this command result"),
                      .failed = false});
      const auto token = observers_.next_token();
      observer_mailbox = observers_.mailbox();
      observer_token = token;
    }

    const std::uint64_t id = next_task_;

#if !defined(_WIN32)
    detail::OperationCallback<detail::ProcessReply> callback{
        [this, id, subprocess, command = std::move(command), allowed_bytes,
         source = result_source, lease, observation, observer_mailbox, observer_token,
         conversion_failure = accepted_internal_failure(
             "the runtime could not translate this command result")](
            detail::OperationResult<detail::ProcessReply> reply) mutable {
          try {
            expected<std::string, CommandFailure> answer =
                !reply.has_value()
                    ? subprocess->interpret_failure_unobserved(command,
                                                               std::move(reply.error()))
                    : subprocess->interpret_unobserved(command, allowed_bytes,
                                                       *std::move(reply));
            finish_completion(id, *source, lease, observation, observer_mailbox,
                              observer_token, std::move(answer));
            return;
          } catch (...) {
          }
          finish_completion(id, *source, lease, observation, observer_mailbox,
                            observer_token, unexpected(std::move(conversion_failure)));
        }};
#else
    detail::OperationCallback<std::string> callback{
        [this, id, source = result_source, lease, observation, observer_mailbox,
         observer_token](detail::OperationResult<std::string> answer) mutable {
          finish_completion(id, *source, lease, observation, observer_mailbox,
                            observer_token, std::move(answer));
        }};
#endif

    if (observation) {
      const bool registered = observers_.register_record(
          *observer_token, ObserverRecord{observation, lease});
      if (!registered) {
        return refuse(immediate_failure(
            FailureKind::cancelled,
            "the command runtime stopped before observation was registered"));
      }
    }

    lease->admit(observation != nullptr);
    ++next_task_;
    detail::Operation<RuntimeRawReply> running;
    try {
#if !defined(_WIN32)
      running = engine_->submit(std::move(request), [this, lease] {
        lease->finish_transport();
        notify_completion();
      });
#else
      running = engine_->submit(subprocess, std::move(command), deadline, allowed,
                                [this, lease] {
                                  lease->finish_transport();
                                  notify_completion();
                                });
#endif
    } catch (...) {
      lease->finish_transport();
      finish_completion(
          id, *result_source, lease, observation, observer_mailbox, observer_token,
          unexpected(accepted_internal_failure(
              "the runtime could not hand an accepted command to its engine")));
      return CommandOperation{std::move(operation_state)};
    }
    operation_state->cancellation = running.cancellation();

    bool subscription_registered = false;
    {
      std::lock_guard task_lock{tasks_mutex_};
      detail::Subscription<RuntimeRawReply> subscription;
      try {
#if !defined(_WIN32)
        if (consume_runtime_subscription_failure()) {
          throw 0;
        }
#endif
        subscription = std::move(running).subscribe(completions_, std::move(callback));
      } catch (...) {
      }
      if (subscription.registered()) {
        tasks_.push_back(Task{.id = id, .subscription = std::move(subscription)});
        subscription_registered = true;
      } else {
        static_cast<void>(subscription.request_cancel());
      }
    }
    if (!subscription_registered) {
      finish_completion(id, *result_source, lease, observation, observer_mailbox,
                        observer_token,
                        unexpected(accepted_internal_failure(
                            "the runtime could not track an accepted command")));
      return CommandOperation{std::move(operation_state)};
    }
    notify_completion();
    return CommandOperation{std::move(operation_state)};
  }

  void request_stop() noexcept {
    {
      std::lock_guard submission_lock{submission_mutex_};
      ledger_->stop();
    }
    {
      std::lock_guard task_lock{tasks_mutex_};
      for (const auto& task : tasks_) {
        static_cast<void>(task.subscription.request_cancel());
      }
    }
    engine_->request_stop();
  }

  [[nodiscard]] CommandRuntimeShutdown close() {
    for (;;) {
      std::unique_lock lifecycle_lock{lifecycle_mutex_};
      if (terminal_) {
        return terminal_report_;
      }
      if (close_joining_) {
        lifecycle_ready_.wait(lifecycle_lock,
                              [this] { return terminal_ || !close_joining_; });
        continue;
      }
      close_joining_ = true;
      break;
    }

    try {
      request_stop();
      bool transports_stopped = true;
#if !defined(_WIN32)
      const auto engine_report = engine_->close();
      transports_stopped = engine_report.complete;
      if (consume_runtime_action_failure(
              detail::RuntimeFailurePoint::engine_shutdown)) {
        transports_stopped = false;
      }
      if (!transports_stopped) {
        store_lifecycle_failure(accepted_internal_failure(
            "the process runtime could not retire every accepted child"));
      }
#else
      engine_->close();
#endif
#if !defined(_WIN32)
      if (consume_runtime_action_failure(detail::RuntimeFailurePoint::close)) {
        throw detail::RuntimeFailurePoint::close;
      }
#endif
      {
        std::lock_guard completion_lock{completion_mutex_};
        finish_completion_thread_ = true;
        completion_wake_ = true;
      }
      completion_ready_.notify_all();
      if (completion_thread_.joinable()) {
        completion_thread_.join();
      }

      const auto final_snapshot = ledger_->snapshot();
      CommandRuntimeShutdown report{
          .pending_results = final_snapshot.pending_results,
          .pending_observers = final_snapshot.pending_observers,
          .transports_stopped = transports_stopped,
          .safe_to_unload = transports_stopped &&
                            final_snapshot.pending_results == 0U &&
                            final_snapshot.pending_observers == 0U &&
                            active_observer_dispositions_.load() == 0U,
          .failure = lifecycle_failure()};
      {
        std::lock_guard lifecycle_lock{lifecycle_mutex_};
        terminal_report_ = report;
        terminal_ = true;
        close_joining_ = false;
      }
      lifecycle_ready_.notify_all();
      return report;
    } catch (...) {
      {
        std::lock_guard lifecycle_lock{lifecycle_mutex_};
        close_joining_ = false;
      }
      lifecycle_ready_.notify_all();
      throw;
    }
  }

  [[nodiscard]] CommandRuntimeSnapshot snapshot() const noexcept {
    return ledger_->snapshot();
  }

  [[nodiscard]] std::size_t dispatch_ready() {
    const ActiveObserverDisposition active{active_observer_dispositions_};
    return observers_.run_ready();
  }

  [[nodiscard]] std::size_t discard_ready() {
    const ActiveObserverDisposition active{active_observer_dispositions_};
    return observers_.discard_ready();
  }

private:
  void notify_completion() noexcept {
    {
      std::lock_guard lock{completion_mutex_};
      completion_wake_ = true;
    }
    completion_ready_.notify_one();
  }

  void completion_loop() noexcept {
    for (;;) {
      {
        std::unique_lock lock{completion_mutex_};
        completion_ready_.wait(
            lock, [this] { return completion_wake_ || finish_completion_thread_; });
        completion_wake_ = false;
      }
      try {
#if !defined(_WIN32)
        if (consume_runtime_action_failure(
                detail::RuntimeFailurePoint::completion_queue)) {
          throw detail::RuntimeFailurePoint::completion_queue;
        }
#endif
        static_cast<void>(completions_.run_ready());
      } catch (...) {
        store_lifecycle_failure(
            accepted_internal_failure("the runtime completion queue failed"));
        std::lock_guard lock{completion_mutex_};
        completion_wake_ = true;
      }
      bool finishing = false;
      {
        std::lock_guard completion_lock{completion_mutex_};
        finishing = finish_completion_thread_;
      }
      std::lock_guard task_lock{tasks_mutex_};
      if (finishing && tasks_.empty()) {
        return;
      }
    }
  }

  void finish_completion(std::uint64_t id, detail::OperationSource<std::string>& source,
                         const std::shared_ptr<RuntimeLease>& lease,
                         const std::shared_ptr<Observation>& observation,
                         const detail::WeakCompletionMailbox& observer_mailbox,
                         std::optional<detail::CompletionToken> observer_token,
                         expected<std::string, CommandFailure> answer) noexcept {
    const auto publication_failure =
        accepted_internal_failure("the runtime could not publish a command result");
    try {
      if (observation) {
        observation->failed = !answer.has_value();
        if (!answer.has_value()) {
          observation->failure = answer.error();
        }
      }
#if !defined(_WIN32)
      if (consume_runtime_action_failure(
              detail::RuntimeFailurePoint::result_publication)) {
        throw detail::RuntimeFailurePoint::result_publication;
      }
#endif
      static_cast<void>(source.publish(std::move(answer)));
    } catch (...) {
      if (observation) {
        observation->failed = true;
        try {
          observation->failure = publication_failure;
        } catch (...) {
        }
      }
      try {
        static_cast<void>(
            source.publish(unexpected(CommandFailure{publication_failure})));
      } catch (...) {
      }
      store_lifecycle_failure(publication_failure);
    }
    source.retire();
    bool observer_ready = true;
    if (observer_token.has_value()) {
#if !defined(_WIN32)
      observer_ready = !consume_runtime_action_failure(
                           detail::RuntimeFailurePoint::observer_enqueue) &&
                       observer_mailbox.enqueue(*observer_token);
#else
      observer_ready = observer_mailbox.enqueue(*observer_token);
#endif
      if (!observer_ready) {
        store_lifecycle_failure(accepted_internal_failure(
            "the runtime could not queue a command observation"));
      }
    }
    if (observer_ready) {
      lease->complete();
#if !defined(_WIN32)
      notify_runtime_completion_observer();
#endif
    }
    {
      std::lock_guard task_lock{tasks_mutex_};
      const auto found =
          std::ranges::find(tasks_, id, [](const Task& task) { return task.id; });
      if (found != tasks_.end()) {
        tasks_.erase(found);
      }
    }
    notify_completion();
  }

  void store_lifecycle_failure(const CommandFailure& failure) noexcept {
    try {
      std::lock_guard lock{failure_mutex_};
      if (!lifecycle_failure_.has_value()) {
        lifecycle_failure_ = failure;
      }
    } catch (...) {
    }
  }

  [[nodiscard]] std::optional<CommandFailure> lifecycle_failure() const {
    std::lock_guard lock{failure_mutex_};
    return lifecycle_failure_;
  }

  std::shared_ptr<RuntimeLedger> ledger_;
  detail::CompletionQueue observers_;
  detail::CompletionQueue completions_;

  std::mutex submission_mutex_;
  std::uint64_t next_task_{};

  std::mutex tasks_mutex_;
  std::vector<Task> tasks_;

  std::mutex completion_mutex_;
  std::condition_variable completion_ready_;
  bool completion_wake_{};
  bool finish_completion_thread_{};

  std::mutex lifecycle_mutex_;
  std::condition_variable lifecycle_ready_;
  bool close_joining_{};
  bool terminal_{};
  CommandRuntimeShutdown terminal_report_{};

  mutable std::mutex failure_mutex_;
  std::optional<CommandFailure> lifecycle_failure_;
  std::atomic_size_t active_observer_dispositions_{};

#if !defined(_WIN32)
  std::shared_ptr<detail::ProcessEngine> engine_;
#else
  std::shared_ptr<detail::CommandEngine> engine_;
#endif
  std::thread completion_thread_;
};

CommandRuntime::CommandRuntime(std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}

expected<CommandRuntime, CommandFailure>
CommandRuntime::start(CommandRuntimeConfig config) {
  if (config.capacity == 0U) {
    return unexpected(immediate_failure(
        FailureKind::validation, "the command runtime capacity must be positive"));
  }
#if !defined(_WIN32)
  if (consume_runtime_start_failure()) {
    return unexpected(immediate_failure(
        FailureKind::pipe, "the command runtime could not start its threads"));
  }
#endif
  try {
    auto ledger = std::make_shared<RuntimeLedger>(config.capacity);
#if !defined(_WIN32)
    auto engine = detail::ProcessEngine::start(
        detail::EngineConfig{.operation_limit = config.capacity,
                             .admission_gate = {},
                             .launch_observer = copy_runtime_launch_observer()});
    if (!engine.has_value()) {
      return unexpected(
          CommandFailure{.kind = process_failure_kind(engine.error().kind),
                         .delivery = engine.error().delivery,
                         .exit_code = 0,
                         .diagnostic = std::move(engine.error().diagnostic)});
    }
#else
    auto engine = detail::CommandEngine::start(detail::CommandEngineConfig{
        .operation_limit = config.capacity, .worker_count = 2U});
    if (!engine.has_value()) {
      return unexpected(std::move(engine.error()));
    }
#endif
    auto state =
        std::make_unique<State>(std::move(ledger), *std::move(engine), config.capacity);
    state->start_completion_thread();
    return CommandRuntime{std::move(state)};
  } catch (const std::exception& failure) {
    return unexpected(immediate_failure(
        FailureKind::pipe, "the command runtime could not start its threads: " +
                               std::string{failure.what()}));
  } catch (...) {
    return unexpected(immediate_failure(
        FailureKind::pipe, "the command runtime could not start its threads"));
  }
}

CommandRuntime::CommandRuntime(CommandRuntime&&) noexcept = default;
CommandRuntime& CommandRuntime::operator=(CommandRuntime&&) noexcept = default;
CommandRuntime::~CommandRuntime() = default;

void CommandRuntime::request_stop() noexcept {
  if (state_) {
    state_->request_stop();
  }
}

CommandRuntimeShutdown CommandRuntime::close() {
  if (!state_) {
    return CommandRuntimeShutdown{.pending_results = 0U,
                                  .pending_observers = 0U,
                                  .transports_stopped = true,
                                  .safe_to_unload = true,
                                  .failure = {}};
  }
  return state_->close();
}

CommandRuntimeSnapshot CommandRuntime::snapshot() const noexcept {
  if (!state_) {
    return {};
  }
  return state_->snapshot();
}

std::size_t CommandRuntime::dispatch_ready() {
  return state_ ? state_->dispatch_ready() : 0U;
}

std::size_t CommandRuntime::discard_ready() {
  return state_ ? state_->discard_ready() : 0U;
}

CommandOperation::CommandOperation(std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}

CommandOperation::CommandOperation(CommandOperation&&) noexcept = default;
CommandOperation& CommandOperation::operator=(CommandOperation&&) noexcept = default;
CommandOperation::~CommandOperation() = default;

expected<std::string, CommandFailure> CommandOperation::wait() && {
  if (!state_) {
    return unexpected(immediate_failure(FailureKind::validation,
                                        "this operation has been waited on"));
  }
  auto state = std::move(state_);
  return detail::sync_wait(std::move(state->result));
}

void CommandOperation::detach() && noexcept { state_.reset(); }

bool CommandOperation::request_cancel() {
  return state_ && state_->cancellation.request_cancel();
}

expected<CommandOperation, CommandFailure>
Server::try_submit(CommandRuntime& runtime, CommandRequest command,
                   std::optional<std::chrono::milliseconds> timeout,
                   std::optional<std::size_t> output_limit) const {
  if (!runtime.state_) {
    return unexpected(immediate_failure(FailureKind::validation,
                                        "this command runtime has been moved from"));
  }
  return runtime.state_->submit(backend_, std::move(command), timeout, output_limit);
}

LIBTMUX_NAMESPACE_END
