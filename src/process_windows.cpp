#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "process.hpp"

#include "libtmux/expected.hpp"
#include "path.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto wait_quantum = std::chrono::milliseconds{10};
constexpr auto post_exit_drain = std::chrono::milliseconds{100};
constexpr auto terminate_wait = std::chrono::milliseconds{500};

[[nodiscard]] bool valid_handle(HANDLE handle) noexcept {
  return handle != nullptr &&
         handle != INVALID_HANDLE_VALUE; // NOLINT(performance-no-int-to-ptr)
}

class OwnedHandle final {
public:
  OwnedHandle() = default;
  explicit OwnedHandle(HANDLE handle) noexcept : handle_(handle) {}

  ~OwnedHandle() { reset(); }

  OwnedHandle(const OwnedHandle&) = delete;
  OwnedHandle& operator=(const OwnedHandle&) = delete;

  OwnedHandle(OwnedHandle&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}

  OwnedHandle& operator=(OwnedHandle&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] bool valid() const noexcept { return valid_handle(handle_); }

  void reset(HANDLE handle = nullptr) noexcept {
    if (valid()) {
      static_cast<void>(::CloseHandle(handle_));
    }
    handle_ = handle;
  }

private:
  HANDLE handle_{nullptr};
};

class EnvironmentStrings final {
public:
  EnvironmentStrings() : value_(::GetEnvironmentStringsW()) {}
  ~EnvironmentStrings() {
    if (value_ != nullptr) {
      static_cast<void>(::FreeEnvironmentStringsW(value_));
    }
  }

  EnvironmentStrings(const EnvironmentStrings&) = delete;
  EnvironmentStrings& operator=(const EnvironmentStrings&) = delete;

  [[nodiscard]] LPWCH get() const noexcept { return value_; }

private:
  LPWCH value_{nullptr};
};

class AttributeList final {
public:
  AttributeList() = default;
  ~AttributeList() {
    if (initialized_) {
      ::DeleteProcThreadAttributeList(get());
    }
  }

  AttributeList(const AttributeList&) = delete;
  AttributeList& operator=(const AttributeList&) = delete;
  AttributeList(AttributeList&&) = delete;
  AttributeList& operator=(AttributeList&&) = delete;

  [[nodiscard]] DWORD initialize() {
    SIZE_T size = 0;
    static_cast<void>(::InitializeProcThreadAttributeList(nullptr, 1, 0, &size));
    if (size == 0U) {
      return ::GetLastError();
    }
    storage_.resize(size);
    if (::InitializeProcThreadAttributeList(get(), 1, 0, &size) == FALSE) {
      return ::GetLastError();
    }
    initialized_ = true;
    return ERROR_SUCCESS;
  }

  [[nodiscard]] DWORD set_inherited_handles(std::span<HANDLE> handles) {
    if (::UpdateProcThreadAttribute(get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    handles.data(), handles.size_bytes(), nullptr,
                                    nullptr) == FALSE) {
      return ::GetLastError();
    }
    return ERROR_SUCCESS;
  }

  [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() noexcept {
    return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
  }

private:
  std::vector<std::byte> storage_;
  bool initialized_{false};
};

struct Pipe final {
  OwnedHandle read;
  OwnedHandle write;
};

struct ChildIo final {
  OwnedHandle input;
  OwnedHandle output;
  OwnedHandle error;
  Pipe stdout_pipe;
  Pipe stderr_pipe;
};

struct Capture final {
  std::vector<std::byte> stdout_bytes;
  std::vector<std::byte> stderr_bytes;
  bool truncated{false};
};

struct SetupFailure final {
  std::string_view operation;
  DWORD error;
};

struct DrainFailure final {
  std::string_view operation;
  DWORD error;
};

struct CaseInsensitiveLess final {
  [[nodiscard]] bool operator()(const std::wstring& left,
                                const std::wstring& right) const noexcept {
    if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return left < right;
    }
    const auto left_size = static_cast<int>(left.size());
    const auto right_size = static_cast<int>(right.size());
    const auto order =
        ::CompareStringOrdinal(left.data(), left_size, right.data(), right_size, TRUE);
    if (order == CSTR_LESS_THAN) {
      return true;
    }
    if (order == CSTR_GREATER_THAN || order == CSTR_EQUAL) {
      return false;
    }
    return left < right;
  }
};

struct PreparedRequest final {
  std::wstring executable;
  std::wstring command_line;
  std::vector<wchar_t> environment;
};

[[nodiscard]] std::error_code windows_error(DWORD error) {
  return {static_cast<int>(error), std::system_category()};
}

[[nodiscard]] ProcessError::Kind spawn_error_kind(DWORD error) {
  switch (error) {
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
  case ERROR_MOD_NOT_FOUND:
  case ERROR_INVALID_NAME:
    return ProcessError::Kind::spawn;
  default:
    return ProcessError::Kind::pre_exec;
  }
}

[[nodiscard]] std::string_view spawn_operation(ProcessError::Kind kind) {
  return kind == ProcessError::Kind::spawn ? "spawn" : "pre-exec";
}

[[nodiscard]] bool contains_nul(std::string_view value) {
  return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] std::string escape_diagnostic_value(std::string_view value) {
  constexpr std::string_view hexadecimal{"0123456789abcdef"};
  std::string escaped;
  escaped.reserve(value.size());
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte >= 0x20U && byte <= 0x7eU && byte != '\\') {
      escaped.push_back(character);
      continue;
    }
    if (byte == '\\') {
      escaped.append("\\\\");
      continue;
    }
    escaped.append("\\x");
    escaped.push_back(hexadecimal[byte >> 4U]);
    escaped.push_back(hexadecimal[byte & 0x0fU]);
  }
  return escaped;
}

[[nodiscard]] std::string render_request(const ProcessRequest& request) {
  std::string rendered =
      escape_diagnostic_value(libtmux_path::command_string(request.executable));
  for (const auto& argument : request.arguments) {
    rendered.push_back(' ');
    rendered.append(argument.sensitivity == Sensitivity::secret
                        ? "[REDACTED]"
                        : escape_diagnostic_value(argument.value));
  }
  return rendered;
}

[[nodiscard]] std::string diagnostic(std::string_view operation,
                                     const ProcessRequest& request,
                                     const std::error_code& cause) {
  std::string result{operation};
  result.append(" failure running ");
  result.append(render_request(request));
  result.append(": ");
  result.append(cause.message());
  return result;
}

[[nodiscard]] ProcessError make_error(ProcessError::Kind kind, DeliveryStatus delivery,
                                      std::string_view operation,
                                      const ProcessRequest& request,
                                      std::error_code cause, Capture capture = {}) {
  return {
      .kind = kind,
      .delivery = delivery,
      .diagnostic = diagnostic(operation, request, cause),
      .stdout_bytes = std::move(capture.stdout_bytes),
      .stderr_bytes = std::move(capture.stderr_bytes),
      .output_truncated = capture.truncated,
  };
}

[[nodiscard]] bool is_console(HANDLE handle) noexcept {
  DWORD mode = 0;
  return valid_handle(handle) && ::GetConsoleMode(handle, &mode) != FALSE;
}

[[nodiscard]] std::optional<ProcessError>
validate_request(const ProcessRequest& request) {
  const auto invalid = [&]() {
    return make_error(ProcessError::Kind::validation, DeliveryStatus::not_started,
                      "validation", request,
                      std::make_error_code(std::errc::invalid_argument));
  };

  const auto executable = libtmux_path::command_string(request.executable);
  if (executable.empty() || contains_nul(executable)) {
    return invalid();
  }
  for (const auto& argument : request.arguments) {
    if (contains_nul(argument.value)) {
      return invalid();
    }
  }
  for (const auto& [name, value] : request.environment) {
    if (name.empty() || name.find('=') != std::string::npos || contains_nul(name) ||
        (value.has_value() && contains_nul(*value))) {
      return invalid();
    }
  }
  if (request.stdio == StdioPolicy::inherit_terminal &&
      (!is_console(::GetStdHandle(STD_INPUT_HANDLE)) ||
       !is_console(::GetStdHandle(STD_OUTPUT_HANDLE)) ||
       !valid_handle(::GetStdHandle(STD_ERROR_HANDLE)))) {
    return invalid();
  }
  return std::nullopt;
}

[[nodiscard]] expected<std::wstring, DWORD> widen_utf8(std::string_view value) {
  if (value.empty()) {
    return std::wstring{};
  }
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return unexpected(static_cast<DWORD>(ERROR_BAD_LENGTH));
  }
  const auto input_size = static_cast<int>(value.size());
  const auto output_size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                 value.data(), input_size, nullptr, 0);
  if (output_size == 0) {
    return unexpected(::GetLastError());
  }
  std::wstring result(static_cast<std::size_t>(output_size), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size,
                            result.data(), output_size) == 0) {
    return unexpected(::GetLastError());
  }
  return result;
}

[[nodiscard]] expected<std::optional<std::wstring>, DWORD> caller_search_path() {
  ::SetLastError(ERROR_SUCCESS);
  const auto required = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
  if (required == 0U) {
    const auto error = ::GetLastError();
    if (error == ERROR_ENVVAR_NOT_FOUND) {
      return std::optional<std::wstring>{};
    }
    if (error == ERROR_SUCCESS) {
      return std::optional<std::wstring>{std::wstring{}};
    }
    return unexpected(error);
  }

  std::vector<wchar_t> buffer(required);
  for (;;) {
    ::SetLastError(ERROR_SUCCESS);
    const auto length = ::GetEnvironmentVariableW(L"PATH", buffer.data(),
                                                  static_cast<DWORD>(buffer.size()));
    if (length == 0U) {
      const auto error = ::GetLastError();
      if (error == ERROR_ENVVAR_NOT_FOUND) {
        return std::optional<std::wstring>{};
      }
      if (error == ERROR_SUCCESS) {
        return std::optional<std::wstring>{std::wstring{}};
      }
      return unexpected(error);
    }
    if (length < buffer.size()) {
      return std::optional<std::wstring>{std::in_place, buffer.data(),
                                         static_cast<std::size_t>(length)};
    }
    if (static_cast<std::size_t>(length) <= buffer.size()) {
      return unexpected(static_cast<DWORD>(ERROR_INSUFFICIENT_BUFFER));
    }
    buffer.resize(static_cast<std::size_t>(length));
  }
}

[[nodiscard]] expected<std::wstring, DWORD> search_path(const std::wstring& executable,
                                                        const wchar_t* extension,
                                                        const wchar_t* path) {
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    if (buffer.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
      return unexpected(static_cast<DWORD>(ERROR_FILENAME_EXCED_RANGE));
    }
    const auto length =
        ::SearchPathW(path, executable.c_str(), extension,
                      static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length == 0U) {
      return unexpected(::GetLastError());
    }
    if (length < buffer.size()) {
      return std::wstring{buffer.data(), static_cast<std::size_t>(length)};
    }
    if (static_cast<std::size_t>(length) <= buffer.size()) {
      return unexpected(static_cast<DWORD>(ERROR_INSUFFICIENT_BUFFER));
    }
    buffer.resize(static_cast<std::size_t>(length));
  }
}

[[nodiscard]] expected<std::wstring, DWORD>
resolve_executable(const std::wstring& executable) {
  // Executable lookup uses the caller's PATH, as posix_spawnp does; the overlay
  // is installed only in the child process.
  auto caller_path = caller_search_path();
  if (!caller_path) {
    return unexpected(caller_path.error());
  }
  const auto* path = caller_path->has_value() ? caller_path->value().c_str() : nullptr;
  return search_path(executable, L".exe", path);
}

void append_quoted_argument(std::wstring& command_line, std::wstring_view argument) {
  command_line.push_back(L'"');
  std::size_t backslashes = 0;
  for (const auto character : argument) {
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

[[nodiscard]] expected<std::vector<wchar_t>, DWORD> build_environment(
    const std::vector<std::pair<std::wstring, std::optional<std::wstring>>>& overlay) {
  EnvironmentStrings inherited;
  if (inherited.get() == nullptr) {
    return unexpected(::GetLastError());
  }

  std::map<std::wstring, std::wstring, CaseInsensitiveLess> values;
  for (const wchar_t* entry = inherited.get(); *entry != L'\0';
       entry += std::wcslen(entry) + 1U) {
    const std::wstring_view current{entry};
    const auto separator = current.find(L'=', current.front() == L'=' ? 1U : 0U);
    if (separator != std::wstring_view::npos) {
      values.insert_or_assign(std::wstring{current.substr(0, separator)},
                              std::wstring{current.substr(separator + 1U)});
    }
  }
  for (const auto& [name, value] : overlay) {
    values.erase(name);
    if (value.has_value()) {
      values.emplace(name, *value);
    }
  }

  std::vector<wchar_t> block;
  for (const auto& [name, value] : values) {
    block.insert(block.end(), name.begin(), name.end());
    block.push_back(L'=');
    block.insert(block.end(), value.begin(), value.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  if (block.size() == 1U) {
    block.push_back(L'\0');
  }
  return block;
}

[[nodiscard]] expected<PreparedRequest, ProcessError>
prepare_request(const ProcessRequest& request) {
  auto executable = widen_utf8(libtmux_path::command_string(request.executable));
  if (!executable) {
    return unexpected(make_error(ProcessError::Kind::validation,
                                 DeliveryStatus::not_started, "UTF-8 conversion",
                                 request, windows_error(executable.error())));
  }
  auto resolved_executable = resolve_executable(*executable);
  if (!resolved_executable) {
    const auto kind = spawn_error_kind(resolved_executable.error());
    return unexpected(make_error(kind, DeliveryStatus::not_started,
                                 spawn_operation(kind), request,
                                 windows_error(resolved_executable.error())));
  }

  std::vector<std::wstring> arguments;
  arguments.reserve(request.arguments.size() + 1U);
  arguments.push_back(*executable);
  for (const auto& argument : request.arguments) {
    auto widened = widen_utf8(argument.value);
    if (!widened) {
      return unexpected(make_error(ProcessError::Kind::validation,
                                   DeliveryStatus::not_started, "UTF-8 conversion",
                                   request, windows_error(widened.error())));
    }
    arguments.push_back(std::move(*widened));
  }

  std::vector<std::pair<std::wstring, std::optional<std::wstring>>> overlay;
  overlay.reserve(request.environment.size());
  for (const auto& [name, value] : request.environment) {
    auto wide_name = widen_utf8(name);
    if (!wide_name) {
      return unexpected(make_error(ProcessError::Kind::validation,
                                   DeliveryStatus::not_started, "UTF-8 conversion",
                                   request, windows_error(wide_name.error())));
    }
    std::optional<std::wstring> wide_value;
    if (value.has_value()) {
      auto converted = widen_utf8(*value);
      if (!converted) {
        return unexpected(make_error(ProcessError::Kind::validation,
                                     DeliveryStatus::not_started, "UTF-8 conversion",
                                     request, windows_error(converted.error())));
      }
      wide_value = std::move(*converted);
    }
    overlay.emplace_back(std::move(*wide_name), std::move(wide_value));
  }

  auto environment = build_environment(overlay);
  if (!environment) {
    return unexpected(make_error(ProcessError::Kind::pre_exec,
                                 DeliveryStatus::not_started, "environment", request,
                                 windows_error(environment.error())));
  }

  std::wstring command_line;
  for (const auto& argument : arguments) {
    if (!command_line.empty()) {
      command_line.push_back(L' ');
    }
    append_quoted_argument(command_line, argument);
  }
  if (command_line.size() >= 32767U) {
    return unexpected(make_error(
        ProcessError::Kind::validation, DeliveryStatus::not_started, "validation",
        request, std::make_error_code(std::errc::argument_list_too_long)));
  }

  return PreparedRequest{
      .executable = std::move(*resolved_executable),
      .command_line = std::move(command_line),
      .environment = std::move(*environment),
  };
}

[[nodiscard]] expected<Pipe, DWORD> create_pipe() {
  SECURITY_ATTRIBUTES security{
      .nLength = sizeof(SECURITY_ATTRIBUTES),
      .lpSecurityDescriptor = nullptr,
      .bInheritHandle = TRUE,
  };
  HANDLE read = nullptr;
  HANDLE write = nullptr;
  if (::CreatePipe(&read, &write, &security, 0) == FALSE) {
    return unexpected(::GetLastError());
  }
  OwnedHandle owned_read{read};
  OwnedHandle owned_write{write};
  if (::SetHandleInformation(owned_read.get(), HANDLE_FLAG_INHERIT, 0) == FALSE) {
    return unexpected(::GetLastError());
  }
  return Pipe{.read = std::move(owned_read), .write = std::move(owned_write)};
}

[[nodiscard]] expected<OwnedHandle, DWORD> duplicate_inheritable(HANDLE source) {
  if (!valid_handle(source)) {
    return unexpected(static_cast<DWORD>(ERROR_INVALID_HANDLE));
  }
  HANDLE duplicate = nullptr;
  if (::DuplicateHandle(::GetCurrentProcess(), source, ::GetCurrentProcess(),
                        &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS) == FALSE) {
    return unexpected(::GetLastError());
  }
  return OwnedHandle{duplicate};
}

[[nodiscard]] expected<ChildIo, SetupFailure> prepare_child_io(StdioPolicy policy) {
  ChildIo io;
  if (policy == StdioPolicy::capture) {
    auto stdout_pipe = create_pipe();
    if (!stdout_pipe) {
      return unexpected(
          SetupFailure{.operation = "stdout pipe", .error = stdout_pipe.error()});
    }
    io.stdout_pipe = std::move(*stdout_pipe);
    auto stderr_pipe = create_pipe();
    if (!stderr_pipe) {
      return unexpected(
          SetupFailure{.operation = "stderr pipe", .error = stderr_pipe.error()});
    }
    io.stderr_pipe = std::move(*stderr_pipe);

    SECURITY_ATTRIBUTES security{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = nullptr,
        .bInheritHandle = TRUE,
    };
    io.input.reset(::CreateFileW(L"NUL", GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!io.input.valid()) {
      return unexpected(SetupFailure{.operation = "stdin", .error = ::GetLastError()});
    }
    io.output = std::move(io.stdout_pipe.write);
    io.error = std::move(io.stderr_pipe.write);
    return io;
  }

  auto input = duplicate_inheritable(::GetStdHandle(STD_INPUT_HANDLE));
  if (!input) {
    return unexpected(SetupFailure{.operation = "stdin", .error = input.error()});
  }
  auto output = duplicate_inheritable(::GetStdHandle(STD_OUTPUT_HANDLE));
  if (!output) {
    return unexpected(SetupFailure{.operation = "stdout", .error = output.error()});
  }
  auto error = duplicate_inheritable(::GetStdHandle(STD_ERROR_HANDLE));
  if (!error) {
    return unexpected(SetupFailure{.operation = "stderr", .error = error.error()});
  }
  io.input = std::move(*input);
  io.output = std::move(*output);
  io.error = std::move(*error);
  return io;
}

void append_capture(std::vector<std::byte>& destination,
                    std::span<const std::byte> bytes, std::size_t limit,
                    bool& truncated) {
  const auto available = destination.size() < limit ? limit - destination.size() : 0U;
  const auto retained = std::min(available, bytes.size());
  const auto retained_bytes = bytes.first(retained);
  destination.insert(destination.end(), retained_bytes.begin(), retained_bytes.end());
  truncated = truncated || retained != bytes.size();
}

[[nodiscard]] expected<bool, DrainFailure>
drain_once(OwnedHandle& pipe, std::vector<std::byte>& destination, std::size_t limit,
           bool& truncated, std::string_view operation) {
  if (!pipe.valid()) {
    return false;
  }
  DWORD available = 0;
  if (::PeekNamedPipe(pipe.get(), nullptr, 0, nullptr, &available, nullptr) == FALSE) {
    const auto error = ::GetLastError();
    if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
      pipe.reset();
      return false;
    }
    return unexpected(DrainFailure{.operation = operation, .error = error});
  }
  if (available == 0U) {
    return false;
  }

  std::array<std::byte, 16384> buffer{};
  const auto requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
  DWORD count = 0;
  if (::ReadFile(pipe.get(), buffer.data(), requested, &count, nullptr) == FALSE) {
    const auto error = ::GetLastError();
    if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
      pipe.reset();
      return false;
    }
    return unexpected(DrainFailure{.operation = operation, .error = error});
  }
  append_capture(
      destination,
      std::span<const std::byte>{buffer.data(), static_cast<std::size_t>(count)}, limit,
      truncated);
  return count != 0U;
}

[[nodiscard]] expected<bool, DrainFailure> drain_output(OwnedHandle& stdout_read,
                                                        OwnedHandle& stderr_read,
                                                        Capture& capture,
                                                        std::size_t limit) {
  auto stdout_progress = drain_once(stdout_read, capture.stdout_bytes, limit,
                                    capture.truncated, "stdout pipe read");
  if (!stdout_progress) {
    return unexpected(stdout_progress.error());
  }
  auto stderr_progress = drain_once(stderr_read, capture.stderr_bytes, limit,
                                    capture.truncated, "stderr pipe read");
  if (!stderr_progress) {
    return unexpected(stderr_progress.error());
  }
  return *stdout_progress || *stderr_progress;
}

void close_remaining_output(OwnedHandle& stdout_read, OwnedHandle& stderr_read,
                            Capture& capture) {
  if (stdout_read.valid() || stderr_read.valid()) {
    capture.truncated = true;
  }
  stdout_read.reset();
  stderr_read.reset();
}

[[nodiscard]] DWORD wait_milliseconds(Clock::time_point boundary) {
  const auto now = Clock::now();
  if (now >= boundary) {
    return 0;
  }
  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(boundary - now);
  constexpr auto maximum = static_cast<std::chrono::milliseconds::rep>(INFINITE - 1U);
  return static_cast<DWORD>(std::min(remaining.count(), maximum));
}

void terminate_and_drain(HANDLE job, HANDLE process, OwnedHandle& stdout_read,
                         OwnedHandle& stderr_read, Capture& capture,
                         std::size_t capture_limit) {
  if (!valid_handle(job) ||
      ::TerminateJobObject(job, static_cast<UINT>(ERROR_PROCESS_ABORTED)) == FALSE) {
    static_cast<void>(
        ::TerminateProcess(process, static_cast<UINT>(ERROR_PROCESS_ABORTED)));
  }

  const auto deadline = Clock::now() + terminate_wait;
  while (Clock::now() < deadline) {
    auto drained = drain_output(stdout_read, stderr_read, capture, capture_limit);
    if (!drained) {
      capture.truncated = true;
      stdout_read.reset();
      stderr_read.reset();
    }
    const auto wait = ::WaitForSingleObject(
        process, wait_milliseconds(std::min(deadline, Clock::now() + wait_quantum)));
    if (wait == WAIT_OBJECT_0 && !stdout_read.valid() && !stderr_read.valid()) {
      break;
    }
    if (wait == WAIT_FAILED) {
      break;
    }
  }
  close_remaining_output(stdout_read, stderr_read, capture);
}

} // namespace

expected<ProcessReply, ProcessError> run_process(const ProcessRequest& request) {
  if (auto validation = validate_request(request)) {
    return unexpected(std::move(*validation));
  }
  if (request.timeout.has_value() &&
      *request.timeout <= std::chrono::milliseconds::zero()) {
    return unexpected(make_error(ProcessError::Kind::timeout,
                                 DeliveryStatus::not_started, "timeout", request,
                                 std::make_error_code(std::errc::timed_out)));
  }

  std::optional<Clock::time_point> deadline;
  if (request.timeout.has_value()) {
    deadline = Clock::now() + *request.timeout;
  }
  auto prepared = prepare_request(request);
  if (!prepared) {
    return unexpected(std::move(prepared.error()));
  }

  auto io = prepare_child_io(request.stdio);
  if (!io) {
    return unexpected(make_error(ProcessError::Kind::pipe, DeliveryStatus::not_started,
                                 io.error().operation, request,
                                 windows_error(io.error().error)));
  }

  AttributeList attributes;
  if (const auto error = attributes.initialize(); error != ERROR_SUCCESS) {
    return unexpected(make_error(ProcessError::Kind::pre_exec,
                                 DeliveryStatus::not_started, "handle inheritance",
                                 request, windows_error(error)));
  }
  std::array<HANDLE, 3> inherited_handles{io->input.get(), io->output.get(),
                                          io->error.get()};
  if (const auto error = attributes.set_inherited_handles(inherited_handles);
      error != ERROR_SUCCESS) {
    return unexpected(make_error(ProcessError::Kind::pre_exec,
                                 DeliveryStatus::not_started, "handle inheritance",
                                 request, windows_error(error)));
  }

  // The job is kept alive only while the client runs; a successful tmux command
  // may leave its server behind, so closing the job must not kill its members.
  OwnedHandle job{::CreateJobObjectW(nullptr, nullptr)};
  if (!job.valid()) {
    return unexpected(make_error(ProcessError::Kind::pre_exec,
                                 DeliveryStatus::not_started, "job", request,
                                 windows_error(::GetLastError())));
  }

  if (deadline.has_value() && Clock::now() >= *deadline) {
    return unexpected(make_error(ProcessError::Kind::timeout,
                                 DeliveryStatus::not_started, "timeout", request,
                                 std::make_error_code(std::errc::timed_out)));
  }

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = io->input.get();
  startup.StartupInfo.hStdOutput = io->output.get();
  startup.StartupInfo.hStdError = io->error.get();
  startup.lpAttributeList = attributes.get();

  PROCESS_INFORMATION process_information{};
  const DWORD creation_flags =
      CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
  const auto created = ::CreateProcessW(
      prepared->executable.c_str(), prepared->command_line.data(), nullptr, nullptr,
      TRUE, creation_flags, prepared->environment.data(), nullptr, &startup.StartupInfo,
      &process_information);
  if (created == FALSE) {
    const auto error = ::GetLastError();
    const auto kind = spawn_error_kind(error);
    return unexpected(make_error(kind, DeliveryStatus::not_started,
                                 spawn_operation(kind), request, windows_error(error)));
  }

  OwnedHandle process{process_information.hProcess};
  OwnedHandle thread{process_information.hThread};
  io->input.reset();
  io->output.reset();
  io->error.reset();

  if (::AssignProcessToJobObject(job.get(), process.get()) == FALSE) {
    const auto error = ::GetLastError();
    static_cast<void>(
        ::TerminateProcess(process.get(), static_cast<UINT>(ERROR_PROCESS_ABORTED)));
    static_cast<void>(::WaitForSingleObject(
        process.get(), static_cast<DWORD>(terminate_wait.count())));
    return unexpected(make_error(ProcessError::Kind::pre_exec,
                                 DeliveryStatus::not_started, "job assignment", request,
                                 windows_error(error)));
  }

  if (deadline.has_value() && Clock::now() >= *deadline) {
    Capture capture;
    terminate_and_drain(job.get(), process.get(), io->stdout_pipe.read,
                        io->stderr_pipe.read, capture, request.capture_limit);
    return unexpected(make_error(
        ProcessError::Kind::timeout, DeliveryStatus::not_started, "timeout", request,
        std::make_error_code(std::errc::timed_out), std::move(capture)));
  }

  if (::ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
    const auto error = ::GetLastError();
    Capture capture;
    terminate_and_drain(job.get(), process.get(), io->stdout_pipe.read,
                        io->stderr_pipe.read, capture, request.capture_limit);
    return unexpected(make_error(ProcessError::Kind::pre_exec,
                                 DeliveryStatus::not_started, "resume", request,
                                 windows_error(error), std::move(capture)));
  }
  thread.reset();

  Capture capture;
  capture.stdout_bytes.reserve(std::min(request.capture_limit, std::size_t{4096}));
  capture.stderr_bytes.reserve(std::min(request.capture_limit, std::size_t{4096}));

  bool exited = false;
  DWORD exit_code = 0;
  auto exit_drain_deadline = Clock::time_point::max();
  for (;;) {
    const auto wait = ::WaitForSingleObject(process.get(), 0);
    if (wait == WAIT_OBJECT_0 && !exited) {
      exited = true;
      exit_drain_deadline = Clock::now() + post_exit_drain;
      if (::GetExitCodeProcess(process.get(), &exit_code) == FALSE) {
        const auto error = ::GetLastError();
        terminate_and_drain(job.get(), process.get(), io->stdout_pipe.read,
                            io->stderr_pipe.read, capture, request.capture_limit);
        return unexpected(make_error(
            ProcessError::Kind::pipe, DeliveryStatus::indeterminate, "process status",
            request, windows_error(error), std::move(capture)));
      }
    } else if (wait == WAIT_FAILED) {
      const auto error = ::GetLastError();
      terminate_and_drain(job.get(), process.get(), io->stdout_pipe.read,
                          io->stderr_pipe.read, capture, request.capture_limit);
      return unexpected(make_error(ProcessError::Kind::pipe,
                                   DeliveryStatus::indeterminate, "wait", request,
                                   windows_error(error), std::move(capture)));
    }

    auto drained = drain_output(io->stdout_pipe.read, io->stderr_pipe.read, capture,
                                request.capture_limit);
    if (!drained) {
      const auto failure = drained.error();
      terminate_and_drain(job.get(), process.get(), io->stdout_pipe.read,
                          io->stderr_pipe.read, capture, request.capture_limit);
      return unexpected(make_error(
          ProcessError::Kind::pipe, DeliveryStatus::indeterminate, failure.operation,
          request, windows_error(failure.error), std::move(capture)));
    }

    if (exited && !io->stdout_pipe.read.valid() && !io->stderr_pipe.read.valid()) {
      break;
    }
    if (exited && Clock::now() >= exit_drain_deadline) {
      close_remaining_output(io->stdout_pipe.read, io->stderr_pipe.read, capture);
      break;
    }
    if (!exited && deadline.has_value() && Clock::now() >= *deadline) {
      terminate_and_drain(job.get(), process.get(), io->stdout_pipe.read,
                          io->stderr_pipe.read, capture, request.capture_limit);
      return unexpected(make_error(
          ProcessError::Kind::timeout, DeliveryStatus::indeterminate, "timeout",
          request, std::make_error_code(std::errc::timed_out), std::move(capture)));
    }
    if (*drained) {
      continue;
    }

    const auto quantum_boundary = Clock::now() + wait_quantum;
    const auto boundary =
        exited ? std::min(exit_drain_deadline, quantum_boundary)
               : (deadline.has_value() ? std::min(*deadline, quantum_boundary)
                                       : quantum_boundary);
    const auto delay = wait_milliseconds(boundary);
    if (exited) {
      ::Sleep(delay);
    } else {
      const auto waited = ::WaitForSingleObject(process.get(), delay);
      if (waited == WAIT_FAILED) {
        const auto error = ::GetLastError();
        terminate_and_drain(job.get(), process.get(), io->stdout_pipe.read,
                            io->stderr_pipe.read, capture, request.capture_limit);
        return unexpected(make_error(ProcessError::Kind::pipe,
                                     DeliveryStatus::indeterminate, "wait", request,
                                     windows_error(error), std::move(capture)));
      }
    }
  }

  return ProcessReply{
      .termination = Exited{static_cast<int>(static_cast<std::int32_t>(exit_code))},
      .stdout_bytes = std::move(capture.stdout_bytes),
      .stderr_bytes = std::move(capture.stderr_bytes),
      .output_truncated = capture.truncated,
  };
}

} // namespace detail
LIBTMUX_NAMESPACE_END
