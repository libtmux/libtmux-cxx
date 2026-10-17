#include "admission.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <string_view>

#include "process.hpp"

LIBTMUX_NAMESPACE_BEGIN
namespace detail {

namespace {

constexpr std::string_view kUnknownCommand = "unknown command: ";

std::string text_of(const std::vector<std::byte>& bytes) {
  std::string out;
  out.reserve(bytes.size());
  for (const std::byte byte : bytes) {
    out.push_back(static_cast<char>(byte));
  }
  return out;
}

std::string_view trimmed(std::string_view text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                           text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  return text;
}

template <typename Visit> void for_each_line(std::string_view text, Visit&& visit) {
  while (!text.empty()) {
    const std::size_t end = text.find('\n');
    std::string_view line = text.substr(0, end);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    visit(line);
    if (end == std::string_view::npos) {
      break;
    }
    text.remove_prefix(end + 1);
  }
}

FailureKind failure_kind(ProcessError::Kind kind) noexcept {
  switch (kind) {
  case ProcessError::Kind::validation:
    return FailureKind::validation;
  case ProcessError::Kind::spawn:
    return FailureKind::spawn;
  case ProcessError::Kind::pre_exec:
    return FailureKind::pre_exec;
  case ProcessError::Kind::pipe:
  case ProcessError::Kind::output_limit:
    return FailureKind::pipe;
  case ProcessError::Kind::timeout:
    return FailureKind::timeout;
  case ProcessError::Kind::cancelled:
    return FailureKind::cancelled;
  }
  return FailureKind::spawn;
}

CommandFailure process_failure(ProcessError error) {
  return CommandFailure{.kind = failure_kind(error.kind),
                        .delivery = DeliveryStatus::not_started,
                        .exit_code = -1,
                        .diagnostic =
                            "tmux did not report its version before dispatch: " +
                            std::move(error.diagnostic)};
}

// Characters a version token may carry into a format string.
bool plain_token(std::string_view token) {
  return !token.empty() && token.size() <= 64U &&
         std::ranges::all_of(token, [](char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z') || character == '.' ||
                  character == '-';
         });
}

} // namespace

std::string version_marker() {
  static constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                               '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::random_device entropy;
  std::string marker = "libtmux-version-";
  for (int index = 0; index < 4; ++index) {
    const std::uint32_t word = entropy();
    for (int nibble = 0; nibble < 8; ++nibble) {
      marker.push_back(digits[(word >> (nibble * 4)) & 0xFU]);
    }
  }
  return marker;
}

CommandFailure admission_failure(FailureKind kind, std::string diagnostic) {
  return CommandFailure{.kind = kind,
                        .delivery = DeliveryStatus::not_started,
                        .exit_code = 0,
                        .diagnostic = std::move(diagnostic)};
}

std::optional<std::string> probe_report(std::string_view text,
                                        std::string_view marker) {
  const std::string prefix = std::string{kUnknownCommand} + std::string{marker};
  std::optional<std::string> report;
  bool repeated = false;
  for_each_line(text, [&](std::string_view line) {
    if (!line.starts_with(prefix)) {
      return;
    }
    if (report.has_value()) {
      repeated = true;
    }
    // A launching executor appends where the command came from.
    std::string_view version = line.substr(prefix.size());
    version = version.substr(0, version.find(" (running: "));
    report = std::string{version};
  });
  if (repeated) {
    return std::nullopt;
  }
  return report;
}

bool endpoint_is_absent(std::string_view text) {
  if (text.find("no server running") != std::string_view::npos) {
    return true;
  }
  return text.find("error connecting to") != std::string_view::npos &&
         (text.find("No such file or directory") != std::string_view::npos ||
          text.find("Connection refused") != std::string_view::npos);
}

std::optional<std::string> executable_report(std::string_view output,
                                             const std::filesystem::path& binary) {
  if (const auto end = output.find_first_of("\r\n"); end != std::string_view::npos) {
    output = output.substr(0, end);
  }
  const std::string name = binary.filename().string();
  for (const std::string& prefix : {name + " ", std::string{"tmux "}}) {
    if (prefix.size() > 1U && output.starts_with(prefix)) {
      return std::string{trimmed(output.substr(prefix.size()))};
    }
  }
  return std::nullopt;
}

expected<EndpointVersion, CommandFailure> read_report(std::string_view token,
                                                      bool live) {
  token = trimmed(token);
  auto parsed = parse_version("tmux " + std::string{token});
  if (!plain_token(token) || !parsed.has_value()) {
    return unexpected(admission_failure(FailureKind::unsupported,
                                        "libtmux cannot read the tmux version '" +
                                            std::string{token} + "' reported by " +
                                            (live ? "the daemon" : "the executable")));
  }
  return EndpointVersion{.live = live, .version = *parsed, .token = std::string{token}};
}

expected<void, CommandFailure> require_supported(const EndpointVersion& endpoint,
                                                 const Version& floor) {
  const std::string whom = endpoint.live ? "the daemon" : "the executable";
  if (endpoint.version.unbounded) {
    return unexpected(admission_failure(
        FailureKind::unsupported,
        "libtmux cannot order the tmux version '" + endpoint.token + "' reported by " +
            whom + "; an undated master build does not establish a release"));
  }
  if (endpoint.version < floor) {
    return unexpected(admission_failure(FailureKind::unsupported,
                                        "libtmux requires tmux 3.3a or newer; " + whom +
                                            " reports '" + endpoint.token + "'"));
  }
  return {};
}

expected<EndpointVersion, CommandFailure>
query_endpoint(const std::filesystem::path& binary,
               const std::vector<std::string>& selectors,
               std::optional<std::chrono::milliseconds> timeout) {
  const std::string marker = version_marker() + "-";
  ProcessRequest probe;
  probe.executable = binary;
  probe.timeout = timeout;
  probe.capture_limit = 64U * 1024U;
  for (const std::string& selector : selectors) {
    probe.arguments.push_back(Argument{selector});
  }
  probe.arguments.push_back(Argument{"-N"});
  probe.arguments.push_back(Argument{"run-shell"});
  probe.arguments.push_back(Argument{"-C"});
  probe.arguments.push_back(Argument{marker + "#{version}"});
  auto reply = run_process(probe);
  if (!reply.has_value()) {
    return unexpected(process_failure(std::move(reply.error())));
  }
  const std::string error_text = text_of(reply->stderr_bytes);
  const auto* exited = std::get_if<Exited>(&reply->termination);
  if (exited != nullptr && exited->code == 1) {
    if (auto report = probe_report(error_text, marker); report.has_value()) {
      return read_report(*report, true);
    }
  }
  if (endpoint_is_absent(error_text)) {
    ProcessRequest version;
    version.executable = binary;
    version.timeout = timeout;
    version.capture_limit = 64U * 1024U;
    version.arguments.push_back(Argument{"-V"});
    auto printed = run_process(version);
    if (!printed.has_value()) {
      return unexpected(process_failure(std::move(printed.error())));
    }
    const auto* ran = std::get_if<Exited>(&printed->termination);
    const auto report = ran != nullptr && ran->code == 0
                            ? executable_report(text_of(printed->stdout_bytes), binary)
                            : std::nullopt;
    if (!report.has_value()) {
      return unexpected(admission_failure(
          FailureKind::unsupported, "libtmux cannot establish the tmux version from '" +
                                        binary.string() + " -V' before dispatch"));
    }
    return read_report(*report, false);
  }
  std::string detail{trimmed(error_text)};
  if (detail.empty()) {
    detail = "tmux did not report a daemon version";
  }
  return unexpected(admission_failure(FailureKind::refused, std::move(detail)));
}

std::vector<Argument> version_fence(std::string_view token, std::string_view marker) {
  std::vector<Argument> fence;
  fence.push_back(Argument{"if-shell"});
  fence.push_back(Argument{"-F"});
  fence.push_back(Argument{"#{==:#{version}," + std::string{token} + "}"});
  fence.push_back(Argument{""});
  fence.push_back(Argument{std::string{marker}});
  fence.push_back(Argument{";"});
  return fence;
}

bool fence_fired(std::string_view stderr_text, std::string_view marker) {
  const std::string wanted = std::string{kUnknownCommand} + std::string{marker};
  bool fired = false;
  for_each_line(stderr_text, [&](std::string_view line) {
    if (line == wanted) {
      fired = true;
    }
  });
  return fired;
}

CommandFailure replaced_daemon() {
  return admission_failure(
      FailureKind::unsupported,
      "the tmux daemon changed after libtmux admitted it; no command ran, so "
      "retrying admits the new daemon");
}

} // namespace detail
LIBTMUX_NAMESPACE_END
