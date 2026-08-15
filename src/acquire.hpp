#pragma once

// Turning one tmux command into entities.
//
// Both shapes are here because they fail differently. A listing that matches
// nothing is an empty list; a query about one named object that matches
// nothing is a missing object — and tmux does not distinguish them for us.

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libtmux/command.hpp"
#include "libtmux/expected.hpp"
#include "libtmux/snapshot.hpp"

#include "backend.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

// An entity read out of a recording has no server behind it. Reading and
// filtering still work; reaching tmux cannot.
[[nodiscard]] inline CommandFailure disconnected() {
  return CommandFailure{.kind = FailureKind::validation,
                        .dispatched = false,
                        .exit_code = 0,
                        .diagnostic =
                            "this snapshot has no connection to a tmux server"};
}

template <typename Entity>
[[nodiscard]] expected<std::vector<Entity>, CommandFailure>
list_entities(std::shared_ptr<const Backend> backend,
              std::vector<std::string> request) {
  if (backend == nullptr) {
    return unexpected(disconnected());
  }
  auto snapshot =
      Snapshot::take(std::move(backend), Entity::kFields, std::move(request));
  if (!snapshot.has_value()) {
    return unexpected(snapshot.error());
  }
  const std::size_t rows = (*snapshot)->rows().size();
  std::vector<Entity> entities;
  entities.reserve(rows);
  for (std::size_t row = 0; row < rows; ++row) {
    entities.emplace_back(*snapshot, row);
  }
  return entities;
}

// One object, named by `target`.
//
// `display-message` answers a question about an object tmux no longer has by
// printing empty fields and exiting zero, so the identity column is checked
// here rather than handing back an entity whose id is the empty string.
template <typename Entity>
[[nodiscard]] expected<Entity, CommandFailure>
one_entity(std::shared_ptr<const Backend> backend, std::vector<std::string> request,
           FormatArgument placement, std::string_view target,
           std::string_view expected_identity = {}) {
  if (backend == nullptr) {
    return unexpected(disconnected());
  }
  auto snapshot = Snapshot::take(std::move(backend), Entity::kFields,
                                 std::move(request), placement);
  if (!snapshot.has_value()) {
    return unexpected(snapshot.error());
  }
  const auto& rows = (*snapshot)->rows();
  // tmux answers a question about an object it cannot resolve in two ways,
  // both with a zero exit status: empty fields, or — for a target that names
  // a container it can resolve, such as `$0:@dead` — the fields of whatever
  // that container currently holds. Checking the identity that came back
  // catches the second, which is the one that would otherwise hand a caller
  // a different object under the name it asked for.
  if (rows.size() != 1 || rows.front().front().empty() ||
      (!expected_identity.empty() && rows.front().front() != expected_identity)) {
    return unexpected(CommandFailure{.kind = FailureKind::missing,
                                     .dispatched = true,
                                     .exit_code = 0,
                                     .diagnostic = "tmux has no " +
                                                   std::string{Entity::kNoun} + " " +
                                                   std::string{target}});
  }
  return Entity{*snapshot, 0};
}

// Append `-e NAME=VALUE` for each variable, refusing a name tmux would take
// and quietly do nothing with.
//
// tmux accepts a `-e` argument holding no `=` with a zero exit status and
// creates no variable, so a name carrying its own `=` -- or no name at all --
// has to be caught here. The value is unconstrained: an empty one sets the
// variable to empty, which is a thing a caller may mean.
[[nodiscard]] inline expected<void, CommandFailure>
append_environment(std::vector<std::string>& command,
                   const std::vector<std::pair<std::string, std::string>>& variables) {
  for (const auto& [name, value] : variables) {
    if (name.empty() || name.find('=') != std::string::npos) {
      return unexpected(CommandFailure{
          .kind = FailureKind::validation,
          .dispatched = false,
          .exit_code = 0,
          .diagnostic = "an environment variable name cannot be empty or hold "
                        "an '=': tmux would accept it and set nothing"});
    }
    command.emplace_back("-e");
    command.push_back(name + "=" + value);
  }
  return {};
}

// tmux ends an expansion with a newline of its own — exactly one, whatever
// the format ended with — so removing one is lossless.
[[nodiscard]] inline std::string without_trailing_newline(std::string text) {
  if (!text.empty() && text.back() == '\n') {
    text.pop_back();
  }
  return text;
}

// Expand a caller's format against one target.
//
// The trap `one_entity` guards is here too, and worse: the fields are the
// caller's, so there is no identity column in the answer to check. tmux
// resolves what it can, expands what it cannot to nothing, prints the
// literals in between and exits zero — so a dead target produces a
// well-formed answer that happens to be blank where the object used to be.
//
// Asking for the target's own id in front of the caller's format restores the
// column. An answer that does not open with it did not come from this object.
//
// Not a template: the only things an entity contributes are the name of its
// id field and the noun for the diagnostic, both of them string views. The
// body is compiled once and the three entity types share it.
[[nodiscard]] inline expected<std::string, CommandFailure>
expand_format(const std::shared_ptr<const Backend>& backend, std::string_view target,
              std::string_view identity_field, std::string_view noun,
              std::string_view format) {
  if (backend == nullptr) {
    return unexpected(disconnected());
  }
  std::string request{"#{"};
  request += identity_field;
  request += "}";
  request += kFormatSeparator;
  request += format;
  auto reply = backend->run(
      {"display-message", "-p", "-t", std::string{target}, "--", std::move(request)});
  if (!reply.has_value()) {
    return unexpected(reply.error());
  }
  std::string answer = without_trailing_newline(std::move(*reply));
  std::string opening{target};
  opening += kFormatSeparator;
  if (!answer.starts_with(opening)) {
    return unexpected(CommandFailure{.kind = FailureKind::missing,
                                     .dispatched = true,
                                     .exit_code = 0,
                                     .diagnostic = "tmux has no " + std::string{noun} +
                                                   " " + std::string{target}});
  }
  answer.erase(0, opening.size());
  return answer;
}

// The same call, naming the entity rather than its two constants, so a call
// site cannot pair one entity's id field with another's noun.
template <typename Entity>
[[nodiscard]] expected<std::string, CommandFailure>
expand_format(const std::shared_ptr<const Backend>& backend, std::string_view target,
              std::string_view format) {
  return expand_format(backend, target, Entity::kFields.front(), Entity::kNoun, format);
}

// Every to-one link and every lookup by target is the same tmux question:
// describe the object at this target. `display-message` answers it in one
// command for any entity kind, and resolves a target the way tmux does — a
// session target names that session's active pane, and its window.
template <typename Entity>
[[nodiscard]] expected<Entity, CommandFailure>
describe(const std::shared_ptr<const Backend>& backend, std::string_view target,
         std::string_view expected_identity = {}) {
  return one_entity<Entity>(backend,
                            {"display-message", "-p", "-t", std::string{target}},
                            FormatArgument::message, target, expected_identity);
}

} // namespace detail
LIBTMUX_NAMESPACE_END
