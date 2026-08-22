#pragma once

#include <optional>
#include <string>
#include <utility>

#include "libtmux/server.hpp"
#include "libtmux_consumers/mcp.hpp"
#include "protocol_types.hpp"

namespace libtmux::mcp::server {

struct CallRequest {
  json id;
  json params;
  std::optional<json> progress_token;
  ProtocolEra era{ProtocolEra::legacy};
  bool tool_input_errors_are_results{};
};

struct Route {
  std::optional<json> reply;
  std::optional<CallRequest> call;
  std::optional<json> cancellation;

  [[nodiscard]] static Route answering(json message);
  [[nodiscard]] static Route dispatching(CallRequest request);
  [[nodiscard]] static Route cancelling(json id);
};

class ProtocolSession {
public:
  explicit ProtocolSession(libtmux::Server server);

  [[nodiscard]] Route route(const json& request);
  [[nodiscard]] json execute(const CallRequest& request, const CallContext& context);
  [[nodiscard]] bool accepts_batches() const noexcept;
  [[nodiscard]] bool uses_legacy_request_ids() const noexcept;

private:
  enum class LegacyState : std::uint8_t { fresh, awaiting_initialized, ready };

  [[nodiscard]] Route initialize(const json& id, const json& params, bool notification);
  [[nodiscard]] Route initialized(const json& id, const json& params,
                                  bool notification);
  [[nodiscard]] Route discover(const json& id, const json& params, bool notification);
  [[nodiscard]] Route cancelled(const json& params, bool notification) const;
  [[nodiscard]] Route list_tools(const json& id, const json& params, bool notification,
                                 ProtocolEra era) const;
  [[nodiscard]] Route call_tool(const json& id, const json& params, bool notification,
                                ProtocolEra era) const;

  libtmux::Server server_;
  ToolSet tools_;
  ProtocolEra era_{ProtocolEra::undecided};
  LegacyState legacy_state_{LegacyState::fresh};
  std::string legacy_version_;
};

} // namespace libtmux::mcp::server
