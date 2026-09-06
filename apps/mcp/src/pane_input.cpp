#include "pane_input.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <tuple>
#include <utility>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace libtmux::mcp::detail {
namespace {

struct PaneInputIdentity {
  PaneInputEndpointIdentity endpoint;
  std::uint64_t server_pid{};
  std::uint64_t server_start_time{};
  std::string pane_id;

  auto operator<=>(const PaneInputIdentity&) const = default;
};

struct Reservation {
  std::uint64_t token{};
  PaneInputReservationKind kind{};
};

struct Registry {
  std::mutex mutex;
  std::map<PaneInputIdentity, Reservation> active;
  std::uint64_t next_token{};
};

[[nodiscard]] Registry& registry() {
  static Registry* const process_registry = new Registry;
  return *process_registry;
}

[[nodiscard]] bool canonical_pane_id(std::string_view value) {
  if (value.size() < 2U || value.front() != '%' ||
      (value.size() > 2U && value[1] == '0')) {
    return false;
  }
  std::uint32_t parsed = 0U;
  const char* const first = value.data() + 1U;
  const char* const last = value.data() + value.size();
  const auto answer = std::from_chars(first, last, parsed);
  return answer.ec == std::errc{} && answer.ptr == last;
}

[[nodiscard]] bool contains_control_byte(std::string_view value) {
  return std::ranges::any_of(value, [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte <= 0x1FU || byte == 0x7FU;
  });
}

[[nodiscard]] std::optional<PaneInputEndpointIdentity>
endpoint_identity(std::string_view endpoint) {
  const std::filesystem::path path{endpoint};
  if (endpoint.empty() || !path.is_absolute() || contains_control_byte(endpoint)) {
    return std::nullopt;
  }
#if defined(_WIN32)
  return std::nullopt;
#else
  struct stat status {};
  if (::stat(path.c_str(), &status) != 0 || !S_ISSOCK(status.st_mode)) {
    return std::nullopt;
  }
  return PaneInputEndpointIdentity{.device = static_cast<std::uintmax_t>(status.st_dev),
                                   .inode = static_cast<std::uintmax_t>(status.st_ino)};
#endif
}

[[nodiscard]] std::vector<PaneInputIdentity>
identities(const PaneInputEndpointIdentity& endpoint, std::uint64_t server_pid,
           std::uint64_t server_start_time, const std::vector<std::string>& pane_ids) {
  std::vector<PaneInputIdentity> answer;
  answer.reserve(pane_ids.size());
  for (const std::string& pane_id : pane_ids) {
    answer.push_back(PaneInputIdentity{.endpoint = endpoint,
                                       .server_pid = server_pid,
                                       .server_start_time = server_start_time,
                                       .pane_id = pane_id});
  }
  std::ranges::sort(answer);
  return answer;
}

[[nodiscard]] ToolError invalid_reservation() {
  return ToolError{false, "pane input reservation identity is invalid"};
}

[[nodiscard]] ToolError conflict(std::string_view pane_id, std::string_view tool_name,
                                 PaneInputReservationKind kind) {
  if (kind == PaneInputReservationKind::run) {
    return ToolError{false, std::string{tool_name} + " refuses pane " +
                                std::string{pane_id} +
                                " because a command started by this MCP process is "
                                "still active; wait for that run to finish"};
  }
  return ToolError{false, std::string{tool_name} + " refuses pane " +
                              std::string{pane_id} +
                              " because another pane-input dispatch is in progress; "
                              "wait for it to finish and retry"};
}

} // namespace

std::optional<PaneInputEndpointIdentity>
pane_input_endpoint_identity(std::string_view endpoint) {
  return endpoint_identity(endpoint);
}

class PaneInputLease::Impl {
public:
  Impl(Registry& owner, std::vector<PaneInputIdentity> identities, std::uint64_t token,
       PaneInputReservationKind kind)
      : owner_{owner}, identities_{std::move(identities)}, token_{token}, kind_{kind} {}

  void release() {
    std::lock_guard lock{owner_.mutex};
    if (released_) {
      return;
    }
    for (const PaneInputIdentity& identity : identities_) {
      const auto found = owner_.active.find(identity);
      if (found != owner_.active.end() && found->second.token == token_ &&
          found->second.kind == kind_) {
        owner_.active.erase(found);
      }
    }
    released_ = true;
  }

  [[nodiscard]] bool covers(std::vector<PaneInputIdentity> candidates) const {
    std::lock_guard lock{owner_.mutex};
    if (released_ || candidates != identities_) {
      return false;
    }
    return std::ranges::all_of(identities_, [&](const PaneInputIdentity& identity) {
      const auto found = owner_.active.find(identity);
      return found != owner_.active.end() && found->second.token == token_ &&
             found->second.kind == kind_;
    });
  }

private:
  Registry& owner_;
  std::vector<PaneInputIdentity> identities_;
  std::uint64_t token_{};
  PaneInputReservationKind kind_{};
  bool released_{};
};

PaneInputLease::PaneInputLease(std::unique_ptr<Impl> implementation)
    : implementation_{std::move(implementation)} {}

PaneInputLease::PaneInputLease(PaneInputLease&&) noexcept = default;

PaneInputLease& PaneInputLease::operator=(PaneInputLease&& other) noexcept {
  if (this != &other) {
    release();
    implementation_ = std::move(other.implementation_);
  }
  return *this;
}

PaneInputLease::~PaneInputLease() {
  if (implementation_ != nullptr) {
    implementation_->release();
  }
}

bool PaneInputLease::covers(std::string_view endpoint, std::uint64_t server_pid,
                            std::uint64_t server_start_time,
                            const std::vector<std::string>& pane_ids) const {
  const auto physical = endpoint_identity(endpoint);
  return implementation_ != nullptr && physical.has_value() &&
         implementation_->covers(
             identities(*physical, server_pid, server_start_time, pane_ids));
}

void PaneInputLease::release() {
  if (implementation_ != nullptr) {
    implementation_->release();
  }
}

void PaneInputLease::abandon() noexcept {
  static_cast<void>(implementation_.release());
}

libtmux::expected<PaneInputLease, ToolError>
reserve_pane_input(std::string endpoint, std::uint64_t server_pid,
                   std::uint64_t server_start_time, std::vector<std::string> pane_ids,
                   PaneInputReservationKind kind, std::string_view tool_name) {
  const auto physical = endpoint_identity(endpoint);
  if (!physical.has_value() || server_pid == 0U || server_start_time == 0U ||
      pane_ids.empty() || tool_name.empty() ||
      !std::ranges::all_of(pane_ids, canonical_pane_id)) {
    return libtmux::unexpected(invalid_reservation());
  }
  std::ranges::sort(pane_ids);
  if (std::ranges::adjacent_find(pane_ids) != pane_ids.end()) {
    return libtmux::unexpected(invalid_reservation());
  }
  std::vector<PaneInputIdentity> wanted =
      identities(*physical, server_pid, server_start_time, pane_ids);
  Registry& owner = registry();
  std::lock_guard lock{owner.mutex};
  for (const PaneInputIdentity& identity : wanted) {
    const auto found = owner.active.find(identity);
    if (found != owner.active.end()) {
      return libtmux::unexpected(
          conflict(identity.pane_id, tool_name, found->second.kind));
    }
  }
  const std::uint64_t token = ++owner.next_token;
  for (const PaneInputIdentity& identity : wanted) {
    owner.active.emplace(identity, Reservation{.token = token, .kind = kind});
  }
  return PaneInputLease{
      std::make_unique<PaneInputLease::Impl>(owner, std::move(wanted), token, kind)};
}

} // namespace libtmux::mcp::detail
