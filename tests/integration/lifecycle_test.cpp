#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <iomanip>
#include <libtmux/libtmux.hpp>
#include <libtmux/testing/scoped_server.hpp>
#include <optional>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
using libtmux::Server;
using libtmux::test::ScopedTmuxServer;
using libtmux::test::ScopedTmuxServerOptions;

class Lifecycle : public testing::Test {
protected:
  void SetUp() override {
    auto started = ScopedTmuxServer::start(
        {.tmux_binary = LIBTMUX_TEST_TMUX_PATH,
         .socket_namespace = libtmux::test::SocketNamespace::consumer("life"),
         .teardown_report = report});
    ASSERT_TRUE(started) << started.error();
    fixture.emplace(std::move(*started));
    std::cerr << "owned fixture " << fixture->tmux_tmpdir() << " pid "
              << fixture->server_pid() << "\n";
    auto connected = Server::at_socket_path(fixture->socket_path(), {}, policy());
    ASSERT_TRUE(connected) << connected.error().diagnostic;
    server.emplace(*connected);
  }
  void TearDown() override {
    const auto root = fixture ? fixture->tmux_tmpdir() : std::filesystem::path{};
    for (const auto& path : extra_endpoints) {
      auto extra = Server::at_socket_path(path, {}, policy());
      if (!extra)
        continue;
      auto pid = extra->run({"display-message", "-p", "#{pid}"}, 200ms);
      if (!pid)
        continue;
      const int process = std::stoi(*pid);
      int pidfd = -1;
#if defined(__linux__)
      pidfd = static_cast<int>(::syscall(SYS_pidfd_open, process, 0));
#endif
      if (pidfd >= 0) {
        static_cast<void>(extra->kill());
        pollfd watched{pidfd, POLLIN, 0};
        const bool exited =
            ::poll(&watched, 1, 2000) == 1 && (watched.revents & POLLIN) != 0;
        EXPECT_TRUE(exited);
        if (!exited) {
          std::error_code error;
          std::filesystem::rename(root, root.string() + ".retained", error);
          std::cerr << "unverified extra daemon; retained root " << root << "\n";
        }
        ::close(pidfd);
      } else {
        static_cast<void>(extra->kill());
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (::kill(process, 0) == 0 && std::chrono::steady_clock::now() < deadline)
          std::this_thread::sleep_for(5ms);
        const bool exited = ::kill(process, 0) != 0 && errno == ESRCH;
        EXPECT_TRUE(exited);
        if (!exited) {
          std::error_code error;
          std::filesystem::rename(root, root.string() + ".retained", error);
        }
      }
    }
    server.reset();
    fixture.reset();
    if (!root.empty()) {
      EXPECT_FALSE(std::filesystem::exists(root));
      ASSERT_FALSE(report->messages.empty());
      EXPECT_EQ(report->messages.back(), "server teardown complete");
    }
  }
  libtmux::ExecutionPolicy policy() {
    return {.timeout = 2s,
            .tmux_binary = LIBTMUX_TEST_TMUX_PATH,
            .child_environment = fixture->child_environment()};
  }
  bool wait_for_fixture_exit() {
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (fixture->is_alive() && std::chrono::steady_clock::now() < end)
      std::this_thread::sleep_for(5ms);
    return !fixture->is_alive();
  }
  libtmux::Session session() {
    auto value = server->session(std::string{fixture->session_name()});
    if (!value)
      throw std::runtime_error{value.error().diagnostic};
    return *value;
  }
  // Wrapper failure happens after the real tmux client prints its receipt.
  Server fault(std::string mode) {
    const auto script = fixture->tmux_tmpdir() / "client.py";
    const auto armed = fixture->tmux_tmpdir() / "armed";
    std::ofstream{armed} << mode;
    std::ofstream file{script};
    file << "#!/usr/bin/python3\nimport os,sys,subprocess,time\n"
         << "armed=" << std::quoted(armed.string()) << "\n"
         << "args=[" << std::quoted(std::string{LIBTMUX_TEST_TMUX_PATH})
         << "]+sys.argv[1:]\n"
         << "if 'if-shell' in args and os.path.exists(armed):\n"
         << "    mode=open(armed).read();os.unlink(armed)\n"
         << "    result=subprocess.run(args,capture_output=True)\n"
         << "    if "
            "mode!='lost':sys.stdout.buffer.write(result.stdout);sys.stdout.flush()\n"
         << "    sys.stderr.buffer.write(result.stderr);sys.stderr.flush()\n"
         << "    if mode=='timeout':time.sleep(1)\n"
         << "    sys.stderr.write('injected failure after receipt\\n');sys.exit(77)\n"
         << "os.execv(args[0],args)\n";
    file.close();
    std::filesystem::permissions(script, std::filesystem::perms::owner_all);
    auto configured = policy();
    configured.tmux_binary = script;
    auto connected = Server::at_socket_path(fixture->socket_path(), {}, configured);
    EXPECT_TRUE(connected);
    return *connected;
  }
  std::vector<std::filesystem::path> extra_endpoints;
  std::optional<ScopedTmuxServer> fixture;
  std::optional<Server> server;
  std::shared_ptr<libtmux::test::TeardownReport> report =
      std::make_shared<libtmux::test::TeardownReport>();
};

TEST_F(Lifecycle, OwnedSessionRetainsIdAcrossRenameAndUnwinds) {
  std::string id;
  std::optional<libtmux::CleanupHandle> cleanup;
  try {
    auto owned = libtmux::own_session(*server, {.name = "owned"});
    ASSERT_TRUE(owned) << owned.error().primary.diagnostic;
    id = (*owned)->id().value();
    cleanup = owned->cleanup();
    ASSERT_TRUE((*owned)->rename("renamed"));
    throw std::runtime_error{"body"};
  } catch (const std::runtime_error&) {
  }
  ASSERT_TRUE(cleanup);
  EXPECT_TRUE(cleanup->report().complete);
  EXPECT_FALSE(server->session(id));
  EXPECT_TRUE(server->session(std::string{fixture->session_name()}));
  EXPECT_TRUE(cleanup->close());
  EXPECT_EQ(cleanup->report().attempts, 1U);
}

TEST_F(Lifecycle, BorrowedObjectsDoNotDestroyAndEachEntityCanBeAdopted) {
  std::string session_id;
  {
    auto borrowed = server->new_session("borrowed");
    ASSERT_TRUE(borrowed);
    session_id = borrowed->id().value();
  }
  auto existing = server->session(session_id);
  ASSERT_TRUE(existing);
  {
    auto owner = libtmux::adopt(*existing);
    ASSERT_TRUE(owner) << owner.error().primary.diagnostic;
    auto window = existing->new_window("child");
    ASSERT_TRUE(window);
    auto window_owner = libtmux::adopt(*window);
    ASSERT_TRUE(window_owner);
    auto pane = window->split();
    ASSERT_TRUE(pane);
    const auto pane_id = std::string{pane->id().value()};
    {
      auto pane_owner = libtmux::adopt(*pane);
      ASSERT_TRUE(pane_owner);
    }
    EXPECT_FALSE(server->pane(pane_id));
    EXPECT_TRUE(server->window(window->id().value()));
  }
  EXPECT_FALSE(server->session(session_id));
}

TEST_F(Lifecycle, PublicWindowAndPaneOwnersCleanUp) {
  const auto base = session();
  std::string window_id;
  {
    auto window = libtmux::own_window(base, {.name = "worker"});
    ASSERT_TRUE(window) << window.error().primary.diagnostic;
    window_id = (*window)->id().value();
    auto first = (*window)->active_pane();
    ASSERT_TRUE(first);
    std::string pane_id;
    {
      auto pane = libtmux::own_pane(*first, {.percentage = 30});
      ASSERT_TRUE(pane) << pane.error().primary.diagnostic;
      pane_id = (*pane)->id().value();
    }
    EXPECT_FALSE(server->pane(pane_id));
    EXPECT_TRUE(server->pane(first->id().value()));
  }
  EXPECT_FALSE(server->window(window_id));
}

TEST_F(Lifecycle, CleanupFailureRemainsRetryableAndBothErrorsSurvive) {
  auto owner = libtmux::own_session(*server, {.name = "retry"});
  ASSERT_TRUE(owner);
  const auto token = owner->cleanup().receipt().generation;
  ASSERT_TRUE(
      server->set_server_option("@libtmux_owner_generation", std::string(32, 'f')));
  auto result = libtmux::with_owned(
      *owner, [](const auto&) -> libtmux::expected<void, libtmux::CommandFailure> {
        return libtmux::unexpected(
            libtmux::CommandFailure{.diagnostic = "body refused"});
      });
  EXPECT_FALSE(result.ok());
  ASSERT_TRUE(result.body_failure);
  EXPECT_EQ(result.body_failure->diagnostic, "body refused");
  ASSERT_TRUE(result.cleanup.failure);
  EXPECT_FALSE(result.cleanup.complete);
  EXPECT_TRUE(server->session((*owner)->id().value()));
  ASSERT_TRUE(server->set_server_option("@libtmux_owner_generation", token));
  EXPECT_TRUE(owner->close());
  EXPECT_TRUE(owner->cleanup().report().complete);
}

TEST_F(Lifecycle, ExceptionBodyAndCleanupErrorRemainSeparate) {
  auto owner = libtmux::own_window(session(), {.name = "retry"});
  ASSERT_TRUE(owner);
  const auto token = owner->cleanup().receipt().generation;
  ASSERT_TRUE(
      server->set_server_option("@libtmux_owner_generation", std::string(32, 'f')));
  const auto result = libtmux::with_owned(
      *owner, [](const auto&) -> libtmux::expected<void, libtmux::CommandFailure> {
        throw std::runtime_error{"body threw"};
      });
  EXPECT_TRUE(result.body_exception);
  EXPECT_TRUE(result.cleanup.failure);
  EXPECT_THROW(std::rethrow_exception(result.body_exception), std::runtime_error);
  ASSERT_TRUE(server->set_server_option("@libtmux_owner_generation", token));
  EXPECT_TRUE(owner->close());
}

TEST_F(Lifecycle, AdoptionPreservesValidTokenAndRefusesEmptyOrMalformed) {
  ASSERT_TRUE(server->set_server_option("@libtmux_owner_generation",
                                        "ABCDEF0123456789ABCDEF0123456789"));
  auto created = server->new_session("token");
  ASSERT_TRUE(created);
  {
    auto owner = libtmux::adopt(*created);
    ASSERT_TRUE(owner);
    EXPECT_EQ(owner->cleanup().receipt().generation,
              "ABCDEF0123456789ABCDEF0123456789");
  }
  for (const std::string token : {"", "nothex", "1111111111111111111111111111111g"}) {
    ASSERT_TRUE(server->set_server_option("@libtmux_owner_generation", token));
    auto result = libtmux::adopt(session());
    EXPECT_FALSE(result);
    auto read = server->run({"show-options", "-sv", "@libtmux_owner_generation"});
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, token + "\n");
  }
}

TEST_F(Lifecycle, CLocaleAndPunctuationDoNotBreakCreationReceipts) {
  auto environment = fixture->child_environment();
  libtmux::test::set_environment(environment, "LC_ALL", "C");
  auto configured = policy();
  configured.child_environment = environment;
  auto selected = Server::at_socket_path(fixture->socket_path(), {}, configured);
  ASSERT_TRUE(selected);
  auto owned = libtmux::own_session(
      *selected, {.name = "quoted #'$; name", .shell_command = "sleep 30"});
  ASSERT_TRUE(owned) << owned.error().primary.diagnostic;
  EXPECT_EQ((*owned)->name(), "quoted #'$; name");
  auto window = libtmux::own_window(
      owned->get(), {.name = "space $;#'\" name", .shell_command = "sleep 30"});
  ASSERT_TRUE(window) << window.error().primary.diagnostic;
  EXPECT_EQ((*window)->name(), "space $;#'\" name");
}

class ReceiptFailure : public Lifecycle,
                       public testing::WithParamInterface<std::string> {};
TEST_P(ReceiptFailure, RollsBackAfterRealCreationPrintedAReceipt) {
  auto wrapped = fault(GetParam());
  const auto before = server->sessions();
  ASSERT_TRUE(before);
  auto owned = libtmux::own_session(wrapped, {.name = "receipt"}, {.timeout = 150ms});
  ASSERT_FALSE(owned);
  ASSERT_TRUE(owned.error().receipt) << owned.error().primary.diagnostic;
  EXPECT_FALSE(owned.error().rollback)
      << (owned.error().rollback ? owned.error().rollback->diagnostic : "");
  EXPECT_FALSE(server->session(owned.error().receipt->id));
  auto after = server->sessions();
  ASSERT_TRUE(after);
  EXPECT_EQ(after->size(), before->size());
  EXPECT_EQ(owned.error().primary.kind, GetParam() == "timeout"
                                            ? libtmux::FailureKind::timeout
                                            : libtmux::FailureKind::refused);
}
INSTANTIATE_TEST_SUITE_P(AfterReceipt, ReceiptFailure,
                         testing::Values("refused", "timeout"));

TEST_F(Lifecycle, WindowAndPaneReceiptsSurviveFailure) {
  auto wrapped = fault("refused");
  auto base = wrapped.session(session().id().value());
  ASSERT_TRUE(base);
  auto window = libtmux::own_window(*base, {.name = "receipt"});
  ASSERT_FALSE(window);
  ASSERT_TRUE(window.error().receipt);
  EXPECT_FALSE(window.error().rollback);
  EXPECT_FALSE(server->window(window.error().receipt->id));
  wrapped = fault("refused");
  base = wrapped.session(session().id().value());
  ASSERT_TRUE(base);
  auto target = base->active_pane();
  ASSERT_TRUE(target);
  auto pane = libtmux::own_pane(*target);
  ASSERT_FALSE(pane);
  ASSERT_TRUE(pane.error().receipt);
  EXPECT_FALSE(pane.error().rollback);
  EXPECT_FALSE(server->pane(pane.error().receipt->id));
}

TEST_F(Lifecycle, LostReceiptExposesUnknownDispatchWithoutGuessing) {
  auto wrapped = fault("lost");
  const auto owned = libtmux::own_session(wrapped, {.name = "unknown"});
  ASSERT_FALSE(owned);
  EXPECT_TRUE(owned.error().uncertain);
  EXPECT_FALSE(owned.error().receipt);
  EXPECT_TRUE(server->session("unknown"));
}

TEST_F(Lifecycle, CancellationAfterReceiptRollsBackBeforeReturningAnOwner) {
  auto wrapped = fault("timeout");
  std::atomic_bool stop{};
  std::thread interrupt{[&] {
    std::this_thread::sleep_for(150ms);
    stop = true;
  }};
  auto result =
      libtmux::own_session(wrapped, {.name = "cancelled"},
                           {.timeout = 2s, .cancelled = [&] { return stop.load(); }});
  interrupt.join();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().primary.kind, libtmux::FailureKind::cancelled);
  ASSERT_TRUE(result.error().receipt);
  EXPECT_FALSE(result.error().rollback);
  EXPECT_FALSE(server->session(result.error().receipt->id));
}

TEST_F(Lifecycle, PreCancelledCallsDoNotMutate) {
  auto owned = libtmux::own_session(*server, {.name = "cancelled"},
                                    {.cancelled = [] { return true; }});
  ASSERT_FALSE(owned);
  EXPECT_EQ(owned.error().primary.delivery, libtmux::DeliveryStatus::not_started);
  EXPECT_FALSE(server->session("cancelled"));
}

TEST_F(Lifecycle, FindsOrCreatesEachEntityAndKeepsReusedValuesBorrowed) {
  auto first = libtmux::find_or_create_session(*server, {.name = "find"});
  ASSERT_TRUE(first) << first.error().primary.diagnostic;
  EXPECT_TRUE(first->created());
  {
    auto second = libtmux::find_or_create_session(*server, {.name = "find"});
    ASSERT_TRUE(second);
    EXPECT_FALSE(second->created());
  }
  EXPECT_TRUE(server->session(first->value.id().value()));
  auto window = libtmux::find_or_create_window(first->value, {.name = "build"});
  ASSERT_TRUE(window);
  EXPECT_TRUE(window->created());
  auto window_again = libtmux::find_or_create_window(first->value, {.name = "build"});
  ASSERT_TRUE(window_again);
  EXPECT_FALSE(window_again->created());
  auto pane = libtmux::find_or_create_pane(window->value, "compiler");
  ASSERT_TRUE(pane) << pane.error().primary.diagnostic;
  EXPECT_TRUE(pane->created());
  auto pane_again = libtmux::find_or_create_pane(window->value, "compiler");
  ASSERT_TRUE(pane_again) << pane_again.error().primary.diagnostic;
  EXPECT_FALSE(pane_again->created());
  EXPECT_EQ(pane_again->value.id(), pane->value.id());
}

TEST_F(Lifecycle, AmbiguousWindowOrPaneIdentityFails) {
  auto base = session();
  ASSERT_TRUE(base.new_window("twins"));
  ASSERT_TRUE(base.new_window("twins"));
  auto result = libtmux::find_or_create_window(base, {.name = "twins"});
  ASSERT_FALSE(result);
  auto window = base.active_window();
  ASSERT_TRUE(window);
  auto first = window->active_pane();
  ASSERT_TRUE(first);
  auto second = first->split();
  ASSERT_TRUE(second);
  ASSERT_TRUE(first->set_option("@libtmux_pane_key", "twins"));
  ASSERT_TRUE(second->set_option("@libtmux_pane_key", "twins"));
  auto pane = libtmux::find_or_create_pane(*window, "twins");
  EXPECT_FALSE(pane);
}

TEST_F(Lifecycle, FindCreationFailuresRollBackSessionWindowAndPane) {
  auto selected = fault("refused");
  auto created_session =
      libtmux::find_or_create_session(selected, {.name = "failed-find"});
  ASSERT_FALSE(created_session);
  ASSERT_TRUE(created_session.error().receipt);
  EXPECT_FALSE(created_session.error().rollback);
  EXPECT_FALSE(server->session(created_session.error().receipt->id));

  selected = fault("refused");
  auto parent = selected.session(std::string{fixture->session_name()});
  ASSERT_TRUE(parent);
  auto created_window =
      libtmux::find_or_create_window(*parent, {.name = "failed-find"});
  ASSERT_FALSE(created_window);
  ASSERT_TRUE(created_window.error().receipt);
  EXPECT_FALSE(created_window.error().rollback);
  EXPECT_FALSE(server->window(created_window.error().receipt->id));

  selected = fault("refused");
  auto windows = selected.windows();
  ASSERT_TRUE(windows);
  ASSERT_EQ(windows->size(), 1U);
  auto created_pane = libtmux::find_or_create_pane(windows->front(), "failed-find");
  ASSERT_FALSE(created_pane);
  ASSERT_TRUE(created_pane.error().receipt);
  EXPECT_FALSE(created_pane.error().rollback);
  EXPECT_FALSE(server->pane(created_pane.error().receipt->id));
}

TEST_F(Lifecycle, CompetingFindCallsShareOneCreatedOwner) {
  auto a = std::async(std::launch::async, [&] {
    return libtmux::find_or_create_session(*server, {.name = "race"});
  });
  auto b = std::async(std::launch::async, [&] {
    return libtmux::find_or_create_session(*server, {.name = "race"});
  });
  auto first = a.get();
  auto second = b.get();
  ASSERT_TRUE(first) << first.error().primary.diagnostic;
  ASSERT_TRUE(second) << second.error().primary.diagnostic;
  EXPECT_NE(first->created(), second->created());
  EXPECT_EQ(first->value.id(), second->value.id());
}

TEST_F(Lifecycle, DiscoveryFindsTwoServersAndReportsRootsStaleSocketsAndLimits) {
  auto second = ScopedTmuxServer::start(
      {.tmux_binary = LIBTMUX_TEST_TMUX_PATH,
       .socket_namespace = libtmux::test::SocketNamespace::consumer("find")});
  ASSERT_TRUE(second);
  const auto stale = fixture->tmux_tmpdir() / "stale.sock";
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  ASSERT_LT(stale.string().size(), sizeof(address.sun_path));
  const auto stale_text = stale.string();
  std::copy(stale_text.begin(), stale_text.end(), address.sun_path);
  ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  ::close(fd);
  libtmux::DiscoveryOptions options{.roots = {fixture->socket_path().parent_path(),
                                              second->socket_path().parent_path(),
                                              fixture->tmux_tmpdir() / "missing"},
                                    .configured_roots = false,
                                    .policy = policy()};
  auto result = libtmux::discover_servers(options);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->servers.size(), 2U);
  EXPECT_FALSE(result->truncated);
  EXPECT_TRUE(std::ranges::any_of(result->diagnostics, [](const auto& item) {
    return item.outcome == "probe failed";
  }));
  EXPECT_TRUE(std::ranges::any_of(result->diagnostics, [](const auto& item) {
    return item.outcome.starts_with("root: ");
  }));
  options.max_probes = 1;
  auto limited = libtmux::discover_servers(options);
  ASSERT_TRUE(limited);
  EXPECT_TRUE(limited->truncated);
  EXPECT_EQ(limited->probes, 1U);
}

TEST_F(Lifecycle, DiscoveryPreservesSymlinkDotDotAndMissingComponents) {
  auto second = ScopedTmuxServer::start(
      {.tmux_binary = LIBTMUX_TEST_TMUX_PATH,
       .socket_namespace = libtmux::test::SocketNamespace::consumer("dots")});
  ASSERT_TRUE(second);
  const auto nested = second->tmux_tmpdir() / "inside";
  std::filesystem::create_directory(nested);
  std::filesystem::create_directory_symlink(nested, fixture->tmux_tmpdir() / "link");
  auto result =
      libtmux::discover_servers({.roots = {fixture->tmux_tmpdir() / "link/..",
                                           fixture->tmux_tmpdir() / "missing/.."},
                                 .configured_roots = false,
                                 .policy = policy()});
  ASSERT_TRUE(result);
  ASSERT_EQ(result->servers.size(), 1U);
  EXPECT_EQ(result->servers.front().pid, std::to_string(second->server_pid()));
  EXPECT_TRUE(std::ranges::any_of(result->diagnostics, [](const auto& item) {
    return item.outcome.starts_with("root: ");
  }));
  result = libtmux::discover_servers({.roots = {nested, nested, nested},
                                      .configured_roots = false,
                                      .max_roots = 2,
                                      .policy = policy()});
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->truncated);
  EXPECT_EQ(result->roots_examined, 2U);
}

TEST_F(Lifecycle, DiscoveryExposesTimeoutCancellationAndEntryBounds) {
  const auto script = fixture->tmux_tmpdir() / "slow-probe.py";
  {
    std::ofstream file{script};
    file << "#!/usr/bin/python3\nimport os,sys,time\nreal="
         << std::quoted(std::string{LIBTMUX_TEST_TMUX_PATH}) << "\n"
         << "if any('__libtmux_discovery__' in a for a in sys.argv):time.sleep(1)\n"
         << "os.execv(real,[real]+sys.argv[1:])\n";
  }
  std::filesystem::permissions(script, std::filesystem::perms::owner_all);
  auto configured = policy();
  configured.tmux_binary = script;
  libtmux::DiscoveryOptions options{.roots = {fixture->tmux_tmpdir()},
                                    .configured_roots = false,
                                    .probe_timeout = 30ms,
                                    .policy = configured};
  auto found = libtmux::discover_servers(options);
  ASSERT_TRUE(found);
  EXPECT_TRUE(found->servers.empty());
  EXPECT_TRUE(std::ranges::any_of(found->diagnostics, [](const auto& diagnostic) {
    return diagnostic.outcome == "probe failed" && diagnostic.failure &&
           diagnostic.failure->kind == libtmux::FailureKind::timeout;
  }));
  EXPECT_TRUE(fixture->is_alive());
  options.policy = policy();
  options.cancelled = [] { return true; };
  found = libtmux::discover_servers(options);
  ASSERT_TRUE(found);
  EXPECT_TRUE(found->truncated);
  EXPECT_EQ(found->roots_examined, 0U);
  ASSERT_EQ(found->diagnostics.size(), 1U);
  EXPECT_EQ(found->diagnostics.front().outcome, "cancelled");
  options.cancelled = {};
  options.max_entries = 1;
  found = libtmux::discover_servers(options);
  ASSERT_TRUE(found);
  EXPECT_TRUE(found->truncated);
  EXPECT_EQ(found->entries_examined, 1U);
  EXPECT_TRUE(std::ranges::any_of(found->diagnostics, [](const auto& diagnostic) {
    return diagnostic.outcome == "entry limit";
  }));
}

TEST_F(Lifecycle, ExplicitServerAdoptionKillsOnlyTheAcceptedDaemon) {
  auto owner = libtmux::adopt(*server);
  ASSERT_TRUE(owner) << owner.error().primary.diagnostic;
  const auto receipt = owner->cleanup().receipt();
  EXPECT_TRUE(receipt.id.empty());
  EXPECT_TRUE(owner->close());
  EXPECT_TRUE(wait_for_fixture_exit());
}

class ServerExit final {
public:
  explicit ServerExit(const std::string& pid) : pid_{std::stoi(pid)} {
#if defined(__linux__)
    fd_ = static_cast<int>(::syscall(SYS_pidfd_open, pid_, 0));
#endif
  }
  ~ServerExit() {
    EXPECT_TRUE(observed());
    if (fd_ >= 0)
      ::close(fd_);
  }
  bool observed() {
    if (fd_ >= 0) {
      pollfd watched{fd_, POLLIN, 0};
      return ::poll(&watched, 1, 2000) == 1 && (watched.revents & POLLIN) != 0;
    }
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (::kill(pid_, 0) == 0 && std::chrono::steady_clock::now() < end) {
      std::this_thread::sleep_for(5ms);
    }
    return errno == ESRCH;
  }

private:
  int pid_;
  int fd_{-1};
};

TEST_F(Lifecycle, OwnedServerStartupUsesNonceAndFindReusesWithoutTakingOwnership) {
  const auto path = fixture->tmux_tmpdir() / "owned.sock";
  extra_endpoints.push_back(path);
  auto cold = Server::startable_at_socket_path(path, std::filesystem::path{"/dev/null"},
                                               {}, policy());
  ASSERT_TRUE(cold);
  auto first = libtmux::find_or_create_server(*cold, {.name = "seed"});
  ASSERT_TRUE(first) << first.error().primary.diagnostic;
  ASSERT_TRUE(first->created());
  ServerExit exit{first->owner->cleanup().receipt().pid};
  auto repeated = libtmux::find_or_create_server(*cold, {.name = "ignored"});
  ASSERT_TRUE(repeated) << repeated.error().primary.diagnostic;
  EXPECT_FALSE(repeated->created());
  EXPECT_TRUE(repeated->value.session("seed"));
  auto refused = libtmux::own_server(*cold, {.name = "other"});
  EXPECT_FALSE(refused);
  EXPECT_FALSE(repeated->value.session("other"));
  ASSERT_TRUE(first->owner->close());
  EXPECT_TRUE(exit.observed());
  EXPECT_TRUE(fixture->is_alive());
}

TEST_F(Lifecycle, CompetingWindowAndPaneCallsCreateOnce) {
  auto base = session();
  auto wa = std::async(std::launch::async, [&] {
    return libtmux::find_or_create_window(base, {.name = "race"});
  });
  auto wb = std::async(std::launch::async, [&] {
    return libtmux::find_or_create_window(base, {.name = "race"});
  });
  auto first = wa.get();
  auto second = wb.get();
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_NE(first->created(), second->created());
  EXPECT_EQ(first->value.id(), second->value.id());
  auto pa = std::async(std::launch::async, [&] {
    return libtmux::find_or_create_pane(first->value, "race");
  });
  auto pb = std::async(std::launch::async, [&] {
    return libtmux::find_or_create_pane(first->value, "race");
  });
  auto pane_a = pa.get();
  auto pane_b = pb.get();
  ASSERT_TRUE(pane_a);
  ASSERT_TRUE(pane_b);
  EXPECT_NE(pane_a->created(), pane_b->created());
  EXPECT_EQ(pane_a->value.id(), pane_b->value.id());
}

TEST_F(Lifecycle, CompetingServerCallsCreateOneDaemon) {
  const auto path = fixture->tmux_tmpdir() / "race.sock";
  extra_endpoints.push_back(path);
  auto cold_a = Server::startable_at_socket_path(
      path, std::filesystem::path{"/dev/null"}, {}, policy());
  auto cold_b = Server::startable_at_socket_path(
      path, std::filesystem::path{"/dev/null"}, {}, policy());
  ASSERT_TRUE(cold_a);
  ASSERT_TRUE(cold_b);
  auto a = std::async(std::launch::async, [&] {
    return libtmux::find_or_create_server(*cold_a, {.name = "seed"});
  });
  auto b = std::async(std::launch::async, [&] {
    return libtmux::find_or_create_server(*cold_b, {.name = "seed"});
  });
  auto first = a.get();
  auto second = b.get();
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_NE(first->created(), second->created());
  auto& owner = first->created() ? *first->owner : *second->owner;
  ServerExit exit{owner.cleanup().receipt().pid};
  ASSERT_TRUE(owner.close());
  EXPECT_TRUE(exit.observed());
}

TEST_F(Lifecycle, CleanupRefusesReplacementEvenWhenNumericIdentityIsForgedToMatch) {
  const auto script = fixture->tmux_tmpdir() / "redirect.py";
  const auto redirect = fixture->tmux_tmpdir() / "redirect";
  {
    std::ofstream file{script};
    file << "#!/usr/bin/python3\nimport os,sys,json,re\n"
         << "args=sys.argv[1:]\npath=" << std::quoted(redirect.string()) << "\n"
         << "if os.path.exists(path) and 'if-shell' in args:\n"
         << "    target,pid,start=json.load(open(path))\n"
         << "    args[args.index('-S')+1]=target\n"
         << "    i=args.index('if-shell')+2\n"
         << "    args[i]=re.sub(r'(#\\{pid\\},)[0-9]+',lambda m:m[1]+pid,args[i])\n"
         << "    args[i]=re.sub(r'(#\\{start_time\\},)[0-9]+',lambda "
            "m:m[1]+start,args[i])\n"
         << "os.execv(" << std::quoted(std::string{LIBTMUX_TEST_TMUX_PATH}) << ",["
         << std::quoted(std::string{LIBTMUX_TEST_TMUX_PATH}) << "]+args)\n";
  }
  std::filesystem::permissions(script, std::filesystem::perms::owner_all);
  auto configured = policy();
  configured.tmux_binary = script;
  auto selected = Server::at_socket_path(fixture->socket_path(), {}, configured);
  ASSERT_TRUE(selected);
  auto s = selected->sessions();
  ASSERT_TRUE(s);
  auto w = s->front().active_window();
  ASSERT_TRUE(w);
  auto p = w->active_pane();
  ASSERT_TRUE(p);
  auto so = libtmux::adopt(*selected);
  ASSERT_TRUE(so);
  auto ss = libtmux::adopt(s->front());
  ASSERT_TRUE(ss);
  auto wo = libtmux::adopt(*w);
  ASSERT_TRUE(wo);
  auto po = libtmux::adopt(*p);
  ASSERT_TRUE(po);
  ASSERT_TRUE(server->kill());
  ASSERT_TRUE(wait_for_fixture_exit());
  auto replacement = ScopedTmuxServer::start(
      {.tmux_binary = LIBTMUX_TEST_TMUX_PATH,
       .socket_namespace = libtmux::test::SocketNamespace::consumer("replace")});
  ASSERT_TRUE(replacement);
  auto current = Server::at_socket_path(replacement->socket_path(), {}, policy());
  ASSERT_TRUE(current);
  ASSERT_TRUE(
      current->set_server_option("@libtmux_owner_generation", std::string(32, 'e')));
  auto identity = current->run({"display-message", "-p", "#{pid} #{start_time}"});
  ASSERT_TRUE(identity);
  std::istringstream fields{*identity};
  std::string pid, start;
  fields >> pid >> start;
  {
    std::ofstream file{redirect};
    file << '[' << std::quoted(replacement->socket_path().string()) << ','
         << std::quoted(pid) << ',' << std::quoted(start) << ']';
  }
  EXPECT_FALSE(so->close());
  EXPECT_FALSE(ss->close());
  EXPECT_FALSE(wo->close());
  EXPECT_FALSE(po->close());
  EXPECT_TRUE(replacement->is_alive());
  EXPECT_TRUE(current->session(s->front().id().value()));
  EXPECT_TRUE(current->window(w->id().value()));
  EXPECT_TRUE(current->pane(p->id().value()));
  ASSERT_TRUE(so->cleanup().report().failure);
  EXPECT_NE(so->cleanup().report().failure->diagnostic.find("generation changed"),
            std::string::npos);
}

TEST_F(Lifecycle, CoordinationCancellationDoesNotWaitForAnotherAcquisition) {
  auto wrapped = fault("timeout");
  auto pending = std::async(std::launch::async, [&] {
    return libtmux::find_or_create_session(wrapped, {.name = "queued"},
                                           {.timeout = 500ms});
  });
  std::this_thread::sleep_for(100ms);
  std::atomic_bool cancel{};
  std::thread interrupt{[&] {
    std::this_thread::sleep_for(50ms);
    cancel = true;
  }};
  const auto before = std::chrono::steady_clock::now();
  auto second = libtmux::find_or_create_window(
      session(), {.name = "queued"}, {.cancelled = [&] { return cancel.load(); }});
  const auto elapsed = std::chrono::steady_clock::now() - before;
  interrupt.join();
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().primary.kind, libtmux::FailureKind::cancelled);
  EXPECT_LT(elapsed, 250ms);
  auto first = pending.get();
  EXPECT_FALSE(first);
  EXPECT_FALSE(server->session("queued"));
}

TEST_F(Lifecycle, ObserverExceptionAfterReceiptRollsBackAndRetainsException) {
  auto selected = Server::at_socket_path(
      fixture->socket_path(),
      [](const libtmux::CommandReport& report) {
        if (report.command.find("if-shell") != std::string_view::npos)
          throw std::runtime_error{"observer"};
      },
      policy());
  ASSERT_TRUE(selected);
  auto result = libtmux::own_session(*selected, {.name = "observed"});
  ASSERT_FALSE(result);
  ASSERT_TRUE(result.error().receipt);
  EXPECT_TRUE(result.error().exception);
  EXPECT_FALSE(server->session(result.error().receipt->id));
  // The cleanup command also ran, then the same observer threw. Its failure
  // must remain visible even though reading back proves the session is gone.
  EXPECT_TRUE(result.error().rollback);
}

TEST_F(Lifecycle, ServerFindFailureAfterReceiptRollsBackTheNewDaemon) {
  const auto script = fixture->tmux_tmpdir() / "startup.py";
  {
    std::ofstream file{script};
    file << "#!/usr/bin/python3\nimport os,sys,subprocess\n"
         << "real=" << std::quoted(std::string{LIBTMUX_TEST_TMUX_PATH}) << "\n"
         << "if 'new-session' in sys.argv:\n"
         << "    result=subprocess.run([real]+sys.argv[1:]);sys.exit(77 if "
            "result.returncode==0 else result.returncode)\n"
         << "os.execv(real,[real]+sys.argv[1:])\n";
  }
  std::filesystem::permissions(script, std::filesystem::perms::owner_all);
  auto configured = policy();
  configured.tmux_binary = script;
  const auto path = fixture->tmux_tmpdir() / "failed.sock";
  extra_endpoints.push_back(path);
  auto cold = Server::startable_at_socket_path(path, std::filesystem::path{"/dev/null"},
                                               {}, configured);
  ASSERT_TRUE(cold);
  auto result = libtmux::find_or_create_server(*cold, {.name = "seed"});
  ASSERT_FALSE(result);
  ASSERT_TRUE(result.error().receipt) << result.error().primary.diagnostic;
  EXPECT_FALSE(result.error().rollback)
      << (result.error().rollback ? result.error().rollback->diagnostic : "");
  ServerExit exit{result.error().receipt->pid};
  EXPECT_TRUE(exit.observed());
  EXPECT_EQ(result.error().primary.exit_code, 77);
}

TEST_F(Lifecycle, ConcurrentForeignStartupNeverTransfersServerOwnership) {
  const auto script = fixture->tmux_tmpdir() / "peer.py";
  {
    std::ofstream file{script};
    file << "#!/usr/bin/python3\nimport os,sys,subprocess\n"
         << "real=" << std::quoted(std::string{LIBTMUX_TEST_TMUX_PATH}) << "\n"
         << "if 'new-session' in sys.argv:\n"
         << "    env=dict(os.environ);env.pop('LIBTMUX_OWNER_STARTUP_NONCE',None)\n"
         << "    prefix=sys.argv[1:sys.argv.index('new-session')]\n"
         << "    "
            "subprocess.run([real]+prefix+['new-session','-d','-s','foreign'],env=env,"
            "check=True)\n"
         << "os.execv(real,[real]+sys.argv[1:])\n";
  }
  std::filesystem::permissions(script, std::filesystem::perms::owner_all);
  auto configured = policy();
  configured.tmux_binary = script;
  const auto path = fixture->tmux_tmpdir() / "peer.sock";
  extra_endpoints.push_back(path);
  auto cold = Server::startable_at_socket_path(path, std::filesystem::path{"/dev/null"},
                                               {}, configured);
  ASSERT_TRUE(cold);
  auto result = libtmux::own_server(*cold, {.name = "seed"});
  ASSERT_FALSE(result);
  ASSERT_TRUE(result.error().receipt) << result.error().primary.diagnostic;
  ServerExit exit{result.error().receipt->pid};
  auto current = Server::at_socket_path(path, {}, policy());
  ASSERT_TRUE(current);
  EXPECT_TRUE(current->session("foreign"));
  EXPECT_FALSE(current->session("seed"));
  EXPECT_FALSE(result.error().rollback);
  ASSERT_TRUE(current->kill());
  EXPECT_TRUE(exit.observed());
}

TEST_F(Lifecycle, ForeignStartupRefusalPreservesCapturedFailures) {
  int sequence = 0;
  for (const bool find_first : {false, true}) {
    for (const bool fail_command : {false, true}) {
      for (const bool fail_observer : {false, true}) {
        const auto name = "foreign-" + std::to_string(sequence++);
        SCOPED_TRACE(name);
        const auto path = fixture->tmux_tmpdir() / (name + ".sock");
        const auto script = fixture->tmux_tmpdir() / (name + ".py");
        {
          std::ofstream file{script};
          file << "#!/usr/bin/python3\nimport os,sys,subprocess\n"
               << "real=" << std::quoted(std::string{LIBTMUX_TEST_TMUX_PATH}) << "\n"
               << "peer=" << std::quoted(path.string()) << "\n"
               << "source=" << std::quoted(fixture->socket_path().string()) << "\n"
               << "if 'new-session' in sys.argv:\n"
               << "    if not os.path.lexists(peer):os.symlink(source,peer)\n"
               << "    subprocess.run([real]+sys.argv[1:],check=True)\n"
               << "    if " << fail_command << ":\n"
               << "        sys.stderr.write('original startup client failed\\n');"
                  "sys.exit(77)\n"
               << "    sys.exit(0)\n"
               << "os.execv(real,[real]+sys.argv[1:])\n";
        }
        std::filesystem::permissions(script, std::filesystem::perms::owner_all);
        auto configured = policy();
        configured.tmux_binary = script;
        const auto observer_error =
            std::make_exception_ptr(std::range_error{"foreign startup observer"});
        std::optional<libtmux::CommandFailure> observed;
        auto cold = Server::startable_at_socket_path(
            path, std::filesystem::path{"/dev/null"},
            [&](const libtmux::CommandReport& command) {
              if (command.command.starts_with("new-session")) {
                if (command.failure)
                  observed = *command.failure;
                if (fail_observer)
                  std::rethrow_exception(observer_error);
              }
            },
            configured);
        ASSERT_TRUE(cold);
        const auto inspect = [&](const auto& result) {
          ASSERT_FALSE(result);
          const auto& error = result.error();
          SCOPED_TRACE(error.primary.diagnostic);
          EXPECT_EQ(error.primary.kind, libtmux::FailureKind::refused);
          EXPECT_EQ(error.primary.delivery, libtmux::DeliveryStatus::replied);
          EXPECT_NE(error.primary.diagnostic.find("another client started"),
                    std::string::npos);
          ASSERT_TRUE(error.receipt);
          EXPECT_EQ(error.receipt->pid, std::to_string(fixture->server_pid()));
          ASSERT_TRUE(error.cleanup);
          EXPECT_TRUE(error.cleanup->report().complete);
          EXPECT_FALSE(error.rollback);
          EXPECT_FALSE(error.uncertain);
          EXPECT_FALSE(server->session(error.receipt->id));
          EXPECT_TRUE(server->session(std::string{fixture->session_name()}));
          EXPECT_EQ(error.exception,
                    fail_observer ? observer_error : std::exception_ptr{});
          EXPECT_EQ(error.command_failure.has_value(), fail_command || fail_observer);
          if (fail_command) {
            ASSERT_TRUE(observed);
            ASSERT_TRUE(error.command_failure);
            EXPECT_EQ(error.command_failure->kind, observed->kind);
            EXPECT_EQ(error.command_failure->delivery, observed->delivery);
            EXPECT_EQ(error.command_failure->exit_code, 77);
            EXPECT_EQ(error.command_failure->exit_code, observed->exit_code);
            EXPECT_EQ(error.command_failure->diagnostic, observed->diagnostic);
            EXPECT_TRUE(error.command_failure->diagnostic.starts_with(
                "original startup client failed"));
          } else if (fail_observer) {
            ASSERT_TRUE(error.command_failure);
            EXPECT_EQ(error.command_failure->kind, libtmux::FailureKind::pipe);
            EXPECT_EQ(error.command_failure->delivery,
                      libtmux::DeliveryStatus::replied);
            EXPECT_EQ(error.command_failure->diagnostic,
                      "command observer threw after receipt capture");
          }
        };
        if (find_first)
          inspect(libtmux::find_or_create_server(*cold, {.name = name}));
        else
          inspect(libtmux::own_server(*cold, {.name = name}));
      }
    }
  }
}

TEST_F(Lifecycle, ContradictoryReceiptsPreserveCapturedFailuresWithoutCleanup) {
  int sequence = 0;
  for (const std::string_view kind : {"session", "window", "pane"}) {
    for (const bool fail_command : {false, true}) {
      for (const bool fail_observer : {false, true}) {
        const auto name = "contradictory-" + std::to_string(sequence++);
        SCOPED_TRACE(name + " " + std::string{kind});
        const auto script = fixture->tmux_tmpdir() / (name + ".py");
        {
          std::ofstream file{script};
          file << "#!/usr/bin/python3\nimport os,sys,subprocess\n"
               << "real=" << std::quoted(std::string{LIBTMUX_TEST_TMUX_PATH}) << "\n"
               << "if 'if-shell' in sys.argv:\n"
               << "    result=subprocess.run([real]+sys.argv[1:],capture_output=True,"
                  "check=True)\n"
               << "    for line in result.stdout.splitlines():\n"
               << "        if line.startswith(b'__libtmux_owned__|'):\n"
               << "            fields=line.split(b'|')\n"
               << "            fields[3]=b'1'*32 if fields[3]!=b'1'*32 else b'2'*32\n"
               << "            line=b'|'.join(fields)\n"
               << "        sys.stdout.buffer.write(line+b'\\n')\n"
               << "    sys.stdout.flush()\n"
               << "    if " << fail_command << ":\n"
               << "        sys.stderr.write('original creation client failed\\n');"
                  "sys.exit(77)\n"
               << "    sys.exit(0)\n"
               << "os.execv(real,[real]+sys.argv[1:])\n";
        }
        std::filesystem::permissions(script, std::filesystem::perms::owner_all);
        auto configured = policy();
        configured.tmux_binary = script;
        const auto observer_error =
            std::make_exception_ptr(std::range_error{"contradictory receipt observer"});
        std::optional<libtmux::CommandFailure> observed;
        auto selected = Server::at_socket_path(
            fixture->socket_path(),
            [&](const libtmux::CommandReport& command) {
              if (command.command.starts_with("if-shell")) {
                if (command.failure)
                  observed = *command.failure;
                if (fail_observer)
                  std::rethrow_exception(observer_error);
              }
            },
            configured);
        ASSERT_TRUE(selected);
        const auto inspect = [&](const auto& result) {
          ASSERT_FALSE(result);
          const auto& error = result.error();
          SCOPED_TRACE(error.primary.diagnostic);
          EXPECT_TRUE(error.uncertain);
          EXPECT_EQ(error.primary.kind, libtmux::FailureKind::pipe);
          EXPECT_EQ(error.primary.delivery, libtmux::DeliveryStatus::indeterminate);
          EXPECT_EQ(error.primary.diagnostic,
                    "creation receipt changed daemon identity");
          ASSERT_TRUE(error.receipt);
          EXPECT_FALSE(error.cleanup);
          EXPECT_FALSE(error.rollback);
          if (kind == "session")
            EXPECT_TRUE(server->session(error.receipt->id));
          else if (kind == "window")
            EXPECT_TRUE(server->window(error.receipt->id));
          else
            EXPECT_TRUE(server->pane(error.receipt->id));
          EXPECT_EQ(error.exception,
                    fail_observer ? observer_error : std::exception_ptr{});
          EXPECT_EQ(error.command_failure.has_value(), fail_command || fail_observer);
          if (fail_command) {
            ASSERT_TRUE(observed);
            ASSERT_TRUE(error.command_failure);
            EXPECT_EQ(error.command_failure->kind, observed->kind);
            EXPECT_EQ(error.command_failure->delivery, observed->delivery);
            EXPECT_EQ(error.command_failure->exit_code, 77);
            EXPECT_EQ(error.command_failure->exit_code, observed->exit_code);
            EXPECT_EQ(error.command_failure->diagnostic, observed->diagnostic);
            EXPECT_TRUE(error.command_failure->diagnostic.starts_with(
                "original creation client failed"));
          } else if (fail_observer) {
            ASSERT_TRUE(error.command_failure);
            EXPECT_EQ(error.command_failure->kind, libtmux::FailureKind::pipe);
            EXPECT_EQ(error.command_failure->delivery,
                      libtmux::DeliveryStatus::replied);
            EXPECT_EQ(error.command_failure->diagnostic,
                      "command observer threw after receipt capture");
          }
        };
        if (kind == "session") {
          inspect(libtmux::own_session(*selected, {.name = name}));
        } else {
          auto parent = selected->session(std::string{fixture->session_name()});
          ASSERT_TRUE(parent);
          if (kind == "window") {
            inspect(libtmux::own_window(*parent, {.name = name}));
          } else {
            auto window = parent->new_window(name);
            ASSERT_TRUE(window);
            auto pane = window->active_pane();
            ASSERT_TRUE(pane);
            inspect(libtmux::own_pane(*pane));
          }
        }
      }
    }
  }
}

TEST_F(Lifecycle, SnapshotRefusalAfterReceiptRollsBack) {
  const auto script = fixture->tmux_tmpdir() / "snapshot.py";
  {
    std::ofstream file{script};
    file << "#!/usr/bin/python3\nimport os,sys\n"
         << "if 'display-message' in sys.argv and '-t' in sys.argv:\n"
         << "    sys.stderr.write('snapshot refused\\n');sys.exit(77)\n"
         << "real=" << std::quoted(std::string{LIBTMUX_TEST_TMUX_PATH}) << "\n"
         << "os.execv(real,[real]+sys.argv[1:])\n";
  }
  std::filesystem::permissions(script, std::filesystem::perms::owner_all);
  auto configured = policy();
  configured.tmux_binary = script;
  const auto observer_error =
      std::make_exception_ptr(std::range_error{"snapshot observer detail"});
  auto selected = Server::at_socket_path(
      fixture->socket_path(),
      [&](const libtmux::CommandReport& command) {
        if (command.command.find("display-message") != std::string_view::npos &&
            command.command.find(" -t ") != std::string_view::npos)
          std::rethrow_exception(observer_error);
      },
      configured);
  ASSERT_TRUE(selected);
  auto result = libtmux::own_session(*selected, {.name = "snapshot"});
  ASSERT_FALSE(result);
  ASSERT_TRUE(result.error().receipt);
  EXPECT_FALSE(result.error().rollback);
  EXPECT_EQ(result.error().primary.exit_code, 77);
  EXPECT_EQ(result.error().exception, observer_error);
  EXPECT_NE(result.error().primary.diagnostic.find("snapshot refused"),
            std::string::npos);
  EXPECT_FALSE(server->session(result.error().receipt->id));
}

TEST_F(Lifecycle, AdoptionRetainsMovedPaneAndLinkedWindowIdentity) {
  auto other = server->new_session("other");
  ASSERT_TRUE(other);
  auto original = session();
  auto window = original.new_window("links");
  ASSERT_TRUE(window);
  auto owner = libtmux::adopt(*window);
  ASSERT_TRUE(owner);
  ASSERT_TRUE(server->run(
      {"link-window", "-s", window->id().value(), "-t", other->id().value()}));
  auto pane = window->split();
  ASSERT_TRUE(pane);
  auto pane_owner = libtmux::adopt(*pane);
  ASSERT_TRUE(pane_owner);
  auto destination = other->active_pane();
  ASSERT_TRUE(destination);
  ASSERT_TRUE(server->run(
      {"join-pane", "-s", pane->id().value(), "-t", destination->id().value()}));
  ASSERT_TRUE(pane_owner->close());
  EXPECT_FALSE(server->pane(pane->id().value()));
  ASSERT_TRUE(owner->close());
  EXPECT_FALSE(server->window(window->id().value()));
  auto remaining = other->windows();
  ASSERT_TRUE(remaining);
  EXPECT_TRUE(std::ranges::none_of(
      *remaining, [&](const auto& item) { return item.id() == window->id(); }));
}

TEST_F(Lifecycle, ConcurrentCleanupCallsShareOneSuccessfulAttempt) {
  auto owner = libtmux::own_session(*server, {.name = "close"});
  ASSERT_TRUE(owner);
  const auto handle = owner->cleanup();
  auto first = std::async(std::launch::async, [&] { return handle.close(); });
  auto second = std::async(std::launch::async, [&] { return handle.close(); });
  EXPECT_TRUE(first.get());
  EXPECT_TRUE(second.get());
  EXPECT_EQ(handle.report().attempts, 1U);
}

TEST_F(Lifecycle, AcquisitionAndRollbackFailuresRemainInspectable) {
  const auto script = fixture->tmux_tmpdir() / "both.py";
  const auto phase = fixture->tmux_tmpdir() / "phase";
  {
    std::ofstream file{script};
    file << "#!/usr/bin/python3\nimport os,sys,subprocess\n"
         << "real=" << std::quoted(std::string{LIBTMUX_TEST_TMUX_PATH}) << "\n"
         << "phase=" << std::quoted(phase.string()) << "\n"
         << "if 'if-shell' in sys.argv:\n"
         << "    if os.path.exists(phase):sys.stderr.write('rollback "
            "refused\\n');sys.exit(78)\n"
         << "    "
            "open(phase,'w').close();subprocess.run([real]+sys.argv[1:]);sys.stderr."
            "write('creation failed\\n');sys.exit(77)\n"
         << "os.execv(real,[real]+sys.argv[1:])\n";
  }
  std::filesystem::permissions(script, std::filesystem::perms::owner_all);
  auto configured = policy();
  configured.tmux_binary = script;
  const auto creation_observer =
      std::make_exception_ptr(std::runtime_error{"creation observer detail"});
  const auto rollback_observer =
      std::make_exception_ptr(std::range_error{"rollback observer detail"});
  bool fail_observer = true;
  int observed = 0;
  auto selected = Server::at_socket_path(
      fixture->socket_path(),
      [&](const libtmux::CommandReport& command) {
        if (fail_observer && command.command.find("if-shell") != std::string_view::npos)
          std::rethrow_exception(++observed == 1 ? creation_observer
                                                 : rollback_observer);
      },
      configured);
  ASSERT_TRUE(selected);
  auto result = libtmux::own_session(*selected, {.name = "both"});
  ASSERT_FALSE(result);
  ASSERT_TRUE(result.error().receipt);
  ASSERT_TRUE(result.error().rollback);
  EXPECT_EQ(result.error().primary.exit_code, 77);
  EXPECT_EQ(result.error().rollback->exit_code, 78);
  EXPECT_EQ(result.error().exception, creation_observer);
  EXPECT_TRUE(result.error().rollback->diagnostic.starts_with("rollback refused"));
  EXPECT_TRUE(server->session(result.error().receipt->id));
  ASSERT_TRUE(result.error().cleanup);
  EXPECT_EQ(result.error().cleanup->report().attempts, 1U);
  EXPECT_FALSE(result.error().cleanup->report().complete);
  EXPECT_EQ(result.error().cleanup->report().exception, rollback_observer);
  ASSERT_TRUE(result.error().cleanup->report().failure);
  EXPECT_EQ(result.error().cleanup->report().failure->exit_code, 78);
  {
    std::ofstream repaired{script};
    repaired << "#!/usr/bin/python3\nimport os,sys\nreal="
             << std::quoted(std::string{LIBTMUX_TEST_TMUX_PATH})
             << "\nos.execv(real,[real]+sys.argv[1:])\n";
  }
  fail_observer = false;
  EXPECT_TRUE(result.error().cleanup->close());
  EXPECT_EQ(result.error().cleanup->report().attempts, 2U);
  EXPECT_TRUE(result.error().cleanup->report().complete);
  EXPECT_FALSE(result.error().cleanup->report().exception);
  EXPECT_FALSE(result.error().cleanup->report().failure);
  EXPECT_EQ(result.error().exception, creation_observer);
  EXPECT_FALSE(server->session(result.error().receipt->id));
}

TEST_F(Lifecycle, BodyAndCleanupRetainDistinctOriginalExceptions) {
  const auto cleanup_error =
      std::make_exception_ptr(std::range_error{"cleanup observer detail"});
  bool active = false;
  auto selected = Server::at_socket_path(
      fixture->socket_path(),
      [&](const libtmux::CommandReport&) {
        if (active)
          std::rethrow_exception(cleanup_error);
      },
      policy());
  ASSERT_TRUE(selected);
  auto owner = libtmux::own_session(*selected, {.name = "paired-exceptions"});
  ASSERT_TRUE(owner);
  const auto id = (*owner)->id().value();
  active = true;
  const auto result = libtmux::with_owned(
      *owner,
      [](const libtmux::Session&) -> libtmux::expected<void, libtmux::CommandFailure> {
        throw std::logic_error{"body exception detail"};
      });
  active = false;
  ASSERT_TRUE(result.body_exception);
  try {
    std::rethrow_exception(result.body_exception);
  } catch (const std::logic_error& error) {
    EXPECT_STREQ(error.what(), "body exception detail");
  }
  ASSERT_TRUE(result.cleanup.exception);
  EXPECT_EQ(result.cleanup.exception, cleanup_error);
  try {
    std::rethrow_exception(result.cleanup.exception);
  } catch (const std::range_error& error) {
    EXPECT_STREQ(error.what(), "cleanup observer detail");
  }
  EXPECT_TRUE(result.cleanup.failure);
  EXPECT_EQ(result.cleanup.attempts, 1U);
  EXPECT_FALSE(result.cleanup.complete);
  EXPECT_FALSE(server->session(id));
}

TEST_F(Lifecycle, SnapshotObserverExceptionSurvivesSuccessfulRollback) {
  const auto observer_error =
      std::make_exception_ptr(std::range_error{"snapshot observer detail"});
  auto selected = Server::at_socket_path(
      fixture->socket_path(),
      [&](const libtmux::CommandReport& command) {
        if (command.command.find("display-message") != std::string_view::npos &&
            command.command.find(" -t ") != std::string_view::npos)
          std::rethrow_exception(observer_error);
      },
      policy());
  ASSERT_TRUE(selected);
  auto result = libtmux::own_session(*selected, {.name = "snapshot-exception"});
  ASSERT_FALSE(result);
  ASSERT_TRUE(result.error().receipt);
  EXPECT_EQ(result.error().exception, observer_error);
  EXPECT_FALSE(result.error().rollback);
  ASSERT_TRUE(result.error().cleanup);
  EXPECT_TRUE(result.error().cleanup->report().complete);
  EXPECT_FALSE(server->session(result.error().receipt->id));
}

TEST_F(Lifecycle, FindReadsPreserveObserverExceptions) {
  const auto observer_error =
      std::make_exception_ptr(std::range_error{"find observer detail"});
  std::string_view command_to_fail;
  auto selected = Server::at_socket_path(
      fixture->socket_path(),
      [&](const libtmux::CommandReport& command) {
        if (!command_to_fail.empty() &&
            command.command.find(command_to_fail) != std::string_view::npos)
          std::rethrow_exception(observer_error);
      },
      policy());
  ASSERT_TRUE(selected);
  auto parent = selected->session(std::string{fixture->session_name()});
  ASSERT_TRUE(parent);
  auto windows = parent->windows();
  ASSERT_TRUE(windows);
  ASSERT_EQ(windows->size(), 1U);
  const auto check = [&](const auto& result) {
    EXPECT_FALSE(result);
    if (!result)
      EXPECT_EQ(result.error().exception, observer_error);
  };
  command_to_fail = "list-sessions";
  check(libtmux::find_or_create_session(*selected, {.name = "unreached"}));
  command_to_fail = "list-windows";
  check(libtmux::find_or_create_window(*parent, {.name = "unreached"}));
  command_to_fail = "list-panes";
  check(libtmux::find_or_create_pane(windows->front(), "unreached"));
  command_to_fail = "@libtmux_pane_key";
  check(libtmux::find_or_create_pane(windows->front(), "unreached"));
  command_to_fail = "#{pid}";
  check(libtmux::find_or_create_server(*selected, {.name = "unreached"}));
}

TEST_F(Lifecycle, CleanupObserverCanInspectReportAndRecursiveCloseIsRefused) {
  std::optional<libtmux::CleanupHandle> cleanup;
  bool inspected = false;
  auto selected = Server::at_socket_path(
      fixture->socket_path(),
      [&](const libtmux::CommandReport&) {
        if (!cleanup)
          return;
        inspected = cleanup->report().in_progress;
        auto recursive = cleanup->close();
        EXPECT_FALSE(recursive);
        if (!recursive)
          EXPECT_EQ(recursive.error().kind, libtmux::FailureKind::overloaded);
      },
      policy());
  ASSERT_TRUE(selected);
  auto owner = libtmux::own_session(*selected, {.name = "observer"});
  ASSERT_TRUE(owner);
  cleanup = owner->cleanup();
  EXPECT_TRUE(owner->close());
  EXPECT_TRUE(inspected);
}

TEST_F(Lifecycle, FindObserverReentryReturnsWithoutDeadlock) {
  bool observed = false;
  bool active = true;
  auto selected = Server::at_socket_path(
      fixture->socket_path(),
      [&](const libtmux::CommandReport&) {
        if (!active)
          return;
        auto recursive =
            libtmux::find_or_create_session(*server, {.name = "recursive"});
        EXPECT_FALSE(recursive);
        if (!recursive)
          observed = recursive.error().primary.kind == libtmux::FailureKind::overloaded;
      },
      policy());
  ASSERT_TRUE(selected);
  auto result = libtmux::find_or_create_session(*selected, {.name = "outer"});
  ASSERT_TRUE(result);
  EXPECT_TRUE(observed);
  EXPECT_FALSE(server->session("recursive"));
  active = false;
}
} // namespace
