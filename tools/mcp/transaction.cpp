#include "mcp_swap.hpp"

#include "storage.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <random>
#include <ranges>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

namespace libtmux::mcp_swap {
namespace {

using detail::BoundFile;
using detail::FileBinding;
using detail::MissingBinding;
using detail::PathClaim;
using detail::TransactionLock;
using Json = nlohmann::json;

constexpr int state_version = 2;
constexpr std::string_view backup_marker = ".bak.mcp-swap-cxx-";

struct RecoveryEntry {
  std::filesystem::path config_path;
  std::filesystem::path backup_path;
  std::string server;
  ChangeAction action{};
  std::string swapped_at;
  std::uint64_t sequence{};
  std::filesystem::path target_path;
  std::string original_digest;
  unsigned original_mode{};
  std::string expected_digest;
  unsigned expected_mode{};
  FileBinding config_binding;
  FileBinding backup_binding;
};

struct StateSnapshot {
  std::map<std::string, RecoveryEntry, std::less<>> entries;
  std::optional<Json> transaction;
  std::optional<std::string> raw;
  std::optional<FileBinding> binding;
  std::optional<MissingBinding> missing;
};

struct ProtectedRoute {
  std::string owner;
  std::filesystem::path path;
  std::optional<BoundFile> file;
  std::optional<MissingBinding> missing;
};

struct ApplyItem {
  Client client;
  Scope scope{};
  std::string key;
  std::string label;
  BoundFile before;
  std::string after;
  ChangeAction action{};
  std::filesystem::path backup_path;
  std::optional<MissingBinding> backup_missing;
  std::optional<BoundFile> backup;
  std::optional<RecoveryEntry> prior;
  std::optional<FileBinding> after_binding;
};

struct RecoveryAuthentication {
  std::map<std::string, BoundFile, std::less<>> backups;
  std::map<std::string, BoundFile, std::less<>> current;
  std::map<std::string, std::vector<std::string>, std::less<>> groups;
};

struct ApplyPlan {
  std::filesystem::path repository;
  std::string server;
  ServerSpec spec;
  std::vector<std::pair<std::string, ServerSpec>> preflights;
  std::vector<ApplyItem> items;
  std::vector<ProtectedRoute> protected_routes;
  RecoveryAuthentication recovery;
  StateSnapshot state;
  std::string timestamp;
};

struct RevertLayer {
  std::string key;
  std::string label;
  RecoveryEntry entry;
  BoundFile backup;
};

struct RevertTarget {
  Client client;
  std::filesystem::path config_path;
  BoundFile current;
  std::vector<RevertLayer> layers;
  std::string restored_bytes;
  unsigned restored_mode{};
  std::optional<FileBinding> restored_binding;
};

struct RevertPlan {
  StateSnapshot state;
  std::vector<RevertTarget> targets;
  std::vector<ProtectedRoute> protected_routes;
  RecoveryAuthentication recovery;
  std::set<std::string, std::less<>> selected;
};

std::filesystem::path swap_directory(const Runtime& runtime) {
  return runtime.paths.state_home / "libtmux-mcp-dev/swap";
}

std::filesystem::path state_directory(const Runtime& runtime) {
  return swap_directory(runtime) / "cxx";
}

std::filesystem::path state_path(const Runtime& runtime) {
  return state_directory(runtime) / "state.json";
}

std::string scope_name(Scope scope) {
  return scope == Scope::user ? "user" : "project";
}

Scope normalized_scope(const Client& client, std::optional<Scope> scope) {
  if (client.name != "claude") {
    return Scope::user;
  }
  return scope.value_or(Scope::project);
}

std::string recovery_key(const Client& client, Scope scope) {
  return client.name + ":" + scope_name(scope);
}

std::string recovery_label(const Client& client, Scope scope) {
  return client.name == "claude" ? recovery_key(client, scope) : client.name;
}

Json entry_json(const RecoveryEntry& entry) {
  return {{"config_path", entry.config_path.string()},
          {"backup_path", entry.backup_path.string()},
          {"server", entry.server},
          {"action", entry.action == ChangeAction::replaced ? "replaced" : "added"},
          {"swapped_at", entry.swapped_at},
          {"seq_no", entry.sequence},
          {"target_path", entry.target_path.string()},
          {"original_sha256", entry.original_digest},
          {"original_mode", entry.original_mode},
          {"expected_sha256", entry.expected_digest},
          {"expected_mode", entry.expected_mode},
          {"config_binding", detail::to_json(entry.config_binding)},
          {"backup_binding", detail::to_json(entry.backup_binding)}};
}

std::string required_string(const Json& value, std::string_view key) {
  const auto found = value.find(key);
  if (found == value.end() || !found->is_string() ||
      found->get_ref<const std::string&>().empty()) {
    throw Error{"recovery entry has no valid " + std::string{key}};
  }
  return found->get<std::string>();
}

unsigned required_mode(const Json& value, std::string_view key) {
  const auto found = value.find(key);
  if (found == value.end() || !found->is_number_unsigned()) {
    throw Error{"recovery entry has no valid " + std::string{key}};
  }
  const auto mode = found->get<unsigned>();
  if (mode > 0777U) {
    throw Error{"recovery entry has an invalid mode"};
  }
  return mode;
}

bool digest_valid(std::string_view value) {
  return value.size() == 64U && std::ranges::all_of(value, [](char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

RecoveryEntry entry_from_json(const Json& value) {
  static const std::set<std::string, std::less<>> keys{
      "config_path",   "backup_path",     "server",        "action",
      "swapped_at",    "seq_no",          "target_path",   "original_sha256",
      "original_mode", "expected_sha256", "expected_mode", "config_binding",
      "backup_binding"};
  if (!value.is_object() || value.size() != keys.size() ||
      !std::ranges::all_of(keys,
                           [&](const auto& key) { return value.contains(key); })) {
    throw Error{"recovery entry has unknown or missing fields"};
  }
  RecoveryEntry result;
  result.config_path = required_string(value, "config_path");
  result.backup_path = required_string(value, "backup_path");
  result.server = required_string(value, "server");
  const auto action = required_string(value, "action");
  if (action == "replaced") {
    result.action = ChangeAction::replaced;
  } else if (action == "added") {
    result.action = ChangeAction::added;
  } else {
    throw Error{"recovery entry has an invalid action"};
  }
  result.swapped_at = required_string(value, "swapped_at");
  if (!value["seq_no"].is_number_unsigned()) {
    throw Error{"recovery entry has an invalid sequence"};
  }
  result.sequence = value["seq_no"].get<std::uint64_t>();
  result.target_path = required_string(value, "target_path");
  result.original_digest = required_string(value, "original_sha256");
  result.expected_digest = required_string(value, "expected_sha256");
  if (!digest_valid(result.original_digest) || !digest_valid(result.expected_digest)) {
    throw Error{"recovery entry has an invalid digest"};
  }
  result.original_mode = required_mode(value, "original_mode");
  result.expected_mode = required_mode(value, "expected_mode");
  result.config_binding = detail::binding_from_json(value["config_binding"]);
  result.backup_binding = detail::binding_from_json(value["backup_binding"]);
  if (!result.config_path.is_absolute() || !result.backup_path.is_absolute() ||
      !result.target_path.is_absolute() ||
      result.target_path != result.config_binding.resolved ||
      result.expected_mode != result.config_binding.mode ||
      result.backup_binding.mode != 0600U) {
    throw Error{"recovery entry paths or modes are inconsistent"};
  }
  return result;
}

Json state_payload(const std::map<std::string, RecoveryEntry, std::less<>>& entries,
                   const std::optional<Json>& transaction) {
  Json encoded_entries = Json::object();
  for (const auto& [key, entry] : entries) {
    encoded_entries[key] = entry_json(entry);
  }
  return {{"version", state_version},
          {"entries", std::move(encoded_entries)},
          {"transaction", transaction.has_value() ? *transaction : Json(nullptr)}};
}

std::string
encode_state(const std::map<std::string, RecoveryEntry, std::less<>>& entries,
             const std::optional<Json>& transaction) {
  auto payload = state_payload(entries, transaction);
  auto document = payload;
  document["checksum"] = detail::sha256(payload.dump());
  return document.dump(2) + "\n";
}

bool recovery_key_valid(std::string_view key) {
  const auto colon = key.find(':');
  if (colon == std::string_view::npos || key.find(':', colon + 1) != key.npos) {
    return false;
  }
  static constexpr std::array clients{"claude", "codex", "cursor",   "gemini",
                                      "grok",   "agy",   "opencode", "pi"};
  const auto name = key.substr(0, colon);
  const auto scope = key.substr(colon + 1);
  return std::ranges::find(clients, name) != clients.end() &&
         (scope == "user" || (name == "claude" && scope == "project"));
}

std::optional<Json> parse_transaction(const Json& raw) {
  if (raw.is_null()) {
    return std::nullopt;
  }
  static const std::set<std::string, std::less<>> transaction_keys{"id", "kind",
                                                                   "changes"};
  if (!raw.is_object() || raw.size() != transaction_keys.size() ||
      !std::ranges::all_of(transaction_keys,
                           [&](const auto& key) { return raw.contains(key); })) {
    throw Error{"swap state has an invalid pending transaction"};
  }
  const auto identifier = required_string(raw, "id");
  const auto kind = required_string(raw, "kind");
  if (identifier.size() != 32U || !digest_valid(identifier + std::string(32, '0')) ||
      (kind != "apply" && kind != "revert") || !raw["changes"].is_array() ||
      raw["changes"].empty()) {
    throw Error{"swap state has an invalid pending transaction"};
  }
  static const std::set<std::string, std::less<>> change_keys{
      "key",         "config_path",    "target_path",  "before_sha256",
      "before_mode", "before_binding", "after_sha256", "after_mode",
      "backup_path", "backup_binding"};
  std::set<std::string, std::less<>> keys;
  for (const auto& change : raw["changes"]) {
    if (!change.is_object() || change.size() != change_keys.size() ||
        !std::ranges::all_of(change_keys,
                             [&](const auto& key) { return change.contains(key); })) {
      throw Error{"swap state has an invalid pending change"};
    }
    const auto key = required_string(change, "key");
    const auto config = std::filesystem::path{required_string(change, "config_path")};
    const auto target = std::filesystem::path{required_string(change, "target_path")};
    const auto backup = std::filesystem::path{required_string(change, "backup_path")};
    const auto before = detail::binding_from_json(change["before_binding"]);
    const auto before_mode = required_mode(change, "before_mode");
    const auto after_mode = required_mode(change, "after_mode");
    const auto before_digest = required_string(change, "before_sha256");
    const auto after_digest = required_string(change, "after_sha256");
    if (!recovery_key_valid(key) || !keys.insert(key).second || !config.is_absolute() ||
        !target.is_absolute() || !backup.is_absolute() || before.resolved != target ||
        before.mode != before_mode || !digest_valid(before_digest) ||
        !digest_valid(after_digest)) {
      throw Error{"swap state has an invalid pending change"};
    }
    static_cast<void>(after_mode);
    if (!change["backup_binding"].is_null()) {
      const auto binding = detail::binding_from_json(change["backup_binding"]);
      if (binding.mode != 0600U) {
        throw Error{"swap state has an invalid pending backup"};
      }
    }
  }
  return raw;
}

std::pair<std::map<std::string, RecoveryEntry, std::less<>>, std::optional<Json>>
decode_state(std::string_view raw) {
  Json document;
  try {
    document = Json::parse(raw);
  } catch (const nlohmann::json::exception& error) {
    throw Error{"swap state is not valid JSON: " + std::string{error.what()}};
  }
  if (!document.is_object() || document.size() != 4 || !document.contains("version") ||
      !document.contains("entries") || !document.contains("transaction") ||
      !document.contains("checksum") || document["version"] != state_version ||
      !document["entries"].is_object() || !document["checksum"].is_string()) {
    throw Error{"swap state has unknown, missing, or invalid fields"};
  }
  auto payload = document;
  const auto checksum = payload["checksum"].get<std::string>();
  payload.erase("checksum");
  if (!digest_valid(checksum) || checksum != detail::sha256(payload.dump())) {
    throw Error{"swap state checksum does not match its contents"};
  }
  auto transaction = parse_transaction(document["transaction"]);
  std::map<std::string, RecoveryEntry, std::less<>> entries;
  std::set<std::uint64_t> sequences;
  for (const auto& [key, encoded] : document["entries"].items()) {
    if (!recovery_key_valid(key)) {
      throw Error{"swap state has an invalid recovery key"};
    }
    auto entry = entry_from_json(encoded);
    if (!sequences.insert(entry.sequence).second) {
      throw Error{"swap state has duplicate recovery sequence numbers"};
    }
    entries.emplace(key, std::move(entry));
  }
  return {std::move(entries), std::move(transaction)};
}

StateSnapshot load_state(const Runtime& runtime, TransactionLock* lock = nullptr) {
  const auto path = state_path(runtime);
  if (!detail::lexists(path)) {
    StateSnapshot missing;
    missing.missing = detail::capture_missing(path);
    return missing;
  }
  const auto file = detail::read_bound(path, lock);
  detail::validate_private_path(path, file.binding);
  const auto [entries, transaction] = decode_state(file.bytes);
  return {entries, transaction, file.bytes, file.binding, std::nullopt};
}

void validate_state_snapshot(const Runtime& runtime, const StateSnapshot& snapshot,
                             TransactionLock* lock) {
  const auto path = state_path(runtime);
  if (snapshot.raw.has_value() && snapshot.binding.has_value()) {
    detail::validate_bound(path, *snapshot.raw, *snapshot.binding, lock);
    detail::validate_private_path(path, *snapshot.binding);
    static_cast<void>(decode_state(*snapshot.raw));
    return;
  }
  if (snapshot.missing.has_value()) {
    detail::validate_missing(path, *snapshot.missing);
    return;
  }
  throw Error{"swap state has no authenticated destination"};
}

StateSnapshot
write_state(const Runtime& runtime, TransactionLock& lock,
            const std::map<std::string, RecoveryEntry, std::less<>>& entries,
            const std::optional<Json>& transaction, const StateSnapshot& expected,
            const std::function<void()>& validate_world = {}) {
  runtime.boundary("before-state-write", state_path(runtime));
  lock.validate();
  validate_state_snapshot(runtime, expected, &lock);
  if (validate_world) {
    validate_world();
  }
  const auto raw = encode_state(entries, transaction);
  detail::ensure_private_directory(state_directory(runtime));
  detail::atomic_write(
      state_path(runtime), raw, 0600U,
      expected.binding.has_value() ? std::optional{expected.binding->target}
                                   : std::nullopt,
      expected.binding.has_value() ? expected.binding->parent_identity
                                   : expected.missing->parent_identity);
  auto file = detail::read_bound(state_path(runtime), &lock);
  detail::validate_private_path(state_path(runtime), file.binding);
  static_cast<void>(decode_state(file.bytes));
  if (file.bytes != raw) {
    throw Error{"swap state changed while it was written"};
  }
  return {entries, transaction, std::move(file.bytes), std::move(file.binding),
          std::nullopt};
}

StateSnapshot remove_state(const Runtime& runtime, TransactionLock& lock,
                           const StateSnapshot& expected,
                           const std::function<void()>& validate_world = {}) {
  runtime.boundary("before-state-remove", state_path(runtime));
  lock.validate();
  validate_state_snapshot(runtime, expected, &lock);
  if (validate_world) {
    validate_world();
  }
  if (!expected.raw.has_value() || !expected.binding.has_value()) {
    throw Error{"swap state has no file to remove"};
  }
  detail::remove_bound(state_path(runtime), *expected.raw, *expected.binding, &lock);
  StateSnapshot missing;
  missing.missing = detail::capture_missing(state_path(runtime));
  return missing;
}

const Client& client_for_key(std::span<const Client> clients, std::string_view key) {
  const auto colon = key.find(':');
  const auto name = key.substr(0, colon);
  const auto found = std::ranges::find(clients, name, &Client::name);
  if (found == clients.end()) {
    throw Error{"swap state names an unknown client: " + std::string{name}};
  }
  if (name != "claude" && key.substr(colon + 1) != "user") {
    throw Error{"swap state gives a non-Claude client project scope"};
  }
  return *found;
}

PathClaim state_claim(const StateSnapshot& state) {
  if (state.binding.has_value()) {
    return {"swap", "state", state.binding->resolved, state.binding->target};
  }
  if (state.missing.has_value()) {
    return {"swap", "state", state.missing->resolved, std::nullopt};
  }
  throw Error{"swap state has no authenticated claim"};
}

RecoveryAuthentication authenticate_recovery(const StateSnapshot& state,
                                             std::span<const Client> clients,
                                             TransactionLock* lock) {
  if (state.transaction.has_value()) {
    throw Error{
        "swap state records an interrupted transaction; inspect it before retrying"};
  }
  RecoveryAuthentication result;
  std::vector<PathClaim> claims{state_claim(state)};
  if (lock != nullptr) {
    claims.push_back(
        {"swap", "lock", lock->binding().resolved, lock->binding().target});
  }
  for (const auto& [key, entry] : state.entries) {
    const auto& client = client_for_key(clients, key);
    if (detail::absolute_logical(entry.config_path) !=
        detail::absolute_logical(client.config_path)) {
      throw Error{key + " config path changed"};
    }
    auto backup = detail::read_bound(entry.backup_path, lock);
    detail::validate_private_path(entry.backup_path, backup.binding);
    if (backup.binding != entry.backup_binding ||
        detail::sha256(backup.bytes) != entry.original_digest ||
        backup.binding.mode != 0600U) {
      throw Error{key + " backup changed after the swap"};
    }
    result.backups.emplace(key, backup);
    const auto logical = detail::absolute_logical(entry.config_path).string();
    result.groups[logical].push_back(key);
    claims.push_back({key, "backup", backup.binding.resolved, backup.binding.target});
  }
  for (auto& [logical, keys] : result.groups) {
    std::ranges::sort(keys, std::greater{}, [&](const std::string& key) {
      return state.entries.at(key).sequence;
    });
    const auto& top = state.entries.at(keys.front());
    auto current = detail::read_bound(top.config_path, lock);
    if (current.binding != top.config_binding ||
        current.binding.resolved != top.target_path ||
        current.binding.mode != top.expected_mode ||
        detail::sha256(current.bytes) != top.expected_digest) {
      throw Error{keys.front() + " configuration changed after the swap"};
    }
    for (std::size_t index = 0; index + 1 < keys.size(); ++index) {
      const auto& above = state.entries.at(keys[index]);
      const auto& below = state.entries.at(keys[index + 1]);
      const auto& backup = result.backups.at(keys[index]);
      if (detail::sha256(backup.bytes) != below.expected_digest ||
          above.original_mode != below.expected_mode ||
          above.target_path != below.target_path) {
        throw Error{keys[index] + " breaks the recovery chain"};
      }
    }
    result.current.emplace(logical, current);
    claims.push_back({keys.front(), "configuration", current.binding.resolved,
                      current.binding.target});
  }
  detail::validate_distinct(claims);
  return result;
}

std::vector<ProtectedRoute>
capture_known_routes(std::span<const Client> clients,
                     const std::set<std::string, std::less<>>& selected_paths,
                     TransactionLock* lock) {
  std::vector<ProtectedRoute> routes;
  for (const auto& client : clients) {
    const auto logical = detail::absolute_logical(client.config_path).string();
    if (selected_paths.contains(logical)) {
      continue;
    }
    if (detail::lexists(client.config_path)) {
      routes.push_back({client.name, client.config_path,
                        detail::read_bound(client.config_path, lock), std::nullopt});
    } else {
      routes.push_back({client.name, client.config_path, std::nullopt,
                        detail::capture_missing(client.config_path)});
    }
  }
  return routes;
}

void validate_protected(std::span<const ProtectedRoute> routes, TransactionLock* lock) {
  for (const auto& route : routes) {
    if (route.file.has_value()) {
      detail::validate_bound(route.path, route.file->bytes, route.file->binding, lock);
    } else if (route.missing.has_value()) {
      detail::validate_missing(route.path, *route.missing);
    } else {
      throw Error{"protected route has no authenticated state"};
    }
  }
}

std::filesystem::path next_backup_path(const std::filesystem::path& config,
                                       std::string_view timestamp, const Client& client,
                                       Scope scope,
                                       std::set<std::filesystem::path>& reserved) {
  auto base = std::filesystem::path{
      config.string() + std::string{backup_marker} + std::string{timestamp} +
      (client.name == "claude" ? "-" + scope_name(scope) : "")};
  auto candidate = base;
  std::size_t attempt = 0;
  while (reserved.contains(detail::absolute_logical(candidate)) ||
         detail::lexists(candidate)) {
    candidate = base.string() + "-" + std::to_string(++attempt);
  }
  reserved.insert(detail::absolute_logical(candidate));
  return candidate;
}

void validate_plan_claims(const Runtime& runtime, const StateSnapshot& state,
                          const RecoveryAuthentication& recovery,
                          std::span<const ApplyItem> items,
                          std::span<const ProtectedRoute> protected_routes,
                          const TransactionLock* lock) {
  static_cast<void>(runtime);
  std::vector<PathClaim> claims{state_claim(state)};
  if (lock != nullptr) {
    claims.push_back(
        {"swap", "lock", lock->binding().resolved, lock->binding().target});
  }
  for (const auto& item : items) {
    claims.push_back({item.label, "configuration", item.before.binding.resolved,
                      item.before.binding.target});
    if (item.backup_missing.has_value()) {
      claims.push_back(
          {item.label, "backup", item.backup_missing->resolved, std::nullopt});
    }
  }
  for (const auto& [key, backup] : recovery.backups) {
    claims.push_back({key, "backup", backup.binding.resolved, backup.binding.target});
  }
  for (const auto& route : protected_routes) {
    if (route.file.has_value()) {
      claims.push_back({route.owner, "protected configuration",
                        route.file->binding.resolved, route.file->binding.target});
    } else {
      claims.push_back({route.owner, "protected configuration", route.missing->resolved,
                        std::nullopt});
    }
  }
  detail::validate_distinct(claims);
}

bool points_at(const ServerSpec& current, const ServerSpec& replacement,
               std::span<const std::pair<std::string, std::string>> overrides) {
  if (current.command != replacement.command ||
      current.arguments != replacement.arguments) {
    return false;
  }
  const std::map<std::string, std::string, std::less<>> environment(
      current.environment.begin(), current.environment.end());
  if (environment.contains("LIBTMUX_SAFETY") &&
      std::ranges::any_of(overrides, [](const auto& pair) {
        return pair.first == "LIBTMUX_TOOLSETS";
      })) {
    return false;
  }
  return std::ranges::all_of(overrides, [&](const auto& pair) {
    const auto found = environment.find(pair.first);
    return found != environment.end() && found->second == pair.second;
  });
}

ApplyPlan prepare_apply(const Runtime& runtime, const std::filesystem::path& repository,
                        std::string server, const ServerSpec& spec,
                        std::span<const std::pair<std::string, std::string>> overrides,
                        std::span<const Client> selected,
                        std::optional<Scope> requested_scope, std::string timestamp,
                        TransactionLock* lock) {
  auto clients = known_clients(runtime.paths);
  auto state = load_state(runtime, lock);
  const auto recovery = authenticate_recovery(state, clients, lock);
  std::vector<ApplyItem> items;
  std::vector<std::pair<std::string, ServerSpec>> preflights;
  std::set<std::filesystem::path> reserved;
  std::set<std::string, std::less<>> selected_paths;
  for (const auto& client : selected) {
    const auto logical = detail::absolute_logical(client.config_path).string();
    if (!selected_paths.insert(logical).second) {
      throw Error{"selected clients duplicate one configuration path"};
    }
    if (!detail::lexists(client.config_path)) {
      throw Error{"[" + client.name + "] config not found at " +
                  client.config_path.string()};
    }
    const auto before = detail::read_bound(client.config_path, lock);
    const auto scope = normalized_scope(client, requested_scope);
    const auto key = recovery_key(client, scope);
    const auto label = recovery_label(client, scope);
    const auto current = read_server(client, before.bytes, server, repository, scope);
    if (current.has_value() && points_at(*current, spec, overrides)) {
      preflights.emplace_back(label, *current);
      continue;
    }
    const auto rendered =
        render_server(client, before.bytes, server, spec, repository, scope);
    preflights.emplace_back(label, rendered.effective);
    ApplyItem item;
    item.client = client;
    item.scope = scope;
    item.key = key;
    item.label = label;
    item.before = before;
    item.after = rendered.bytes;
    item.action = rendered.action;
    const auto prior = state.entries.find(key);
    if (prior != state.entries.end()) {
      const auto group = recovery.groups.find(logical);
      if (group == recovery.groups.end() || group->second.front() != key) {
        throw Error{"[" + item.label + "] is not the active recovery layer"};
      }
      item.prior = prior->second;
      item.backup_path = prior->second.backup_path;
      item.backup = recovery.backups.at(key);
    } else {
      item.backup_path =
          next_backup_path(client.config_path, timestamp, client, scope, reserved);
      item.backup_missing = detail::capture_missing(item.backup_path);
    }
    items.push_back(std::move(item));
  }
  std::set<std::string, std::less<>> changed_paths;
  for (const auto& item : items) {
    changed_paths.insert(detail::absolute_logical(item.client.config_path).string());
  }
  auto protected_routes = capture_known_routes(clients, changed_paths, lock);
  validate_plan_claims(runtime, state, recovery, items, protected_routes, lock);
  return {
      repository,          std::move(server),           spec,     std::move(preflights),
      std::move(items),    std::move(protected_routes), recovery, std::move(state),
      std::move(timestamp)};
}

Json pending_apply(const ApplyPlan& plan, std::string_view id) {
  auto changes = Json::array();
  for (const auto& item : plan.items) {
    changes.push_back({{"key", item.key},
                       {"config_path", item.client.config_path.string()},
                       {"target_path", item.before.binding.resolved.string()},
                       {"before_sha256", detail::sha256(item.before.bytes)},
                       {"before_mode", item.before.binding.mode},
                       {"before_binding", detail::to_json(item.before.binding)},
                       {"after_sha256", detail::sha256(item.after)},
                       {"after_mode", item.before.binding.mode},
                       {"backup_path", item.backup_path.string()},
                       {"backup_binding", item.backup.has_value()
                                              ? detail::to_json(item.backup->binding)
                                              : Json(nullptr)}});
  }
  return {{"id", id}, {"kind", "apply"}, {"changes", std::move(changes)}};
}

void validate_apply_world(const Runtime& runtime, TransactionLock& lock,
                          const ApplyPlan& plan, const StateSnapshot& state,
                          std::size_t applied, bool include_protected = true,
                          bool include_missing = true) {
  lock.validate();
  validate_state_snapshot(runtime, state, &lock);
  if (include_protected) {
    validate_protected(plan.protected_routes, &lock);
  }
  for (const auto& [key, backup] : plan.recovery.backups) {
    static_cast<void>(key);
    detail::validate_bound(backup.binding.resolved, backup.bytes, backup.binding,
                           &lock);
    detail::validate_private_path(backup.binding.resolved, backup.binding);
  }
  for (std::size_t index = 0; index < plan.items.size(); ++index) {
    const auto& item = plan.items[index];
    if (index < applied) {
      if (!item.after_binding.has_value()) {
        throw Error{"applied config has no authenticated identity"};
      }
      detail::validate_bound(item.client.config_path, item.after, *item.after_binding,
                             &lock);
    } else {
      detail::validate_bound(item.client.config_path, item.before.bytes,
                             item.before.binding, &lock);
    }
    if (item.backup.has_value()) {
      detail::validate_bound(item.backup_path, item.backup->bytes, item.backup->binding,
                             &lock);
      detail::validate_private_path(item.backup_path, item.backup->binding);
    } else if (include_missing && item.backup_missing.has_value()) {
      detail::validate_missing(item.backup_path, *item.backup_missing);
    }
  }
}

std::uint64_t
next_sequence(const std::map<std::string, RecoveryEntry, std::less<>>& entries) {
  std::uint64_t result = 0;
  for (const auto& [key, entry] : entries) {
    static_cast<void>(key);
    result = std::max(result, entry.sequence + 1U);
  }
  return result;
}

void remove_new_backups(const Runtime& runtime, TransactionLock& lock, ApplyPlan& plan,
                        const StateSnapshot& state) {
  for (auto item = plan.items.rbegin(); item != plan.items.rend(); ++item) {
    if (item->prior.has_value() || !item->backup.has_value()) {
      continue;
    }
    runtime.boundary("before-backup-remove", item->backup_path);
    validate_apply_world(runtime, lock, plan, state, 0, false, false);
    detail::remove_bound(item->backup_path, item->backup->bytes, item->backup->binding,
                         &lock);
    item->backup.reset();
  }
}

StateSnapshot restore_prior_state(const Runtime& runtime, TransactionLock& lock,
                                  const ApplyPlan& plan, const StateSnapshot& pending) {
  auto entries = plan.state.entries;
  for (const auto& item : plan.items) {
    if (!item.prior.has_value()) {
      continue;
    }
    auto& entry = entries.at(item.key);
    if (detail::sha256(item.before.bytes) != entry.expected_digest ||
        item.before.binding.mode != entry.expected_mode) {
      throw Error{"[" + item.label + "] rollback does not match prior recovery"};
    }
    entry.config_binding = item.before.binding;
    entry.target_path = item.before.binding.resolved;
  }
  if (entries.empty()) {
    return remove_state(runtime, lock, pending, [&] {
      validate_apply_world(runtime, lock, plan, pending, 0, false, false);
    });
  }
  return write_state(runtime, lock, entries, std::nullopt, pending, [&] {
    validate_apply_world(runtime, lock, plan, pending, 0, false, false);
  });
}

int apply_plan(Runtime& runtime, TransactionLock& lock, ApplyPlan& plan,
               std::ostream& output, std::ostream& error) {
  if (plan.items.empty()) {
    return 0;
  }
  auto pending = write_state(
      runtime, lock, plan.state.entries, pending_apply(plan, runtime.transaction_id()),
      plan.state, [&] { validate_apply_world(runtime, lock, plan, plan.state, 0); });
  std::size_t applied = 0;
  try {
    for (auto& item : plan.items) {
      if (item.backup.has_value()) {
        continue;
      }
      runtime.boundary("before-backup-write", item.backup_path);
      validate_apply_world(runtime, lock, plan, pending, applied);
      detail::validate_missing(item.backup_path, *item.backup_missing);
      const auto binding = detail::create_private_file(
          item.backup_path, item.before.bytes, *item.backup_missing);
      item.backup = BoundFile{item.before.bytes, binding};
    }
    pending = write_state(
        runtime, lock, plan.state.entries,
        pending_apply(plan, pending.transaction->at("id").get<std::string>()), pending,
        [&] { validate_apply_world(runtime, lock, plan, pending, applied); });
    validate_apply_world(runtime, lock, plan, pending, applied);
    auto entries = plan.state.entries;
    auto sequence = next_sequence(entries);
    for (auto& item : plan.items) {
      runtime.boundary("before-config-write", item.client.config_path);
      validate_apply_world(runtime, lock, plan, pending, applied);
      detail::atomic_write(item.before.binding.resolved, item.after,
                           item.before.binding.mode, item.before.binding.target,
                           item.before.binding.parent_identity);
      const auto written = detail::read_bound(item.client.config_path, &lock);
      if (written.bytes != item.after ||
          written.binding.mode != item.before.binding.mode) {
        throw Error{"[" + item.label + "] config changed while it was written"};
      }
      static_cast<void>(read_server(item.client, written.bytes, plan.server,
                                    plan.repository, item.scope));
      item.after_binding = written.binding;
      ++applied;
      RecoveryEntry entry;
      if (item.prior.has_value()) {
        entry = *item.prior;
      } else {
        entry.config_path = detail::absolute_logical(item.client.config_path);
        entry.backup_path = detail::absolute_logical(item.backup_path);
        entry.server = plan.server;
        entry.action = item.action;
        entry.swapped_at = plan.timestamp;
        entry.sequence = sequence++;
        entry.original_digest = detail::sha256(item.before.bytes);
        entry.original_mode = item.before.binding.mode;
      }
      entry.target_path = written.binding.resolved;
      entry.expected_digest = detail::sha256(written.bytes);
      entry.expected_mode = written.binding.mode;
      entry.config_binding = written.binding;
      entry.backup_binding = item.backup->binding;
      entries[item.key] = std::move(entry);
    }
    static_cast<void>(write_state(runtime, lock, entries, std::nullopt, pending, [&] {
      validate_apply_world(runtime, lock, plan, pending, applied);
    }));
    for (const auto& item : plan.items) {
      output << '[' << item.label << "] "
             << (item.action == ChangeAction::replaced ? "replaced" : "added") << "; "
             << (item.prior.has_value() ? "pre-swap backup kept: " : "backup: ")
             << item.backup_path << '\n';
    }
    return 0;
  } catch (const std::exception& failure) {
    std::string diagnostic = failure.what();
    try {
      for (std::size_t index = applied; index > 0; --index) {
        auto& item = plan.items[index - 1];
        runtime.boundary("before-config-rollback", item.client.config_path);
        validate_apply_world(runtime, lock, plan, pending, applied);
        detail::atomic_write(item.after_binding->resolved, item.before.bytes,
                             item.before.binding.mode, item.after_binding->target,
                             item.after_binding->parent_identity);
        const auto restored = detail::read_bound(item.client.config_path, &lock);
        if (restored.bytes != item.before.bytes ||
            restored.binding.mode != item.before.binding.mode) {
          throw Error{"[" + item.label + "] rollback did not restore the config"};
        }
        item.before.binding = restored.binding;
        item.after_binding.reset();
        --applied;
      }
      remove_new_backups(runtime, lock, plan, pending);
      static_cast<void>(restore_prior_state(runtime, lock, plan, pending));
    } catch (const std::exception& rollback) {
      diagnostic += "; rollback failed: " + std::string{rollback.what()};
    }
    error << "swap failed: " << diagnostic << '\n';
    return 1;
  }
}

std::filesystem::path resolve_from(const std::filesystem::path& path,
                                   const std::filesystem::path& base) {
  return detail::absolute_logical(path.is_absolute() ? path : base / path);
}

std::filesystem::path find_repository(const Runtime& runtime,
                                      const std::filesystem::path& requested) {
  auto candidate = resolve_from(requested, runtime.paths.working_directory);
  while (true) {
    if (std::filesystem::is_regular_file(candidate / "apps/mcp/CMakeLists.txt")) {
      return std::filesystem::canonical(candidate);
    }
    if (candidate == candidate.root_path()) {
      break;
    }
    candidate = candidate.parent_path();
  }
  throw Error{"no libtmux checkout at or above " + requested.string()};
}

std::string read_unbound(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    throw Error{"could not read " + path.string()};
  }
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::pair<std::string, std::string>
repository_names(const std::filesystem::path& repository) {
  const auto cmake = read_unbound(repository / "apps/mcp/CMakeLists.txt");
  const std::regex expression{R"(OUTPUT_NAME\s+([A-Za-z0-9_.-]+))"};
  std::smatch match;
  if (!std::regex_search(cmake, match, expression)) {
    throw Error{"apps/mcp/CMakeLists.txt sets no MCP OUTPUT_NAME"};
  }
  return {"libtmux", match[1].str()};
}

std::optional<std::filesystem::path> which(std::string_view command,
                                           std::string_view search_path,
                                           const std::filesystem::path& working) {
  if (command.find('/') != std::string_view::npos) {
    const auto candidate = resolve_from(std::filesystem::path{command}, working);
    if (::access(candidate.c_str(), X_OK) == 0 &&
        std::filesystem::is_regular_file(candidate)) {
      return std::filesystem::canonical(candidate);
    }
    return std::nullopt;
  }
  std::size_t start = 0;
  while (start <= search_path.size()) {
    const auto colon = search_path.find(':', start);
    const auto end = colon == std::string_view::npos ? search_path.size() : colon;
    auto directory = std::filesystem::path{search_path.substr(start, end - start)};
    if (directory.empty()) {
      directory = working;
    }
    const auto candidate = directory / command;
    if (::access(candidate.c_str(), X_OK) == 0 &&
        std::filesystem::is_regular_file(candidate)) {
      return std::filesystem::canonical(candidate);
    }
    if (colon == std::string_view::npos) {
      break;
    }
    start = colon + 1;
  }
  return std::nullopt;
}

bool path_is_under(const std::filesystem::path& candidate,
                   const std::filesystem::path& root,
                   const std::filesystem::path& working) {
  if (candidate.string().find('/') == std::string::npos) {
    return false;
  }
  std::error_code error;
  const auto resolved_candidate =
      std::filesystem::weakly_canonical(resolve_from(candidate, working), error);
  if (error) {
    return false;
  }
  const auto resolved_root = std::filesystem::canonical(root, error);
  if (error) {
    return false;
  }
  const auto relative = resolved_candidate.lexically_relative(resolved_root);
  return !relative.empty() && relative != "." && *relative.begin() != "..";
}

std::string describe_spec(const ServerSpec& spec,
                          const std::filesystem::path& repository,
                          const Runtime& runtime) {
  if (spec.command.find('/') == std::string::npos) {
    return "on PATH: " + spec.command;
  }
  const auto command = resolve_from(spec.command, runtime.paths.working_directory);
  if (path_is_under(command, repository, runtime.paths.working_directory)) {
    std::error_code error;
    const auto relative = std::filesystem::weakly_canonical(command, error)
                              .lexically_relative(repository);
    return "local build: " + (error ? command.string() : relative.string());
  }
  return "installed: " + command.string();
}

std::vector<std::string> repo_pointing_names(const Client& client,
                                             std::string_view bytes,
                                             const std::filesystem::path& repository,
                                             const Runtime& runtime) {
  std::set<std::string, std::less<>> names;
  const auto collect = [&](Scope scope) {
    for (const auto& [name, spec] : read_servers(client, bytes, repository, scope)) {
      if (path_is_under(spec.command, repository, runtime.paths.working_directory)) {
        names.insert(name);
      }
    }
  };
  collect(Scope::user);
  if (client.name == "claude") {
    collect(Scope::project);
  }
  return {names.begin(), names.end()};
}

void print_diff_line(std::ostream& output, char marker, std::string_view line) {
  output << marker << line;
  if (!line.ends_with('\n')) {
    output << "\n\\ No newline at end of file\n";
  }
}

std::vector<std::string_view> split_lines(std::string_view bytes) {
  std::vector<std::string_view> result;
  for (std::size_t offset = 0; offset < bytes.size();) {
    const auto newline = bytes.find('\n', offset);
    const auto end = newline == std::string_view::npos ? bytes.size() : newline + 1;
    result.push_back(bytes.substr(offset, end - offset));
    offset = end;
  }
  return result;
}

void print_unified_diff(std::ostream& output, const std::filesystem::path& path,
                        std::string_view before, std::string_view after) {
  const auto old_lines = split_lines(before);
  const auto new_lines = split_lines(after);
  output << "--- " << path << " (current)\n"
         << "+++ " << path << " (proposed)\n"
         << "@@ -1," << old_lines.size() << " +1," << new_lines.size() << " @@\n";
  for (const auto line : old_lines) {
    print_diff_line(output, '-', line);
  }
  for (const auto line : new_lines) {
    print_diff_line(output, '+', line);
  }
}

std::filesystem::path local_binary(const Runtime& runtime, const Options& options,
                                   const std::filesystem::path& repository,
                                   std::string_view binary) {
  std::vector<std::filesystem::path> roots;
  if (options.build_dir.has_value()) {
    roots.push_back(resolve_from(*options.build_dir, runtime.paths.working_directory));
  } else {
    for (const auto& path : {"cxx/build/cxx-dev", "cxx/build/cxx-gcc",
                             "cxx/build/cxx20", "build/cxx-dev"}) {
      roots.push_back(repository / path);
    }
  }
  for (const auto& root : roots) {
    for (const auto& subdirectory : {"consumers/mcp", "apps/mcp"}) {
      const auto candidate = root / subdirectory / binary;
      if (::access(candidate.c_str(), X_OK) == 0 &&
          std::filesystem::is_regular_file(candidate)) {
        return std::filesystem::canonical(candidate);
      }
    }
  }
  throw Error{"no built " + std::string{binary} +
              " under the selected build directory"};
}

std::filesystem::path published_binary(const Runtime& runtime, const Options& options,
                                       std::string_view binary) {
  if (options.prefix.has_value()) {
    const auto prefix = resolve_from(*options.prefix, runtime.paths.working_directory);
    for (const auto& candidate :
         {prefix / "bin" / binary, prefix / "tools/libtmux" / binary}) {
      if (::access(candidate.c_str(), X_OK) == 0 &&
          std::filesystem::is_regular_file(candidate)) {
        return std::filesystem::canonical(candidate);
      }
    }
    throw Error{"no executable " + std::string{binary} + " under " + prefix.string()};
  }
  const auto found =
      which(binary, runtime.search_path, runtime.paths.working_directory);
  if (!found.has_value()) {
    throw Error{std::string{binary} + " is not on PATH"};
  }
  return *found;
}

std::vector<Client> default_clients(const Runtime& runtime,
                                    std::span<const Client> clients) {
  std::vector<Client> result;
  for (const auto& client : clients) {
    if (detail::lexists(client.config_path) &&
        which(client.binary, runtime.search_path, runtime.paths.working_directory)
            .has_value()) {
      result.push_back(client);
    }
  }
  return result;
}

std::vector<Client> chosen_clients(const Runtime& runtime, const Options& options,
                                   std::span<const Client> clients) {
  if (!options.clients.empty()) {
    return select_clients(clients, options.clients);
  }
  return default_clients(runtime, clients);
}

int command_use(const Options& options, Runtime& runtime, std::ostream& output,
                std::ostream& error) {
  const auto repository = find_repository(runtime, options.repo);
  const auto [default_server, default_binary] = repository_names(repository);
  const auto server = options.server.value_or(default_server);
  const auto binary_name = options.entry.value_or(default_binary);
  const auto binary = options.source == Source::local
                          ? local_binary(runtime, options, repository, binary_name)
                          : published_binary(runtime, options, binary_name);
  error << (options.source == Source::local ? "local: " : "published: ") << binary
        << '\n';
  ServerSpec spec{binary.string(), {}, options.environment};
  if (options.socket.has_value()) {
    spec.arguments.push_back(options.socket->string());
  }
  const auto clients = known_clients(runtime.paths);
  const auto selected = chosen_clients(runtime, options, clients);
  if (selected.empty()) {
    throw Error{"no clients detected; nothing to do"};
  }
  std::set<std::string, std::less<>> pointing_names;
  bool requested_name_points = false;
  for (const auto& client : clients) {
    if (!detail::lexists(client.config_path)) {
      continue;
    }
    try {
      const auto bytes = detail::read_bound(client.config_path).bytes;
      for (const auto& name : repo_pointing_names(client, bytes, repository, runtime)) {
        requested_name_points = requested_name_points || name == server;
        if (name != server) {
          pointing_names.insert(name);
        }
      }
    } catch (const std::exception&) {
    }
  }
  if (!requested_name_points && !pointing_names.empty()) {
    error << "note: nothing is registered under server '" << server
          << "', but this repo is registered under another name; pass --server "
          << *pointing_names.begin() << " to target it\n";
  }
  const auto timestamp = runtime.timestamp();
  if (options.dry_run) {
    auto plan = prepare_apply(runtime, repository, server, spec, options.environment,
                              selected, options.scope, timestamp, nullptr);
    std::set<std::string, std::less<>> changed;
    for (const auto& item : plan.items) {
      changed.insert(item.label);
    }
    for (const auto& [label, final_spec] : plan.preflights) {
      if (!changed.contains(label)) {
        output << '[' << label << "] already "
               << describe_spec(final_spec, repository, runtime) << " - no change\n";
      }
    }
    for (const auto& item : plan.items) {
      print_unified_diff(output, item.client.config_path, item.before.bytes,
                         item.after);
    }
    return 0;
  }
  std::optional<std::vector<std::pair<std::string, ServerSpec>>> preflighted;
  if (!options.no_preflight) {
    auto preview = prepare_apply(runtime, repository, server, spec, options.environment,
                                 selected, options.scope, timestamp, nullptr);
    preflighted = preview.preflights;
    for (const auto& [label, final_spec] : *preflighted) {
      error << "preflight: [" << label << "] " << final_spec.command;
      for (const auto& argument : final_spec.arguments) {
        error << ' ' << argument;
      }
      error << '\n';
      const auto failure = runtime.preflight(final_spec);
      if (failure.has_value()) {
        error << "preflight failed for [" << label << "], nothing written:\n"
              << *failure << '\n';
        return 1;
      }
    }
  }
  TransactionLock lock{swap_directory(runtime)};
  detail::ensure_private_directory(state_directory(runtime));
  auto plan = prepare_apply(runtime, repository, server, spec, options.environment,
                            selected, options.scope, timestamp, &lock);
  if (preflighted.has_value() && plan.preflights != *preflighted) {
    error << "selected configuration changed after preflight; nothing written; "
             "retry the swap\n";
    return 1;
  }
  std::set<std::string, std::less<>> changed;
  for (const auto& item : plan.items) {
    changed.insert(item.label);
  }
  for (const auto& [label, final_spec] : plan.preflights) {
    if (!changed.contains(label)) {
      output << '[' << label << "] already "
             << describe_spec(final_spec, repository, runtime) << " - no change\n";
    }
  }
  return apply_plan(runtime, lock, plan, output, error);
}

std::set<std::string, std::less<>>
selected_recovery_keys(const Options& options, const StateSnapshot& state,
                       std::span<const Client> clients, std::ostream& output) {
  std::vector<Client> selected;
  if (options.clients.empty()) {
    std::set<std::string, std::less<>> names;
    for (const auto& [key, entry] : state.entries) {
      static_cast<void>(entry);
      names.insert(key.substr(0, key.find(':')));
    }
    std::vector<std::string> selectors(names.begin(), names.end());
    selected = select_clients(clients, selectors);
  } else {
    selected = select_clients(clients, options.clients);
  }
  if (selected.empty()) {
    throw Error{"no recorded swaps; nothing to revert"};
  }
  std::set<std::string, std::less<>> keys;
  for (const auto& client : selected) {
    bool matched = false;
    for (const auto& [key, entry] : state.entries) {
      static_cast<void>(entry);
      if (!key.starts_with(client.name + ":")) {
        continue;
      }
      if (options.scope.has_value() &&
          key != recovery_key(client, normalized_scope(client, options.scope))) {
        continue;
      }
      keys.insert(key);
      matched = true;
    }
    if (!matched) {
      output << '[' << client.name << "] no state entry; skip\n";
    }
  }
  return keys;
}

void validate_revert_claims(const StateSnapshot& state,
                            const RecoveryAuthentication& recovery,
                            std::span<const RevertTarget> targets,
                            std::span<const ProtectedRoute> protected_routes,
                            const TransactionLock* lock) {
  std::vector<PathClaim> claims{state_claim(state)};
  if (lock != nullptr) {
    claims.push_back(
        {"swap", "lock", lock->binding().resolved, lock->binding().target});
  }
  for (const auto& target : targets) {
    claims.push_back({target.layers.front().label, "configuration",
                      target.current.binding.resolved, target.current.binding.target});
  }
  for (const auto& [key, backup] : recovery.backups) {
    claims.push_back({key, "backup", backup.binding.resolved, backup.binding.target});
  }
  for (const auto& route : protected_routes) {
    if (route.file.has_value()) {
      claims.push_back({route.owner, "protected configuration",
                        route.file->binding.resolved, route.file->binding.target});
    } else {
      claims.push_back({route.owner, "protected configuration", route.missing->resolved,
                        std::nullopt});
    }
  }
  detail::validate_distinct(claims);
}

RevertPlan prepare_revert(const Runtime& runtime, const Options& options,
                          std::ostream& output, TransactionLock* lock) {
  const auto clients = known_clients(runtime.paths);
  auto state = load_state(runtime, lock);
  const auto recovery = authenticate_recovery(state, clients, lock);
  const auto selected = selected_recovery_keys(options, state, clients, output);
  std::vector<RevertTarget> targets;
  std::set<std::string, std::less<>> target_paths;
  for (const auto& [logical, keys] : recovery.groups) {
    std::vector<std::string> chosen;
    for (const auto& key : keys) {
      if (selected.contains(key)) {
        chosen.push_back(key);
      }
    }
    if (chosen.empty()) {
      continue;
    }
    if (!std::ranges::equal(chosen, std::span{keys}.first(chosen.size()))) {
      throw Error{logical + " has a newer recovery layer"};
    }
    const auto& top_client = client_for_key(clients, keys.front());
    RevertTarget target;
    target.client = top_client;
    target.config_path = state.entries.at(keys.front()).config_path;
    target.current = recovery.current.at(logical);
    for (const auto& key : chosen) {
      const auto& entry = state.entries.at(key);
      const auto colon = key.find(':');
      const auto scope = key.substr(colon + 1) == "user" ? Scope::user : Scope::project;
      target.layers.push_back({key, recovery_label(client_for_key(clients, key), scope),
                               entry, recovery.backups.at(key)});
    }
    target.restored_bytes = target.layers.back().backup.bytes;
    target.restored_mode = target.layers.back().entry.original_mode;
    targets.push_back(std::move(target));
    target_paths.insert(logical);
  }
  auto protected_routes = capture_known_routes(clients, target_paths, lock);
  std::ranges::sort(targets, std::greater{}, [](const RevertTarget& target) {
    return target.layers.front().entry.sequence;
  });
  validate_revert_claims(state, recovery, targets, protected_routes, lock);
  return {std::move(state), std::move(targets), std::move(protected_routes), recovery,
          selected};
}

Json pending_revert(const RevertPlan& plan, std::string_view id) {
  auto changes = Json::array();
  for (const auto& target : plan.targets) {
    changes.push_back(
        {{"key", target.layers.front().key},
         {"config_path", target.config_path.string()},
         {"target_path", target.current.binding.resolved.string()},
         {"before_sha256", detail::sha256(target.current.bytes)},
         {"before_mode", target.current.binding.mode},
         {"before_binding", detail::to_json(target.current.binding)},
         {"after_sha256", detail::sha256(target.restored_bytes)},
         {"after_mode", target.restored_mode},
         {"backup_path", target.layers.back().entry.backup_path.string()},
         {"backup_binding", detail::to_json(target.layers.back().backup.binding)}});
  }
  return {{"id", id}, {"kind", "revert"}, {"changes", std::move(changes)}};
}

void validate_revert_world(const Runtime& runtime, TransactionLock& lock,
                           const RevertPlan& plan, const StateSnapshot& pending,
                           std::size_t applied, std::span<RevertLayer* const> removed) {
  lock.validate();
  validate_state_snapshot(runtime, pending, &lock);
  validate_protected(plan.protected_routes, &lock);
  for (const auto& [key, backup] : plan.recovery.backups) {
    if (plan.selected.contains(key)) {
      continue;
    }
    detail::validate_bound(backup.binding.resolved, backup.bytes, backup.binding,
                           &lock);
    detail::validate_private_path(backup.binding.resolved, backup.binding);
  }
  for (std::size_t index = 0; index < plan.targets.size(); ++index) {
    const auto& target = plan.targets[index];
    if (index < applied) {
      if (!target.restored_binding.has_value()) {
        throw Error{"reverted config has no authenticated identity"};
      }
      detail::validate_bound(target.config_path, target.restored_bytes,
                             *target.restored_binding, &lock);
    } else {
      detail::validate_bound(target.config_path, target.current.bytes,
                             target.current.binding, &lock);
    }
    for (const auto& layer : target.layers) {
      if (std::ranges::find(removed, &layer) != removed.end()) {
        continue;
      }
      detail::validate_bound(layer.entry.backup_path, layer.backup.bytes,
                             layer.backup.binding, &lock);
      detail::validate_private_path(layer.entry.backup_path, layer.backup.binding);
    }
  }
}

int apply_revert(Runtime& runtime, TransactionLock& lock, RevertPlan& plan,
                 std::ostream& output, std::ostream& error) {
  if (plan.targets.empty()) {
    return 0;
  }
  auto pending =
      write_state(runtime, lock, plan.state.entries,
                  pending_revert(plan, runtime.transaction_id()), plan.state, [&] {
                    validate_revert_world(runtime, lock, plan, plan.state, 0, {});
                  });
  std::size_t applied = 0;
  std::vector<RevertLayer*> removed;
  try {
    for (auto& target : plan.targets) {
      runtime.boundary("before-revert-config-write", target.config_path);
      validate_revert_world(runtime, lock, plan, pending, applied, {});
      detail::atomic_write(target.current.binding.resolved, target.restored_bytes,
                           target.restored_mode, target.current.binding.target,
                           target.current.binding.parent_identity);
      const auto written = detail::read_bound(target.config_path, &lock);
      if (written.bytes != target.restored_bytes ||
          written.binding.mode != target.restored_mode) {
        throw Error{"[" + target.layers.front().label +
                    "] restored config could not be authenticated"};
      }
      target.restored_binding = written.binding;
      ++applied;
    }

    auto entries = plan.state.entries;
    for (auto& target : plan.targets) {
      for (auto& layer : target.layers) {
        runtime.boundary("before-backup-remove", layer.entry.backup_path);
        validate_revert_world(runtime, lock, plan, pending, applied, removed);
        detail::remove_bound(layer.entry.backup_path, layer.backup.bytes,
                             layer.backup.binding, &lock);
        removed.push_back(&layer);
        entries.erase(layer.key);
      }
      std::vector<std::pair<std::string, RecoveryEntry*>> remaining;
      for (auto& [key, entry] : entries) {
        if (detail::absolute_logical(entry.config_path) ==
            detail::absolute_logical(target.config_path)) {
          remaining.emplace_back(key, &entry);
        }
      }
      if (!remaining.empty()) {
        const auto top = std::ranges::max_element(
            remaining, {}, [](const auto& pair) { return pair.second->sequence; });
        if ((*top).second->expected_digest != detail::sha256(target.restored_bytes) ||
            (*top).second->expected_mode != target.restored_mode) {
          throw Error{"remaining recovery layer does not match restored config"};
        }
        (*top).second->config_binding = *target.restored_binding;
        (*top).second->target_path = target.restored_binding->resolved;
      }
    }
    if (entries.empty()) {
      static_cast<void>(remove_state(runtime, lock, pending, [&] {
        validate_revert_world(runtime, lock, plan, pending, applied, removed);
      }));
    } else {
      static_cast<void>(write_state(runtime, lock, entries, std::nullopt, pending, [&] {
        validate_revert_world(runtime, lock, plan, pending, applied, removed);
      }));
    }
    for (const auto& target : plan.targets) {
      for (const auto& layer : target.layers) {
        output << '[' << layer.label << "] restored from " << layer.entry.backup_path
               << '\n';
      }
    }
    return 0;
  } catch (const std::exception& failure) {
    std::string diagnostic = failure.what();
    try {
      auto entries = plan.state.entries;
      for (auto* layer : removed) {
        if (detail::lexists(layer->entry.backup_path)) {
          continue;
        }
        const auto missing = detail::capture_missing(layer->entry.backup_path);
        const auto binding = detail::create_private_file(layer->entry.backup_path,
                                                         layer->backup.bytes, missing);
        layer->backup.binding = binding;
        entries.at(layer->key).backup_binding = binding;
      }
      for (std::size_t index = applied; index > 0; --index) {
        auto& target = plan.targets[index - 1];
        runtime.boundary("before-revert-rollback", target.config_path);
        lock.validate();
        validate_state_snapshot(runtime, pending, &lock);
        validate_protected(plan.protected_routes, &lock);
        detail::validate_bound(target.config_path, target.restored_bytes,
                               *target.restored_binding, &lock);
        detail::atomic_write(target.restored_binding->resolved, target.current.bytes,
                             target.current.binding.mode,
                             target.restored_binding->target,
                             target.restored_binding->parent_identity);
        const auto restored = detail::read_bound(target.config_path, &lock);
        if (restored.bytes != target.current.bytes ||
            restored.binding.mode != target.current.binding.mode) {
          throw Error{"revert rollback did not restore the swapped config"};
        }
        RecoveryEntry* top = nullptr;
        for (auto& [key, entry] : entries) {
          static_cast<void>(key);
          if (detail::absolute_logical(entry.config_path) ==
              detail::absolute_logical(target.config_path)) {
            if (top == nullptr || entry.sequence > top->sequence) {
              top = &entry;
            }
          }
        }
        if (top == nullptr || top->expected_digest != detail::sha256(restored.bytes) ||
            top->expected_mode != restored.binding.mode) {
          throw Error{"revert rollback does not match prior recovery"};
        }
        top->config_binding = restored.binding;
        top->target_path = restored.binding.resolved;
        target.current.binding = restored.binding;
        target.restored_binding.reset();
        --applied;
      }
      static_cast<void>(write_state(runtime, lock, entries, std::nullopt, pending, [&] {
        validate_revert_world(runtime, lock, plan, pending, 0, {});
      }));
    } catch (const std::exception& rollback) {
      diagnostic += "; rollback failed: " + std::string{rollback.what()};
    }
    error << "revert failed: " << diagnostic << '\n';
    return 1;
  }
}

int command_revert(const Options& options, Runtime& runtime, std::ostream& output,
                   std::ostream& error) {
  auto plan = prepare_revert(runtime, options, output, nullptr);
  if (options.dry_run) {
    for (const auto& target : plan.targets) {
      for (const auto& layer : target.layers) {
        output << '[' << layer.label << "] would restore "
               << target.current.binding.resolved << " from " << layer.entry.backup_path
               << '\n';
      }
    }
    return 0;
  }
  TransactionLock lock{swap_directory(runtime)};
  detail::ensure_private_directory(state_directory(runtime));
  std::ostringstream quiet;
  plan = prepare_revert(runtime, options, quiet, &lock);
  return apply_revert(runtime, lock, plan, output, error);
}

int command_detect(const Runtime& runtime, std::ostream& output) {
  for (const auto& client : known_clients(runtime.paths)) {
    const bool binary =
        which(client.binary, runtime.search_path, runtime.paths.working_directory)
            .has_value();
    const bool config = detail::lexists(client.config_path);
    std::vector<std::string> diagnostics;
    if (!binary) {
      diagnostics.emplace_back("binary missing");
    }
    if (!config) {
      diagnostics.push_back("config missing: " + client.config_path.string());
    }
    if (client.name == "pi") {
      std::error_code error;
      const auto adapter =
          runtime.paths.home / ".pi/agent/npm/node_modules/pi-mcp-adapter";
      if (!std::filesystem::is_directory(adapter, error)) {
        diagnostics.emplace_back(
            "needs the pi-mcp-adapter package; pi has no built-in MCP client");
      }
    }
    output << "  [" << (binary && config ? "yes" : " no") << "] " << std::left
           << std::setw(9) << client.name << std::right;
    if (!diagnostics.empty()) {
      output << "  (";
      for (std::size_t index = 0; index < diagnostics.size(); ++index) {
        if (index != 0) {
          output << ", ";
        }
        output << diagnostics[index];
      }
      output << ')';
    }
    output << '\n';
  }
  return 0;
}

int command_status(const Options& options, const Runtime& runtime, std::ostream& output,
                   std::ostream& error) {
  const auto repository = find_repository(runtime, options.repo);
  const auto server = options.server.value_or(repository_names(repository).first);
  const auto clients = known_clients(runtime.paths);
  const auto selected = options.clients.empty()
                            ? default_clients(runtime, clients)
                            : select_clients(clients, options.clients);
  for (const auto& client : selected) {
    if (!detail::lexists(client.config_path)) {
      output << '[' << client.name << "] (no config at " << client.config_path << ")\n";
      continue;
    }
    try {
      const auto bytes = detail::read_bound(client.config_path).bytes;
      const std::array scopes = client.name == "claude"
                                    ? std::array{Scope::user, Scope::project}
                                    : std::array{Scope::user, Scope::user};
      bool shown = false;
      for (std::size_t index = 0; index < (client.name == "claude" ? 2U : 1U);
           ++index) {
        if (options.scope.has_value() && client.name == "claude" &&
            scopes[index] != *options.scope) {
          continue;
        }
        const auto spec = read_server(client, bytes, server, repository, scopes[index]);
        if (spec.has_value()) {
          output << '[' << recovery_label(client, scopes[index]) << "] " << server
                 << " = " << spec->command;
          for (const auto& argument : spec->arguments) {
            output << ' ' << argument;
          }
          output << "  (" << describe_spec(*spec, repository, runtime) << ")\n";
          shown = true;
        }
      }
      if (!shown) {
        output << '[' << client.name;
        if (client.name == "claude" && options.scope.has_value()) {
          output << ':' << scope_name(*options.scope);
        }
        output << "] no entry for '" << server << "'\n";
      }
    } catch (const std::exception& failure) {
      error << '[' << client.name << "] " << failure.what() << '\n';
    }
  }
  return 0;
}

int command_doctor(const Options& options, const Runtime& runtime,
                   std::ostream& output) {
  const auto repository = find_repository(runtime, options.repo);
  const auto server = options.server.value_or(repository_names(repository).first);
  output << "mcp-swap doctor\n  repo:   " << repository << "\n  server: " << server
         << "  (derived default; override with --server)\n"
         << "  entries by CLI:\n";
  const auto clients = known_clients(runtime.paths);
  std::set<std::string, std::less<>> all_repo_names;
  for (const auto& client : clients) {
    if (!detail::lexists(client.config_path)) {
      continue;
    }
    try {
      const auto bytes = detail::read_bound(client.config_path).bytes;
      std::map<std::string, ServerSpec, std::less<>> specs;
      for (const auto& entry : read_servers(client, bytes, repository, Scope::user)) {
        specs.insert_or_assign(entry.first, entry.second);
      }
      if (client.name == "claude") {
        for (const auto& entry :
             read_servers(client, bytes, repository, Scope::project)) {
          specs.insert_or_assign(entry.first, entry.second);
        }
      }
      if (const auto found = specs.find(server); found != specs.end()) {
        output << "    [" << client.name << "] " << server << " = "
               << describe_spec(found->second, repository, runtime) << '\n';
      }
      for (const auto& [name, spec] : specs) {
        if (!path_is_under(spec.command, repository, runtime.paths.working_directory)) {
          continue;
        }
        all_repo_names.insert(name);
        if (name != server) {
          output << "    [" << client.name << "] " << name
                 << " = local: this repo  (other name)\n";
        }
      }
    } catch (const std::exception& failure) {
      output << "    [" << client.name << "] config unreadable: " << failure.what()
             << '\n';
    }
  }
  if (all_repo_names.empty()) {
    output << "    (no CLI currently points at this repo)\n";
  } else if (!all_repo_names.contains(server)) {
    output << "  ! server name mismatch: this repo is registered as [";
    std::size_t index = 0;
    for (const auto& name : all_repo_names) {
      output << (index++ == 0 ? "'" : ", '") << name << '\'';
    }
    output << "], not '" << server << "' - use --server " << *all_repo_names.begin()
           << '\n';
  }

  const auto state = load_state(runtime);
  if (state.transaction.has_value()) {
    output << "  ! interrupted " << state.transaction->value("kind", "unknown")
           << " transaction\n";
  }
  if (!state.entries.empty()) {
    output << "  outstanding swaps (un-reverted):\n";
    std::vector<std::pair<std::string, const RecoveryEntry*>> ordered;
    for (const auto& [key, entry] : state.entries) {
      ordered.emplace_back(key, &entry);
    }
    std::ranges::sort(ordered, {},
                      [](const auto& pair) { return pair.second->sequence; });
    for (const auto& [key, entry] : ordered) {
      output << "    " << key << "  swapped_at=" << entry->swapped_at;
      if (!detail::lexists(entry->backup_path)) {
        output << "  ! BACKUP MISSING - revert would fail for this entry";
      }
      output << '\n';
    }
  }

  std::set<std::filesystem::path> referenced;
  for (const auto& [key, entry] : state.entries) {
    static_cast<void>(key);
    referenced.insert(detail::absolute_logical(entry.backup_path));
  }
  std::size_t orphan_count = 0;
  std::uintmax_t orphan_bytes = 0;
  for (const auto& client : clients) {
    std::error_code error;
    std::filesystem::directory_iterator entries{client.config_path.parent_path(),
                                                error};
    if (error) {
      continue;
    }
    const auto prefix =
        client.config_path.filename().string() + std::string{backup_marker};
    for (const auto& entry : entries) {
      if (!entry.path().filename().string().starts_with(prefix) ||
          referenced.contains(detail::absolute_logical(entry.path()))) {
        continue;
      }
      ++orphan_count;
      const auto size = entry.file_size(error);
      if (!error) {
        orphan_bytes += size;
      }
      error.clear();
    }
  }
  if (orphan_count != 0) {
    output << "  orphaned backups: " << orphan_count << " file(s), " << orphan_bytes
           << " bytes not tracked by state - inspect before deleting: an "
              "untracked backup can be the only surviving pre-swap copy of a "
              "config\n";
  }

  bool auth_heading = false;
  for (const auto& [name, client] :
       std::array<std::pair<std::string_view, std::string_view>, 6>{
           {{"ANTHROPIC_API_KEY", "claude"},
            {"OPENAI_API_KEY", "codex"},
            {"GEMINI_API_KEY", "gemini"},
            {"GOOGLE_API_KEY", "gemini"},
            {"XAI_API_KEY", "grok"},
            {"GROK_API_KEY", "grok"}}}) {
    if (const char* value = std::getenv(name.data());
        value != nullptr && *value != '\0') {
      if (!auth_heading) {
        output << "  auth-overriding env vars set:\n";
        auth_heading = true;
      }
      output << "    ! " << name << " overrides " << client
             << "'s stored login - prefix with `env -u " << name
             << "` to use the subscription/OAuth auth instead\n";
    }
  }
  return 0;
}

} // namespace

Runtime system_runtime() {
  const char* home_value = std::getenv("HOME");
  if (home_value == nullptr || *home_value == '\0') {
    throw Error{"HOME is not set"};
  }
  const std::filesystem::path home{home_value};
  const char* config_value = std::getenv("XDG_CONFIG_HOME");
  const char* state_value = std::getenv("XDG_STATE_HOME");
  const auto config =
      config_value != nullptr && std::filesystem::path{config_value}.is_absolute()
          ? std::filesystem::path{config_value}
          : home / ".config";
  const auto state =
      state_value != nullptr && std::filesystem::path{state_value}.is_absolute()
          ? std::filesystem::path{state_value}
          : home / ".local/state";
  Runtime result;
  result.paths = {home, config, state, std::filesystem::current_path()};
  if (const char* path = std::getenv("PATH"); path != nullptr) {
    result.search_path = path;
  }
  result.preflight = [](const ServerSpec& spec) { return preflight_spec(spec); };
  result.boundary = [](std::string_view, const std::filesystem::path&) {};
  result.timestamp = [] {
    const auto now = std::chrono::system_clock::now();
    const auto value = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    static_cast<void>(::localtime_r(&value, &local));
    std::ostringstream rendered;
    rendered << std::put_time(&local, "%Y%m%d%H%M%S");
    return rendered.str();
  };
  result.transaction_id = [] {
    std::random_device random;
    std::ostringstream rendered;
    rendered << std::hex << std::setfill('0');
    for (int index = 0; index < 16; ++index) {
      rendered << std::setw(2) << (random() & 0xffU);
    }
    return rendered.str();
  };
  return result;
}

int execute(std::span<const std::string> arguments, Runtime& runtime,
            std::ostream& output, std::ostream& error) {
  Options options;
  try {
    options = parse_arguments(arguments);
  } catch (const std::exception& failure) {
    error << "mcp-swap: " << failure.what() << "\n\n" << usage();
    return 2;
  }
  if (options.command == Command::help) {
    output << usage();
    return 0;
  }
  try {
    switch (options.command) {
    case Command::detect:
      return command_detect(runtime, output);
    case Command::status:
      return command_status(options, runtime, output, error);
    case Command::use_local:
      return command_use(options, runtime, output, error);
    case Command::revert:
      return command_revert(options, runtime, output, error);
    case Command::doctor:
      return command_doctor(options, runtime, output);
    case Command::help:
      break;
    }
  } catch (const std::exception& failure) {
    error << "mcp-swap: " << failure.what() << '\n';
    return 1;
  }
  return 2;
}

} // namespace libtmux::mcp_swap
