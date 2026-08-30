#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <userenv.h>
#include <windows.h>

#include <libtmux/libtmux.hpp>

#include "backend.hpp"
#include "environment.hpp"
#include "process.hpp"

#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using libtmux::CommandFailure;
using libtmux::Server;
using libtmux::detail::Argument;
using libtmux::detail::Exited;
using libtmux::detail::ProcessRequest;

constexpr std::string_view kNamespacePrefix{"libtmux-cxx-smoke-"};

[[nodiscard]] bool not_started(const CommandFailure& failure) noexcept {
  return failure.delivery == libtmux::DeliveryStatus::not_started;
}

[[nodiscard]] bool contains_case_insensitive(std::string_view text,
                                             std::string_view needle) {
  const auto fold = [](char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  };
  for (std::size_t start = 0; start + needle.size() <= text.size(); ++start) {
    std::size_t offset = 0;
    while (offset < needle.size() &&
           fold(text[start + offset]) == fold(needle[offset])) {
      ++offset;
    }
    if (offset == needle.size()) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

struct RawReply {
  int exit_code;
  std::string output;
};

[[nodiscard]] std::string text(const std::vector<std::byte>& bytes) {
  std::string value;
  value.reserve(bytes.size());
  for (const std::byte byte : bytes) {
    value.push_back(static_cast<char>(byte));
  }
  return value;
}

[[nodiscard]] std::optional<RawReply>
raw_tmux(const std::vector<std::string>& command,
         std::optional<std::chrono::milliseconds> timeout = std::chrono::seconds{20}) {
  ProcessRequest request;
  request.executable = "tmux";
  request.timeout = timeout;
  request.environment = {{"PATHEXT", ".COM;.EXE;.BAT;.CMD"},
                         {"PSMUX_NO_WARM", "1"},
                         {"TMUX", std::nullopt},
                         {"TMUX_PANE", std::nullopt},
                         {"PSMUX_ACTIVE", std::nullopt},
                         {"PSMUX_SESSION", std::nullopt},
                         {"PSMUX_SESSION_NAME", std::nullopt},
                         {"PSMUX_TARGET_FULL", std::nullopt},
                         {"PSMUX_TARGET_SESSION", std::nullopt},
                         {"PSMUX_REMOTE_ATTACH", std::nullopt}};
  request.arguments = {Argument{"-u"}};
  for (const std::string& argument : command) {
    request.arguments.push_back(Argument{argument});
  }
  auto reply = libtmux::detail::run_process(request);
  if (!reply.has_value()) {
    std::cerr << "FAIL: raw psmux process failed: " << reply.error().diagnostic << '\n';
    return std::nullopt;
  }
  const auto* exited = std::get_if<Exited>(&reply->termination);
  return RawReply{.exit_code = exited == nullptr ? -1 : exited->code,
                  .output = text(reply->stdout_bytes)};
}

[[nodiscard]] std::optional<RawReply>
raw_psmux(std::string_view socket_name, const std::vector<std::string>& command,
          std::optional<std::chrono::milliseconds> timeout = std::chrono::seconds{20}) {
  std::vector<std::string> namespaced{"-L", std::string{socket_name}};
  namespaced.insert(namespaced.end(), command.begin(), command.end());
  return raw_tmux(namespaced, timeout);
}

class DelayedSignal final {
public:
  DelayedSignal(std::string socket_name, std::string channel,
                std::chrono::milliseconds delay)
      : socket_name_{std::move(socket_name)}, channel_{std::move(channel)},
        thread_{[this, delay] {
          std::unique_lock lock{mutex_};
          if (stopped_.wait_for(lock, delay, [this] { return stop_; })) {
            return;
          }
          lock.unlock();
          static_cast<void>(raw_psmux(socket_name_, {"wait-for", "-S", channel_}));
        }} {}

  ~DelayedSignal() { stop(); }
  DelayedSignal(const DelayedSignal&) = delete;
  DelayedSignal& operator=(const DelayedSignal&) = delete;

  void stop() {
    {
      std::lock_guard lock{mutex_};
      stop_ = true;
    }
    stopped_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

private:
  std::string socket_name_;
  std::string channel_;
  std::mutex mutex_;
  std::condition_variable stopped_;
  bool stop_{false};
  std::thread thread_;
};

[[nodiscard]] bool cleanup_registry_files(std::string_view socket_name, bool report);

class ScopedRawSession final {
public:
  ScopedRawSession(std::string socket_name, std::string session_name)
      : socket_name_{std::move(socket_name)}, session_name_{std::move(session_name)} {}
  ~ScopedRawSession() {
    if (!cleaned_) {
      static_cast<void>(finish(false));
    }
  }

  ScopedRawSession(const ScopedRawSession&) = delete;
  ScopedRawSession& operator=(const ScopedRawSession&) = delete;

  [[nodiscard]] bool alive() const {
    const auto reply = raw_psmux(socket_name_, {"has-session", "-t", session_name_});
    return reply.has_value() && reply->exit_code == 0;
  }

  [[nodiscard]] bool finish(bool report = true) {
    if (cleaned_) {
      return true;
    }
    const auto reply = raw_psmux(socket_name_, {"kill-session", "-t", session_name_});
    cleaned_ = reply.has_value() && reply->exit_code == 0 && !alive() &&
               cleanup_registry_files(socket_name_, report);
    return cleaned_;
  }

private:
  std::string socket_name_;
  std::string session_name_;
  bool cleaned_{false};
};

class ScopedFile final {
public:
  explicit ScopedFile(std::filesystem::path path) : path_{std::move(path)} {}
  ~ScopedFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  ScopedFile(const ScopedFile&) = delete;
  ScopedFile& operator=(const ScopedFile&) = delete;

  [[nodiscard]] bool finish() {
    std::error_code remove_failed;
    const bool removed = std::filesystem::remove(path_, remove_failed);
    if (remove_failed) {
      return false;
    }
    std::error_code exists_failed;
    const bool exists = std::filesystem::exists(path_, exists_failed);
    return !exists_failed && (removed || !exists);
  }

private:
  std::filesystem::path path_;
};

struct RecoveryTarget {
  std::string socket_name;
  std::string session_name;
  bool cleanup_proven{false};
};

class RecoveryState final {
public:
  void note_session_may_exist(std::string socket_name, std::string session_name) {
    targets_.push_back(RecoveryTarget{.socket_name = std::move(socket_name),
                                      .session_name = std::move(session_name)});
  }

  void prove_namespace_cleanup(std::string_view socket_name) noexcept {
    for (auto& target : targets_) {
      if (target.socket_name == socket_name) {
        target.cleanup_proven = true;
      }
    }
  }

  [[nodiscard]] bool preserve_config() const noexcept {
    for (const auto& target : targets_) {
      if (!target.cleanup_proven) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] const std::vector<RecoveryTarget>& targets() const noexcept {
    return targets_;
  }

private:
  std::vector<RecoveryTarget> targets_;
};

[[nodiscard]] bool recovery_state_self_test() {
  RecoveryState state;
  if (state.preserve_config()) {
    return false;
  }
  state.note_session_may_exist("self-test", "first");
  if (!state.preserve_config()) {
    return false;
  }
  state.prove_namespace_cleanup("other");
  if (!state.preserve_config()) {
    return false;
  }
  state.prove_namespace_cleanup("self-test");
  if (state.preserve_config()) {
    return false;
  }
  state.note_session_may_exist("self-test", "replacement");
  if (!state.preserve_config()) {
    return false;
  }
  state.prove_namespace_cleanup("self-test");
  return !state.preserve_config();
}

class ScopedRecoveryConfig final {
public:
  explicit ScopedRecoveryConfig(std::filesystem::path path) : path_{std::move(path)} {}
  ~ScopedRecoveryConfig() {
    if (removed_) {
      return;
    }
    if (state_.preserve_config()) {
      report_unproved_cleanup();
      return;
    }
    static_cast<void>(remove_and_verify(true));
  }

  ScopedRecoveryConfig(const ScopedRecoveryConfig&) = delete;
  ScopedRecoveryConfig& operator=(const ScopedRecoveryConfig&) = delete;

  void note_session_may_exist(std::string_view socket_name,
                              std::string_view session_name) {
    state_.note_session_may_exist(std::string{socket_name}, std::string{session_name});
  }

  void prove_namespace_cleanup(std::string_view socket_name) noexcept {
    state_.prove_namespace_cleanup(socket_name);
  }

  [[nodiscard]] bool finish() {
    if (removed_) {
      return true;
    }
    if (state_.preserve_config()) {
      report_unproved_cleanup();
      return false;
    }
    return remove_and_verify(true);
  }

private:
  [[nodiscard]] bool remove_and_verify(bool report) {
    std::error_code remove_failed;
    static_cast<void>(std::filesystem::remove(path_, remove_failed));
    if (remove_failed) {
      if (report) {
        std::cerr << "FAIL: could not remove the task-owned psmux configuration: "
                  << remove_failed.message() << '\n'
                  << "RECOVERY: PSMUX_CONFIG_FILE=" << path_ << '\n';
      }
      return false;
    }
    std::error_code exists_failed;
    const bool exists = std::filesystem::exists(path_, exists_failed);
    if (exists_failed || exists) {
      if (report) {
        std::cerr << "FAIL: could not verify removal of the task-owned psmux "
                     "configuration";
        if (exists_failed) {
          std::cerr << ": " << exists_failed.message();
        }
        std::cerr << '\n' << "RECOVERY: PSMUX_CONFIG_FILE=" << path_ << '\n';
      }
      return false;
    }
    removed_ = true;
    return true;
  }

  void report_unproved_cleanup() {
    if (recovery_reported_) {
      return;
    }
    recovery_reported_ = true;
    std::cerr << "FAIL: exact psmux cleanup was not proven; preserving the trusted "
                 "configuration\n"
              << "RECOVERY: PSMUX_CONFIG_FILE=" << path_ << '\n';
    for (const auto& target : state_.targets()) {
      if (!target.cleanup_proven) {
        std::cerr << "RECOVERY: namespace=[" << target.socket_name << "] session=["
                  << target.session_name << "]\n";
      }
    }
    std::cerr << "RECOVERY: inspect only these exact identifiers; do not use "
                 "default-server or broad cleanup\n";
  }

  std::filesystem::path path_;
  RecoveryState state_;
  bool removed_{false};
  bool recovery_reported_{false};
};

void report_command(std::string_view message, const CommandFailure& failure) {
  std::cerr << "FAIL: " << message << ": " << failure.diagnostic << '\n';
}

[[nodiscard]] std::string unique_namespace() {
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::string{kNamespacePrefix} + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(tick);
}

[[nodiscard]] bool safe_namespace(std::string_view name) {
  if (!name.starts_with(kNamespacePrefix)) {
    return false;
  }
  const std::string_view suffix = name.substr(kNamespacePrefix.size());
  const std::size_t separator = suffix.find('-');
  if (separator == std::string_view::npos || separator == 0U ||
      separator + 1U == suffix.size() ||
      suffix.find('-', separator + 1U) != std::string_view::npos) {
    return false;
  }
  for (const char value : suffix) {
    if (value != '-' && (value < '0' || value > '9')) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::optional<std::wstring> environment(std::wstring_view name) {
  const DWORD size = GetEnvironmentVariableW(name.data(), nullptr, 0);
  if (size == 0U) {
    return std::nullopt;
  }
  std::wstring value(size, L'\0');
  const DWORD written = GetEnvironmentVariableW(name.data(), value.data(), size);
  if (written == 0U || written >= size) {
    return std::nullopt;
  }
  value.resize(written);
  return value;
}

[[nodiscard]] std::optional<std::filesystem::path> psmux_data_directory() {
  if (auto profile = environment(L"USERPROFILE"); profile.has_value()) {
    return std::filesystem::path{std::move(*profile)} / L".psmux";
  }
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != 0) {
    DWORD size = 0;
    static_cast<void>(GetUserProfileDirectoryW(token, nullptr, &size));
    std::wstring profile(size, L'\0');
    if (size != 0U && GetUserProfileDirectoryW(token, profile.data(), &size) != 0 &&
        size > 1U) {
      CloseHandle(token);
      profile.resize(size - 1U);
      return std::filesystem::path{std::move(profile)} / L".psmux";
    }
    CloseHandle(token);
  }
  const auto drive = environment(L"HOMEDRIVE");
  const auto home_path = environment(L"HOMEPATH");
  if (drive.has_value() && home_path.has_value()) {
    const std::filesystem::path profile{*drive + *home_path};
    std::error_code status_error;
    if (std::filesystem::is_directory(profile, status_error) && !status_error) {
      return profile / L".psmux";
    }
  }
  if (auto home = environment(L"HOME"); home.has_value()) {
    return std::filesystem::path{std::move(*home)} / L".psmux";
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> ensure_psmux_data_directory() {
  const auto directory = psmux_data_directory();
  if (!directory.has_value()) {
    return std::nullopt;
  }

  std::error_code inspected;
  auto status = std::filesystem::symlink_status(*directory, inspected);
  if (status.type() == std::filesystem::file_type::not_found) {
    inspected.clear();
    static_cast<void>(std::filesystem::create_directory(*directory, inspected));
    if (!inspected) {
      status = std::filesystem::symlink_status(*directory, inspected);
    }
  }
  if (inspected || !std::filesystem::is_directory(status)) {
    return std::nullopt;
  }
  return directory;
}

[[nodiscard]] std::optional<std::size_t>
decimal_regular_file(const std::filesystem::path& path) {
  std::error_code inspected;
  const auto status = std::filesystem::symlink_status(path, inspected);
  if (inspected || !std::filesystem::is_regular_file(status)) {
    return std::nullopt;
  }
  std::ifstream input{path};
  std::string value;
  std::string extra;
  if (!(input >> value) || (input >> extra)) {
    return std::nullopt;
  }
  std::size_t parsed = 0U;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return std::nullopt;
  }
  return parsed;
}

struct DurableSessionIdentity {
  std::size_t id;
  std::size_t next_id;
};

[[nodiscard]] std::optional<DurableSessionIdentity>
wait_for_durable_session(std::string_view socket_name, std::string_view session_name,
                         std::chrono::milliseconds timeout) {
  const auto directory = psmux_data_directory();
  if (!directory.has_value()) {
    return std::nullopt;
  }
  const std::filesystem::path base =
      *directory / (std::string{socket_name} + "__" + std::string{session_name});
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    bool complete = true;
    for (const std::string_view extension : {".port", ".key", ".pid", ".sid"}) {
      std::error_code inspected;
      const auto status = std::filesystem::symlink_status(
          std::filesystem::path{base.string() + std::string{extension}}, inspected);
      complete = complete && !inspected && std::filesystem::is_regular_file(status);
    }
    const auto id = decimal_regular_file(base.string() + ".sid");
    const auto next_id = decimal_regular_file(*directory / "next_session_id");
    if (complete && id.has_value() && next_id.has_value() && *next_id > *id) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      auto routed = raw_psmux(socket_name,
                              {"display-message", "-p", "-t", std::string{session_name},
                               "#{session_id}|#{session_name}"},
                              remaining);
      if (routed.has_value() && routed->exit_code == 0) {
        while (!routed->output.empty() &&
               (routed->output.back() == '\n' || routed->output.back() == '\r')) {
          routed->output.pop_back();
        }
        const std::string expected =
            "$" + std::to_string(*id) + '|' + std::string{session_name};
        if (routed->output == expected) {
          return DurableSessionIdentity{.id = *id, .next_id = *next_id};
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  } while (std::chrono::steady_clock::now() < deadline);
  return std::nullopt;
}

[[nodiscard]] bool wait_for_exact_rename(std::string_view socket_name,
                                         std::string_view old_name,
                                         std::string_view new_name,
                                         std::chrono::milliseconds timeout) {
  const auto directory = psmux_data_directory();
  if (!directory.has_value()) {
    return false;
  }
  const std::filesystem::path old_base =
      *directory / (std::string{socket_name} + "__" + std::string{old_name});
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    auto identity = raw_psmux(
        socket_name,
        {"display-message", "-p", "-t", std::string{new_name}, "#{session_name}"},
        remaining);
    bool old_absent = true;
    for (const std::string_view extension : {".port", ".key", ".pid", ".sid"}) {
      std::error_code inspected;
      const bool exists = std::filesystem::exists(
          old_base.string() + std::string{extension}, inspected);
      old_absent = old_absent && !inspected && !exists;
    }
    if (identity.has_value() && identity->exit_code == 0) {
      while (!identity->output.empty() &&
             (identity->output.back() == '\n' || identity->output.back() == '\r')) {
        identity->output.pop_back();
      }
      if (identity->output == new_name && old_absent) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }
  return false;
}

[[nodiscard]] bool wait_for_registry_absence(std::string_view socket_name,
                                             std::string_view session_name,
                                             std::chrono::milliseconds timeout) {
  const auto directory = psmux_data_directory();
  if (!directory.has_value()) {
    return false;
  }
  const std::filesystem::path base =
      *directory / (std::string{socket_name} + "__" + std::string{session_name});
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    bool absent = true;
    for (const std::string_view extension : {".port", ".key", ".pid", ".sid"}) {
      std::error_code inspected;
      const bool exists =
          std::filesystem::exists(base.string() + std::string{extension}, inspected);
      absent = absent && !inspected && !exists;
    }
    if (absent) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

[[nodiscard]] bool exact_registry_file(const std::filesystem::path& path,
                                       std::string_view socket_name) {
  const std::wstring namespace_prefix{socket_name.begin(), socket_name.end()};
  const std::wstring prefix = namespace_prefix + L"__";
  const std::wstring stem = path.stem().wstring();
  if (!stem.starts_with(prefix)) {
    return false;
  }
  const std::wstring_view session = std::wstring_view{stem}.substr(prefix.size());
  return !session.empty() && session.find(L"__") == std::wstring_view::npos;
}

[[nodiscard]] bool cleanup_registry_files(std::string_view socket_name, bool report) {
  const auto directory = psmux_data_directory();
  if (!directory.has_value()) {
    if (report) {
      std::cerr << "FAIL: could not locate the psmux data directory\n";
    }
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  std::vector<std::filesystem::path> residue;
  do {
    residue.clear();
    std::error_code failed;
    std::filesystem::directory_iterator entry{*directory, failed};
    const std::filesystem::directory_iterator end;
    while (!failed && entry != end) {
      const std::filesystem::path path = entry->path();
      if (exact_registry_file(path, socket_name)) {
        if (path.extension() == L".sid") {
          std::error_code status_failed;
          const auto status = std::filesystem::symlink_status(path, status_failed);
          if (status_failed || status.type() != std::filesystem::file_type::regular) {
            residue.push_back(path);
          } else {
            std::error_code remove_failed;
            std::filesystem::remove(path, remove_failed);
            if (remove_failed) {
              residue.push_back(path);
            }
          }
        } else {
          residue.push_back(path);
        }
      }
      entry.increment(failed);
    }
    if (failed) {
      if (report) {
        std::cerr << "FAIL: could not inspect the psmux data directory: "
                  << failed.message() << '\n';
      }
      return false;
    }
    if (residue.empty()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  } while (std::chrono::steady_clock::now() < deadline);

  if (report) {
    for (const auto& path : residue) {
      std::cerr << "FAIL: exact psmux registry file remained: " << path.string()
                << '\n';
    }
  }
  return false;
}

class ScopedServer final {
public:
  ScopedServer(Server server, std::string socket_name)
      : server_{std::move(server)}, socket_name_{std::move(socket_name)} {}
  ~ScopedServer() { static_cast<void>(cleanup(false)); }

  ScopedServer(const ScopedServer&) = delete;
  ScopedServer& operator=(const ScopedServer&) = delete;

  [[nodiscard]] Server& get() noexcept { return server_; }
  [[nodiscard]] bool finish() { return cleanup(true); }

private:
  [[nodiscard]] bool cleanup(bool report) {
    if (cleaned_) {
      return cleanup_succeeded_;
    }
    if (!safe_namespace(socket_name_)) {
      if (report) {
        std::cerr << "FAIL: refusing cleanup for an unsafe psmux namespace\n";
      }
      return false;
    }

    const auto killed = server_.kill();
    cleanup_succeeded_ = killed.has_value();
    if (!killed && report) {
      report_command("namespaced psmux kill did not succeed", killed.error());
    }
    cleanup_succeeded_ =
        cleanup_registry_files(socket_name_, report) && cleanup_succeeded_;
    cleaned_ = cleanup_succeeded_;
    return cleanup_succeeded_;
  }

  Server server_;
  std::string socket_name_;
  bool cleaned_{false};
  bool cleanup_succeeded_{true};
};

[[nodiscard]] bool configure_environment(const std::filesystem::path& config) {
  if (SetEnvironmentVariableW(L"PATHEXT", L".CPL") == 0) {
    std::cerr << "FAIL: could not set PATHEXT (Windows error " << GetLastError()
              << ")\n";
    return false;
  }
  if (SetEnvironmentVariableW(L"PSMUX_NO_WARM", L"0") == 0) {
    std::cerr << "FAIL: could not seed hostile psmux warming state (Windows error "
              << GetLastError() << ")\n";
    return false;
  }
  constexpr const wchar_t* cleared_state[]{L"PSMUX_DATA_DIR"};
  for (const auto* name : cleared_state) {
    if (SetEnvironmentVariableW(name, nullptr) == 0) {
      std::cerr << "FAIL: could not clear inherited psmux state (Windows error "
                << GetLastError() << ")\n";
      return false;
    }
  }
  if (SetEnvironmentVariableW(L"TMUX", L"C:\\outside\\default,1,0") == 0 ||
      SetEnvironmentVariableW(L"TMUX_PANE", L"%999") == 0 ||
      SetEnvironmentVariableW(L"PSMUX_ACTIVE", L"1") == 0 ||
      SetEnvironmentVariableW(L"PSMUX_SESSION", L"outside") == 0 ||
      SetEnvironmentVariableW(L"PSMUX_SESSION_NAME", L"outside") == 0 ||
      SetEnvironmentVariableW(L"PSMUX_TARGET_FULL", L"outside:%999") == 0 ||
      SetEnvironmentVariableW(L"PSMUX_TARGET_SESSION", L"outside") == 0 ||
      SetEnvironmentVariableW(L"PSMUX_REMOTE_ATTACH", L"1") == 0) {
    std::cerr << "FAIL: could not seed hostile inherited psmux routing state\n";
    return false;
  }
  if (SetEnvironmentVariableW(L"PSMUX_CONFIG_FILE", config.c_str()) == 0) {
    std::cerr << "FAIL: could not isolate the psmux config (Windows error "
              << GetLastError() << ")\n";
    return false;
  }
  return true;
}

class ReboundSessionBackend final : public libtmux::detail::Backend {
public:
  libtmux::expected<std::string, CommandFailure>
  run(const libtmux::CommandRequest&, std::optional<std::chrono::milliseconds>,
      std::optional<std::size_t>) const override {
    return libtmux::unexpected(
        CommandFailure{.kind = libtmux::FailureKind::refused,
                       .delivery = libtmux::DeliveryStatus::not_started,
                       .exit_code = 0,
                       .diagnostic = "the routed snapshot must use run_in_session"});
  }

  libtmux::expected<std::string, CommandFailure>
  run_in_session(const libtmux::CommandRequest&, std::string_view, std::string_view,
                 std::optional<std::chrono::milliseconds>,
                 std::optional<std::size_t>) const override {
    const std::string separator{libtmux::kFormatSeparator};
    return "@1" + separator + "replacement" + separator + "1" + separator + "$2" +
           separator + "0" + separator + "1" + separator + "80" + separator + "24" +
           separator + "layout" + separator + "0" + separator + "0" + separator + "0" +
           separator + "1" + separator + "\n";
  }

  const std::vector<std::string>& connection() const noexcept override {
    return connection_;
  }

  libtmux::expected<libtmux::Version, CommandFailure> version() const override {
    return libtmux::Version{.major = 3, .minor = 3, .revision = 7};
  }

private:
  std::vector<std::string> connection_{"-L", "scripted"};
};

[[nodiscard]] bool rejects_rebound_session_snapshot() {
  auto backend = std::make_shared<ReboundSessionBackend>();
  auto snapshot = libtmux::Snapshot::take_in_session(
      std::move(backend), libtmux::Window::kFields, {"list-windows"},
      libtmux::FormatArgument::flag, "$1", "captured");
  return !snapshot && snapshot.error().kind == libtmux::FailureKind::missing &&
         snapshot.error().delivery == libtmux::DeliveryStatus::replied;
}

} // namespace

int main() {
  using namespace std::chrono_literals;

  if (!require(recovery_state_self_test(),
               "the recovery state must preserve unproved cleanup only")) {
    return EXIT_FAILURE;
  }

  const std::string socket_name = unique_namespace();
  std::error_code directory_error;
  const auto temporary = std::filesystem::temp_directory_path(directory_error);
  if (directory_error) {
    std::cerr << "FAIL: could not locate the temporary directory: "
              << directory_error.message() << '\n';
    return EXIT_FAILURE;
  }
  const auto config = temporary / (socket_name + ".conf");
  std::ofstream empty_config{config};
  if (!empty_config) {
    std::cerr << "FAIL: could not create the isolated psmux config\n";
    return EXIT_FAILURE;
  }
  empty_config.close();
  ScopedRecoveryConfig config_cleanup{config};
  if (!configure_environment(config)) {
    return EXIT_FAILURE;
  }
  const auto psmux_directory = ensure_psmux_data_directory();
  if (!require(psmux_directory.has_value(),
               "could not create and verify the isolated psmux data directory")) {
    return EXIT_FAILURE;
  }
  const auto child_environment = libtmux_env::psmux_child_environment();
  bool disables_warm_claiming = false;
  bool repairs_pathext = false;
  std::size_t cleared_routes = 0;
  for (const auto& [name, value] : child_environment) {
    disables_warm_claiming =
        disables_warm_claiming || (name == "PSMUX_NO_WARM" && value == "1");
    repairs_pathext = repairs_pathext || (name == "PATHEXT" && value.has_value() &&
                                          contains_case_insensitive(*value, ".EXE"));
    if ((name == "TMUX" || name == "TMUX_PANE" || name == "PSMUX_ACTIVE" ||
         name == "PSMUX_SESSION" || name == "PSMUX_SESSION_NAME" ||
         name == "PSMUX_TARGET_FULL" || name == "PSMUX_TARGET_SESSION" ||
         name == "PSMUX_REMOTE_ATTACH") &&
        !value.has_value()) {
      ++cleared_routes;
    }
  }
  if (!require(disables_warm_claiming && repairs_pathext && cleared_routes == 8U,
               "the backend must isolate every psmux child environment")) {
    return EXIT_FAILURE;
  }
  if (!require(rejects_rebound_session_snapshot(),
               "routed snapshots must reject a replacement session reply")) {
    return EXIT_FAILURE;
  }

  auto socket_path = Server::at_socket_path(R"(C:\libtmux-psmux-smoke.sock)");
  if (!require(!socket_path, "psmux must reject -S socket paths") ||
      !require(socket_path.error().kind == libtmux::FailureKind::unsupported,
               "socket paths must report an unsupported backend feature") ||
      !require(contains_case_insensitive(socket_path.error().diagnostic, "socket") &&
                   contains_case_insensitive(socket_path.error().diagnostic, "psmux"),
               "socket-path rejection must name sockets and psmux")) {
    return EXIT_FAILURE;
  }

  auto ambiguous_default = Server::at_socket_name("default");
  if (!require(!ambiguous_default,
               "psmux must reject the ambiguous -L default namespace") ||
      !require(ambiguous_default.error().kind == libtmux::FailureKind::validation,
               "the -L default rejection must be a validation failure")) {
    return EXIT_FAILURE;
  }

  auto missing_result = Server::at_socket_name(socket_name + "-missing");
  if (!missing_result) {
    std::cerr << "FAIL: could not construct missing-namespace server: "
              << missing_result.error().diagnostic << '\n';
    return EXIT_FAILURE;
  }
  if (!require(!missing_result->is_alive(2s),
               "a nonexistent psmux namespace must not be alive")) {
    return EXIT_FAILURE;
  }

  auto server_result = Server::at_socket_name(socket_name);
  if (!server_result) {
    std::cerr << "FAIL: could not construct psmux server: "
              << server_result.error().diagnostic << '\n';
    return EXIT_FAILURE;
  }
  ScopedServer scoped{std::move(*server_result), socket_name};
  Server& server = scoped.get();

  const auto capabilities = server.capabilities();
  if (!require(capabilities.implementation == libtmux::ServerImplementation::psmux &&
                   capabilities.backend == libtmux::BackendKind::subprocess &&
                   capabilities.supports(libtmux::ServerFeature::exact_inspection) &&
                   capabilities.supports(libtmux::ServerFeature::server_cleanup),
               "psmux must advertise only its proven local contract")) {
    return EXIT_FAILURE;
  }
  for (const auto feature : {
           libtmux::ServerFeature::server_entity_lookup,
           libtmux::ServerFeature::session_creation,
           libtmux::ServerFeature::window_creation,
           libtmux::ServerFeature::captured_mutation,
           libtmux::ServerFeature::pane_io,
           libtmux::ServerFeature::terminal_attach,
           libtmux::ServerFeature::reusable_window_target,
           libtmux::ServerFeature::server_state,
           libtmux::ServerFeature::wait_channels,
           libtmux::ServerFeature::control_mode,
       }) {
    if (!require(!capabilities.supports(feature),
                 "psmux must fail unsupported capability checks closed")) {
      return EXIT_FAILURE;
    }
  }

  auto raw_version = server.run({"-V"});
  if (!raw_version) {
    std::cerr << "FAIL: tmux.exe -V failed: " << raw_version.error().diagnostic << '\n';
    return EXIT_FAILURE;
  }
  if (!require(raw_version->find("psmux ") != std::string::npos,
               "tmux.exe -V must identify psmux")) {
    return EXIT_FAILURE;
  }
  const auto parsed_version = libtmux::parse_version(*raw_version);
  if (!require(parsed_version.has_value(), "the psmux semantic version must parse")) {
    return EXIT_FAILURE;
  }
  const auto reported_version = server.tmux_version();
  if (!reported_version) {
    std::cerr << "FAIL: Server::tmux_version failed: "
              << reported_version.error().diagnostic << '\n';
    return EXIT_FAILURE;
  }
  if (!require(*reported_version == *parsed_version,
               "Server::tmux_version must parse the psmux version line")) {
    return EXIT_FAILURE;
  }
  config_cleanup.note_session_may_exist(socket_name, "must-not-create");
  const auto typed_creation = server.new_session("must-not-create");
  if (!require(!typed_creation &&
                   typed_creation.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(typed_creation.error()),
               "racy psmux session creation must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  config_cleanup.note_session_may_exist(socket_name, "alpha");
  const auto alpha_created =
      server.run({"new-session", "-d", "-s", "alpha", "-n", "alpha-main", "--", "cmd"});
  if (!require(alpha_created.has_value(), "could not create isolated alpha fixture")) {
    return EXIT_FAILURE;
  }
  const auto alpha_identity = wait_for_durable_session(socket_name, "alpha", 2s);
  if (!require(alpha_identity.has_value(),
               "alpha identity did not become durable before its deadline")) {
    return EXIT_FAILURE;
  }

  const std::string completion_channel = socket_name + "-submit-completion";
  DelayedSignal completion_fallback{socket_name, completion_channel, 2s};
  const auto submit_started = std::chrono::steady_clock::now();
  auto submitted = server.submit({"wait-for", completion_channel}, 10s);
  const auto submit_took = std::chrono::steady_clock::now() - submit_started;
  const auto completion_signal =
      raw_psmux(socket_name, {"wait-for", "-S", completion_channel});
  completion_fallback.stop();
  if (!submitted) {
    report_command("Server::submit rejected a psmux waiter", submitted.error());
    return EXIT_FAILURE;
  }
  auto completed = std::move(*submitted).wait();
  if (!require(submit_took < 500ms,
               "Server::submit must return before psmux completes") ||
      !require(completion_signal && completion_signal->exit_code == 0,
               "could not release the submitted psmux waiter") ||
      !require(completed.has_value(),
               "a released submitted psmux waiter must complete")) {
    return EXIT_FAILURE;
  }

  const std::string cancellation_channel = socket_name + "-submit-cancellation";
  DelayedSignal cancellation_fallback{socket_name, cancellation_channel, 2s};
  auto cancellable = server.submit({"wait-for", cancellation_channel}, 10s);
  std::this_thread::sleep_for(200ms);
  const bool cancellation_requested =
      cancellable.has_value() && cancellable->request_cancel();
  std::optional<libtmux::expected<std::string, CommandFailure>> cancelled;
  if (cancellable.has_value()) {
    cancelled = std::move(*cancellable).wait();
  }
  const auto cancellation_cleanup =
      raw_psmux(socket_name, {"wait-for", "-S", cancellation_channel});
  cancellation_fallback.stop();
  if (!require(cancellation_requested,
               "a running submitted psmux command must accept cancellation") ||
      !require(
          cancelled.has_value() && !cancelled->has_value() &&
              cancelled->error().kind == libtmux::FailureKind::cancelled &&
              cancelled->error().delivery == libtmux::DeliveryStatus::indeterminate,
          "a cancelled submitted psmux command must report indeterminate delivery") ||
      !require(cancellation_cleanup && cancellation_cleanup->exit_code == 0,
               "could not release the cancelled psmux wait channel")) {
    return EXIT_FAILURE;
  }

  config_cleanup.note_session_may_exist(socket_name, "beta");
  const auto beta_created = raw_psmux(
      socket_name, {"new-session", "-d", "-s", "beta", "-x", "117", "-y", "31"});
  if (!require(beta_created && beta_created->exit_code == 0,
               "could not create isolated beta fixture")) {
    return EXIT_FAILURE;
  }
  const auto beta_identity = wait_for_durable_session(socket_name, "beta", 2s);
  if (!require(beta_identity.has_value() && beta_identity->id > alpha_identity->id &&
                   beta_identity->next_id > beta_identity->id,
               "beta identity was not durable and distinct from alpha")) {
    return EXIT_FAILURE;
  }
  auto first_session = server.session("alpha");
  if (!first_session) {
    report_command("could not acquire session alpha", first_session.error());
    return EXIT_FAILURE;
  }
  auto second_session = server.session("beta");
  if (!second_session) {
    report_command("could not acquire session beta", second_session.error());
    return EXIT_FAILURE;
  }
  if (!require(first_session->id() == "$" + std::to_string(alpha_identity->id) &&
                   second_session->id() == "$" + std::to_string(beta_identity->id),
               "typed psmux sessions did not retain their durable registry IDs")) {
    return EXIT_FAILURE;
  }

  const auto registry = psmux_data_directory();
  if (!registry.has_value()) {
    std::cerr << "FAIL: could not locate psmux's global registry\n";
    return EXIT_FAILURE;
  }
  const std::filesystem::path alpha_base = *registry / (socket_name + "__alpha");
  const std::filesystem::path duplicate_base =
      *registry / (socket_name + "__duplicate");
  const std::filesystem::path duplicate_port{duplicate_base.string() + ".port"};
  const std::filesystem::path duplicate_key{duplicate_base.string() + ".key"};
  const std::filesystem::path duplicate_pid{duplicate_base.string() + ".pid"};
  const std::filesystem::path duplicate_sid{duplicate_base.string() + ".sid"};
  ScopedFile duplicate_port_cleanup{duplicate_port};
  ScopedFile duplicate_key_cleanup{duplicate_key};
  ScopedFile duplicate_pid_cleanup{duplicate_pid};
  ScopedFile duplicate_sid_cleanup{duplicate_sid};
  std::error_code duplicate_error;
  std::filesystem::copy_file(
      std::filesystem::path{alpha_base.string() + ".port"}, duplicate_port,
      std::filesystem::copy_options::overwrite_existing, duplicate_error);
  if (!duplicate_error) {
    std::filesystem::copy_file(
        std::filesystem::path{alpha_base.string() + ".key"}, duplicate_key,
        std::filesystem::copy_options::overwrite_existing, duplicate_error);
  }
  if (!duplicate_error) {
    std::filesystem::copy_file(
        std::filesystem::path{alpha_base.string() + ".pid"}, duplicate_pid,
        std::filesystem::copy_options::overwrite_existing, duplicate_error);
  }
  if (duplicate_error) {
    std::cerr << "FAIL: could not duplicate a live psmux registry entry: "
              << duplicate_error.message() << '\n';
    return EXIT_FAILURE;
  }
  {
    std::ofstream duplicate{duplicate_sid};
    duplicate << std::string_view{first_session->id()}.substr(1U) << '\n';
    if (!duplicate) {
      std::cerr << "FAIL: could not create a duplicate psmux session id\n";
      return EXIT_FAILURE;
    }
  }
  const auto duplicate_listing = server.sessions();
  if (!require(!duplicate_listing &&
                   duplicate_listing.error().kind == libtmux::FailureKind::refused &&
                   contains_case_insensitive(duplicate_listing.error().diagnostic,
                                             "session ID"),
               "duplicate live psmux registry entries must fail closed") ||
      !require(duplicate_sid_cleanup.finish() && duplicate_pid_cleanup.finish() &&
                   duplicate_key_cleanup.finish() && duplicate_port_cleanup.finish(),
               "could not remove the duplicate live psmux registry probe")) {
    return EXIT_FAILURE;
  }

  config_cleanup.note_session_may_exist(socket_name, "alpha.1");
  auto ambiguous_session = server.new_session("alpha.1");
  if (!require(!ambiguous_session,
               "psmux target-shaped session names must be rejected") ||
      !require(ambiguous_session.error().kind == libtmux::FailureKind::validation,
               "a target-shaped session name must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  config_cleanup.note_session_may_exist(socket_name, "safe ; kill-session");
  const auto unsafe_session = server.new_session("safe ; kill-session");
  if (!require(!unsafe_session &&
                   unsafe_session.error().kind == libtmux::FailureKind::validation &&
                   not_started(unsafe_session.error()),
               "a psmux command separator in a session name must not dispatch")) {
    return EXIT_FAILURE;
  }
  const auto unsafe_inherited_path = first_session->new_window("must-not-open");
  if (!require(!unsafe_inherited_path &&
                   unsafe_inherited_path.error().kind ==
                       libtmux::FailureKind::unsupported &&
                   not_started(unsafe_inherited_path.error()),
               "typed psmux new-window must fail before dispatch")) {
    return EXIT_FAILURE;
  }

  const std::string nested_socket = socket_name + "__nested";
  config_cleanup.note_session_may_exist(nested_socket, "inner");
  const auto nested_created =
      raw_psmux(nested_socket, {"new-session", "-d", "-s", "inner", "-P"});
  if (!require(nested_created.has_value() && nested_created->exit_code == 0,
               "could not create the adversarial nested psmux namespace")) {
    return EXIT_FAILURE;
  }
  ScopedRawSession nested{nested_socket, "inner"};

  auto exact_sessions = server.sessions();
  if (!exact_sessions) {
    report_command("could not list the exact psmux namespace", exact_sessions.error());
    return EXIT_FAILURE;
  }
  bool saw_alpha = false;
  bool saw_beta = false;
  bool saw_inner = false;
  for (const auto& session : *exact_sessions) {
    saw_alpha = saw_alpha || session.name() == "alpha";
    saw_beta = saw_beta || session.name() == "beta";
    saw_inner = saw_inner || session.name() == "inner";
  }
  if (!require(saw_alpha && saw_beta && !saw_inner && exact_sessions->size() == 2U,
               "session listing must filter nested -L prefix collisions")) {
    return EXIT_FAILURE;
  }
  if (!require(server.is_alive(2s), "the populated psmux namespace must be alive")) {
    return EXIT_FAILURE;
  }
  const auto server_expansion = server.expand("server-expand-ok");
  if (!require(!server_expansion &&
                   server_expansion.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(server_expansion.error()),
               "unscoped psmux format state must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto shell_ran = server.run_shell("cmd /c exit 0");
  if (!require(!shell_ran &&
                   shell_ran.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(shell_ran.error()),
               "unscoped psmux run-shell state must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto bound =
      server.bind_key("prefix", "F12", {"display-message", "psmux-binding"});
  if (!require(!bound && bound.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(bound.error()),
               "unscoped psmux key bindings must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto commands = server.commands();
  if (!require(!commands &&
                   commands.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(commands.error()),
               "psmux command metadata must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto scoped_options = server.options("alpha");
  const auto scoped_hooks = server.hooks("alpha");
  if (!require(!scoped_options && !scoped_hooks &&
                   scoped_options.error().kind == libtmux::FailureKind::unsupported &&
                   scoped_hooks.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(scoped_options.error()) &&
                   not_started(scoped_hooks.error()),
               "psmux session-scoped server state must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto checked = server.check_file(L"libtmux-psmux-must-not-run.conf");
  if (!require(!checked, "psmux check_file must be rejected") ||
      !require(checked.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(checked.error()),
               "check_file must be rejected before psmux can execute it")) {
    return EXIT_FAILURE;
  }
  const auto leading_buffer = server.set_buffer("", "-unsafe");
  if (!require(!leading_buffer, "psmux must reject unscoped buffer state") ||
      !require(leading_buffer.error().kind == libtmux::FailureKind::unsupported,
               "unscoped buffer state must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto first_session_name = first_session->expand("#{session_name}");
  if (!first_session_name) {
    report_command("could not expand alpha's session name", first_session_name.error());
    return EXIT_FAILURE;
  }
  if (!require(*first_session_name == "alpha",
               "a name-qualified session must retain its numeric identity")) {
    return EXIT_FAILURE;
  }
  for (const std::string_view unsafe_format : {
           std::string_view{"#{session_name} ; kill-session"},
           std::string_view{"#{session_name}\nkill-session"},
           std::string_view{"#{session_name}\rkill-session"},
       }) {
    const auto rejected_format = first_session->expand(unsafe_format);
    if (!require(!rejected_format &&
                     rejected_format.error().kind == libtmux::FailureKind::validation &&
                     not_started(rejected_format.error()),
                 "psmux command delimiters must fail before typed dispatch")) {
      return EXIT_FAILURE;
    }
  }
  const auto detached_clients = first_session->detach_clients();
  if (!require(!detached_clients &&
                   detached_clients.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(detached_clients.error()),
               "unsafe psmux detach-client must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto session_message = first_session->show_message("must-not-dispatch");
  if (!require(!session_message &&
                   session_message.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(session_message.error()),
               "unsafe psmux session messages must fail before dispatch")) {
    return EXIT_FAILURE;
  }

  const std::wstring wide_socket_name{socket_name.begin(), socket_name.end()};
  const std::wstring tmux_environment = L"/tmp/psmux-" +
                                        std::to_wstring(GetCurrentProcessId()) + L"/" +
                                        wide_socket_name + L",4242,0";
  const bool set_tmux = SetEnvironmentVariableW(L"TMUX", tmux_environment.c_str()) != 0;
  const bool set_psmux = SetEnvironmentVariableW(L"PSMUX_SESSION", L"alpha") != 0;
  auto inherited_server = Server::from_env();
  SetEnvironmentVariableW(L"TMUX", L"C:\\outside\\default,1,0");
  SetEnvironmentVariableW(L"PSMUX_SESSION", L"outside");
  if (!require(set_tmux && set_psmux,
               "could not install a process-local psmux environment")) {
    return EXIT_FAILURE;
  }
  if (!inherited_server) {
    std::cerr << "FAIL: Server::from_env rejected a psmux environment: "
              << inherited_server.error().diagnostic << '\n';
    return EXIT_FAILURE;
  }
  auto inherited_sessions = inherited_server->sessions();
  if (!inherited_sessions) {
    report_command("Server::from_env did not reach the psmux namespace",
                   inherited_sessions.error());
    return EXIT_FAILURE;
  }
  bool inherited_alpha = false;
  bool inherited_beta = false;
  for (const auto& session : *inherited_sessions) {
    inherited_alpha = inherited_alpha || session.name() == "alpha";
    inherited_beta = inherited_beta || session.name() == "beta";
  }
  if (!require(inherited_alpha && inherited_beta,
               "Server::from_env must preserve the psmux -L namespace")) {
    return EXIT_FAILURE;
  }

  auto first_window = first_session->active_window();
  if (!first_window) {
    report_command("could not read alpha's active window", first_window.error());
    return EXIT_FAILURE;
  }
  if (!require(first_window->name() == "alpha-main",
               "psmux must preserve a safe initial window name")) {
    return EXIT_FAILURE;
  }
  auto second_window = second_session->active_window();
  if (!second_window) {
    report_command("could not read beta's active window", second_window.error());
    return EXIT_FAILURE;
  }
  const auto stale_probe_created = raw_psmux(
      socket_name, {"new-window", "-d", "-t", "alpha:", "-n", "stale-window-probe"});
  auto alpha_windows_with_probe = first_session->windows();
  std::optional<libtmux::Window> stale_window_probe;
  if (alpha_windows_with_probe) {
    for (const auto& window : *alpha_windows_with_probe) {
      if (window.name() == "stale-window-probe") {
        stale_window_probe = window;
      }
    }
  }
  const auto stale_probe_killed =
      raw_psmux(socket_name, {"kill-window", "-t", "alpha:stale-window-probe"});
  if (!require(stale_probe_created && stale_probe_created->exit_code == 0 &&
                   alpha_windows_with_probe && stale_window_probe.has_value() &&
                   stale_probe_killed && stale_probe_killed->exit_code == 0,
               "could not create and remove the stale-window probe")) {
    return EXIT_FAILURE;
  }
  const auto stale_probe_panes = stale_window_probe->panes();
  if (!require(!stale_probe_panes &&
                   stale_probe_panes.error().kind == libtmux::FailureKind::missing,
               "a stale psmux window must not return an empty successful pane list")) {
    return EXIT_FAILURE;
  }
  const auto no_next_window = first_session->select_next_window();
  const auto no_previous_window = first_session->select_previous_window();
  const auto no_last_window = first_session->select_last_window();
  if (!require(!no_next_window && !no_previous_window && !no_last_window &&
                   no_next_window.error().kind == libtmux::FailureKind::unsupported &&
                   no_previous_window.error().kind ==
                       libtmux::FailureKind::unsupported &&
                   no_last_window.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(no_next_window.error()) &&
                   not_started(no_previous_window.error()) &&
                   not_started(no_last_window.error()),
               "psmux session navigation must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto unsupported_new_window = first_session->new_window("must-not-open");
  if (!require(!unsupported_new_window &&
                   unsupported_new_window.error().kind ==
                       libtmux::FailureKind::unsupported &&
                   not_started(unsupported_new_window.error()),
               "untruthful psmux new-window must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto ignored_split = first_window->split(libtmux::SplitOptions{.before = true});
  if (!require(!ignored_split &&
                   ignored_split.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(ignored_split.error()),
               "psmux-ignored split-window options must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  if (!require(first_window->id() == second_window->id(),
               "psmux should expose its repeated per-session window ID") ||
      !require(first_window->session_id() != second_window->session_id(),
               "repeated window IDs must retain distinct session IDs") ||
      !require(*first_window != *second_window,
               "window identity must disambiguate repeated IDs by session")) {
    return EXIT_FAILURE;
  }
  const auto first_window_name = first_window->expand("#{session_name}");
  if (!first_window_name) {
    report_command("could not target alpha's repeated window ID",
                   first_window_name.error());
    return EXIT_FAILURE;
  }
  const auto second_window_name = second_window->expand("#{session_name}");
  if (!second_window_name) {
    report_command("could not target beta's repeated window ID",
                   second_window_name.error());
    return EXIT_FAILURE;
  }
  if (!require(*first_window_name == "alpha" && *second_window_name == "beta",
               "window commands must target the owning psmux session")) {
    return EXIT_FAILURE;
  }
  constexpr std::string_view escaped_format = R"psmux(C:\\tmp\"quoted"\tail)psmux";
  const auto escaped_expansion = first_window->expand(escaped_format);
  if (!require(escaped_expansion && *escaped_expansion == escaped_format,
               "psmux format expansion must preserve backslashes and quotes")) {
    return EXIT_FAILURE;
  }
  const auto renamed_window = first_window->rename("alpha-window");
  if (!require(!renamed_window &&
                   renamed_window.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(renamed_window.error()),
               "unsafe psmux rename-window must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto refreshed_window = first_window->refresh();
  if (!refreshed_window) {
    report_command("could not refresh the renamed psmux window",
                   refreshed_window.error());
    return EXIT_FAILURE;
  }
  if (!require(refreshed_window->id() == first_window->id() &&
                   refreshed_window->name() == first_window->name(),
               "rejecting rename-window must leave the window intact")) {
    return EXIT_FAILURE;
  }
  const auto killed_window = first_window->kill();
  const auto unlinked_window = first_window->unlink();
  if (!require(!killed_window && !unlinked_window &&
                   killed_window.error().kind == libtmux::FailureKind::unsupported &&
                   unlinked_window.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(killed_window.error()) &&
                   not_started(unlinked_window.error()),
               "destructive active-fallback window calls must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto resized_window = first_window->resize(100, 30);
  if (!require(!resized_window &&
                   resized_window.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(resized_window.error()),
               "psmux resize-window no-op must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto all_windows = server.windows();
  if (!all_windows) {
    report_command("Server::windows failed through psmux", all_windows.error());
    return EXIT_FAILURE;
  }
  bool nested_window = false;
  for (const auto& window : *all_windows) {
    nested_window = nested_window || window.session_name() == "inner";
  }
  if (!require(all_windows->size() == 2U && !nested_window,
               "server-wide windows must exclude nested namespaces")) {
    return EXIT_FAILURE;
  }
  const auto ambiguous_window = server.window(first_window->id());
  if (!require(!ambiguous_window, "global psmux window lookup must fail closed") ||
      !require(ambiguous_window.error().kind == libtmux::FailureKind::unsupported,
               "global psmux window lookup must report unsupported")) {
    return EXIT_FAILURE;
  }

  auto first_pane = first_session->active_pane();
  if (!first_pane) {
    report_command("could not read alpha's active pane", first_pane.error());
    return EXIT_FAILURE;
  }
  if (!require(!first_pane->dead(),
               "psmux must resolve extensionless cmd with repaired PATHEXT")) {
    return EXIT_FAILURE;
  }
  const auto panes_in_first_window = first_window->panes();
  const auto active_in_first_window = first_window->active_pane();
  const auto owner_of_first_pane = first_pane->window();
  const auto session_of_first_window = first_window->session();
  const auto session_of_first_pane = first_pane->session();
  if (!require(panes_in_first_window && panes_in_first_window->size() == 1U &&
                   active_in_first_window &&
                   active_in_first_window->id() == first_pane->id() &&
                   owner_of_first_pane &&
                   owner_of_first_pane->id() == first_window->id() &&
                   session_of_first_window && session_of_first_pane &&
                   session_of_first_window->id() == first_session->id() &&
                   session_of_first_pane->id() == first_session->id(),
               "psmux child relations must filter through the stable session")) {
    return EXIT_FAILURE;
  }
  auto second_pane = second_session->active_pane();
  if (!second_pane) {
    report_command("could not read beta's active pane", second_pane.error());
    return EXIT_FAILURE;
  }
  if (!require(second_pane->width() == 117 && second_pane->height() == 31,
               "the native psmux fixture must preserve initial dimensions") ||
      !require(first_pane->id() == second_pane->id(),
               "psmux should expose its repeated per-session pane ID") ||
      !require(first_pane->session_id() != second_pane->session_id(),
               "repeated pane IDs must retain distinct session IDs") ||
      !require(*first_pane != *second_pane,
               "pane identity must disambiguate repeated IDs by session")) {
    return EXIT_FAILURE;
  }

  const auto first_name = first_pane->expand("#{session_name}");
  if (!first_name) {
    report_command("could not target alpha's repeated pane ID", first_name.error());
    return EXIT_FAILURE;
  }
  const auto second_name = second_pane->expand("#{session_name}");
  if (!second_name) {
    report_command("could not target beta's repeated pane ID", second_name.error());
    return EXIT_FAILURE;
  }
  if (!require(*first_name == "alpha" && *second_name == "beta",
               "pane commands must target the owning psmux session")) {
    return EXIT_FAILURE;
  }
  const auto unsafe_text = first_pane->send_text("-psmux-would-drop-this");
  if (!require(!unsafe_text &&
                   unsafe_text.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(unsafe_text.error()),
               "psmux-dropped leading-dash text must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto trailing_capture =
      first_pane->capture(libtmux::CaptureOptions{.keep_trailing_spaces = true});
  if (!require(!trailing_capture &&
                   trailing_capture.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(trailing_capture.error()),
               "psmux-ignored capture options must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto pane_options = first_pane->options();
  if (!require(!pane_options &&
                   pane_options.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(pane_options.error()),
               "unsupported psmux pane options must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto respawned = first_pane->respawn(false);
  if (!require(!respawned &&
                   respawned.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(respawned.error()),
               "unsafe psmux respawn-pane must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto broken_out = first_pane->break_out("must-not-exist");
  if (!require(!broken_out &&
                   broken_out.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(broken_out.error()),
               "unreportable psmux break-pane must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto joined = first_pane->join(*second_window);
  if (!require(!joined && joined.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(joined.error()),
               "unsafe psmux join-pane must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto swapped = first_pane->swap_with(*second_pane);
  if (!require(!swapped && swapped.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(swapped.error()),
               "unsafe psmux swap-pane must fail before dispatch")) {
    return EXIT_FAILURE;
  }
  const auto all_panes = server.panes();
  if (!all_panes) {
    report_command("Server::panes failed through psmux", all_panes.error());
    return EXIT_FAILURE;
  }
  bool nested_pane = false;
  for (const auto& pane : *all_panes) {
    nested_pane = nested_pane || pane.session_name() == "inner";
  }
  if (!require(all_panes->size() == 2U && !nested_pane,
               "server-wide panes must exclude nested namespaces")) {
    return EXIT_FAILURE;
  }
  const auto ambiguous_pane = server.pane(first_pane->id());
  if (!require(!ambiguous_pane, "global psmux pane lookup must fail closed") ||
      !require(ambiguous_pane.error().kind == libtmux::FailureKind::unsupported,
               "global psmux pane lookup must report unsupported")) {
    return EXIT_FAILURE;
  }

  const auto position = first_session->option("status-position");
  const auto session_options = first_session->options();
  const auto position_set = first_session->set_option("status-position", "top");
  const auto position_unset = first_session->unset_option("status-position");
  if (!require(
          !position && !session_options && !position_set && !position_unset &&
              position.error().kind == libtmux::FailureKind::unsupported &&
              session_options.error().kind == libtmux::FailureKind::unsupported &&
              position_set.error().kind == libtmux::FailureKind::unsupported &&
              position_unset.error().kind == libtmux::FailureKind::unsupported &&
              not_started(position.error()) && not_started(session_options.error()) &&
              not_started(position_set.error()) && not_started(position_unset.error()),
          "psmux session option state must fail before dispatch")) {
    return EXIT_FAILURE;
  }

  const auto hook_set =
      first_session->set_hook("after-new-window", "display-message libtmux-psmux-hook");
  const auto hooks = first_session->hooks();
  if (!require(!hooks && !hook_set &&
                   hooks.error().kind == libtmux::FailureKind::unsupported &&
                   hook_set.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(hooks.error()) && not_started(hook_set.error()),
               "psmux session hook state must fail before dispatch")) {
    return EXIT_FAILURE;
  }

  const auto selected_pane = first_pane->select();
  const auto killed_pane = first_pane->kill();
  const auto beta_after_pane_rejection = second_pane->expand("#{session_name}");
  if (!require(!selected_pane && !killed_pane &&
                   selected_pane.error().kind == libtmux::FailureKind::unsupported &&
                   killed_pane.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(selected_pane.error()) &&
                   not_started(killed_pane.error()) && beta_after_pane_rejection &&
                   *beta_after_pane_rejection == "beta",
               "captured psmux pane mutations must fail before dispatch")) {
    return EXIT_FAILURE;
  }

  const auto sent = first_pane->send_text("echo LIBTMUX_PSMUX_SMOKE_OK");
  const auto entered = first_pane->send_key("Enter");
  const auto captured = first_pane->capture();
  if (!require(!sent && !entered && !captured &&
                   sent.error().kind == libtmux::FailureKind::unsupported &&
                   entered.error().kind == libtmux::FailureKind::unsupported &&
                   captured.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(sent.error()) && not_started(entered.error()) &&
                   not_started(captured.error()),
               "active-fallback pane I/O must fail before dispatch")) {
    return EXIT_FAILURE;
  }

  const auto attach = first_session->attach_command();
  const auto checked_target = first_window->checked_target();
  if (!require(!attach && !checked_target && first_window->target().empty() &&
                   attach.error().kind == libtmux::FailureKind::unsupported &&
                   checked_target.error().kind == libtmux::FailureKind::unsupported &&
                   not_started(attach.error()) && not_started(checked_target.error()),
               "checked psmux targets must explain the compatibility sentinel")) {
    return EXIT_FAILURE;
  }

  const auto clobbering_rename = first_session->rename("beta");
  if (!require(!clobbering_rename &&
                   clobbering_rename.error().kind ==
                       libtmux::FailureKind::unsupported &&
                   not_started(clobbering_rename.error()),
               "psmux rename-session must fail before it can clobber beta")) {
    return EXIT_FAILURE;
  }
  const auto beta_after_rejection = second_session->expand("#{session_name}");
  if (!require(beta_after_rejection && *beta_after_rejection == "beta",
               "rejecting rename-session must leave beta intact")) {
    return EXIT_FAILURE;
  }
  config_cleanup.note_session_may_exist(socket_name, "gamma");
  const auto external_rename =
      raw_psmux(socket_name, {"rename-session", "-t", "alpha", "gamma"});
  if (!require(external_rename.has_value() && external_rename->exit_code == 0,
               "could not simulate an external psmux session rename")) {
    return EXIT_FAILURE;
  }
  if (!require(wait_for_exact_rename(socket_name, "alpha", "gamma", 2s),
               "external psmux rename did not settle before its deadline")) {
    return EXIT_FAILURE;
  }
  const auto renamed_gamma = server.session("gamma");
  if (!require(renamed_gamma && renamed_gamma->id() == first_session->id(),
               "the exact renamed psmux session must retain its identity")) {
    return EXIT_FAILURE;
  }
  const auto gamma_killed = raw_psmux(socket_name, {"kill-session", "-t", "gamma"});
  if (!require(gamma_killed && gamma_killed->exit_code == 0 &&
                   wait_for_registry_absence(socket_name, "gamma", 2s),
               "could not remove the renamed fixture before reusing its old name")) {
    return EXIT_FAILURE;
  }
  config_cleanup.note_session_may_exist(socket_name, "alpha");
  const auto replacement_created =
      raw_psmux(socket_name, {"new-session", "-d", "-s", "alpha"});
  if (!require(replacement_created && replacement_created->exit_code == 0,
               "could not create the replacement alpha fixture")) {
    return EXIT_FAILURE;
  }
  auto replacement_alpha = server.session("alpha");
  if (!replacement_alpha) {
    report_command("could not acquire the replacement alpha session",
                   replacement_alpha.error());
    return EXIT_FAILURE;
  }
  const auto refreshed_session = first_session->refresh();
  const auto renamed_window_owner = first_window->expand("#{session_name}");
  const auto renamed_pane_owner = first_pane->expand("#{session_name}");
  const auto stale_window_session = first_window->session();
  const auto stale_window_panes = first_window->panes();
  const auto stale_window_active_pane = first_window->active_pane();
  const auto stale_pane_window = first_pane->window();
  const auto stale_pane_session = first_pane->session();
  if (!require(
          !refreshed_session && !renamed_window_owner && !renamed_pane_owner &&
              !stale_window_session && !stale_window_panes &&
              !stale_window_active_pane && !stale_pane_window && !stale_pane_session &&
              refreshed_session.error().kind == libtmux::FailureKind::missing &&
              renamed_window_owner.error().kind == libtmux::FailureKind::missing &&
              renamed_pane_owner.error().kind == libtmux::FailureKind::missing &&
              stale_window_session.error().kind == libtmux::FailureKind::missing &&
              stale_window_panes.error().kind == libtmux::FailureKind::missing &&
              stale_window_active_pane.error().kind == libtmux::FailureKind::missing &&
              stale_pane_window.error().kind == libtmux::FailureKind::missing &&
              stale_pane_session.error().kind == libtmux::FailureKind::missing,
          "stale psmux handles must not follow a replacement session")) {
    return EXIT_FAILURE;
  }
  const auto missing_gamma = server.session("gamma");
  const auto found_alpha = server.session("alpha");
  if (!require(!missing_gamma &&
                   missing_gamma.error().kind == libtmux::FailureKind::missing &&
                   found_alpha && found_alpha->id() == replacement_alpha->id(),
               "Server::session must use the exact filtered psmux listing")) {
    return EXIT_FAILURE;
  }

  const auto control = server.control("alpha");
  if (!require(!control, "psmux control mode must be rejected") ||
      !require(contains_case_insensitive(control.error().message, "control") &&
                   contains_case_insensitive(control.error().message, "psmux"),
               "control-mode rejection must name control mode and psmux")) {
    return EXIT_FAILURE;
  }
  const libtmux::ConnectionOptions unavailable_options{
      .tmux_binary = "Z:\\libtmux-must-not-launch\\tmux.exe",
      .socket_path = "Z:\\libtmux-must-not-use\\socket",
      .session_name = "caller-must-not-select-this",
      .pane_output = true,
      .pause_after = std::chrono::seconds{1},
  };
  const auto configured_control =
      server.control_with_options("alpha", unavailable_options);
  if (!require(
          !configured_control &&
              contains_case_insensitive(configured_control.error().message,
                                        "control") &&
              contains_case_insensitive(configured_control.error().message, "psmux"),
          "configured psmux control must fail closed before launch")) {
    return EXIT_FAILURE;
  }

  config_cleanup.note_session_may_exist(socket_name, "alpha.1");
  const auto dotted_created =
      raw_psmux(socket_name, {"new-session", "-d", "-s", "alpha.1"});
  config_cleanup.note_session_may_exist(socket_name, "=alpha");
  const auto equals_created =
      raw_psmux(socket_name, {"new-session", "-d", "-s", "=alpha"});
  if (!require(dotted_created && dotted_created->exit_code == 0 && equals_created &&
                   equals_created->exit_code == 0,
               "could not create target-shaped cleanup adversaries")) {
    return EXIT_FAILURE;
  }
  const auto cleanup_candidates = server.sessions();
  bool listed_dotted = false;
  bool listed_equals = false;
  if (cleanup_candidates) {
    for (const auto& session : *cleanup_candidates) {
      listed_dotted = listed_dotted || session.name() == "alpha.1";
      listed_equals = listed_equals || session.name() == "=alpha";
    }
  }
  if (!require(cleanup_candidates && listed_dotted && listed_equals,
               "typed listing must see both target-shaped cleanup adversaries")) {
    return EXIT_FAILURE;
  }

  if (!scoped.finish()) {
    return EXIT_FAILURE;
  }
  config_cleanup.prove_namespace_cleanup(socket_name);
  if (!require(nested.alive(),
               "killing a psmux namespace must not kill a nested prefix")) {
    return EXIT_FAILURE;
  }
  if (!require(nested.finish(),
               "could not clean the adversarial nested psmux session")) {
    return EXIT_FAILURE;
  }
  config_cleanup.prove_namespace_cleanup(nested_socket);
  if (!require(config_cleanup.finish(),
               "could not remove the task-owned psmux configuration")) {
    return EXIT_FAILURE;
  }
  std::cout << "PASS: native psmux " << parsed_version->major << '.'
            << parsed_version->minor << '.' << parsed_version->revision
            << " through namespace " << socket_name << '\n';
  return EXIT_SUCCESS;
}
