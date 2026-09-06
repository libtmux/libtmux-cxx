#pragma once

#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libtmux::mcp_swap {

enum class ConfigFormat { json, jsonc, toml };
enum class EntryDialect { standard, claude, opencode };
enum class Scope { user, project };
enum class Command { detect, status, use_local, revert, doctor, help };
enum class Source { local, published };

struct Options {
  Command command{Command::help};
  Source source{Source::local};
  std::filesystem::path repo{"."};
  std::optional<std::filesystem::path> build_dir;
  std::optional<std::filesystem::path> prefix;
  std::optional<std::filesystem::path> socket;
  std::optional<std::string> server;
  std::optional<std::string> entry;
  std::vector<std::string> clients;
  std::vector<std::pair<std::string, std::string>> environment;
  std::optional<Scope> scope;
  bool dry_run{};
  bool no_preflight{};

  auto operator<=>(const Options&) const = default;
};

struct Client {
  std::string name;
  std::string binary;
  std::filesystem::path config_path;
  std::string container;
  ConfigFormat format;
  EntryDialect dialect;

  auto operator<=>(const Client&) const = default;
};

struct Paths {
  std::filesystem::path home;
  std::filesystem::path config_home;
  std::filesystem::path state_home;
  std::filesystem::path working_directory;
};

struct ServerSpec {
  std::string command;
  std::vector<std::string> arguments;
  std::vector<std::pair<std::string, std::string>> environment;

  auto operator<=>(const ServerSpec&) const = default;
};

enum class ChangeAction { added, replaced };

struct RenderedConfig {
  std::string bytes;
  ChangeAction action;
  ServerSpec effective;
};

struct Runtime {
  Paths paths;
  std::string search_path;
  std::function<std::optional<std::string>(const ServerSpec&)> preflight;
  std::function<void(std::string_view, const std::filesystem::path&)> boundary;
  std::function<std::string()> timestamp;
  std::function<std::string()> transaction_id;
};

// Avoid Ubuntu's libc++/libc++abi std::runtime_error allocator mismatch under ASan.
class Error final : public std::exception {
public:
  explicit Error(std::string message) : message_(std::move(message)) {}

  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

private:
  std::string message_;
};

[[nodiscard]] std::vector<Client> known_clients(const Paths& paths);
[[nodiscard]] std::vector<Client>
select_clients(std::span<const Client> clients, std::span<const std::string> selectors);
[[nodiscard]] Options parse_arguments(std::span<const std::string> arguments);
[[nodiscard]] std::string usage();
[[nodiscard]] std::optional<ServerSpec>
read_server(const Client& client, std::string_view bytes, std::string_view server,
            const std::filesystem::path& repository, Scope scope);
[[nodiscard]] std::vector<std::pair<std::string, ServerSpec>>
read_servers(const Client& client, std::string_view bytes,
             const std::filesystem::path& repository, Scope scope);
[[nodiscard]] RenderedConfig render_server(const Client& client, std::string_view bytes,
                                           std::string_view server,
                                           const ServerSpec& replacement,
                                           const std::filesystem::path& repository,
                                           Scope scope);
[[nodiscard]] Runtime system_runtime();
[[nodiscard]] int execute(std::span<const std::string> arguments, Runtime& runtime,
                          std::ostream& output, std::ostream& error);
[[nodiscard]] std::optional<std::string>
preflight_spec(const ServerSpec& spec,
               std::chrono::milliseconds timeout = std::chrono::minutes{5});

} // namespace libtmux::mcp_swap
