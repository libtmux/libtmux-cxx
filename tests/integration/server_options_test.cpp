// Options and hooks at the scopes only the server has.
//
// tmux keeps options at four scopes and resolves a lookup up through them, so
// a session-scope test says nothing about the server scope: different command
// flags, a different store, and different defaults. `options_test.cpp` covers
// the session; these are the calls that reach past it.

#include <algorithm>
#include <string>

#include <gtest/gtest.h>

#include "libtmux/command.hpp"
#include "libtmux/options.hpp"
#include "libtmux/server.hpp"
#include "libtmux/testing/scoped_server.hpp"

namespace {

using libtmux::OptionEntry;
using libtmux::Server;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

const OptionEntry* find(const std::vector<OptionEntry>& entries,
                        std::string_view name) {
  const auto found = std::ranges::find_if(
      entries, [&](const OptionEntry& entry) { return entry.name == name; });
  return found == entries.end() ? nullptr : &*found;
}

TEST(ServerOptions, SetsAndReadsBackAServerScopeOption) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // A server option with a value tmux will not normalise away.
  const auto set = server.set_server_option("buffer-limit", "37");
  ASSERT_TRUE(set.has_value()) << set.error().diagnostic;

  const auto entries = server.server_options();
  ASSERT_TRUE(entries.has_value()) << entries.error().diagnostic;
  const OptionEntry* entry = find(*entries, "buffer-limit");
  ASSERT_NE(entry, nullptr) << "buffer-limit absent from the server scope";
  EXPECT_EQ(entry->value, "37");
}

TEST(ServerOptions, GlobalScopeIsNotTheServerScope) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto server_scope = server.server_options();
  ASSERT_TRUE(server_scope.has_value()) << server_scope.error().diagnostic;
  const auto global_scope = server.global_options();
  ASSERT_TRUE(global_scope.has_value()) << global_scope.error().diagnostic;

  // `buffer-limit` is a server option and `status` a global session one, so
  // each scope holds what the other does not. Reading one for the other is
  // the mistake this separates.
  EXPECT_NE(find(*server_scope, "buffer-limit"), nullptr);
  EXPECT_EQ(find(*server_scope, "status"), nullptr);
  EXPECT_NE(find(*global_scope, "status"), nullptr);
}

TEST(ServerOptions, ResolvesAnOptionForAParticularTarget) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // No target is the server's own resolution; a session target resolves up
  // through the scopes tmux stacks for it.
  const auto resolved = server.options(std::string{fixture->session_name()});
  ASSERT_TRUE(resolved.has_value()) << resolved.error().diagnostic;
  EXPECT_FALSE(resolved->empty());
  EXPECT_NE(find(*resolved, "status"), nullptr)
      << "a session target should resolve session options";
}

TEST(ServerHooks, SetsAGlobalHookAndReadsItBack) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto set = server.set_global_hook("alert-bell", "display-message 'hooked'");
  ASSERT_TRUE(set.has_value()) << set.error().diagnostic;

  const auto hooks = server.global_hooks();
  ASSERT_TRUE(hooks.has_value()) << hooks.error().diagnostic;
  const OptionEntry* entry = find(*hooks, "alert-bell");
  ASSERT_NE(entry, nullptr) << "the hook just set is not in the global scope";
  EXPECT_NE(entry->value.find("hooked"), std::string::npos) << entry->value;
}

// Hooks do not resolve up through scopes the way options do.
//
// `show-options -t <session>` answers with what the session resolves to,
// global values included. `show-hooks -t <session>` answers with what is set
// on that session and nothing else, so a global hook is invisible there. The
// two commands read alike and do not behave alike, which is worth having
// written down somewhere that fails if tmux ever changes its mind.
TEST(ServerHooks, ATargetSeesOnlyItsOwnHooksNotTheGlobalOnes) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto set = server.set_global_hook("alert-activity", "display-message 'a'");
  ASSERT_TRUE(set.has_value()) << set.error().diagnostic;

  const auto global = server.global_hooks();
  ASSERT_TRUE(global.has_value()) << global.error().diagnostic;
  EXPECT_NE(find(*global, "alert-activity"), nullptr);

  const auto scoped = server.hooks(std::string{fixture->session_name()});
  ASSERT_TRUE(scoped.has_value()) << scoped.error().diagnostic;
  EXPECT_EQ(find(*scoped, "alert-activity"), nullptr)
      << "a global hook appeared at session scope; tmux stopped scoping hooks "
         "differently from options";
}

TEST(ServerOptions, RefusesAnOptionTmuxDoesNotKnow) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto refused = server.set_server_option("not-an-option", "1");
  ASSERT_FALSE(refused.has_value());
  // tmux ran and said no, which is not the same as the request being malformed.
  EXPECT_TRUE(refused.error().dispatched);
  EXPECT_FALSE(refused.error().diagnostic.empty());
}

} // namespace
