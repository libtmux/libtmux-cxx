#pragma once

#include <chrono>
#include <functional>
#include <iosfwd>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace libtmux::workspace::cli {
using Json = nlohmann::ordered_json;
struct Failure : std::runtime_error {
  int exit_code;
  std::string code;
  Json retained_state;
  Failure(int status, std::string category, std::string message)
      : std::runtime_error{std::move(message)}, exit_code{status},
        code{std::move(category)} {}
};
struct Request {
  std::string command;
  std::string importer;
  std::map<std::string, std::vector<std::string>> values;
  bool json{}, ndjson{}, terminal_allowed{};
  bool machine() const { return json || ndjson; }
  bool flag(const std::string& key) const { return values.contains(key); }
  std::string value(const std::string& key, std::string fallback = {}) const {
    const auto found = values.find(key);
    return found == values.end() || found->second.empty() ? fallback
                                                          : found->second.back();
  }
  std::vector<std::string> list(const std::string& key) const {
    const auto found = values.find(key);
    return found == values.end() ? std::vector<std::string>{} : found->second;
  }
};
using EventSink = std::function<void(const std::string&, Json)>;
struct ChildOutput {
  int code;
  std::string out;
  std::string err;
};
std::vector<std::string> split_command(const std::string& value);
ChildOutput
run_child(const std::vector<std::string>& arguments, bool terminal = false,
          std::optional<std::chrono::milliseconds> timeout = std::chrono::seconds{5});
Json execute(const Request& request, const EventSink& event);
std::string encoded(const Json& value, int indent = -1);
std::string human_result(const Request& request, const Json& result, bool colour);
void validate(const Request& request);
} // namespace libtmux::workspace::cli
