#include "libtmux/snapshot.hpp"

#include <algorithm>

#include <string>
#include <utility>

#include "backend.hpp"

LIBTMUX_NAMESPACE_BEGIN

Snapshot::Snapshot(std::shared_ptr<const detail::Backend> backend,
                   std::vector<std::string_view> fields, std::string output)
    : backend_{std::move(backend)}, fields_{std::move(fields)},
      output_{std::move(output)} {}

Snapshot::~Snapshot() = default;

expected<std::shared_ptr<const Snapshot>, CommandFailure>
Snapshot::take(std::shared_ptr<const detail::Backend> backend,
               std::span<const std::string_view> fields,
               std::vector<std::string> request, FormatArgument placement) {
  if (placement == FormatArgument::flag) {
    // Before the terminator, not after it. `--` ends the flags: everything
    // past it is the caller's shell command and its arguments, so a `-F`
    // appended there is handed to that program instead of to tmux, which
    // then answers in its default shape and every field comes back missing.
    const auto terminator = std::ranges::find(request, "--");
    request.insert(terminator, {"-F", format_request(fields)});
  } else {
    request.push_back(format_request(fields));
  }

  auto output = backend->run(request);
  if (!output.has_value()) {
    return unexpected(output.error());
  }

  std::shared_ptr<Snapshot> snapshot{new Snapshot{
      std::move(backend), std::vector<std::string_view>{fields.begin(), fields.end()},
      *std::move(output)}};
  if (!snapshot->parse()) {
    return unexpected(
        CommandFailure{.kind = FailureKind::refused,
                       .dispatched = true,
                       .exit_code = 0,
                       .diagnostic = "tmux output did not match the fields asked for"});
  }
  return std::shared_ptr<const Snapshot>{std::move(snapshot)};
}

std::shared_ptr<const Snapshot>
Snapshot::from_recording(std::span<const std::string_view> fields, std::string output) {
  std::shared_ptr<Snapshot> snapshot{
      new Snapshot{nullptr, std::vector<std::string_view>{fields.begin(), fields.end()},
                   std::move(output)}};
  if (!snapshot->parse()) {
    return nullptr;
  }
  return snapshot;
}

std::size_t Snapshot::index_of(std::string_view field) const noexcept {
  for (std::size_t index = 0; index < fields_.size(); ++index) {
    if (fields_[index] == field) {
      return index;
    }
  }
  return fields_.size();
}

// Parse every line, keeping no partial rows when any line does not match the
// requested field count.
bool Snapshot::parse() {
  rows_.clear();
  std::vector<std::string_view> values;
  std::size_t position = 0;
  while (position < output_.size()) {
    const std::size_t end = output_.find('\n', position);
    const std::string_view line = std::string_view{output_}.substr(
        position, end == std::string::npos ? std::string::npos : end - position);
    if (!line.empty()) {
      if (!split_row(line, fields_.size(), values)) {
        rows_.clear();
        return false;
      }
      rows_.push_back(values);
    }
    if (end == std::string::npos) {
      break;
    }
    position = end + 1;
  }
  return true;
}

LIBTMUX_NAMESPACE_END
