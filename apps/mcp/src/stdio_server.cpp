#include "stdio_server.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <streambuf>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "batch_response.hpp"
#include "dispatcher.hpp"
#include "protocol.hpp"
#include "protocol_types.hpp"

namespace libtmux::mcp::server {
namespace {

enum class ReadStatus : std::uint8_t { line, too_long, end };

struct BoundedLine {
  ReadStatus status{ReadStatus::end};
  std::string text;
};

[[nodiscard]] bool response_message(const json& message) {
  return message.is_object() && !message.contains("method") &&
         (message.contains("result") || message.contains("error"));
}

[[nodiscard]] BoundedLine read_line(std::streambuf& input) {
  std::string line;
  bool oversized = false;
  bool saw_byte = false;
  while (true) {
    const auto value = input.sbumpc();
    if (value == std::char_traits<char>::eof()) {
      if (!saw_byte) {
        return {};
      }
      return {.status = oversized ? ReadStatus::too_long : ReadStatus::line,
              .text = std::move(line)};
    }
    saw_byte = true;
    const char byte = static_cast<char>(value);
    if (byte == '\n') {
      return {.status = oversized ? ReadStatus::too_long : ReadStatus::line,
              .text = std::move(line)};
    }
    if (line.size() < kMaximumLineBytes) {
      line.push_back(byte);
    } else {
      oversized = true;
    }
  }
}

[[nodiscard]] const json* identifier(const json& request) {
  if (!request.is_object()) {
    return nullptr;
  }
  const auto id = request.find("id");
  return id != request.end() && request_id(*id) ? &*id : nullptr;
}

[[nodiscard]] bool starts_legacy_lifecycle(const json& request) {
  if (!request.is_object()) {
    return false;
  }
  const auto method = request.find("method");
  return method != request.end() && method->is_string() && *method == "initialize";
}

void route_single(const json& request, ProtocolSession& session, Dispatcher& dispatcher,
                  Writer& writer) {
  if (response_message(request)) {
    return;
  }
  std::optional<RequestTicket> ticket;
  if (const json* id = identifier(request); id != nullptr) {
    RequestTicket reserved;
    if (auto refused = dispatcher.reserve(
            *id, session.uses_legacy_request_ids() || starts_legacy_lifecycle(request),
            reserved);
        refused.has_value()) {
      writer.send(*refused);
      return;
    }
    ticket = std::move(reserved);
  }

  Route action = session.route(request);
  if (action.reply.has_value()) {
    writer.send(*action.reply);
    if (ticket.has_value()) {
      dispatcher.release(*ticket);
    }
    return;
  }
  if (action.cancellation.has_value()) {
    dispatcher.cancel(*action.cancellation);
    return;
  }
  if (action.call.has_value() && ticket.has_value()) {
    const RequestTicket reserved = *ticket;
    Completion completion = [&writer, &dispatcher,
                             reserved](std::optional<json> response) {
      if (response.has_value()) {
        writer.send(*response);
      }
      dispatcher.release(reserved);
    };
    if (auto refused =
            dispatcher.submit(*std::move(action.call), reserved, std::move(completion));
        refused.has_value()) {
      writer.send(*refused);
      dispatcher.release(reserved);
    }
    return;
  }
  if (ticket.has_value()) {
    writer.send(
        failure(request.at("id"), kInvalidRequest, "request produced no response"));
    dispatcher.release(*ticket);
  }
}

void route_batch(const json& requests, ProtocolSession& session, Dispatcher& dispatcher,
                 Writer& writer) {
  if (requests.empty()) {
    writer.send(failure(nullptr, kInvalidRequest, "a JSON-RPC batch cannot be empty"));
    return;
  }
  if (!session.accepts_batches()) {
    writer.send(failure(nullptr, kInvalidRequest,
                        "JSON-RPC batches require negotiated MCP 2025-03-26"));
    return;
  }

  auto batch = std::make_shared<BatchResponse>(requests.size(), writer, dispatcher);
  std::unordered_map<std::string, std::size_t> occurrences;
  for (const json& request : requests) {
    if (response_message(request)) {
      continue;
    }
    if (const json* id = identifier(request); id != nullptr) {
      ++occurrences[request_key(*id)];
    }
  }

  std::vector<std::optional<RequestTicket>> tickets(requests.size());
  std::vector<std::optional<json>> preflight_errors(requests.size());
  std::unordered_set<std::string> reserved_duplicates;
  for (std::size_t index = 0; index < requests.size(); ++index) {
    const json& request = requests[index];
    if (response_message(request)) {
      continue;
    }
    if (const json* id = identifier(request); id != nullptr) {
      const std::string key = request_key(*id);
      if (occurrences.at(key) > 1U) {
        if (reserved_duplicates.insert(key).second) {
          RequestTicket reserved;
          if (!dispatcher.reserve(*id, true, reserved).has_value()) {
            batch->add_ticket(std::move(reserved));
          }
        }
        preflight_errors[index] =
            failure(*id, kInvalidRequest, "duplicate request id in batch");
        continue;
      }
      RequestTicket reserved;
      if (auto refused = dispatcher.reserve(*id, true, reserved); refused.has_value()) {
        preflight_errors[index] = *std::move(refused);
        continue;
      }
      batch->add_ticket(reserved);
      tickets[index] = std::move(reserved);
    }
  }

  for (std::size_t index = 0; index < requests.size(); ++index) {
    const json& request = requests[index];
    if (response_message(request)) {
      batch->settle(index, std::nullopt);
      continue;
    }
    if (preflight_errors[index].has_value()) {
      batch->settle(index, *std::move(preflight_errors[index]));
      continue;
    }

    Route action = session.route(request);
    if (action.reply.has_value()) {
      batch->settle(index, *std::move(action.reply));
      continue;
    }
    if (action.cancellation.has_value()) {
      dispatcher.cancel(*action.cancellation);
      batch->settle(index, std::nullopt);
      continue;
    }
    if (action.call.has_value() && tickets[index].has_value()) {
      Completion completion = [batch, index](std::optional<json> response) {
        batch->settle(index, std::move(response));
      };
      if (auto refused = dispatcher.submit(*std::move(action.call), *tickets[index],
                                           std::move(completion));
          refused.has_value()) {
        batch->settle(index, *std::move(refused));
      }
      continue;
    }
    if (tickets[index].has_value()) {
      batch->settle(index, failure(request.at("id"), kInvalidRequest,
                                   "request produced no response"));
    } else {
      batch->settle(index, std::nullopt);
    }
  }
  batch->seal();
}

} // namespace

int serve_stdio(libtmux::Server server) {
  std::ios::sync_with_stdio(false);
  Writer writer;
  ProtocolSession session{std::move(server)};
  Dispatcher dispatcher{session, writer};
  std::streambuf& input = *std::cin.rdbuf();
  while (true) {
    BoundedLine line = read_line(input);
    if (line.status == ReadStatus::end) {
      break;
    }
    if (line.status == ReadStatus::too_long) {
      writer.send(failure(nullptr, kInvalidRequest, "request line too long"));
      continue;
    }
    if (line.text.empty()) {
      continue;
    }
    try {
      const json request = json::parse(line.text);
      if (request.is_array()) {
        route_batch(request, session, dispatcher, writer);
      } else {
        route_single(request, session, dispatcher, writer);
      }
    } catch (const json::exception& error) {
      writer.send(failure(nullptr, kParseError, error.what()));
    }
  }
  dispatcher.finish();
  return 0;
}

} // namespace libtmux::mcp::server
