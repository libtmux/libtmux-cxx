#pragma once

// Crossing between this library's error types.
//
// There are two runtime failure types, because the two transports answer
// different questions: `CommandFailure` says whether tmux acted, and
// `ProtocolError` says what the control wire did. A caller that handles both
// surfaces wants one type, and wrote the same adapter to get it.
//
// The validation enums are a separate matter. Each names why a pure argument
// builder refused, and none of them ever reached tmux — which is why they are
// their own small types rather than failures with a delivery status. Folding
// one into a `CommandFailure` is always `validation` and `not_started`, and
// this is where that is written once.
//
// Its own header, not `command.hpp`: only a caller that crosses surfaces pays
// for including every error type at once.

#include "libtmux/abi.hpp"
#include "libtmux/cardinality.hpp"
#include "libtmux/command.hpp"
#include "libtmux/control.hpp"
#include "libtmux/delivery.hpp"
#include "libtmux/keys.hpp"
#include "libtmux/legacy_lookup.hpp"
#include "libtmux/socket.hpp"
#include "libtmux/target.hpp"
#include "libtmux/version.hpp"

#include <string>
#include <utility>

LIBTMUX_NAMESPACE_BEGIN

/// A control-wire failure as a command failure, keeping what it says about
// delivery — the one thing a caller cannot reconstruct. A protocol error that
// never started is a validation refusal; past that point the wire is what
// broke, which is `pipe`.
[[nodiscard]] inline CommandFailure as_command_failure(ProtocolError error) {
  return CommandFailure{.kind = error.delivery == DeliveryStatus::not_started
                                    ? FailureKind::validation
                                    : FailureKind::pipe,
                        .delivery = error.delivery,
                        .exit_code = -1,
                        .diagnostic = std::move(error.message)};
}

/// The other direction, for a caller handing a command failure to a surface
// that speaks the wire's type. The kind is dropped because the wire has no
// word for it; the diagnostic carries what it said.
[[nodiscard]] inline ProtocolError as_protocol_error(CommandFailure failure) {
  return ProtocolError{.message = std::move(failure.diagnostic),
                       .delivery = failure.delivery};
}

/// Which error types name a validation reason rather than a runtime failure.
// Opted in one by one rather than matched on being an enum: `FailureKind` and
// `DeliveryStatus` are enums too, and neither is a reason a call was refused.
template <typename Reason> inline constexpr bool is_validation_reason = false;
template <> inline constexpr bool is_validation_reason<TargetError> = true;
template <> inline constexpr bool is_validation_reason<SocketError> = true;
template <> inline constexpr bool is_validation_reason<CardinalityError> = true;
template <> inline constexpr bool is_validation_reason<LookupParseError> = true;
template <> inline constexpr bool is_validation_reason<VersionError> = true;
template <> inline constexpr bool is_validation_reason<KeyError> = true;

/// Why a pure argument builder refused, as a command failure. Nothing was
// dispatched, so the delivery is `not_started` and there is no exit status.
template <typename Reason>
  requires is_validation_reason<Reason>
[[nodiscard]] CommandFailure as_command_failure(Reason reason) {
  return CommandFailure{.kind = FailureKind::validation,
                        .delivery = DeliveryStatus::not_started,
                        .exit_code = 0,
                        .diagnostic = std::string{to_string(reason)}};
}

LIBTMUX_NAMESPACE_END
