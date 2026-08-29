#pragma once

// What this Server can promise without probing tmux.
//
// These describe the local backend contract, not the executable on PATH.
// An unrecognised backend reports no features, so every check says no.

#include "libtmux/abi.hpp"
#include <string_view>

LIBTMUX_NAMESPACE_BEGIN

enum class ServerImplementation {
  unknown,
  tmux,
  psmux,
};

[[nodiscard]] constexpr std::string_view
to_string(ServerImplementation implementation) noexcept {
  switch (implementation) {
  case ServerImplementation::unknown:
    return "unknown";
  case ServerImplementation::tmux:
    return "tmux";
  case ServerImplementation::psmux:
    return "psmux";
  }
  return "unknown";
}

enum class BackendKind {
  custom,
  subprocess,
};

[[nodiscard]] constexpr std::string_view to_string(BackendKind backend) noexcept {
  switch (backend) {
  case BackendKind::custom:
    return "custom";
  case BackendKind::subprocess:
    return "subprocess";
  }
  return "unknown";
}

// Coarse-grained promises callers choose around. Raw commands remain unchecked;
// the psmux preview promises only exact inspection and namespace cleanup.
enum class ServerFeature {
  // Exact session/window/pane listings, traversal, refresh, and format reads.
  exact_inspection,
  // Cleanup of the selected namespace under the Server's execution policy.
  server_cleanup,
  // `Server::window` and `Server::pane` without an owning session handle.
  server_entity_lookup,
  // Typed session creation with an attributable result.
  session_creation,
  // Typed window creation with an attributable result.
  window_creation,
  // Mutating an object captured in an earlier snapshot.
  captured_mutation,
  // Pane input, capture, copy mode, history, and piping.
  pane_io,
  // Producing argv that attaches a caller-owned terminal to a session.
  terminal_attach,
  // A reusable public target that identifies one captured window link.
  reusable_window_target,
  // Clients, buffers, commands, configuration, options, and hooks.
  server_state,
  // Latched `wait-for` channels.
  wait_channels,
  // This Server can open a persistent control connection.
  control_mode,
};

[[nodiscard]] constexpr std::string_view to_string(ServerFeature feature) noexcept {
  switch (feature) {
  case ServerFeature::exact_inspection:
    return "exact inspection";
  case ServerFeature::server_cleanup:
    return "server cleanup";
  case ServerFeature::server_entity_lookup:
    return "server-wide entity lookup";
  case ServerFeature::session_creation:
    return "session creation";
  case ServerFeature::window_creation:
    return "window creation";
  case ServerFeature::captured_mutation:
    return "captured entity mutation";
  case ServerFeature::pane_io:
    return "pane input and output";
  case ServerFeature::terminal_attach:
    return "terminal attachment";
  case ServerFeature::reusable_window_target:
    return "reusable window targets";
  case ServerFeature::server_state:
    return "server-scoped state";
  case ServerFeature::wait_channels:
    return "wait channels";
  case ServerFeature::control_mode:
    return "control mode";
  }
  return "unknown feature";
}

struct ServerCapabilities {
  ServerImplementation implementation{ServerImplementation::unknown};
  BackendKind backend{BackendKind::custom};

  // Purely local: this never launches tmux or touches a server.
  [[nodiscard]] constexpr bool supports(ServerFeature feature) const noexcept {
    if (implementation == ServerImplementation::unknown) {
      return false;
    }
    switch (feature) {
    case ServerFeature::exact_inspection:
    case ServerFeature::server_cleanup:
      return true;
    case ServerFeature::server_entity_lookup:
    case ServerFeature::session_creation:
    case ServerFeature::window_creation:
    case ServerFeature::captured_mutation:
    case ServerFeature::pane_io:
    case ServerFeature::terminal_attach:
    case ServerFeature::reusable_window_target:
    case ServerFeature::server_state:
    case ServerFeature::wait_channels:
    case ServerFeature::control_mode:
      return implementation == ServerImplementation::tmux;
    }
    return false;
  }
};

LIBTMUX_NAMESPACE_END
