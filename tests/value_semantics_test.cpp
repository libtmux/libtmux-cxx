// An entity is a value, and a failure is something a caller can say out loud.
//
// These are the six ordinary things a C++ caller does with a value the moment
// they have one — compare it, store it, key a map with it, print it, chain
// past a failure, and name that failure — and each of them is part of the
// surface that cannot change once the package is published.

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/capture.hpp"
#include "libtmux/cardinality.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/error.hpp"
#include "libtmux/server.hpp"
#include "libtmux/testing/scoped_server.hpp"
#include "libtmux/version.hpp"

namespace {

using libtmux::CommandFailure;
using libtmux::FailureKind;
using libtmux::Pane;
using libtmux::Server;
using libtmux::Session;
using libtmux::Window;

// What "an entity is a value" has to keep meaning, checked where it is cheap
// to check: at compile time, on every build, rather than by remembering.
//
// A member that is const or a reference silently takes assignment away, and
// one that allocates takes the nothrow move away — which is what lets a
// vector of these grow without copying. Neither shows up in a behaviour test.
template <typename T> constexpr bool is_an_entity_value() {
  static_assert(std::copy_constructible<T>);
  static_assert(std::is_copy_assignable_v<T>);
  static_assert(std::is_nothrow_move_constructible_v<T>);
  static_assert(std::is_nothrow_move_assignable_v<T>);
  static_assert(std::equality_comparable<T>);
  static_assert(requires(const T& value) { std::hash<T>{}(value); });
  // Deliberately not default constructible, so `std::regular` does not hold:
  // an entity is a row of a snapshot, and there is no such thing as one that
  // is not. A default-constructed entity could only be a null to check for.
  static_assert(!std::is_default_constructible_v<T>);
  // The representation the entity bake-off chose: a shared snapshot and which
  // of its rows this is, and nothing else. Stated structurally so it stays a
  // claim about the design rather than a byte count that drifts.
  static_assert(sizeof(T) <= sizeof(std::shared_ptr<void>) + sizeof(std::size_t));
  return true;
}

static_assert(is_an_entity_value<Session>());
static_assert(is_an_entity_value<Window>());
static_assert(is_an_entity_value<Pane>());
static_assert(is_an_entity_value<libtmux::Client>());

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

TEST(ValueSemantics, TheSameObjectFromTwoListingsIsOneValue) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  auto first = server.sessions();
  auto second = server.sessions();
  ASSERT_TRUE(first.has_value() && second.has_value());
  ASSERT_EQ(first->size(), 1U);

  // Different listings, different snapshots, same tmux object.
  EXPECT_EQ(first->at(0), second->at(0));

  const auto created = first->at(0).new_window("valued");
  ASSERT_TRUE(created.has_value()) << created.error().diagnostic;
  const auto renamed = created->rename("renamed");
  ASSERT_TRUE(renamed.has_value()) << renamed.error().diagnostic;
  const auto current = created->refresh();
  ASSERT_TRUE(current.has_value()) << current.error().diagnostic;
  const auto checked_target = created->checked_target();
  ASSERT_TRUE(checked_target.has_value()) << checked_target.error().diagnostic;
  EXPECT_EQ(*checked_target, created->target());

  // Equality is identity, not observation: a window that has been renamed is
  // still the window it was.
  EXPECT_EQ(*current, *created);
  EXPECT_NE(current->name(), created->name());
}

TEST(ValueSemantics, EntitiesKeyTheOrdinaryContainers) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value());
  ASSERT_TRUE(sessions->at(0).new_window("second").has_value());

  const auto windows = server.windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  ASSERT_EQ(windows->size(), 2U);

  std::unordered_set<Window> unique{windows->begin(), windows->end()};
  EXPECT_EQ(unique.size(), 2U);

  // Listed again, the same windows land on the entries already there.
  const auto again = server.windows();
  ASSERT_TRUE(again.has_value()) << again.error().diagnostic;
  for (const Window& window : *again) {
    EXPECT_TRUE(unique.contains(window)) << window.id() << " hashed elsewhere";
  }

  std::unordered_map<Window, std::string> named;
  for (const Window& window : *windows) {
    named.emplace(window, std::string{window.name()});
  }
  EXPECT_EQ(named.at(again->at(0)), again->at(0).name());
}

TEST(ValueSemantics, AnEntityPrintsAsSomethingAReaderRecognises) {
  const auto recorded = libtmux::Snapshot::from_recording(
      Pane::kFields,
      "%4␞nvim␞1␞@2␞$1␞0␞editor␞991␞/dev/pts/7␞/tmp␞80␞24␞0␞0␞1␞0␞1␞1␞0␞0␞0␞␞\n");
  ASSERT_NE(recorded, nullptr);

  std::ostringstream out;
  out << Pane{recorded, 0};
  EXPECT_EQ(out.str(), "Pane(%4 nvim)");

  // The formatter is the same renderer, so a caller building a message and a
  // caller writing to a stream cannot be shown two different panes.
  EXPECT_EQ(std::format("{}", Pane{recorded, 0}), out.str());
  EXPECT_EQ(libtmux::to_string(Pane{recorded, 0}), out.str());
  // Inheriting the string formatter is what keeps the spec working.
  EXPECT_EQ(std::format("[{:>14}]", Pane{recorded, 0}), "[ Pane(%4 nvim)]");
}

TEST(ValueSemantics, AFailureComposesAndCanBeNamed) {
  // The factory reports what every other call reports, so the first two calls
  // anyone writes chain instead of failing to compile.
  const auto sessions = Server::at_socket_path("").and_then(
      [](const Server& server) { return server.sessions(); });
  ASSERT_FALSE(sessions.has_value());
  EXPECT_EQ(sessions.error().kind, FailureKind::validation);
  EXPECT_EQ(sessions.error().delivery, libtmux::DeliveryStatus::not_started);

  // A failure says itself: what happened, what tmux said, and how far it got.
  const CommandFailure refused{.kind = FailureKind::refused,
                               .delivery = libtmux::DeliveryStatus::replied,
                               .exit_code = 1,
                               .diagnostic = "can't find session: nope"};
  EXPECT_EQ(std::format("{}", refused),
            "tmux refused the command: can't find session: nope (exit 1, replied)");
  EXPECT_EQ(libtmux::to_string(refused), std::format("{}", refused));
  // A failure that never reached tmux has no exit status. The backend writes
  // -1 there, and printing it as a code would report a run that never happened.
  EXPECT_EQ(
      std::format("{}", CommandFailure{.kind = FailureKind::spawn,
                                       .delivery = libtmux::DeliveryStatus::not_started,
                                       .exit_code = -1,
                                       .diagnostic = "fork failed"}),
      "tmux could not be started: fork failed (not_started)");

  // tmux answering is not enough on its own: a child killed by a signal has
  // no exit status either, and the transport marks that the same way.
  EXPECT_EQ(
      std::format("{}", CommandFailure{.kind = FailureKind::refused,
                                       .delivery = libtmux::DeliveryStatus::replied,
                                       .exit_code = -1,
                                       .diagnostic = "killed by a signal"}),
      "tmux refused the command: killed by a signal (replied)");

  // A status tmux never reached carries no exit code, so none is printed.
  EXPECT_EQ(
      std::format("{}", CommandFailure{.kind = FailureKind::validation,
                                       .delivery = libtmux::DeliveryStatus::not_started,
                                       .exit_code = 0,
                                       .diagnostic = {}}),
      "the request was rejected before tmux ran (not_started)");

  // Nothing here throws on its own, and a caller who wants an exception at a
  // boundary asks for one by name — the same name in either build.
  EXPECT_THROW(static_cast<void>(Server::at_socket_path("").value()),
               libtmux::bad_expected_access<CommandFailure>);

  // Every failure a caller can be handed says what it is.
  for (const FailureKind kind :
       {FailureKind::validation, FailureKind::spawn, FailureKind::pre_exec,
        FailureKind::pipe, FailureKind::timeout, FailureKind::refused,
        FailureKind::missing, FailureKind::truncated, FailureKind::unsupported}) {
    EXPECT_FALSE(libtmux::to_string(kind).empty());
  }
  EXPECT_EQ(static_cast<int>(FailureKind::truncated), 7);
  EXPECT_EQ(static_cast<int>(FailureKind::unsupported), 8);
  EXPECT_EQ(static_cast<int>(libtmux::BackendKind::custom), 0);
  EXPECT_EQ(static_cast<int>(libtmux::BackendKind::subprocess), 1);
  EXPECT_EQ(static_cast<int>(libtmux::BackendKind::control), 2);
  for (const auto implementation :
       {libtmux::ServerImplementation::unknown, libtmux::ServerImplementation::tmux,
        libtmux::ServerImplementation::psmux}) {
    EXPECT_FALSE(libtmux::to_string(implementation).empty());
  }
  for (const auto backend :
       {libtmux::BackendKind::custom, libtmux::BackendKind::subprocess,
        libtmux::BackendKind::control}) {
    EXPECT_FALSE(libtmux::to_string(backend).empty());
  }
  for (const auto feature : {
           libtmux::ServerFeature::exact_inspection,
           libtmux::ServerFeature::server_cleanup,
           libtmux::ServerFeature::server_entity_lookup,
           libtmux::ServerFeature::session_creation,
           libtmux::ServerFeature::window_creation,
           libtmux::ServerFeature::captured_mutation,
           libtmux::ServerFeature::pane_io,
           libtmux::ServerFeature::terminal_attach,
           libtmux::ServerFeature::reusable_window_target,
           libtmux::ServerFeature::server_state,
           libtmux::ServerFeature::wait_channels,
           libtmux::ServerFeature::control_mode,
       }) {
    EXPECT_FALSE(libtmux::to_string(feature).empty());
  }
  EXPECT_FALSE(libtmux::to_string(libtmux::CardinalityError::several_matched).empty());
  EXPECT_FALSE(libtmux::to_string(libtmux::SocketError::path_too_long).empty());
}

// The preprocessor's answer and the linker's must be the same answer.
//
// `LIBTMUX_VERSION_STRING` is written in the header so that `include/libtmux/`
// stays readable without CMake; `library_version()` is compiled from the
// `VERSION` file. Nothing but this ties them together, so a release that bumps
// one and forgets the other fails here rather than shipping a library that
// misreports itself to the preprocessor.
TEST(ValueSemantics, TheCompiledVersionMatchesTheLinkedOne) {
  EXPECT_EQ(std::string_view{LIBTMUX_VERSION_STRING}, libtmux::library_version());

  // And the numeric macros are that same string's leading components, so a
  // consumer branching on them is branching on the version it linked.
  const std::string expected = std::to_string(LIBTMUX_VERSION_MAJOR) + "." +
                               std::to_string(LIBTMUX_VERSION_MINOR) + "." +
                               std::to_string(LIBTMUX_VERSION_PATCH);
  EXPECT_TRUE(std::string_view{LIBTMUX_VERSION_STRING}.starts_with(expected))
      << LIBTMUX_VERSION_STRING << " does not begin with " << expected;
}

// The three shapes a capture shows that are not output.
TEST(ValueSemantics, OutputIsToldApartFromWhatIsMerelyOnScreen) {
  using libtmux::output_confirms;

  // Produced: the pane ran something and printed it above the prompt.
  EXPECT_TRUE(output_confirms("$ echo hi\nhi\n$ ", "hi"));

  // Typed, not yet run: the only occurrence is the row the cursor is on.
  EXPECT_FALSE(output_confirms("$ run-the-thing", "run-the-thing"));

  // Echoed: the shell repeated what we typed, and a redraw moved it off the
  // active row — row position alone would now credit it to the pane.
  EXPECT_FALSE(
      output_confirms("$ deploy now\n$ deploy now", "deploy now", {"deploy now"}));

  // Echoed and also produced: stripping our own bytes leaves the pane's.
  EXPECT_TRUE(output_confirms("$ echo deploy now\ndeploy now\n$ ", "deploy now",
                              {"echo deploy now"}));

  // An empty needle is not a match, however much text there is.
  EXPECT_FALSE(output_confirms("anything at all", ""));
}

// Two runtime failure types, because the two transports answer different
// questions — and a caller handling both wrote the same adapter to get one.
// What it must not lose is what each says about delivery.
TEST(ValueSemantics, ErrorsCrossBetweenSurfacesWithoutAnAdapter) {
  const auto broken = libtmux::as_command_failure(
      libtmux::ProtocolError{.message = "the wire stopped answering",
                             .delivery = libtmux::DeliveryStatus::written});
  EXPECT_EQ(broken.kind, libtmux::FailureKind::pipe);
  EXPECT_EQ(broken.delivery, libtmux::DeliveryStatus::written)
      << "whether tmux may have acted is the one thing a caller cannot rebuild";
  EXPECT_EQ(broken.diagnostic, "the wire stopped answering");

  // Never started is a refusal, not a broken pipe.
  const auto refused = libtmux::as_command_failure(
      libtmux::ProtocolError{.message = "control request group is empty",
                             .delivery = libtmux::DeliveryStatus::not_started});
  EXPECT_EQ(refused.kind, libtmux::FailureKind::validation);
  EXPECT_EQ(refused.delivery, libtmux::DeliveryStatus::not_started);

  const auto wired = libtmux::as_protocol_error(
      libtmux::CommandFailure{.kind = libtmux::FailureKind::timeout,
                              .delivery = libtmux::DeliveryStatus::indeterminate,
                              .exit_code = -1,
                              .diagnostic = "tmux did not answer in time"});
  EXPECT_EQ(wired.message, "tmux did not answer in time");
  EXPECT_EQ(wired.delivery, libtmux::DeliveryStatus::indeterminate);

  // A validation reason comes from a builder that never reached tmux, so it
  // always folds the same way and keeps its own words.
  const auto folded = libtmux::as_command_failure(libtmux::KeyError::unknown_name);
  EXPECT_EQ(folded.kind, libtmux::FailureKind::validation);
  EXPECT_EQ(folded.delivery, libtmux::DeliveryStatus::not_started);
  EXPECT_EQ(folded.exit_code, 0);
  EXPECT_EQ(folded.diagnostic, libtmux::to_string(libtmux::KeyError::unknown_name));
  EXPECT_EQ(libtmux::as_command_failure(libtmux::SocketError::path_too_long).diagnostic,
            libtmux::to_string(libtmux::SocketError::path_too_long));
  EXPECT_EQ(libtmux::as_command_failure(libtmux::TargetError::empty_name).diagnostic,
            libtmux::to_string(libtmux::TargetError::empty_name));

  // Only a validation reason opts in. `FailureKind` is an enum too, and is not
  // a reason a call was refused.
  static_assert(libtmux::is_validation_reason<libtmux::VersionError>);
  static_assert(!libtmux::is_validation_reason<libtmux::FailureKind>);
}

// Reading a field and filtering on it are one contract, not two. Every type
// the server can list appears below, including `Command` and `Buffer`, which
// have no namespace of their own: a gate that enumerates what is present is
// blind to what is absent.
TEST(ValueSemantics, EveryEntityFieldIsReachableFromAFilter) {
  const auto unreachable = [](const std::vector<std::string_view>& handled,
                              const auto& fields) {
    std::string absent;
    for (const std::string_view field : fields) {
      if (std::ranges::find(handled, field) == handled.end()) {
        absent += absent.empty() ? "" : ", ";
        absent += field;
      }
    }
    return absent;
  };

  namespace buffer = libtmux::buffer;
  namespace command = libtmux::command;
  namespace session = libtmux::session;
  namespace window = libtmux::window;
  namespace pane = libtmux::pane;
  namespace client = libtmux::client;

  EXPECT_EQ(unreachable({session::id.field.name, session::name.field.name,
                         session::attached.field.name, session::path.field.name,
                         session::group.field.name, session::grouped.field.name,
                         session::client_count.field.name,
                         session::window_count.field.name, session::created.field.name},
                        libtmux::Session::kFields),
            "")
      << "namespace session has no handle for these";

  EXPECT_EQ(unreachable({window::id.field.name, window::name.field.name,
                         window::active.field.name, window::session_id.field.name,
                         window::layout.field.name, window::zoomed.field.name,
                         window::bell.field.name, window::activity.field.name,
                         window::index.field.name, window::pane_count.field.name,
                         window::width.field.name, window::height.field.name,
                         window::linked_sessions.field.name},
                        libtmux::Window::kFields),
            "")
      << "namespace window has no handle for these";

  EXPECT_EQ(unreachable({pane::id.field.name,         pane::command.field.name,
                         pane::active.field.name,     pane::window_id.field.name,
                         pane::session_id.field.name, pane::title.field.name,
                         pane::tty.field.name,        pane::path.field.name,
                         pane::dead.field.name,       pane::in_mode.field.name,
                         pane::index.field.name,      pane::pid.field.name,
                         pane::width.field.name,      pane::height.field.name,
                         pane::at_top.field.name,     pane::at_bottom.field.name,
                         pane::at_left.field.name,    pane::at_right.field.name,
                         pane::piping.field.name,     pane::left.field.name,
                         pane::top.field.name,        pane::exit_status.field.name},
                        libtmux::Pane::kFields),
            "")
      << "namespace pane has no handle for these";

  EXPECT_EQ(unreachable({client::name.field.name, client::session_name.field.name,
                         client::read_only.field.name, client::tty.field.name,
                         client::terminal.field.name, client::control_mode.field.name,
                         client::width.field.name, client::height.field.name,
                         client::created.field.name, client::last_activity.field.name},
                        libtmux::Client::kFields),
            "")
      << "namespace client has no handle for these";

  EXPECT_EQ(unreachable({command::name.field.name, command::alias.field.name,
                         command::usage.field.name},
                        libtmux::Command::kFields),
            "")
      << "namespace command has no handle for these";

  EXPECT_EQ(unreachable({buffer::name.field.name, buffer::size.field.name,
                         buffer::sample.field.name, buffer::created.field.name},
                        libtmux::Buffer::kFields),
            "")
      << "namespace buffer has no handle for these";
}

TEST(ValueSemantics, CapabilitiesReportWhetherControlCanBeOpened) {
  const libtmux::ServerCapabilities subprocess{
      .implementation = libtmux::ServerImplementation::tmux,
      .backend = libtmux::BackendKind::subprocess};
  EXPECT_TRUE(subprocess.supports(libtmux::ServerFeature::control_mode));

  const libtmux::ServerCapabilities psmux{.implementation =
                                              libtmux::ServerImplementation::psmux,
                                          .backend = libtmux::BackendKind::subprocess};
  EXPECT_FALSE(psmux.supports(libtmux::ServerFeature::control_mode));
}

} // namespace
