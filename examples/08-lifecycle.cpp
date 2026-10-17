// #region lifecycle
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

#include <libtmux/libtmux.hpp>

libtmux::CommandFailure report(const libtmux::LifecycleFailure& failure) {
  if (failure.rollback)
    std::cerr << "rollback: " << failure.rollback->diagnostic << '\n';
  return failure.primary;
}

int main() {
  auto server = libtmux::Server::at_default();
  if (!server) {
    std::cerr << server.error().diagnostic << '\n';
    return 1;
  }
  auto work = libtmux::own_session(
      *server, {.name = "libtmux-lifecycle-" + std::to_string(::getpid())});
  if (!work) {
    std::cerr << work.error().primary.diagnostic << '\n';
    if (work.error().rollback)
      std::cerr << work.error().rollback->diagnostic << '\n';
    return 1;
  }
  const auto result = libtmux::with_owned(
      *work,
      [&](const libtmux::Session& session)
          -> libtmux::expected<void, libtmux::CommandFailure> {
        auto build = libtmux::find_or_create_window(session, {.name = "build"});
        if (!build)
          return libtmux::unexpected(report(build.error()));
        auto reused = libtmux::find_or_create_window(session, {.name = "build"});
        if (!reused)
          return libtmux::unexpected(report(reused.error()));
        std::cout << "window created=" << build->created()
                  << " reused=" << !reused->created() << '\n';
        auto first = build->value.active_pane();
        if (!first)
          return libtmux::unexpected(first.error());
        auto borrowed = first->split();
        if (!borrowed)
          return libtmux::unexpected(borrowed.error());
        auto adopted = libtmux::adopt(*borrowed);
        if (!adopted)
          return libtmux::unexpected(report(adopted.error()));
        if (auto closed = adopted->close(); !closed)
          return closed;
        std::cout << "adopted pane cleaned=" << adopted->cleanup().report().complete
                  << '\n';
        const auto discovery = libtmux::discover_servers(
            {.roots = {std::filesystem::path{server->socket_path()}.parent_path()},
             .configured_roots = false});
        if (!discovery)
          return libtmux::unexpected(discovery.error());
        std::cout << discovery->servers.size()
                  << " server(s) in the selected directory\n";
        for (const auto& diagnostic : discovery->diagnostics) {
          if (diagnostic.failure)
            std::cerr << diagnostic.path << ": " << diagnostic.failure->diagnostic
                      << '\n';
        }
        if (build->owner)
          return build->owner->close();
        return {};
      });
  if (result.body_failure)
    std::cerr << result.body_failure->diagnostic << '\n';
  if (result.cleanup.failure)
    std::cerr << result.cleanup.failure->diagnostic << '\n';
  if (result.body_exception || result.cleanup.exception)
    std::cerr << "body or cleanup threw\n";
  return result.ok() ? 0 : 1;
}
// #endregion lifecycle
