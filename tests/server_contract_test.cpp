// Process behaviour pinned against the shipped library rather than the
// prototype that shares its kernel. These are the contracts a caller can
// observe through Server, which is the only surface that survives deletion of
// the transport spikes.
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <latch>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/batch.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "libtmux/testing/scoped_server.hpp"
#include "support/descriptors.hpp"
#if !defined(_WIN32)
#include "process_engine.hpp"
#endif

namespace {

using libtmux::DeliveryStatus;
using libtmux::Server;

libtmux::CommandRuntime start_runtime(libtmux::CommandRuntimeConfig config = {}) {
  auto runtime = libtmux::CommandRuntime::start(config);
  if (!runtime.has_value()) {
    throw std::runtime_error{runtime.error().diagnostic};
  }
  return *std::move(runtime);
}

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = std::chrono::seconds{3}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  return predicate();
}

struct ObserverTeardownState final {
  std::binary_semaphore started{0};
  std::binary_semaphore release{0};
  std::atomic_bool armed{};
};

struct BlockingObserverTeardown final {
  std::shared_ptr<ObserverTeardownState> state;

  void operator()(std::string_view, const libtmux::CommandFailure*) const {}

  ~BlockingObserverTeardown() {
    if (state && state->armed.exchange(false)) {
      state->started.release();
      state->release.acquire();
    }
  }
};

class ObserverTeardownRelease final {
public:
  explicit ObserverTeardownRelease(
      std::shared_ptr<ObserverTeardownState> state) noexcept
      : state_{std::move(state)} {}
  ~ObserverTeardownRelease() { release(); }
  ObserverTeardownRelease(const ObserverTeardownRelease&) = delete;
  ObserverTeardownRelease& operator=(const ObserverTeardownRelease&) = delete;

  void release() noexcept {
    if (!released_) {
      state_->armed.store(false);
      state_->release.release();
      released_ = true;
    }
  }

private:
  std::shared_ptr<ObserverTeardownState> state_;
  bool released_{};
};

#if !defined(_WIN32)
class RuntimeCompletionGate final {
public:
  RuntimeCompletionGate(std::binary_semaphore& release, std::function<void()> observer)
      : release_{release} {
    libtmux::detail::set_runtime_completion_observer_for_test(std::move(observer));
  }
  ~RuntimeCompletionGate() { release(); }
  RuntimeCompletionGate(const RuntimeCompletionGate&) = delete;
  RuntimeCompletionGate& operator=(const RuntimeCompletionGate&) = delete;

  void release() noexcept {
    if (!released_) {
      libtmux::detail::set_runtime_completion_observer_for_test({});
      release_.release();
      released_ = true;
    }
  }

private:
  std::binary_semaphore& release_;
  bool released_{};
};
#endif

#if defined(__linux__)
std::size_t thread_count() {
  return static_cast<std::size_t>(
      std::distance(std::filesystem::directory_iterator{"/proc/self/task"},
                    std::filesystem::directory_iterator{}));
}
#endif

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

// Several admitted commands may complete in parallel. Each answer must still
// belong to the question that caller asked.
TEST(ServerContract, SubmittedCommandsAreCollectedLater) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto runtime = start_runtime();

  constexpr int asked = 8;
  std::vector<libtmux::CommandOperation> sent;
  sent.reserve(asked);
  for (int index = 0; index < asked; ++index) {
    auto submitted = server.try_submit(
        runtime, {"display-message", "-p", "asked-" + std::to_string(index)});
    ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
    sent.push_back(*std::move(submitted));
  }

  for (int index = 0; index < asked; ++index) {
    auto answer = std::move(sent[static_cast<std::size_t>(index)]).wait();
    ASSERT_TRUE(answer.has_value()) << answer.error().diagnostic;
    EXPECT_EQ(*answer, "asked-" + std::to_string(index) + "\n");
  }
}

// What `run` bounds per call, `try_submit` bounds per call too. The one long
// question in a batch is exactly the one needing a bound of its own, and a
// submission that could only take the server-wide default would leave a
// caller opening a second server to ask it.
TEST(ServerContract, ASubmissionTakesTheBoundTheCallerGaveIt) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto runtime = start_runtime();

  auto submitted = server.try_submit(
      runtime, {"display-message", "-p", "longer than one byte"}, {}, std::size_t{1});
  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
  auto answer = std::move(*submitted).wait();

  ASSERT_FALSE(answer.has_value()) << "the call's own bound was not applied";
  EXPECT_EQ(answer.error().kind, libtmux::FailureKind::truncated);
  EXPECT_EQ(answer.error().delivery, DeliveryStatus::replied);
  // Which bound was passed, not merely that one was. The diagnostic names the
  // number a caller has to change, so naming the server's instead sends them
  // to a setting that was never in force.
  EXPECT_NE(answer.error().diagnostic.find("the 1 byte limit"), std::string::npos)
      << answer.error().diagnostic;
}

// A submitted command reports what tmux said about it, not merely that it ran.
TEST(ServerContract, ASubmittedFailureCarriesWhatTmuxSaid) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::optional<libtmux::FailureKind> observed_failure;
  auto server = Server::at_socket_path(
      fixture->socket_path().string(),
      [&observed_failure](std::string_view, const libtmux::CommandFailure* failure) {
        if (failure != nullptr) {
          observed_failure = failure->kind;
        }
      });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  auto runtime = start_runtime();

  auto submitted =
      server->try_submit(runtime, {"kill-session", "-t", "no-such-session"});
  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
  const auto answer = (*std::move(submitted)).wait();

  ASSERT_FALSE(answer.has_value());
  EXPECT_EQ(answer.error().kind, libtmux::FailureKind::refused);
  EXPECT_NE(answer.error().diagnostic.find("no-such-session"), std::string::npos);
  ASSERT_TRUE(wait_until([&runtime] { return runtime.snapshot().completed == 1U; }));
  EXPECT_FALSE(observed_failure.has_value());
  EXPECT_EQ(runtime.dispatch_ready(), 1U);
  ASSERT_TRUE(observed_failure.has_value());
  EXPECT_EQ(*observed_failure, libtmux::FailureKind::refused);
}

TEST(ServerContract, WaitingDoesNotDispatchTheGlobalObserver) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::size_t observed = 0U;
  auto server = Server::at_socket_path(
      fixture->socket_path().string(),
      [&observed](std::string_view, const libtmux::CommandFailure*) { ++observed; });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  auto runtime = start_runtime({.capacity = 1U});

  auto submitted =
      server->try_submit(runtime, {"display-message", "-p", "observed later"});
  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
  auto answer = std::move(*submitted).wait();

  ASSERT_TRUE(answer.has_value()) << answer.error().diagnostic;
  ASSERT_TRUE(wait_until([&runtime] { return runtime.snapshot().completed == 1U; }));
  EXPECT_EQ(observed, 0U);
  const auto waiting = runtime.snapshot();
  EXPECT_EQ(waiting.pending_results, 0U);
  EXPECT_EQ(waiting.pending_observers, 1U);
  EXPECT_EQ(waiting.in_flight, 1U);
  EXPECT_EQ(runtime.dispatch_ready(), 1U);
  EXPECT_EQ(observed, 1U);
  EXPECT_EQ(runtime.snapshot().in_flight, 0U);
}

TEST(ServerContract, DroppingAnOperationKeepsItsGlobalObservation) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::size_t observed = 0U;
  auto server = Server::at_socket_path(
      fixture->socket_path().string(),
      [&observed](std::string_view, const libtmux::CommandFailure*) { ++observed; });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  auto runtime = start_runtime({.capacity = 1U});

  {
    auto submitted =
        server->try_submit(runtime, {"display-message", "-p", "keep observation"});
    ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
  }

  ASSERT_TRUE(wait_until([&runtime] { return runtime.snapshot().completed == 1U; }));
  const auto dropped = runtime.snapshot();
  EXPECT_EQ(dropped.pending_results, 0U);
  EXPECT_EQ(dropped.pending_observers, 1U);
  EXPECT_EQ(dropped.in_flight, 1U);
  EXPECT_EQ(observed, 0U);
  EXPECT_EQ(runtime.dispatch_ready(), 1U);
  EXPECT_EQ(observed, 1U);
}

TEST(ServerContract, AdmissionRemainsChargedUntilObservationDispatch) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  auto server =
      Server::at_socket_path(fixture->socket_path().string(),
                             [](std::string_view, const libtmux::CommandFailure*) {});
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  auto runtime = start_runtime({.capacity = 1U});

  auto first = server->try_submit(runtime, {"display-message", "-p", "first"});
  ASSERT_TRUE(first.has_value()) << first.error().diagnostic;
  ASSERT_TRUE(std::move(*first).wait().has_value());
  ASSERT_TRUE(wait_until([&runtime] { return runtime.snapshot().completed == 1U; }));

  auto refused = server->try_submit(runtime, {"display-message", "-p", "second"});
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, libtmux::FailureKind::overloaded);
  EXPECT_EQ(refused.error().delivery, DeliveryStatus::not_started);
  EXPECT_EQ(runtime.dispatch_ready(), 1U);

  auto admitted = server->try_submit(runtime, {"display-message", "-p", "second"});
  ASSERT_TRUE(admitted.has_value()) << admitted.error().diagnostic;
  EXPECT_TRUE(std::move(*admitted).wait().has_value());
  ASSERT_TRUE(wait_until([&runtime] { return runtime.snapshot().completed == 2U; }));
  EXPECT_EQ(runtime.discard_ready(), 1U);
}

TEST(ServerContract, DetachingAnOperationKeepsOnlyItsGlobalObservation) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::size_t observed = 0U;
  auto server = Server::at_socket_path(
      fixture->socket_path().string(),
      [&observed](std::string_view, const libtmux::CommandFailure*) { ++observed; });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  auto runtime = start_runtime({.capacity = 1U});

  auto submitted = server->try_submit(runtime, {"display-message", "-p", "detached"});
  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
  std::move(*submitted).detach();

  ASSERT_TRUE(wait_until([&runtime] { return runtime.snapshot().completed == 1U; }));
  const auto detached = runtime.snapshot();
  EXPECT_EQ(detached.pending_results, 0U);
  EXPECT_EQ(detached.pending_observers, 1U);
  EXPECT_EQ(detached.in_flight, 1U);
  EXPECT_EQ(runtime.dispatch_ready(), 1U);
  EXPECT_EQ(observed, 1U);
}

TEST(ServerContract, SnapshotCountersCoverEveryAdmissionDisposition) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  auto server =
      Server::at_socket_path(fixture->socket_path().string(),
                             [](std::string_view, const libtmux::CommandFailure*) {});
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  auto runtime = start_runtime({.capacity = 1U});

  const auto initial = runtime.snapshot();
  EXPECT_EQ(initial.capacity, 1U);
  EXPECT_EQ(initial.in_flight, 0U);
  EXPECT_EQ(initial.pending_results, 0U);
  EXPECT_EQ(initial.pending_observers, 0U);
  EXPECT_EQ(initial.accepted, 0U);
  EXPECT_EQ(initial.refused, 0U);
  EXPECT_EQ(initial.completed, 0U);
  EXPECT_TRUE(initial.accepting);

  auto accepted = server->try_submit(runtime, {"display-message", "-p", "counted"});
  ASSERT_TRUE(accepted.has_value()) << accepted.error().diagnostic;
  auto refused = server->try_submit(runtime, {"display-message", "-p", "too many"});
  ASSERT_FALSE(refused.has_value());

  const auto admitted = runtime.snapshot();
  EXPECT_EQ(admitted.in_flight, 1U);
  EXPECT_EQ(admitted.pending_results, 1U);
  EXPECT_EQ(admitted.pending_observers, 1U);
  EXPECT_EQ(admitted.accepted, 1U);
  EXPECT_EQ(admitted.refused, 1U);

  EXPECT_TRUE(std::move(*accepted).wait().has_value());
  ASSERT_TRUE(wait_until([&runtime] { return runtime.snapshot().completed == 1U; }));
  const auto completed = runtime.snapshot();
  EXPECT_EQ(completed.pending_results, 0U);
  EXPECT_EQ(completed.pending_observers, 1U);
  EXPECT_EQ(completed.in_flight, 1U);
  EXPECT_EQ(completed.accepted, 1U);
  EXPECT_EQ(completed.refused, 1U);
  EXPECT_EQ(completed.completed, 1U);

  EXPECT_EQ(runtime.discard_ready(), 1U);
  runtime.request_stop();
  auto stopped = server->try_submit(runtime, {"display-message", "-p", "stopped"});
  ASSERT_FALSE(stopped.has_value());
  const auto terminal = runtime.snapshot();
  EXPECT_EQ(terminal.in_flight, 0U);
  EXPECT_EQ(terminal.pending_results, 0U);
  EXPECT_EQ(terminal.pending_observers, 0U);
  EXPECT_EQ(terminal.accepted, 1U);
  EXPECT_EQ(terminal.refused, 2U);
  EXPECT_EQ(terminal.completed, 1U);
  EXPECT_FALSE(terminal.accepting);
}

TEST(ServerContract, ImmediateFailuresNeverConsumeAdmissionOrObservation) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::size_t observed = 0U;
  auto server = Server::at_socket_path(
      fixture->socket_path().string(),
      [&observed](std::string_view, const libtmux::CommandFailure*) { ++observed; });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  auto runtime = start_runtime({.capacity = 1U});

  auto invalid = server->try_submit(runtime, {});
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(invalid.error().kind, libtmux::FailureKind::validation);
  EXPECT_EQ(invalid.error().delivery, DeliveryStatus::not_started);
  libtmux::CommandRequest embedded_nul{"display-message", "-p"};
  embedded_nul.emplace_back(std::string{"not\0run", 7U});
  auto invalid_value = server->try_submit(runtime, std::move(embedded_nul));
  ASSERT_FALSE(invalid_value.has_value());
  EXPECT_EQ(invalid_value.error().kind, libtmux::FailureKind::validation);
  EXPECT_EQ(invalid_value.error().delivery, DeliveryStatus::not_started);
  EXPECT_EQ(runtime.snapshot().accepted, 0U);
  EXPECT_EQ(runtime.snapshot().in_flight, 0U);
  EXPECT_EQ(runtime.dispatch_ready(), 0U);
  EXPECT_EQ(observed, 0U);

  auto accepted =
      server->try_submit(runtime, {"display-message", "-p", "after-invalid"});
  ASSERT_TRUE(accepted.has_value()) << accepted.error().diagnostic;
  EXPECT_TRUE(std::move(*accepted).wait().has_value());
  ASSERT_TRUE(wait_until([&runtime] { return runtime.snapshot().completed == 1U; }));
  EXPECT_EQ(runtime.dispatch_ready(), 1U);
  runtime.request_stop();
  auto stopped = server->try_submit(runtime, {"display-message", "-p", "stopped"});
  ASSERT_FALSE(stopped.has_value());
  EXPECT_EQ(stopped.error().kind, libtmux::FailureKind::cancelled);
  EXPECT_EQ(stopped.error().delivery, DeliveryStatus::not_started);

  const auto snapshot = runtime.snapshot();
  EXPECT_EQ(snapshot.accepted, 1U);
  EXPECT_EQ(snapshot.refused, 3U);
  EXPECT_EQ(snapshot.in_flight, 0U);
  EXPECT_EQ(snapshot.pending_results, 0U);
  EXPECT_EQ(snapshot.pending_observers, 0U);
  EXPECT_EQ(snapshot.completed, 1U);
  EXPECT_EQ(observed, 1U);
  EXPECT_EQ(runtime.dispatch_ready(), 0U);
}

TEST(ServerContract, ZeroCapacityRefusesRuntimeStartup) {
  auto refused =
      libtmux::CommandRuntime::start(libtmux::CommandRuntimeConfig{.capacity = 0U});

  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, libtmux::FailureKind::validation);
  EXPECT_EQ(refused.error().delivery, DeliveryStatus::not_started);
}

TEST(ServerContract, RuntimeStartupFailuresAreValues) {
#if defined(_WIN32)
  GTEST_SKIP() << "the Windows start-failure seam runs in its platform lane";
#else
  libtmux::detail::fail_next_runtime_start_for_test();

  auto failed = libtmux::CommandRuntime::start();

  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().kind, libtmux::FailureKind::pipe);
  EXPECT_EQ(failed.error().delivery, DeliveryStatus::not_started);
#endif
}

TEST(ServerContract, PostAcceptanceSubscriptionFailuresAreIndeterminate) {
#if defined(_WIN32)
  GTEST_SKIP() << "the subscription-failure seam runs in its POSIX lane";
#else
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::optional<libtmux::CommandFailure> observed;
  auto server = Server::at_socket_path(
      fixture->socket_path().string(),
      [&observed](std::string_view, const libtmux::CommandFailure* failure) {
        if (failure != nullptr) {
          observed = *failure;
        }
      });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  auto runtime = start_runtime();
  libtmux::detail::fail_next_runtime_subscription_for_test();

  auto submitted = server->try_submit(runtime, {"display-message", "-p", "accepted"});

  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
  auto answer = std::move(*submitted).wait();
  ASSERT_FALSE(answer.has_value());
  EXPECT_EQ(answer.error().kind, libtmux::FailureKind::pipe);
  EXPECT_EQ(answer.error().delivery, DeliveryStatus::indeterminate);
  ASSERT_TRUE(wait_until([&runtime] { return runtime.snapshot().completed == 1U; }));
  EXPECT_EQ(runtime.dispatch_ready(), 1U);
  ASSERT_TRUE(observed.has_value());
  EXPECT_EQ(observed->kind, libtmux::FailureKind::pipe);
  EXPECT_EQ(observed->delivery, DeliveryStatus::indeterminate);
  EXPECT_TRUE(runtime.close().safe_to_unload);
#endif
}

TEST(ServerContract, PostAdmissionPublicationFailuresAreIndeterminate) {
#if defined(_WIN32)
  GTEST_SKIP() << "the runtime failure seam runs in its POSIX lane";
#else
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::optional<libtmux::CommandFailure> observed;
  auto server = Server::at_socket_path(
      fixture->socket_path().string(),
      [&observed](std::string_view, const libtmux::CommandFailure* failure) {
        if (failure != nullptr) {
          observed = *failure;
        }
      });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  auto runtime = start_runtime();
  libtmux::detail::fail_next_runtime_action_for_test(
      libtmux::detail::RuntimeFailurePoint::result_publication);

  auto submitted = server->try_submit(runtime, {"display-message", "-p", "accepted"});

  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
  const auto answer = std::move(*submitted).wait();
  ASSERT_FALSE(answer.has_value());
  EXPECT_EQ(answer.error().kind, libtmux::FailureKind::pipe);
  EXPECT_EQ(answer.error().delivery, DeliveryStatus::indeterminate);
  ASSERT_TRUE(wait_until([&runtime] { return runtime.snapshot().completed == 1U; }));
  EXPECT_EQ(runtime.dispatch_ready(), 1U);
  ASSERT_TRUE(observed.has_value());
  EXPECT_EQ(observed->kind, libtmux::FailureKind::pipe);
  EXPECT_EQ(observed->delivery, DeliveryStatus::indeterminate);
  EXPECT_EQ(observed->diagnostic, answer.error().diagnostic);
  const auto closed = runtime.close();
  ASSERT_TRUE(closed.failure.has_value());
  EXPECT_EQ(closed.failure->kind, libtmux::FailureKind::pipe);
  EXPECT_EQ(closed.failure->delivery, DeliveryStatus::indeterminate);
  EXPECT_EQ(closed.failure->diagnostic, answer.error().diagnostic);
#endif
}

TEST(ServerContract, LifecycleOnlyFailuresAfterAdmissionAreIndeterminate) {
#if defined(_WIN32)
  GTEST_SKIP() << "the runtime failure seam runs in its POSIX lane";
#else
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  for (const auto failure_point :
       {libtmux::detail::RuntimeFailurePoint::completion_queue,
        libtmux::detail::RuntimeFailurePoint::engine_shutdown}) {
    auto runtime = start_runtime();
    libtmux::detail::fail_next_runtime_action_for_test(failure_point);
    auto submitted = server.try_submit(runtime, {"display-message", "-p", "accepted"});
    ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
    EXPECT_TRUE(std::move(*submitted).wait().has_value());

    const auto closed = runtime.close();

    ASSERT_TRUE(closed.failure.has_value());
    EXPECT_EQ(closed.failure->kind, libtmux::FailureKind::pipe);
    EXPECT_EQ(closed.failure->delivery, DeliveryStatus::indeterminate);
    if (failure_point == libtmux::detail::RuntimeFailurePoint::engine_shutdown) {
      EXPECT_FALSE(closed.transports_stopped);
    }
  }
#endif
}

TEST(ServerContract, ObserverQueueFailuresRemainTerminalObligations) {
#if defined(_WIN32)
  GTEST_SKIP() << "the runtime failure seam runs in its POSIX lane";
#else
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::size_t observed = 0U;
  auto server = Server::at_socket_path(
      fixture->socket_path().string(),
      [&observed](std::string_view, const libtmux::CommandFailure*) { ++observed; });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  auto runtime = start_runtime();
  libtmux::detail::fail_next_runtime_action_for_test(
      libtmux::detail::RuntimeFailurePoint::observer_enqueue);
  auto submitted = server->try_submit(runtime, {"display-message", "-p", "accepted"});
  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;

  EXPECT_TRUE(std::move(*submitted).wait().has_value());
  const auto closed = runtime.close();
  const auto snapshot = runtime.snapshot();

  EXPECT_EQ(snapshot.accepted, 1U);
  EXPECT_EQ(snapshot.completed, 0U);
  EXPECT_EQ(snapshot.in_flight, 1U);
  EXPECT_EQ(snapshot.pending_results, 0U);
  EXPECT_EQ(snapshot.pending_observers, 1U);
  EXPECT_EQ(closed.pending_observers, 1U);
  EXPECT_FALSE(closed.safe_to_unload);
  ASSERT_TRUE(closed.failure.has_value());
  EXPECT_EQ(closed.failure->kind, libtmux::FailureKind::pipe);
  EXPECT_EQ(closed.failure->delivery, DeliveryStatus::indeterminate);
  EXPECT_EQ(runtime.dispatch_ready(), 0U);
  EXPECT_EQ(observed, 0U);
#endif
}

TEST(ServerContract, CompletedMeansResultAndObservationAreReady) {
#if defined(_WIN32)
  GTEST_SKIP() << "the runtime completion seam runs in its POSIX lane";
#else
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::size_t observed = 0U;
  auto server = Server::at_socket_path(
      fixture->socket_path().string(),
      [&observed](std::string_view, const libtmux::CommandFailure*) { ++observed; });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  std::binary_semaphore completion_reached{0};
  std::binary_semaphore release_completion{0};
  auto runtime = start_runtime();
  RuntimeCompletionGate completion_gate{release_completion,
                                        [&completion_reached, &release_completion] {
                                          completion_reached.release();
                                          release_completion.acquire();
                                        }};
  auto submitted = server->try_submit(runtime, {"display-message", "-p", "ordered"});
  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
  auto waiter =
      std::async(std::launch::async, [operation = *std::move(submitted)]() mutable {
        return std::move(operation).wait();
      });

  const bool reached = completion_reached.try_acquire_for(std::chrono::seconds{3});
  const auto completed = runtime.snapshot();
  const bool result_ready =
      waiter.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
  const auto dispatched = runtime.dispatch_ready();
  completion_gate.release();

  ASSERT_TRUE(reached);
  EXPECT_EQ(completed.completed, 1U);
  EXPECT_TRUE(result_ready);
  EXPECT_EQ(dispatched, 1U);
  EXPECT_EQ(observed, 1U);
  EXPECT_TRUE(waiter.get().has_value());
#endif
}

TEST(ServerContract, MovedFromRuntimesRefuseAdmission) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto source = start_runtime();
  auto destination = std::move(source);

  auto refused = server.try_submit(source, {"display-message", "-p", "not run"});

  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, libtmux::FailureKind::validation);
  EXPECT_EQ(refused.error().delivery, DeliveryStatus::not_started);
  EXPECT_EQ(source.snapshot().accepted, 0U);
  EXPECT_FALSE(source.snapshot().accepting);
  EXPECT_TRUE(destination.close().transports_stopped);
}

TEST(ServerContract, MissingServersAreRefusedBeforeAdmission) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  auto runtime = start_runtime();

  auto missing = Server::at_socket_path(
      (fixture->tmux_tmpdir() / "missing-runtime-socket").string());
  ASSERT_TRUE(missing.has_value()) << missing.error().diagnostic;
  auto missing_submit = missing->try_submit(runtime, {"list-sessions"});
  ASSERT_FALSE(missing_submit.has_value());
  EXPECT_EQ(missing_submit.error().kind, libtmux::FailureKind::missing);
  EXPECT_EQ(missing_submit.error().delivery, DeliveryStatus::not_started);

  const auto snapshot = runtime.snapshot();
  EXPECT_EQ(snapshot.accepted, 0U);
  EXPECT_EQ(snapshot.refused, 1U);
  EXPECT_EQ(snapshot.in_flight, 0U);
}

TEST(ServerContract, ADeadPinnedServerNeverFollowsItsReplacement) {
  auto original = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(original.has_value()) << original.error();
  const std::filesystem::path socket = original->socket_path();
  auto stale = Server::at_socket_path(socket.string());
  ASSERT_TRUE(stale.has_value()) << stale.error().diagnostic;
  ASSERT_TRUE(stale->kill().has_value());
  const auto stopped_by = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (original->is_alive() && std::chrono::steady_clock::now() < stopped_by) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  ASSERT_FALSE(original->is_alive());
  std::error_code removed;
  ASSERT_TRUE(std::filesystem::remove(socket, removed));
  ASSERT_FALSE(removed) << removed.message();
  auto replacement = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(replacement.has_value()) << replacement.error();
  std::error_code linked;
  std::filesystem::create_hard_link(replacement->socket_path(), socket, linked);
  ASSERT_FALSE(linked) << linked.message();
  auto runtime = start_runtime();

  auto submitted = stale->try_submit(
      runtime, {"rename-session", "-t", ":", "must-not-reach-replacement"});

  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
  const auto answer = std::move(*submitted).wait();
  ASSERT_FALSE(answer.has_value());
  const auto replacement_server = Server::at_socket_path(socket.string());
  ASSERT_TRUE(replacement_server.has_value()) << replacement_server.error().diagnostic;
  const auto sessions = replacement_server->sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  ASSERT_FALSE(sessions->empty());
  EXPECT_EQ(sessions->front().name(), replacement->session_name());
}

TEST(ServerContract, CancellationRelayExpiresAfterRawRetirement) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto runtime = start_runtime();
  auto submitted = server.try_submit(runtime, {"wait-for", "libtmux-runtime-cancelled"},
                                     std::chrono::seconds{30});
  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;

  runtime.request_stop();
  const auto closed = runtime.close();

  EXPECT_TRUE(closed.transports_stopped);
  EXPECT_FALSE(submitted->request_cancel());
  auto answer = std::move(*submitted).wait();
  ASSERT_FALSE(answer.has_value());
  EXPECT_EQ(answer.error().kind, libtmux::FailureKind::cancelled);
}

TEST(ServerContract, RequestStopDoesNotJoinRunningWork) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto runtime = start_runtime();
  auto submitted = server.try_submit(runtime, {"wait-for", "libtmux-runtime-stop"},
                                     std::chrono::seconds{30});
  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;

  const auto started = std::chrono::steady_clock::now();
  runtime.request_stop();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_LT(elapsed, std::chrono::milliseconds{100});
  const auto closed = runtime.close();
  EXPECT_TRUE(closed.transports_stopped);
  EXPECT_FALSE(std::move(*submitted).wait().has_value());
}

TEST(ServerContract, ObserverExceptionsReleaseCapacityBeforeLeavingDispatch) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::size_t calls = 0U;
  auto server = Server::at_socket_path(
      fixture->socket_path().string(),
      [&calls](std::string_view, const libtmux::CommandFailure*) {
        ++calls;
        if (calls == 1U) {
          throw std::runtime_error{"observer failed"};
        }
      });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  auto runtime = start_runtime({.capacity = 2U});
  auto first = server->try_submit(runtime, {"display-message", "-p", "first"});
  auto second = server->try_submit(runtime, {"display-message", "-p", "second"});
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_TRUE(std::move(*first).wait().has_value());
  EXPECT_TRUE(std::move(*second).wait().has_value());
  ASSERT_TRUE(wait_until([&runtime] { return runtime.snapshot().completed == 2U; }));

  EXPECT_THROW(static_cast<void>(runtime.dispatch_ready()), std::runtime_error);
  EXPECT_EQ(runtime.snapshot().in_flight, 1U);
  EXPECT_EQ(runtime.snapshot().pending_observers, 1U);
  EXPECT_EQ(runtime.dispatch_ready(), 1U);
  EXPECT_EQ(calls, 2U);
  EXPECT_EQ(runtime.snapshot().in_flight, 0U);
}

TEST(ServerContract, CloseDoesNotDispatchGlobalObservers) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::size_t observed = 0U;
  auto server = Server::at_socket_path(
      fixture->socket_path().string(),
      [&observed](std::string_view, const libtmux::CommandFailure*) { ++observed; });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  auto runtime = start_runtime();
  auto submitted = server->try_submit(runtime, {"display-message", "-p", "close"});
  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
  EXPECT_TRUE(std::move(*submitted).wait().has_value());

  const auto closed = runtime.close();

  EXPECT_EQ(observed, 0U);
  EXPECT_EQ(closed.pending_results, 0U);
  EXPECT_EQ(closed.pending_observers, 1U);
  EXPECT_TRUE(closed.transports_stopped);
  EXPECT_FALSE(closed.safe_to_unload);
  EXPECT_EQ(runtime.dispatch_ready(), 1U);
  EXPECT_EQ(observed, 1U);
  const auto repeated = runtime.close();
  EXPECT_EQ(repeated.pending_observers, closed.pending_observers);
  EXPECT_EQ(repeated.safe_to_unload, closed.safe_to_unload);
}

TEST(ServerContract, CloseIsNotSafeToUnloadWhileAnObserverCallbackRuns) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::binary_semaphore callback_started{0};
  std::binary_semaphore release_callback{0};
  auto server =
      Server::at_socket_path(fixture->socket_path().string(),
                             [&callback_started, &release_callback](
                                 std::string_view, const libtmux::CommandFailure*) {
                               callback_started.release();
                               release_callback.acquire();
                             });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  auto runtime = start_runtime();
  auto submitted =
      server->try_submit(runtime, {"display-message", "-p", "dispatching"});
  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
  ASSERT_TRUE(std::move(*submitted).wait().has_value());
  ASSERT_TRUE(wait_until([&runtime] { return runtime.snapshot().completed == 1U; }));

  auto dispatcher =
      std::async(std::launch::async, [&runtime] { return runtime.dispatch_ready(); });
  const bool started = callback_started.try_acquire_for(std::chrono::seconds{3});
  if (!started) {
    release_callback.release();
    EXPECT_TRUE(started);
    EXPECT_EQ(dispatcher.get(), 1U);
    return;
  }
  const auto closed = runtime.close();

  EXPECT_EQ(closed.pending_results, 0U);
  EXPECT_EQ(closed.pending_observers, 0U);
  EXPECT_TRUE(closed.transports_stopped);
  EXPECT_FALSE(closed.safe_to_unload);
  release_callback.release();
  EXPECT_EQ(dispatcher.get(), 1U);
}

TEST(ServerContract, CloseIsNotSafeToUnloadWhileDiscardDestroysObserver) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  auto teardown = std::make_shared<ObserverTeardownState>();
  auto observed = Server::at_socket_path(fixture->socket_path().string(),
                                         BlockingObserverTeardown{teardown});
  ASSERT_TRUE(observed.has_value()) << observed.error().diagnostic;
  const Server quiet = connect(*fixture);
  auto runtime = start_runtime();

  auto first = observed->try_submit(runtime, {"display-message", "-p", "discarded"});
  ASSERT_TRUE(first.has_value()) << first.error().diagnostic;
  ASSERT_TRUE(std::move(*first).wait().has_value());
  ASSERT_TRUE(wait_until([&runtime] { return runtime.snapshot().completed == 1U; }));

  auto second = quiet.try_submit(runtime, {"display-message", "-p", "fence"});
  ASSERT_TRUE(second.has_value()) << second.error().diagnostic;
  ASSERT_TRUE(std::move(*second).wait().has_value());
  ASSERT_TRUE(wait_until([&runtime] { return runtime.snapshot().completed == 2U; }));
  ASSERT_EQ(runtime.snapshot().pending_observers, 1U);

  std::future<std::size_t> discarder;
  ObserverTeardownRelease release_teardown{teardown};
  teardown->armed.store(true);
  discarder =
      std::async(std::launch::async, [&runtime] { return runtime.discard_ready(); });
  const bool started = teardown->started.try_acquire_for(std::chrono::seconds{3});
  const auto disposing = runtime.snapshot();
  std::optional<libtmux::CommandRuntimeShutdown> closed;
  if (started && disposing.pending_observers == 0U) {
    closed = runtime.close();
  }
  release_teardown.release();
  const auto discarded = discarder.get();

  ASSERT_TRUE(started);
  EXPECT_EQ(disposing.pending_results, 0U);
  EXPECT_EQ(disposing.pending_observers, 0U);
  ASSERT_TRUE(closed.has_value());
  EXPECT_TRUE(closed->transports_stopped);
  EXPECT_FALSE(closed->safe_to_unload);
  EXPECT_EQ(discarded, 1U);
}

TEST(ServerContract, ConcurrentCloseCallersReceiveOneTerminalReport) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::size_t observed = 0U;
  auto server = Server::at_socket_path(
      fixture->socket_path().string(),
      [&observed](std::string_view, const libtmux::CommandFailure*) { ++observed; });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  auto runtime = start_runtime();
  auto submitted = server->try_submit(runtime, {"wait-for", "libtmux-runtime-close"},
                                      std::chrono::seconds{30});
  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
  std::latch start{3};
  auto close_once = [&runtime, &start] {
    start.arrive_and_wait();
    return runtime.close();
  };
  auto first = std::async(std::launch::async, close_once);
  auto second = std::async(std::launch::async, close_once);
  start.arrive_and_wait();

  const auto left = first.get();
  const auto right = second.get();

  EXPECT_EQ(left.pending_results, right.pending_results);
  EXPECT_EQ(left.pending_observers, right.pending_observers);
  EXPECT_EQ(left.transports_stopped, right.transports_stopped);
  EXPECT_EQ(left.safe_to_unload, right.safe_to_unload);
  EXPECT_EQ(left.failure.has_value(), right.failure.has_value());
  EXPECT_TRUE(left.transports_stopped);
  EXPECT_FALSE(left.safe_to_unload);
  EXPECT_EQ(observed, 0U);
  EXPECT_FALSE(std::move(*submitted).wait().has_value());
  EXPECT_EQ(runtime.discard_ready(), 1U);
}

TEST(ServerContract, AFailedCloseClaimDoesNotStrandAnotherCaller) {
#if defined(_WIN32)
  GTEST_SKIP() << "the runtime failure seam runs in its POSIX lane";
#else
  auto runtime = start_runtime();
  libtmux::detail::fail_next_runtime_action_for_test(
      libtmux::detail::RuntimeFailurePoint::close);
  std::latch start{3};
  auto close_once = [&runtime, &start] {
    start.arrive_and_wait();
    try {
      return std::optional{runtime.close()};
    } catch (const std::runtime_error&) {
      return std::optional<libtmux::CommandRuntimeShutdown>{};
    }
  };
  auto first = std::async(std::launch::async, close_once);
  auto second = std::async(std::launch::async, close_once);
  start.arrive_and_wait();

  const auto left = first.get();
  const auto right = second.get();

  EXPECT_NE(left.has_value(), right.has_value());
  const auto& successful = left.has_value() ? *left : *right;
  EXPECT_TRUE(successful.transports_stopped);
  EXPECT_TRUE(successful.safe_to_unload);
#endif
}

TEST(ServerContract, CloseReportsSafeToUnloadAfterEveryLegFinishes) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto runtime = start_runtime();
  auto submitted = server.try_submit(runtime, {"display-message", "-p", "finished"});
  ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
  EXPECT_TRUE(std::move(*submitted).wait().has_value());

  const auto closed = runtime.close();

  EXPECT_EQ(closed.pending_results, 0U);
  EXPECT_EQ(closed.pending_observers, 0U);
  EXPECT_TRUE(closed.transports_stopped);
  EXPECT_TRUE(closed.safe_to_unload);
  EXPECT_FALSE(closed.failure.has_value());
}

TEST(ServerContract, AnOperationOutlivesItsRuntimeWithoutOwningRuntimeThreads) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  std::size_t observed = 0U;
  auto server = Server::at_socket_path(
      fixture->socket_path().string(),
      [&observed](std::string_view, const libtmux::CommandFailure*) { ++observed; });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  std::optional<libtmux::CommandOperation> operation;
  {
    auto runtime = start_runtime();
    auto submitted =
        server->try_submit(runtime, {"display-message", "-p", "outlive runtime"});
    ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
    operation = *std::move(submitted);
  }

  EXPECT_FALSE(operation->request_cancel());
  const auto answer = std::move(*operation).wait();
  EXPECT_TRUE(answer.has_value() ||
              answer.error().kind == libtmux::FailureKind::cancelled);
  EXPECT_EQ(observed, 0U);
}

TEST(ServerContract, ImmediateCompletionsCannotMissTheRuntimeWake) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto runtime = start_runtime({.capacity = 1U});

  for (int attempt = 0; attempt < 64; ++attempt) {
    auto submitted = server.try_submit(runtime, {"display-message", "-p", "immediate"});
    ASSERT_TRUE(submitted.has_value()) << submitted.error().diagnostic;
    auto answer = std::move(*submitted).wait();
    ASSERT_TRUE(answer.has_value()) << answer.error().diagnostic;
  }

  EXPECT_EQ(runtime.snapshot().completed, 64U);
  EXPECT_EQ(runtime.snapshot().in_flight, 0U);
}

TEST(ServerContract, OpeningAServerStartsNoAsynchronousThreads) {
#if !defined(__linux__)
  GTEST_SKIP() << "this thread-count probe uses procfs";
#else
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const std::size_t before = thread_count();

  auto server = Server::at_socket_path(fixture->socket_path().string());

  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;
  EXPECT_EQ(thread_count(), before);
#endif
}

TEST(ServerContract, AcceptedCommandsLaunchInFifoOrder) {
#if defined(_WIN32)
  GTEST_SKIP() << "the Windows worker launch order has its own platform lane";
#else
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  std::binary_semaphore launch_gate{0};
  std::once_flag first_launch;
  std::mutex launch_mutex;
  std::vector<std::string> launched;
  libtmux::detail::set_runtime_launch_observer_for_test(
      [&launch_gate, &first_launch, &launch_mutex,
       &launched](const libtmux::detail::ProcessRequest& request) {
        std::call_once(first_launch, [&launch_gate] { launch_gate.acquire(); });
        std::lock_guard lock{launch_mutex};
        launched.push_back(request.arguments.back().value);
      });
  auto runtime = start_runtime({.capacity = 8U});
  libtmux::detail::set_runtime_launch_observer_for_test({});
  std::vector<libtmux::CommandOperation> submitted;
  std::optional<libtmux::CommandFailure> submission_failure;
  for (int index = 0; index < 8; ++index) {
    auto operation = server.try_submit(
        runtime, {"display-message", "-p", "fifo-" + std::to_string(index)});
    if (!operation.has_value()) {
      submission_failure = std::move(operation.error());
      break;
    }
    submitted.push_back(*std::move(operation));
  }
  launch_gate.release();
  ASSERT_FALSE(submission_failure.has_value()) << submission_failure->diagnostic;
  for (auto& operation : submitted) {
    EXPECT_TRUE(std::move(operation).wait().has_value());
  }

  EXPECT_EQ(launched,
            (std::vector<std::string>{"fifo-0", "fifo-1", "fifo-2", "fifo-3", "fifo-4",
                                      "fifo-5", "fifo-6", "fifo-7"}));
#endif
}

TEST(ServerContract, SynchronousObserversRunOnTheCallingThread) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto caller = std::this_thread::get_id();
  std::optional<std::thread::id> observer_thread;
  auto server = Server::at_socket_path(
      fixture->socket_path().string(),
      [&observer_thread](std::string_view, const libtmux::CommandFailure*) {
        observer_thread = std::this_thread::get_id();
      });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;

  ASSERT_TRUE(server->run({"display-message", "-p", "sync"}).has_value());

  ASSERT_TRUE(observer_thread.has_value());
  EXPECT_EQ(*observer_thread, caller);
}

TEST(ServerContract, ConcurrentCallersEachGetTheirOwnAnswer) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  constexpr int callers = 12;
  std::vector<std::string> answers(callers);
  std::vector<std::thread> asking;
  asking.reserve(callers);
  std::latch ready{callers};
  for (int index = 0; index < callers; ++index) {
    asking.emplace_back([&, index] {
      const std::string mine = "caller-" + std::to_string(index);
      ready.arrive_and_wait();
      const auto printed = server.run({"display-message", "-p", mine});
      if (printed.has_value()) {
        answers[static_cast<std::size_t>(index)] = *printed;
      }
    });
  }
  for (auto& thread : asking) {
    thread.join();
  }

  for (int index = 0; index < callers; ++index) {
    EXPECT_EQ(answers[static_cast<std::size_t>(index)],
              "caller-" + std::to_string(index) + "\n");
  }
}

TEST(ServerContract, ArgumentsReachTmuxWithoutAShell) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // A shell would expand these; exec does not.
  for (const std::string literal :
       {"$(echo pwned)", "a; echo pwned", "`id`", "a && echo pwned", "*"}) {
    const auto printed = server.run({"display-message", "-p", literal});
    ASSERT_TRUE(printed.has_value()) << printed.error().diagnostic;
    EXPECT_EQ(*printed, literal + "\n") << "argument was altered: " << literal;
  }
}

TEST(ServerContract, StdoutBytesSurviveUnchanged) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const std::string wide = "\xc3\xa4\xe2\x9d\xaf";
  const auto printed = server.run({"display-message", "-p", wide});
  ASSERT_TRUE(printed.has_value()) << printed.error().diagnostic;
  EXPECT_EQ(*printed, wide + "\n");
}

TEST(ServerContract, ANonzeroExitIsAReplyNotACrash) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto refused = server.run({"kill-session", "-t", "absent"});
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().delivery, DeliveryStatus::replied);
  EXPECT_GT(refused.error().exit_code, 0);
  EXPECT_FALSE(refused.error().diagnostic.empty());
}

TEST(ServerContract, AnUnknownSubcommandIsRefusedNotDispatchedBlindly) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto refused = server.run({"no-such-tmux-command"});
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().delivery, DeliveryStatus::replied);
  EXPECT_NE(refused.error().diagnostic.find("no-such-tmux-command"), std::string::npos);
}

TEST(ServerContract, AnUnreachableSocketFailsWithoutHanging) {
  const auto server = Server::at_socket_path("/nonexistent/libtmux/socket");
  ASSERT_TRUE(server.has_value());
  const auto refused = server->run({"list-sessions"});
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, libtmux::FailureKind::missing);
  EXPECT_EQ(refused.error().delivery, DeliveryStatus::not_started);
}

TEST(ServerContract, RepeatedRunsDoNotLeakDescriptors) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto count_open = libtmux::test::open_descriptor_count;
  ASSERT_TRUE(server.run({"display-message", "-p", "warm"}).has_value());
  const std::size_t before = count_open();
  for (int index = 0; index < 20; ++index) {
    ASSERT_TRUE(server.run({"display-message", "-p", "x"}).has_value());
  }
  EXPECT_EQ(count_open(), before);
}

TEST(ServerContract, ATimeoutIsItsOwnFailureNotARefusal) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // wait-for blocks until someone signals the channel, and nobody does.
  // attach-session is not usable here: without a terminal it refuses at once.
  const auto timed_out = server.run({"wait-for", "libtmux-never-signalled"},
                                    std::chrono::milliseconds{300});
  ASSERT_FALSE(timed_out.has_value());
  EXPECT_EQ(timed_out.error().kind, libtmux::FailureKind::timeout);
  // The child started, but the transport cannot prove whether the command
  // reached tmux before it was terminated.
  EXPECT_EQ(timed_out.error().delivery, DeliveryStatus::indeterminate);
}

TEST(ServerContract, ARefusalIsDistinguishableFromNeverRunning) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto refused = server.run({"kill-session", "-t", "absent"});
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, libtmux::FailureKind::refused);

  const auto empty = server.run_batch(libtmux::CommandBatch{});
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error().kind, libtmux::FailureKind::validation);
  EXPECT_EQ(empty.error().delivery, libtmux::DeliveryStatus::not_started);
}

} // namespace

TEST(ServerContract, TheVersionIsReadableWithoutAServer) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto running = server.tmux_version();
  ASSERT_TRUE(running.has_value()) << running.error().diagnostic;
  EXPECT_TRUE(libtmux::is_supported(*running));

  const auto absent =
      Server::at_socket_path((fixture->tmux_tmpdir() / "absent-version").string());
  ASSERT_TRUE(absent.has_value()) << absent.error().diagnostic;
  const auto without_server = absent->tmux_version();
  ASSERT_TRUE(without_server.has_value()) << without_server.error().diagnostic;
  EXPECT_EQ(*without_server, *running);

  // `tmux -V` does not connect, so the answer survives the server's death.
  ASSERT_TRUE(server.kill().has_value());
  const auto after = server.tmux_version();
  ASSERT_TRUE(after.has_value()) << after.error().diagnostic;
  EXPECT_EQ(*after, *running);
}

TEST(ServerContract, SubprocessVersionHonoursTheServersOutputBound) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const libtmux::ExecutionPolicy bounded{.output_limit = 1U};
  auto server = Server::at_socket_path(fixture->socket_path().string(), {}, bounded);
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;

  const auto version = server->tmux_version();

  ASSERT_FALSE(version.has_value());
  EXPECT_EQ(version.error().kind, libtmux::FailureKind::truncated);
  EXPECT_EQ(version.error().delivery, DeliveryStatus::replied);
}

TEST(ServerContract, LivenessIsAskedAndAnsweredWithoutThrowing) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  EXPECT_TRUE(server.is_alive());
  EXPECT_TRUE(server.check_alive().has_value());

  ASSERT_TRUE(server.kill().has_value());

  EXPECT_FALSE(server.is_alive());
  const auto dead = server.check_alive();
  ASSERT_FALSE(dead.has_value());
  // The reason is kept, which is the difference between the two questions.
  EXPECT_FALSE(dead.error().diagnostic.empty());
}

TEST(ServerContract, ASocketNobodyIsServingIsNotAlive) {
  // Inside the fixture's own directory, so the path is unique to this run and
  // is removed with it.
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const auto absent = fixture->socket_path().string() + ".absent";

  const auto server = Server::at_socket_path(absent);
  ASSERT_TRUE(server.has_value());
  EXPECT_FALSE(server->is_alive());
  EXPECT_FALSE(std::filesystem::exists(absent));
}

TEST(ServerContract, TheSeparatorSurvivesANonUnicodeLocale) {
  // tmux decides whether the terminal is UTF-8 from the environment, and a
  // tmux that decides it is not replaces the multi-byte field separator with
  // an underscore — at which point no row splits and every listing on the
  // server fails. The library passes -u so the answer does not depend on the
  // locale its caller happens to be running under.
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto argv = server.run({"display-message", "-p", "#{command}"});
  ASSERT_TRUE(argv.has_value()) << argv.error().diagnostic;

  const auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  EXPECT_EQ(sessions->size(), 1U);

  // The separator itself, round-tripped through tmux, comes back whole.
  const auto echoed =
      server.run({"display-message", "-p", std::string{libtmux::kFormatSeparator}});
  ASSERT_TRUE(echoed.has_value()) << echoed.error().diagnostic;
  EXPECT_EQ(*echoed, std::string{libtmux::kFormatSeparator} + "\n");
}

TEST(ServerContract, AnArgumentEndingInASeparatorIsNotACommandBoundary) {
  // tmux reads a trailing `;` on an argument as a command separator. Unescaped,
  // `set-option @v 'a;'` stores `a`, and in a batch whatever followed the
  // truncated argument becomes a command of its own — the argv for a two-member
  // batch ending in `kill-server` killed the server.
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  for (const std::string value : {"a;", "trailing;", "two;;", "back\\;"}) {
    ASSERT_TRUE(server.run({"set-option", "-g", "@probe", value}).has_value());
    const auto stored = server.run({"show-options", "-gv", "@probe"});
    ASSERT_TRUE(stored.has_value()) << stored.error().diagnostic;
    EXPECT_EQ(*stored, value + "\n") << "for " << value;
  }

  libtmux::CommandBatch batch;
  ASSERT_TRUE(batch.add({"set-option", "-g", "@first", "value;"}));
  ASSERT_TRUE(batch.add({"set-option", "-g", "@second", "kept"}));
  ASSERT_TRUE(server.run_batch(batch).has_value());
  EXPECT_TRUE(server.is_alive()) << "a batch member ran as its own command";
  const auto second = server.run({"show-options", "-gv", "@second"});
  ASSERT_TRUE(second.has_value()) << second.error().diagnostic;
  EXPECT_EQ(*second, "kept\n");
}

TEST(ServerContract, DataThatLooksLikeAFlagIsStillData) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  const libtmux::Session& session = sessions->at(0);

  ASSERT_TRUE(session.rename("-dashed").has_value());
  const auto renamed = session.refresh();
  ASSERT_TRUE(renamed.has_value()) << renamed.error().diagnostic;
  EXPECT_EQ(renamed->name(), "-dashed");

  const auto pane = session.active_pane();
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  EXPECT_TRUE(pane->send_text("-not-a-flag").has_value());
}

TEST(ServerContract, AnObserverSeesEveryCommandAndWhyOneFailed) {
  // Without this there is no way to find out what the library ran: a caller
  // debugging a tmux interaction has the failures and nothing else, and
  // nothing at all when things work.
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();

  std::vector<std::string> seen;
  std::vector<std::string> failed;
  auto server =
      Server::at_socket_path(fixture->socket_path().string(),
                             [&seen, &failed](std::string_view command,
                                              const libtmux::CommandFailure* failure) {
                               seen.emplace_back(command);
                               if (failure != nullptr) {
                                 failed.emplace_back(command);
                               }
                             });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;

  ASSERT_TRUE(server->sessions().has_value());
  ASSERT_FALSE(server->run({"kill-session", "-t", "absent"}).has_value());

  ASSERT_EQ(seen.size(), 2U);
  EXPECT_TRUE(seen.at(0).starts_with("list-sessions")) << seen.at(0);
  EXPECT_EQ(failed.size(), 1U);
  EXPECT_EQ(failed.at(0), "kill-session -t absent");
}

TEST(ServerContract, AnObserverNeverSeesAnEnvironmentValue) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();

  std::vector<std::string> seen;
  std::vector<std::string> diagnostics;
  auto server = Server::at_socket_path(
      fixture->socket_path().string(),
      [&seen, &diagnostics](std::string_view command,
                            const libtmux::CommandFailure* failure) {
        seen.emplace_back(command);
        if (failure != nullptr) {
          diagnostics.push_back(failure->diagnostic);
        }
      });
  ASSERT_TRUE(server.has_value()) << server.error().diagnostic;

  constexpr std::string_view secret = "known-only-to-tmux-$[] with spaces;";
  libtmux::CommandRequest raw{"display-message", "-p"};
  raw.push_back(libtmux::CommandArgument::sensitive(std::string{secret}));
  const auto printed = server->run(raw);
  ASSERT_TRUE(printed.has_value()) << printed.error().diagnostic;
  EXPECT_EQ(*printed, std::string{secret} + "\n");

  libtmux::NewSessionOptions options;
  options.name = "redacted";
  options.environment = {{"LIBTMUX_SECRET", std::string{secret}}};
  const auto session = server->new_session(std::move(options));
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;

  const auto set_option = server->set_global_option("@secret", secret);
  ASSERT_TRUE(set_option.has_value()) << set_option.error().diagnostic;

  libtmux::CommandBatch batch;
  ASSERT_TRUE(batch.add({"display-message", "-p", "public-prefix"}));
  ASSERT_TRUE(batch.add(raw));
  const auto batched = server->run_batch(batch);
  ASSERT_TRUE(batched.has_value()) << batched.error().diagnostic;
  EXPECT_EQ(*batched, "public-prefix\n" + std::string{secret} + "\n");
  const std::string shell_secret{"shell-secret with spaces"};
  const std::string shell_command =
      "tmux set-option -g @shell-secret '" + shell_secret + "'";
  ASSERT_TRUE(server->run_shell(shell_command).has_value());
  const auto shell_value = server->run({"show-options", "-gv", "@shell-secret"});
  ASSERT_TRUE(shell_value.has_value()) << shell_value.error().diagnostic;
  EXPECT_EQ(*shell_value, shell_secret + "\n");

  const std::string hook_command = "display-message '" + shell_secret + "'";
  ASSERT_TRUE(server->set_global_hook("alert-bell", hook_command).has_value());
  const auto pane = session->active_pane();
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  ASSERT_TRUE(pane->pipe_to("cat >/dev/null # " + shell_secret).has_value());
  ASSERT_TRUE(pane->stop_piping().has_value());

  libtmux::CommandRequest failing{"kill-session", "-t"};
  failing.push_back(libtmux::CommandArgument::sensitive(std::string{secret}));
  const auto refused = server->run(failing);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().diagnostic.find(secret), std::string::npos)
      << refused.error().diagnostic;
  libtmux::CommandBatch failing_batch;
  ASSERT_TRUE(failing_batch.add({"display-message", "-p", "safe-before-failure"}));
  ASSERT_TRUE(failing_batch.add(failing));
  const auto refused_batch = server->run_batch(failing_batch);
  ASSERT_FALSE(refused_batch.has_value());
  EXPECT_EQ(refused_batch.error().diagnostic.find(secret), std::string::npos)
      << refused_batch.error().diagnostic;
  auto runtime = start_runtime();
  auto async_refused = server->try_submit(runtime, failing);
  ASSERT_TRUE(async_refused.has_value()) << async_refused.error().diagnostic;
  auto async_answer = std::move(*async_refused).wait();
  ASSERT_FALSE(async_answer.has_value());
  EXPECT_EQ(async_answer.error().diagnostic.find(secret), std::string::npos)
      << async_answer.error().diagnostic;
  ASSERT_TRUE(wait_until([&runtime] { return runtime.snapshot().completed == 1U; }));
  EXPECT_EQ(runtime.dispatch_ready(), 1U);
  const auto value = server->run(
      {"show-environment", "-t", std::string{session->id()}, "LIBTMUX_SECRET"});
  ASSERT_TRUE(value.has_value()) << value.error().diagnostic;
  EXPECT_EQ(*value, "LIBTMUX_SECRET=" + std::string{secret} + "\n");

  bool replaced = false;
  for (const std::string& command : seen) {
    EXPECT_EQ(command.find(secret), std::string::npos) << command;
    EXPECT_EQ(command.find(shell_secret), std::string::npos) << command;
    replaced = replaced || command.find("[REDACTED]") != std::string::npos;
  }
  EXPECT_TRUE(replaced);
  for (const std::string& diagnostic : diagnostics) {
    EXPECT_EQ(diagnostic.find(secret), std::string::npos) << diagnostic;
    EXPECT_EQ(diagnostic.find(shell_secret), std::string::npos) << diagnostic;
  }
}

TEST(ServerContract, ARefusalNamesTheCommandAndCarriesNoStrayNewline) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto refused = server.run({"kill-session", "-t", "absent"});
  ASSERT_FALSE(refused.has_value());
  const std::string& diagnostic = refused.error().diagnostic;

  // What tmux said, and which command it said it about.
  EXPECT_NE(diagnostic.find("absent"), std::string::npos);
  EXPECT_NE(diagnostic.find("kill-session -t absent"), std::string::npos);
  // Both consumers put this straight into a message field.
  EXPECT_FALSE(diagnostic.ends_with("\n")) << diagnostic;
}

// Typed methods passed no deadline at all, so `window.rename(...)` against a
// tmux that never answers held the calling thread for the life of the process.
// "tmux is normally fast" is not a liveness guarantee; the policy is the floor
// under every call that did not name one of its own.
TEST(ServerContract, ATypedCallInheritsTheServersDeadline) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();

  const libtmux::ExecutionPolicy impatient{.timeout = std::chrono::milliseconds{150}};
  auto server = Server::at_socket_path(fixture->socket_path().string(), {}, impatient);
  ASSERT_TRUE(server.has_value());

  // `run-shell` without `-b` makes tmux wait for the command, so this is a
  // typed call that genuinely does not answer in time rather than one raced
  // against the clock.
  const auto started = std::chrono::steady_clock::now();
  const auto slow = server->run_shell("sleep 5");
  const auto elapsed = std::chrono::steady_clock::now() - started;

  ASSERT_FALSE(slow.has_value()) << "a 150ms deadline should not have been met";
  EXPECT_EQ(slow.error().kind, libtmux::FailureKind::timeout);
  EXPECT_EQ(slow.error().delivery, DeliveryStatus::indeterminate);
  EXPECT_LT(elapsed, std::chrono::seconds{3})
      << "the call outlived the deadline it was given";

  // The same server with a workable deadline answers, so the refusal above was
  // the policy and not a broken fixture.
  const libtmux::ExecutionPolicy patient{.timeout = std::chrono::seconds{20}};
  auto unhurried = Server::at_socket_path(fixture->socket_path().string(), {}, patient);
  ASSERT_TRUE(unhurried.has_value());
  const auto again = unhurried->sessions();
  ASSERT_TRUE(again.has_value()) << again.error().diagnostic;
  EXPECT_FALSE(again->empty());
}

// Waiting is the whole request, so `wait_for` is the one call the floor must
// not cut short. It reaches the transport directly for that reason.
TEST(ServerContract, WaitingOutlivesTheServersDeadline) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();

  const libtmux::ExecutionPolicy impatient{.timeout = std::chrono::milliseconds{1}};
  auto server = Server::at_socket_path(fixture->socket_path().string(), {}, impatient);
  ASSERT_TRUE(server.has_value());

  const auto started = std::chrono::steady_clock::now();
  const auto waited =
      server->wait_for("nobody-signals-this", std::chrono::milliseconds{300});
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_FALSE(waited.has_value());
  // The caller's 300ms, not the policy's 1ms.
  EXPECT_GE(elapsed, std::chrono::milliseconds{250})
      << "the policy cut short a wait the caller asked for";
}
