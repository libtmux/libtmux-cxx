#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "environment.hpp"
#include "process.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

using libtmux::DeliveryStatus;
using libtmux::detail::Argument;
using libtmux::detail::Exited;
using libtmux::detail::ProcessError;
using libtmux::detail::ProcessReply;
using libtmux::detail::ProcessRequest;
using libtmux::detail::run_process;

constexpr std::string_view kArgvMode{"--child-argv"};
constexpr std::string_view kEnvironmentMode{"--child-environment"};
constexpr std::string_view kStreamsMode{"--child-streams"};
constexpr std::string_view kOutputMode{"--child-output"};
constexpr std::string_view kSpawnMode{"--child-spawn"};
constexpr std::string_view kGrandchildMode{"--child-grandchild"};

class Handle final {
public:
  Handle() = default;
  explicit Handle(HANDLE value) noexcept : value_{value} {}
  ~Handle() { reset(); }

  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;

  Handle(Handle&& other) noexcept : value_{std::exchange(other.value_, nullptr)} {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] HANDLE get() const noexcept { return value_; }
  [[nodiscard]] bool valid() const noexcept {
    return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
  }

  void reset(HANDLE value = nullptr) noexcept {
    if (valid()) {
      static_cast<void>(::CloseHandle(value_));
    }
    value_ = value;
  }

private:
  HANDLE value_{nullptr};
};

struct EnvironmentValue final {
  bool present;
  std::wstring value;
};

[[nodiscard]] EnvironmentValue environment(std::wstring_view name) {
  ::SetLastError(ERROR_SUCCESS);
  const DWORD required = ::GetEnvironmentVariableW(name.data(), nullptr, 0);
  if (required == 0U) {
    return {.present = ::GetLastError() != ERROR_ENVVAR_NOT_FOUND, .value = {}};
  }
  std::wstring value(static_cast<std::size_t>(required), L'\0');
  const DWORD written = ::GetEnvironmentVariableW(name.data(), value.data(), required);
  if (written == 0U || written >= required) {
    return {.present = false, .value = {}};
  }
  value.resize(static_cast<std::size_t>(written));
  return {.present = true, .value = std::move(value)};
}

class EnvironmentGuard final {
public:
  explicit EnvironmentGuard(std::wstring name)
      : name_{std::move(name)}, original_{environment(name_)} {}
  ~EnvironmentGuard() {
    static_cast<void>(::SetEnvironmentVariableW(
        name_.c_str(), original_.present ? original_.value.c_str() : nullptr));
  }

  EnvironmentGuard(const EnvironmentGuard&) = delete;
  EnvironmentGuard& operator=(const EnvironmentGuard&) = delete;

private:
  std::wstring name_;
  EnvironmentValue original_;
};

class CurrentDirectoryGuard final {
public:
  CurrentDirectoryGuard() {
    const DWORD required = ::GetCurrentDirectoryW(0, nullptr);
    if (required == 0U) {
      return;
    }
    original_.resize(static_cast<std::size_t>(required), L'\0');
    const DWORD written = ::GetCurrentDirectoryW(required, original_.data());
    if (written == 0U || written >= required) {
      original_.clear();
      return;
    }
    original_.resize(static_cast<std::size_t>(written));
  }

  ~CurrentDirectoryGuard() {
    if (!original_.empty()) {
      static_cast<void>(::SetCurrentDirectoryW(original_.c_str()));
    }
  }

  CurrentDirectoryGuard(const CurrentDirectoryGuard&) = delete;
  CurrentDirectoryGuard& operator=(const CurrentDirectoryGuard&) = delete;

private:
  std::wstring original_;
};

class Checks final {
public:
  bool require(bool condition, std::string_view message) {
    if (!condition) {
      ++failures_;
      std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
  }

  void process_error(std::string_view operation, const ProcessError& error) {
    ++failures_;
    std::cerr << "FAIL: " << operation << ": " << error.diagnostic << '\n';
  }

  [[nodiscard]] int result() const noexcept {
    return failures_ == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }

private:
  int failures_{0};
};

[[nodiscard]] std::optional<std::string> utf8(std::wstring_view value) {
  if (value.empty()) {
    return std::string{};
  }
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }
  const auto input_size = static_cast<int>(value.size());
  const int required =
      ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size,
                            nullptr, 0, nullptr, nullptr);
  if (required == 0) {
    return std::nullopt;
  }
  std::string converted(static_cast<std::size_t>(required), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size,
                            converted.data(), required, nullptr, nullptr) == 0) {
    return std::nullopt;
  }
  return converted;
}

[[nodiscard]] std::wstring widen_ascii(std::string_view value) {
  std::wstring result;
  result.reserve(value.size());
  for (const char character : value) {
    result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
  }
  return result;
}

[[nodiscard]] bool write_all(HANDLE output, std::string_view value) {
  while (!value.empty()) {
    const auto wanted = static_cast<DWORD>(std::min<std::size_t>(
        value.size(), static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
    DWORD written = 0;
    if (::WriteFile(output, value.data(), wanted, &written, nullptr) == FALSE ||
        written == 0U) {
      return false;
    }
    value.remove_prefix(static_cast<std::size_t>(written));
  }
  return true;
}

[[nodiscard]] bool write_field(HANDLE output, std::wstring_view value) {
  const auto converted = utf8(value);
  if (!converted.has_value()) {
    return false;
  }
  constexpr char separator = '\0';
  return write_all(output, *converted) &&
         write_all(output, std::string_view{&separator, 1U});
}

[[nodiscard]] std::string bytes(const std::vector<std::byte>& value) {
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] std::optional<std::filesystem::path> executable_path() {
  std::vector<wchar_t> buffer(260U);
  for (;;) {
    ::SetLastError(ERROR_SUCCESS);
    const DWORD written =
        ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0U) {
      return std::nullopt;
    }
    if (written < buffer.size() - 1U ||
        (written < buffer.size() && ::GetLastError() != ERROR_INSUFFICIENT_BUFFER)) {
      return std::filesystem::path{
          std::wstring_view{buffer.data(), static_cast<std::size_t>(written)}};
    }
    if (buffer.size() >= static_cast<std::size_t>(32768U)) {
      return std::nullopt;
    }
    buffer.resize(std::min<std::size_t>(buffer.size() * 2U, 32768U));
  }
}

void append_quoted_argument(std::wstring& command_line, std::wstring_view argument) {
  command_line.push_back(L'"');
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      command_line.append(backslashes * 2U + 1U, L'\\');
      command_line.push_back(L'"');
      backslashes = 0;
      continue;
    }
    command_line.append(backslashes, L'\\');
    backslashes = 0;
    command_line.push_back(character);
  }
  command_line.append(backslashes * 2U, L'\\');
  command_line.push_back(L'"');
}

[[nodiscard]] ProcessRequest request_for(const std::filesystem::path& executable,
                                         const std::vector<std::string>& arguments) {
  ProcessRequest request;
  request.executable = executable;
  request.arguments.reserve(arguments.size());
  for (const auto& argument : arguments) {
    request.arguments.push_back(Argument{argument});
  }
  return request;
}

[[nodiscard]] const Exited* exited(const ProcessReply& reply) {
  return std::get_if<Exited>(&reply.termination);
}

void fields_equal(std::string_view encoded, const std::vector<std::string>& expected,
                  Checks& checks, std::string_view message) {
  std::size_t offset = 0;
  bool matches = true;
  for (const auto& field : expected) {
    const auto separator = encoded.find('\0', offset);
    if (separator == std::string_view::npos) {
      checks.require(false, message);
      return;
    }
    if (encoded.substr(offset, separator - offset) != field) {
      matches = false;
    }
    offset = separator + 1U;
  }
  matches = matches && offset == encoded.size();
  checks.require(matches, message);
}

[[nodiscard]] int child_argv(int argc, wchar_t* argv[]) {
  const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
  for (int index = 2; index < argc; ++index) {
    if (!write_field(output, argv[index])) {
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}

[[nodiscard]] int child_environment() {
  constexpr std::wstring_view names[]{L"LIBTMUX_PROCESS_CASE_SET",
                                      L"LIBTMUX_PROCESS_CASE_REMOVE",
                                      L"LIBTMUX_PROCESS_UNICODE"};
  const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
  for (const auto name : names) {
    const auto value = environment(name);
    if (value.present) {
      if (!write_field(output, value.value)) {
        return EXIT_FAILURE;
      }
    } else if (!write_field(output, L"<missing>")) {
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}

[[nodiscard]] int child_streams(int argc, wchar_t* argv[]) {
  if (!write_all(::GetStdHandle(STD_OUTPUT_HANDLE), "stdout-token") ||
      !write_all(::GetStdHandle(STD_ERROR_HANDLE), "stderr-token")) {
    return EXIT_FAILURE;
  }
  if (argc != 3) {
    return EXIT_FAILURE;
  }
  wchar_t* end = nullptr;
  const long code = std::wcstol(argv[2], &end, 10);
  if (end == argv[2] || *end != L'\0') {
    return EXIT_FAILURE;
  }
  return static_cast<int>(code);
}

[[nodiscard]] int child_output() {
  return write_all(::GetStdHandle(STD_OUTPUT_HANDLE), std::string(64U, 'o')) &&
                 write_all(::GetStdHandle(STD_ERROR_HANDLE), std::string(64U, 'e'))
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}

[[nodiscard]] int child_grandchild(int argc, wchar_t* argv[]) {
  if (argc != 3) {
    return EXIT_FAILURE;
  }
  Handle ready{::OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[2])};
  if (!ready.valid() || ::SetEvent(ready.get()) == FALSE) {
    return EXIT_FAILURE;
  }
  ::Sleep(INFINITE);
  return EXIT_FAILURE;
}

[[nodiscard]] int child_spawn(int argc, wchar_t* argv[]) {
  if (argc != 3) {
    return EXIT_FAILURE;
  }
  const auto self = executable_path();
  if (!self.has_value()) {
    return EXIT_FAILURE;
  }
  Handle ready{::CreateEventW(nullptr, TRUE, FALSE, argv[2])};
  if (!ready.valid()) {
    return EXIT_FAILURE;
  }

  std::wstring command_line;
  append_quoted_argument(command_line, self->wstring());
  command_line.push_back(L' ');
  append_quoted_argument(command_line, widen_ascii(kGrandchildMode));
  command_line.push_back(L' ');
  append_quoted_argument(command_line, argv[2]);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION information{};
  if (::CreateProcessW(self->c_str(), command_line.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &information) == FALSE) {
    return EXIT_FAILURE;
  }
  Handle process{information.hProcess};
  Handle thread{information.hThread};
  if (::WaitForSingleObject(ready.get(), 5000U) != WAIT_OBJECT_0) {
    static_cast<void>(::TerminateProcess(process.get(), ERROR_PROCESS_ABORTED));
    return EXIT_FAILURE;
  }

  const std::string pid = std::to_string(information.dwProcessId) + "\n";
  if (!write_all(::GetStdHandle(STD_OUTPUT_HANDLE), pid)) {
    return EXIT_FAILURE;
  }
  ::Sleep(INFINITE);
  return EXIT_FAILURE;
}

[[nodiscard]] std::optional<int> child_mode(int argc, wchar_t* argv[]) {
  if (argc < 2) {
    return std::nullopt;
  }
  const std::wstring_view mode{argv[1]};
  if (mode == widen_ascii(kArgvMode)) {
    return child_argv(argc, argv);
  }
  if (mode == widen_ascii(kEnvironmentMode)) {
    return child_environment();
  }
  if (mode == widen_ascii(kStreamsMode)) {
    return child_streams(argc, argv);
  }
  if (mode == widen_ascii(kOutputMode)) {
    return child_output();
  }
  if (mode == widen_ascii(kSpawnMode)) {
    return child_spawn(argc, argv);
  }
  if (mode == widen_ascii(kGrandchildMode)) {
    return child_grandchild(argc, argv);
  }
  return std::nullopt;
}

void test_argv(const std::filesystem::path& self, Checks& checks) {
  std::vector<std::string> expected{"",
                                    "two words",
                                    "quote\"inside",
                                    std::string{"slashes"} + std::string(3U, '\\') +
                                        "\"inside",
                                    std::string{"trailing"} + std::string(3U, '\\'),
                                    "\xe9\x9b\xaa\xe2\x98\x83\xf0\x9f\x98\x80"};
  std::vector<std::string> arguments{std::string{kArgvMode}};
  arguments.insert(arguments.end(), expected.begin(), expected.end());
  const auto reply = run_process(request_for(self, arguments));
  if (!reply.has_value()) {
    checks.process_error("argv child did not run", reply.error());
    return;
  }
  checks.require(exited(*reply) != nullptr && exited(*reply)->code == 0,
                 "argv child must exit zero");
  fields_equal(bytes(reply->stdout_bytes), expected, checks,
               "empty, spaced, quoted, backslash, and Unicode argv must round-trip");
  checks.require(reply->stderr_bytes.empty(), "argv child must not write stderr");
}

void test_repaired_pathext(Checks& checks) {
  constexpr std::string_view standard{".COM;.EXE;.BAT;.CMD"};
  const auto expected_repair = [standard](std::string_view inherited) {
    return std::string{standard} + ";" + std::string{inherited};
  };

  checks.require(libtmux_env::repaired_pathext(std::nullopt) ==
                     std::optional<std::string>{standard},
                 "a missing PATHEXT must receive the standard executable tokens");
  checks.require(libtmux_env::repaired_pathext(std::string{}) ==
                     std::optional<std::string>{standard},
                 "an empty PATHEXT must not add a trailing separator");
  checks.require(libtmux_env::repaired_pathext(std::string{".CPL"}) ==
                     std::optional<std::string>{expected_repair(".CPL")},
                 "a PATHEXT without .EXE must retain its original tokens");
  checks.require(!libtmux_env::repaired_pathext(std::string{".exe"}).has_value(),
                 "a lowercase exact .exe token must need no repair");
  checks.require(libtmux_env::repaired_pathext(std::string{".EXE2"}) ==
                     std::optional<std::string>{expected_repair(".EXE2")},
                 ".EXE2 must not be mistaken for the exact .EXE token");
  checks.require(
      !libtmux_env::repaired_pathext(std::string{".CPL; \t.eXe\t ;.PY"}).has_value(),
      "spaces and tabs around a .EXE token must be ignored");
  checks.require(!libtmux_env::repaired_pathext(std::string{standard}).has_value(),
                 "the normal standard PATHEXT must need no repair");
  checks.require(libtmux_env::repaired_pathext(std::string{".PY;.CPL"}) ==
                     std::optional<std::string>{expected_repair(".PY;.CPL")},
                 "repair must preserve a custom PATHEXT byte for byte");
}

void test_environment(const std::filesystem::path& self, Checks& checks) {
  EnvironmentGuard set_guard{L"LIBTMUX_PROCESS_CASE_SET"};
  EnvironmentGuard remove_guard{L"LIBTMUX_PROCESS_CASE_REMOVE"};
  const bool seeded =
      ::SetEnvironmentVariableW(L"LIBTMUX_PROCESS_CASE_SET", L"inherited") != FALSE &&
      ::SetEnvironmentVariableW(L"LIBTMUX_PROCESS_CASE_REMOVE", L"inherited") != FALSE;
  if (!checks.require(seeded, "environment test variables must be seeded")) {
    return;
  }

  auto request = request_for(self, {std::string{kEnvironmentMode}});
  request.environment = {
      {"libtmux_process_case_set", std::string{"overridden"}},
      {"libtmux_process_case_remove", std::nullopt},
      {"LIBTMUX_PROCESS_UNICODE",
       std::string{"\xe9\x9b\xaa\xe2\x98\x83\xf0\x9f\x98\x80"}},
  };
  const auto reply = run_process(request);
  if (!reply.has_value()) {
    checks.process_error("environment child did not run", reply.error());
    return;
  }
  fields_equal(bytes(reply->stdout_bytes),
               {"overridden", "<missing>", "\xe9\x9b\xaa\xe2\x98\x83\xf0\x9f\x98\x80"},
               checks, "environment replacement and removal must ignore name case");
}

void test_utf8_validation(const std::filesystem::path& self, Checks& checks) {
  std::vector<std::pair<std::string, ProcessRequest>> cases;
  auto argument = request_for(self, {std::string{kStreamsMode}, "0"});
  argument.arguments.back().value = "\xc0\x80";
  cases.emplace_back("argument", std::move(argument));
  auto environment_name = request_for(self, {std::string{kStreamsMode}, "0"});
  environment_name.environment = {{"\xed\xa0\x80", std::string{"value"}}};
  cases.emplace_back("environment name", std::move(environment_name));
  auto environment_value = request_for(self, {std::string{kStreamsMode}, "0"});
  environment_value.environment = {
      {"LIBTMUX_INVALID_UTF8", std::string{"\xf4\x90\x80\x80"}}};
  cases.emplace_back("environment value", std::move(environment_value));

  for (const auto& [field, request] : cases) {
    const auto reply = run_process(request);
    checks.require(!reply.has_value(), "malformed UTF-8 " + field + " must fail");
    if (!reply.has_value()) {
      checks.require(reply.error().kind == ProcessError::Kind::validation,
                     "malformed UTF-8 " + field + " must be validation");
      checks.require(reply.error().delivery == DeliveryStatus::not_started,
                     "malformed UTF-8 " + field + " must not launch");
    }
  }
}

void test_command_line_limit(const std::filesystem::path& self, Checks& checks) {
  auto request = request_for(self, {std::string(32767U, 'x')});
  const auto reply = run_process(request);
  if (checks.require(!reply.has_value(),
                     "an overlong Windows command line must fail before launch")) {
    checks.require(reply.error().kind == ProcessError::Kind::validation,
                   "an overlong Windows command line must be validation");
    checks.require(reply.error().delivery == DeliveryStatus::not_started,
                   "an overlong Windows command line must not launch");
  }
}

void test_streams_and_exit(const std::filesystem::path& self, Checks& checks) {
  const auto reply = run_process(request_for(self, {std::string{kStreamsMode}, "37"}));
  if (!reply.has_value()) {
    checks.process_error("streams child did not run", reply.error());
    return;
  }
  checks.require(exited(*reply) != nullptr && exited(*reply)->code == 37,
                 "a nonzero exit must be a ProcessReply with its exact code");
  checks.require(bytes(reply->stdout_bytes) == "stdout-token",
                 "stdout must remain separate from stderr");
  checks.require(bytes(reply->stderr_bytes) == "stderr-token",
                 "stderr must remain separate from stdout");
  checks.require(!reply->output_truncated, "small separated output must not truncate");
}

void test_output_limit(const std::filesystem::path& self, Checks& checks) {
  auto request = request_for(self, {std::string{kOutputMode}});
  request.capture_limit = 7U;
  const auto reply = run_process(request);
  if (!reply.has_value()) {
    checks.process_error("output-limit child did not run", reply.error());
    return;
  }
  checks.require(bytes(reply->stdout_bytes) == std::string(7U, 'o'),
                 "the stdout capture must stop at its limit");
  checks.require(bytes(reply->stderr_bytes) == std::string(7U, 'e'),
                 "the stderr capture must have its own limit");
  checks.require(reply->output_truncated,
                 "discarding bytes beyond either capture limit must be reported");
}

void test_missing_executable(const std::filesystem::path& self, Checks& checks) {
  const auto missing =
      self.parent_path() / (L"libtmux-process-missing-" +
                            std::to_wstring(::GetCurrentProcessId()) + L".exe");
  const auto reply = run_process(request_for(missing, {}));
  if (checks.require(!reply.has_value(), "a missing executable must fail to spawn")) {
    checks.require(reply.error().kind == ProcessError::Kind::spawn,
                   "a missing executable must be a spawn error");
    checks.require(reply.error().delivery == DeliveryStatus::not_started,
                   "a missing executable must not be reported as dispatched");
  }
}

void test_path_lookup(const std::filesystem::path& self, Checks& checks) {
  EnvironmentGuard path_guard{L"PATH"};
  const auto inherited_path = environment(L"PATH");
  std::wstring path = self.parent_path().wstring();
  if (inherited_path.present && !inherited_path.value.empty()) {
    path.push_back(L';');
    path.append(inherited_path.value);
  }
  if (!checks.require(::SetEnvironmentVariableW(L"PATH", path.c_str()) != FALSE,
                      "PATH must be set for the lookup test")) {
    return;
  }

  wchar_t temporary[MAX_PATH + 1U]{};
  const DWORD length = ::GetTempPathW(MAX_PATH + 1U, temporary);
  if (!checks.require(length != 0U && length <= MAX_PATH,
                      "a temporary current directory must be available")) {
    return;
  }
  CurrentDirectoryGuard directory_guard;
  if (!checks.require(::SetCurrentDirectoryW(temporary) != FALSE,
                      "the lookup test must run away from the executable directory")) {
    return;
  }

  auto request = request_for(self.stem(), {std::string{kStreamsMode}, "0"});
  const auto reply = run_process(request);
  if (!reply.has_value()) {
    checks.process_error("PATH-only extensionless executable did not run",
                         reply.error());
    return;
  }
  checks.require(exited(*reply) != nullptr && exited(*reply)->code == 0,
                 "PATH-only extensionless launch must exit zero");
}

void test_timeout_tree(const std::filesystem::path& self, Checks& checks) {
  const auto unique =
      std::to_wstring(::GetCurrentProcessId()) + L"-" +
      std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::wstring event_name = L"Local\\libtmux-process-smoke-" + unique;
  const auto event_utf8 = utf8(event_name);
  if (!checks.require(event_utf8.has_value(), "the readiness event name must encode")) {
    return;
  }

  auto request = request_for(self, {std::string{kSpawnMode}, *event_utf8});
  request.timeout = std::chrono::milliseconds{2500};
  const auto reply = run_process(request);
  if (!checks.require(!reply.has_value(), "the sleeping process tree must time out")) {
    return;
  }
  const auto& error = reply.error();
  checks.require(error.kind == ProcessError::Kind::timeout,
                 "a post-launch deadline must be a timeout error");
  checks.require(error.delivery == DeliveryStatus::indeterminate,
                 "a resumed process must be reported as possibly dispatched");

  const std::string output = bytes(error.stdout_bytes);
  std::uint32_t pid = 0;
  const auto parsed =
      std::from_chars(output.data(), output.data() + output.size(), pid);
  if (!checks.require(parsed.ec == std::errc{} && parsed.ptr != output.data() &&
                          parsed.ptr != output.data() + output.size() &&
                          *parsed.ptr == '\n',
                      "the timed-out child must report its grandchild PID")) {
    return;
  }

  Handle grandchild{
      ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
  if (!grandchild.valid()) {
    checks.require(::GetLastError() == ERROR_INVALID_PARAMETER,
                   "a vanished grandchild must no longer have a process ID");
    return;
  }
  checks.require(::WaitForSingleObject(grandchild.get(), 2000U) == WAIT_OBJECT_0,
                 "timeout cleanup must terminate the entire descendant job");
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
  if (const auto child = child_mode(argc, argv); child.has_value()) {
    return *child;
  }

  Checks checks;
  const auto self = executable_path();
  if (!checks.require(self.has_value(),
                      "the smoke executable path must be available")) {
    return checks.result();
  }

  test_argv(*self, checks);
  test_repaired_pathext(checks);
  test_environment(*self, checks);
  test_utf8_validation(*self, checks);
  test_command_line_limit(*self, checks);
  test_streams_and_exit(*self, checks);
  test_output_limit(*self, checks);
  test_missing_executable(*self, checks);
  test_path_lookup(*self, checks);
  test_timeout_tree(*self, checks);
  return checks.result();
}
