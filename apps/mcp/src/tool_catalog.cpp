#include "environment_value.hpp"
#include "libtmux_consumers/mcp.hpp"
#include "tool_support.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "libtmux/capture.hpp"
#include "libtmux/format.hpp"
#include "libtmux/keys.hpp"
#include "libtmux/server.hpp"
#include "libtmux/snapshot.hpp"
#include "pane_input.hpp"
#include "wait_for_text.hpp"

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace libtmux::mcp::detail {

const std::string* argument(const Arguments& arguments, std::string_view name) {
  const auto found = arguments.find(name);
  return found == arguments.end() ? nullptr : &found->second;
}

ToolOutput output(StructuredValue::Object structured,
                  std::optional<std::size_t> maximum_response_bytes) {
  return ToolOutput{.structured = std::move(structured),
                    .maximum_response_bytes = maximum_response_bytes};
}

ToolError tmux_error(const CommandFailure& error) {
  return ToolError{error.kind == FailureKind::validation, error.diagnostic};
}

namespace {

[[nodiscard]] std::string shell_quote(std::string_view value) {
  std::string quoted{"'"};
  for (const char character : value) {
    quoted += character == '\'' ? "'\\''" : std::string{character};
  }
  quoted += '\'';
  return quoted;
}

[[nodiscard]] std::string display_writer(std::string_view tmux_executable,
                                         std::string_view socket_path,
                                         std::string_view message) {
  return shell_quote(tmux_executable) + " -N -S " + shell_quote(socket_path) +
         " display-message -p " + std::string{message};
}

[[nodiscard]] std::string_view shell_name(std::string_view command) {
  const std::size_t slash = command.rfind('/');
  std::string_view basename =
      command.substr(slash == std::string_view::npos ? 0U : slash + 1U);
  if (!basename.empty() && basename.front() == '-') {
    basename.remove_prefix(1U);
  }
  return basename;
}

[[nodiscard]] bool contains_control_byte(std::string_view value) {
  return std::ranges::any_of(value, [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte <= 0x1FU || byte == 0x7FU;
  });
}

[[nodiscard]] std::string inherited_trap_capture(std::string_view current_shell,
                                                 std::string_view nonce,
                                                 std::string_view declarations,
                                                 std::string_view capture_status,
                                                 std::string_view inherited_flags) {
  const std::string_view name = shell_name(current_shell);
  if (name != "bash" && name != "zsh") {
    return std::string{declarations} + "=; " + std::string{capture_status} + "=0";
  }

  const std::string file = "__libtmux_mcp_trap_file_" + std::string{nonce};
  const std::string read_owned = "__libtmux_mcp_trap_read_owned_" + std::string{nonce};
  const std::string write_owned =
      "__libtmux_mcp_trap_write_owned_" + std::string{nonce};
  const std::string prefix = "/tmp/libtmux-mcp-traps-" + std::string{nonce};
  const std::string declarations_name{declarations};
  const std::string capture_status_name{capture_status};
  std::string acquire;
  std::string query;
  if (name == "bash") {
    const std::string files = "__libtmux_mcp_trap_files_" + std::string{nonce};
    acquire = "\\set +f; " + files + "=(); if /usr/bin/mktemp " +
              shell_quote(prefix + ".XXXXXX") + " >/dev/null; then " + files + "=(" +
              shell_quote(prefix) +
              ".?????"
              "?); if [ \"${#" +
              files + "[@]}\" -eq 1 ]; then " + file + "=\"${" + files +
              "[0]}\"; else /bin/rm -f \"${" + files + "[@]}\"; fi; fi; case \"$" +
              std::string{inherited_flags} + "\" in *f*) \\set -f ;; esac";
    query = "\\trap -p ERR DEBUG";
  } else {
    acquire = "if /usr/bin/mktemp " + shell_quote(prefix + ".XXXXXX") +
              " | IFS= \\read -r " + file + "; then :; else " + file + "=; fi";
    query = "\\trap";
  }

  constexpr std::size_t maximum_bytes = 64U * 1024U;
  return declarations_name + "=; " + capture_status_name + "=125; " + file + "=; " +
         read_owned + "=0; " + write_owned + "=0; \\umask 077; " + acquire +
         "; if [ -n \"$" + file + "\" ] && [ -f \"$" + file + "\" ] && [ -O \"$" +
         file +
         "\" ] && ! ( : >&8 ) 2>/dev/null && ! ( : <&8 ) 2>/dev/null && "
         "! ( : >&9 ) 2>/dev/null && ! ( : <&9 ) 2>/dev/null && \\exec 8<> \"$" +
         file + "\" && " + write_owned + "=1 && \\exec 9< \"$" + file + "\" && " +
         read_owned + "=1 && /bin/rm -f \"$" + file + "\"; then if " + query +
         " >&8; then " + capture_status_name +
         "=0; fi; fi; \\trap - ERR DEBUG; if [ \"$" + write_owned +
         "\" -eq 1 ]; then if ! \\exec 8>&-; then " + capture_status_name +
         "=125; fi; " + write_owned + "=0; fi; if [ \"$" + capture_status_name +
         "\" -eq 0 ] && [ \"$" + read_owned + "\" -eq 1 ]; then LC_ALL=C; if " +
         declarations_name + "=$(/usr/bin/head -c " +
         std::to_string(maximum_bytes + 1U) + " <&9); then if [ \"${#" +
         declarations_name + "}\" -gt " + std::to_string(maximum_bytes) + " ]; then " +
         declarations_name + "=; " + capture_status_name + "=125; fi; else " +
         declarations_name + "=; " + capture_status_name + "=125; fi; fi; if [ \"$" +
         read_owned + "\" -eq 1 ]; then if ! \\exec 9<&-; then " + capture_status_name +
         "=125; fi; " + read_owned + "=0; fi; if [ -n \"$" + file +
         "\" ]; then /bin/rm -f \"$" + file + "\"; fi";
}

[[nodiscard]] std::string shell_frame(std::string_view command,
                                      std::string_view current_shell,
                                      std::string_view tmux_executable,
                                      std::string_view socket_path,
                                      std::string_view nonce) {
  const std::string marker_parts = "'__LIBTMUX_MCP_DONE_''" + std::string{nonce} + "'";
  const std::string empty = display_writer(tmux_executable, socket_path, "''");
  const std::string opening =
      display_writer(tmux_executable, socket_path, marker_parts + "'__:BEGIN'");
  const std::string closing =
      display_writer(tmux_executable, socket_path, marker_parts + "'__:'\"$1\"");
  const std::string trap_action =
      "\\set -- \"$?\"; " + empty + "; " + closing + "; \\exit 0";
  const std::string flags = "__libtmux_mcp_flags_" + std::string{nonce};
  const std::string command_text = "__libtmux_mcp_command_" + std::string{nonce};
  const std::string declarations = "__libtmux_mcp_traps_" + std::string{nonce};
  const std::string capture_status = "__libtmux_mcp_trap_status_" + std::string{nonce};
  const std::string remember = flags + "=$-";
  const std::string restore = "case \"$" + flags +
                              "\" in *e*) \\set -e ;; esac; case \"$" + flags +
                              "\" in *x*) \\set -x ;; esac";
  return "( " + remember + "; \\set +e; \\set +x; " + command_text + "=" +
         shell_quote(command) + "; " +
         inherited_trap_capture(current_shell, nonce, declarations, capture_status,
                                flags) +
         "; \\trap " + shell_quote(trap_action) + " 0; " + empty + "; " + opening +
         "; if [ \"$" + capture_status + "\" -ne 0 ]; then \\exit 125; fi; ( " +
         restore + "; \\eval \"$" + declarations + "\n$" + command_text +
         "\" ); \\exit \"$?\" )\n";
}

[[nodiscard]] bool valid_shell_nonce(std::string_view nonce) {
  return nonce.size() == 32U && std::ranges::all_of(nonce, [](const char value) {
           return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
         });
}

} // namespace

libtmux::expected<ShellCommandPayload, ToolError>
shell_command_payload(std::string_view command, std::string_view tmux_executable,
                      std::string_view socket_path, std::string_view current_shell,
                      ShellNonceFactory next_nonce) {
  if (contains_control_byte(tmux_executable) || contains_control_byte(socket_path)) {
    return libtmux::unexpected(
        ToolError{false, "shell framing rejects a control byte in its endpoint"});
  }
  if (!std::filesystem::path{tmux_executable}.is_absolute() ||
      !std::filesystem::path{socket_path}.is_absolute() || !next_nonce) {
    return libtmux::unexpected(
        ToolError{false, "shell framing requires an absolute tmux endpoint"});
  }
  std::set<std::string, std::less<>> seen;
  for (std::size_t attempt = 0U; attempt < 32U; ++attempt) {
    const std::string nonce = next_nonce();
    if (!valid_shell_nonce(nonce) || !seen.emplace(nonce).second) {
      continue;
    }
    ShellCommandPayload payload{.marker = "__LIBTMUX_MCP_DONE_" + nonce + "__",
                                .text = {}};
    payload.text =
        shell_frame(command, current_shell, tmux_executable, socket_path, nonce);
    if (payload.text.find(payload.marker) == std::string::npos) {
      return payload;
    }
  }
  return libtmux::unexpected(
      ToolError{false, "collision-free shell framing could not be constructed"});
}

libtmux::expected<std::string, ToolError>
resolve_executable(std::string_view search_path,
                   const std::filesystem::path& current_directory) {
#if defined(_WIN32)
  static_cast<void>(search_path);
  static_cast<void>(current_directory);
  return libtmux::unexpected(
      ToolError{false, "POSIX tmux executable resolution is unavailable"});
#else
  std::size_t begin = 0U;
  while (begin <= search_path.size()) {
    const std::size_t end = search_path.find(':', begin);
    const std::string_view component = search_path.substr(
        begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
    std::filesystem::path directory =
        component.empty() ? current_directory : std::filesystem::path{component};
    if (directory.is_relative()) {
      directory = current_directory / directory;
    }
    const std::filesystem::path candidate = directory / "tmux";
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate, error) && !error &&
        ::access(candidate.c_str(), X_OK) == 0) {
      const std::filesystem::path canonical =
          std::filesystem::canonical(candidate, error);
      if (!error && canonical.is_absolute()) {
        return canonical.string();
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1U;
  }
  return libtmux::unexpected(
      ToolError{false, "tmux executable was not found on the POSIX search path"});
#endif
}

std::optional<ShellCommandCompletion>
shell_command_completion(std::string_view capture, std::string_view marker,
                         std::size_t search_begin) {
  const std::string boundary = "\n" + std::string{marker} + ":BEGIN";
  const auto boundary_begin = capture.find(boundary, search_begin);
  std::size_t text_begin = search_begin;
  if (boundary_begin != std::string_view::npos) {
    const std::size_t padding_begin = boundary_begin + boundary.size();
    const auto boundary_end = capture.find('\n', padding_begin);
    if (boundary_end != std::string_view::npos &&
        std::ranges::all_of(capture.substr(padding_begin, boundary_end - padding_begin),
                            [](const char value) { return value == ' '; })) {
      text_begin = boundary_end + 1U;
    }
  }
  const std::string prefix = "\n" + std::string{marker} + ":";
  const auto record_begin = capture.find(prefix, text_begin);
  if (record_begin == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t status_begin = record_begin + prefix.size();
  const auto status_end = capture.find('\n', status_begin);
  if (status_end == std::string_view::npos || status_end == status_begin) {
    return std::nullopt;
  }
  std::size_t status_value_end = status_end;
  while (status_value_end > status_begin && capture[status_value_end - 1U] == ' ') {
    --status_value_end;
  }
  int status = 0;
  const char* const first = capture.data() + status_begin;
  const char* const last = capture.data() + status_value_end;
  const auto parsed = std::from_chars(first, last, status);
  if (parsed.ec != std::errc{} || parsed.ptr != last || status < 0 || status > 255) {
    return std::nullopt;
  }
  return ShellCommandCompletion{
      .exit_code = status, .text_begin = text_begin, .record_begin = record_begin};
}

std::optional<std::string> parse_linux_process_generation(std::string_view stat) {
  const std::size_t comm_end = stat.rfind(')');
  if (comm_end == std::string_view::npos || comm_end + 2U > stat.size() ||
      stat[comm_end + 1U] != ' ') {
    return std::nullopt;
  }
  std::istringstream fields{std::string{stat.substr(comm_end + 2U)}};
  std::string field;
  for (std::size_t number = 3U; number < 22U; ++number) {
    if (!(fields >> field)) {
      return std::nullopt;
    }
  }
  if (!(fields >> field)) {
    return std::nullopt;
  }
  std::uint64_t ticks = 0U;
  const auto parsed = std::from_chars(field.data(), field.data() + field.size(), ticks);
  if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size() ||
      ticks == 0U) {
    return std::nullopt;
  }
  return std::to_string(ticks);
}

namespace {

constexpr std::array<std::string_view, 11> kPaneInputFields{
    "pane_id",        "window_id",         "session_id",          "pid",
    "start_time",     "pane_synchronized", "pane_in_mode",        "pane_dead",
    "pane_input_off", "window_index",      "pane_current_command"};

constexpr std::array<std::string_view, 8> kPaneInputClientFields{
    "client_control_mode", "session_id", "window_id", "window_index", "pane_id",
    "window_zoomed_flag",  "pid",        "start_time"};

[[nodiscard]] bool canonical_number(std::string_view value) {
  if (value.empty() || (value.size() > 1U && value.front() == '0')) {
    return false;
  }
  std::uint64_t parsed = 0U;
  const auto answer =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  return answer.ec == std::errc{} && answer.ptr == value.data() + value.size();
}

[[nodiscard]] bool canonical_tmux_number(std::string_view value) {
  if (value.empty() || (value.size() > 1U && value.front() == '0')) {
    return false;
  }
  std::uint32_t parsed = 0U;
  const auto answer =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  return answer.ec == std::errc{} && answer.ptr == value.data() + value.size();
}

[[nodiscard]] bool canonical_id(std::string_view value, char prefix) {
  return value.size() > 1U && value.front() == prefix &&
         canonical_tmux_number(value.substr(1U));
}

[[nodiscard]] bool supported_posix_shell(std::string_view command) {
  const std::string_view basename = shell_name(command);
  constexpr std::array<std::string_view, 8> shells{"sh",  "ash",  "bash",  "dash",
                                                   "ksh", "mksh", "pdksh", "zsh"};
  return std::ranges::find(shells, basename) != shells.end();
}

struct PaneInputRow {
  std::string pane_id;
  std::string window_id;
  std::uint32_t window_index{};
  std::string session_id;
  std::uint64_t server_pid{};
  std::uint64_t server_start_time{};
  bool synchronized{};
  std::uint64_t mode{};
  bool dead{};
  bool input_off{};
  std::string command;
};

[[nodiscard]] ToolError invalid_pane_snapshot(std::string_view source_pane_id) {
  return ToolError{false, "tmux returned an invalid pane input snapshot for pane " +
                              std::string{source_pane_id}};
}

[[nodiscard]] ToolError invalid_pane_caller() {
  return ToolError{false, "pane input is refused because the caller identity is "
                          "incomplete, malformed, or inconsistent"};
}

} // namespace

libtmux::expected<PaneInputCaller, ToolError>
parse_pane_input_caller(std::optional<std::string> tmux,
                        std::optional<std::string> tmux_pane,
                        const std::filesystem::path& selected_socket_path) {
  if (!tmux.has_value() && !tmux_pane.has_value()) {
    return PaneInputCaller::detached();
  }
  if (!tmux.has_value() || !tmux_pane.has_value() || tmux->empty() ||
      !canonical_id(*tmux_pane, '%')) {
    return libtmux::unexpected(invalid_pane_caller());
  }

  const std::size_t session_separator = tmux->rfind(',');
  const std::size_t pid_separator = session_separator == std::string::npos
                                        ? std::string::npos
                                        : tmux->rfind(',', session_separator - 1U);
  if (pid_separator == std::string::npos || session_separator == std::string::npos ||
      pid_separator == 0U || pid_separator + 1U == session_separator ||
      session_separator + 1U == tmux->size()) {
    return libtmux::unexpected(invalid_pane_caller());
  }
  const std::string_view pid_text{tmux->data() + pid_separator + 1U,
                                  session_separator - pid_separator - 1U};
  const std::string_view session_text{tmux->data() + session_separator + 1U,
                                      tmux->size() - session_separator - 1U};
  if (!canonical_number(pid_text) || pid_text == "0" ||
      !canonical_tmux_number(session_text)) {
    return libtmux::unexpected(invalid_pane_caller());
  }
  std::uint64_t server_pid = 0U;
  const auto parsed =
      std::from_chars(pid_text.data(), pid_text.data() + pid_text.size(), server_pid);
  if (parsed.ec != std::errc{} || parsed.ptr != pid_text.data() + pid_text.size()) {
    return libtmux::unexpected(invalid_pane_caller());
  }

  const std::string caller_socket_text = tmux->substr(0U, pid_separator);
  const std::string selected_socket_text = selected_socket_path.string();
  const auto caller_endpoint = pane_input_endpoint_identity(caller_socket_text);
  const auto selected_endpoint = pane_input_endpoint_identity(selected_socket_text);
  if (!caller_endpoint.has_value() || !selected_endpoint.has_value()) {
    return libtmux::unexpected(invalid_pane_caller());
  }
  const std::string session_id = "$" + std::string{session_text};
  if (*caller_endpoint != *selected_endpoint) {
    return PaneInputCaller::foreign(std::move(*tmux_pane), session_id, server_pid,
                                    *caller_endpoint);
  }
  return PaneInputCaller::selected(std::move(*tmux_pane), session_id, server_pid,
                                   *caller_endpoint);
}

libtmux::expected<PaneInputPreflight, ToolError>
parse_pane_input_snapshot(std::string_view source_pane_id, std::string raw,
                          PaneInputScope scope, std::string clients,
                          PaneInputCaller caller) {
  if (!canonical_id(source_pane_id, '%') || raw.empty() || raw.back() != '\n' ||
      raw.front() == '\n' || raw.find("\n\n") != std::string::npos) {
    return libtmux::unexpected(invalid_pane_snapshot(source_pane_id));
  }
  const auto snapshot = Snapshot::from_recording(kPaneInputFields, std::move(raw));
  if (snapshot == nullptr || snapshot->rows().empty()) {
    return libtmux::unexpected(invalid_pane_snapshot(source_pane_id));
  }

  std::vector<PaneInputRow> rows;
  rows.reserve(snapshot->rows().size());
  std::map<std::string, std::size_t, std::less<>> pane_rows;
  using PaneInputPlacement = std::tuple<std::string, std::string, std::uint32_t>;
  std::map<std::string, std::set<PaneInputPlacement>, std::less<>> pane_placements;
  std::map<std::string, std::set<std::string, std::less<>>, std::less<>> window_panes;
  std::map<std::string, std::set<PaneInputPlacement>, std::less<>> window_placements;
  std::map<std::pair<std::string, std::uint32_t>, std::string> session_indices;
  std::uint64_t server_pid = 0U;
  std::uint64_t server_start_time = 0U;
  for (const auto& values : snapshot->rows()) {
    if (!canonical_id(values[0], '%') || !canonical_id(values[1], '@') ||
        !canonical_id(values[2], '$') || !canonical_number(values[3]) ||
        values[3] == "0" || !canonical_number(values[4]) || values[4] == "0" ||
        (values[5] != "0" && values[5] != "1") || !canonical_number(values[6]) ||
        (values[7] != "0" && values[7] != "1") ||
        (values[8] != "0" && values[8] != "1") || !canonical_tmux_number(values[9]) ||
        values[10].empty()) {
      return libtmux::unexpected(invalid_pane_snapshot(source_pane_id));
    }
    std::uint64_t mode = 0U;
    const auto parsed =
        std::from_chars(values[6].data(), values[6].data() + values[6].size(), mode);
    std::uint64_t row_server_pid = 0U;
    const auto parsed_pid = std::from_chars(
        values[3].data(), values[3].data() + values[3].size(), row_server_pid);
    std::uint64_t row_server_start_time = 0U;
    const auto parsed_start_time = std::from_chars(
        values[4].data(), values[4].data() + values[4].size(), row_server_start_time);
    std::uint32_t window_index = 0U;
    const auto parsed_window_index = std::from_chars(
        values[9].data(), values[9].data() + values[9].size(), window_index);
    if (parsed.ec != std::errc{} || parsed.ptr != values[6].data() + values[6].size() ||
        parsed_pid.ec != std::errc{} ||
        parsed_pid.ptr != values[3].data() + values[3].size() ||
        parsed_start_time.ec != std::errc{} ||
        parsed_start_time.ptr != values[4].data() + values[4].size() ||
        parsed_window_index.ec != std::errc{} ||
        parsed_window_index.ptr != values[9].data() + values[9].size() ||
        (server_pid != 0U && row_server_pid != server_pid) ||
        (server_start_time != 0U && row_server_start_time != server_start_time)) {
      return libtmux::unexpected(invalid_pane_snapshot(source_pane_id));
    }
    if (server_pid == 0U) {
      server_pid = row_server_pid;
      server_start_time = row_server_start_time;
    }
    PaneInputRow row{.pane_id = std::string{values[0]},
                     .window_id = std::string{values[1]},
                     .window_index = window_index,
                     .session_id = std::string{values[2]},
                     .server_pid = row_server_pid,
                     .server_start_time = row_server_start_time,
                     .synchronized = values[5] == "1",
                     .mode = mode,
                     .dead = values[7] == "1",
                     .input_off = values[8] == "1",
                     .command = std::string{values[10]}};
    const PaneInputPlacement placement{row.session_id, row.window_id, row.window_index};
    if (!pane_placements[row.pane_id].emplace(placement).second) {
      return libtmux::unexpected(invalid_pane_snapshot(source_pane_id));
    }
    window_panes[row.window_id].emplace(row.pane_id);
    window_placements[row.window_id].emplace(placement);
    const auto [indexed_window, unique_index] = session_indices.try_emplace(
        std::pair{row.session_id, row.window_index}, row.window_id);
    if (!unique_index && indexed_window->second != row.window_id) {
      return libtmux::unexpected(invalid_pane_snapshot(source_pane_id));
    }
    const auto [known, first_placement] =
        pane_rows.try_emplace(row.pane_id, rows.size());
    if (!first_placement) {
      const PaneInputRow& same_pane = rows[known->second];
      if (same_pane.window_id != row.window_id ||
          same_pane.server_pid != row.server_pid ||
          same_pane.server_start_time != row.server_start_time ||
          same_pane.synchronized != row.synchronized || same_pane.mode != row.mode ||
          same_pane.dead != row.dead || same_pane.input_off != row.input_off ||
          same_pane.command != row.command) {
        return libtmux::unexpected(invalid_pane_snapshot(source_pane_id));
      }
    }
    rows.push_back(std::move(row));
  }
  for (const auto& [window_id, panes] : window_panes) {
    for (const std::string& pane_id : panes) {
      for (const PaneInputPlacement& placement : window_placements.at(window_id)) {
        if (!pane_placements.at(pane_id).contains(placement)) {
          return libtmux::unexpected(invalid_pane_snapshot(source_pane_id));
        }
      }
    }
  }

  const auto source = std::ranges::find(rows, source_pane_id, &PaneInputRow::pane_id);
  if (source == rows.end()) {
    return libtmux::unexpected(invalid_pane_snapshot(source_pane_id));
  }
  if (caller.relation == PaneInputCallerRelation::selected) {
    const auto caller_row = std::ranges::find_if(rows, [&](const PaneInputRow& row) {
      return row.pane_id == caller.pane_id && row.session_id == caller.session_id;
    });
    if (caller_row == rows.end() || caller_row->server_pid != caller.server_pid) {
      return libtmux::unexpected(invalid_pane_caller());
    }
  }

  std::set<std::string, std::less<>> attended;
  std::vector<PaneInputClientState> terminal_clients;
  if (!clients.empty()) {
    if (clients.back() != '\n' || clients.front() == '\n' ||
        clients.find("\n\n") != std::string::npos) {
      return libtmux::unexpected(invalid_pane_snapshot(source_pane_id));
    }
    const auto client_snapshot =
        Snapshot::from_recording(kPaneInputClientFields, std::move(clients));
    if (client_snapshot == nullptr || client_snapshot->rows().empty()) {
      return libtmux::unexpected(invalid_pane_snapshot(source_pane_id));
    }
    for (const auto& values : client_snapshot->rows()) {
      if (values[0] != "0" && values[0] != "1") {
        return libtmux::unexpected(invalid_pane_snapshot(source_pane_id));
      }
      if (values[0] == "1") {
        continue;
      }
      if (!canonical_id(values[1], '$') || !canonical_id(values[2], '@') ||
          !canonical_tmux_number(values[3]) || !canonical_id(values[4], '%') ||
          (values[5] != "0" && values[5] != "1") || !canonical_number(values[6]) ||
          values[6] == "0" || !canonical_number(values[7]) || values[7] == "0") {
        return libtmux::unexpected(invalid_pane_snapshot(source_pane_id));
      }
      std::uint32_t window_index = 0U;
      const auto parsed_window_index = std::from_chars(
          values[3].data(), values[3].data() + values[3].size(), window_index);
      std::uint64_t client_server_pid = 0U;
      const auto parsed_pid = std::from_chars(
          values[6].data(), values[6].data() + values[6].size(), client_server_pid);
      std::uint64_t client_server_start_time = 0U;
      const auto parsed_start_time =
          std::from_chars(values[7].data(), values[7].data() + values[7].size(),
                          client_server_start_time);
      if (parsed_window_index.ec != std::errc{} ||
          parsed_window_index.ptr != values[3].data() + values[3].size() ||
          parsed_pid.ec != std::errc{} ||
          parsed_pid.ptr != values[6].data() + values[6].size() ||
          parsed_start_time.ec != std::errc{} ||
          parsed_start_time.ptr != values[7].data() + values[7].size() ||
          client_server_pid != server_pid ||
          client_server_start_time != server_start_time) {
        return libtmux::unexpected(invalid_pane_snapshot(source_pane_id));
      }
      const auto active = std::ranges::find_if(rows, [&](const PaneInputRow& row) {
        return row.session_id == values[1] && row.window_id == values[2] &&
               row.window_index == window_index && row.pane_id == values[4];
      });
      if (active == rows.end()) {
        return libtmux::unexpected(invalid_pane_snapshot(source_pane_id));
      }
      terminal_clients.push_back(PaneInputClientState{
          .session_id = std::string{values[1]},
          .window_id = std::string{values[2]},
          .window_index = window_index,
          .pane_id = std::string{values[4]},
          .zoomed = values[5] == "1",
      });
      if (values[5] == "1") {
        attended.emplace(active->pane_id);
      } else {
        for (const PaneInputRow& row : rows) {
          if (row.session_id == active->session_id &&
              row.window_id == active->window_id &&
              row.window_index == active->window_index) {
            attended.emplace(row.pane_id);
          }
        }
      }
    }
  }
  std::ranges::sort(terminal_clients);
  std::vector<const PaneInputRow*> configured;
  if (scope == PaneInputScope::target_only || !source->synchronized) {
    configured.push_back(&*source);
  } else {
    std::set<std::string, std::less<>> configured_ids;
    for (const PaneInputRow& row : rows) {
      if (row.window_id == source->window_id && row.synchronized &&
          configured_ids.emplace(row.pane_id).second) {
        configured.push_back(&row);
      }
    }
  }
  std::ranges::sort(configured, {},
                    [](const PaneInputRow* row) { return row->pane_id; });

  PaneInputPreflight result{.configured_pane_ids = {},
                            .configured_state = {},
                            .terminal_clients = std::move(terminal_clients),
                            .caller = caller,
                            .endpoint_path = {},
                            .window_id = source->window_id,
                            .server_pid = source->server_pid,
                            .server_start_time = source->server_start_time,
                            .server_process_generation = {},
                            .foreground_command = source->command};
  result.configured_pane_ids.reserve(configured.size());
  for (const PaneInputRow* row : configured) {
    if (row->dead) {
      return libtmux::unexpected(ToolError{
          false, "pane " + row->pane_id +
                     " input is refused because its configured process is dead"});
    }
    if (row->input_off) {
      return libtmux::unexpected(
          ToolError{false, "pane " + row->pane_id +
                               " input is refused because tmux input is disabled"});
    }
    if (row->mode != 0U) {
      return libtmux::unexpected(ToolError{
          false, "pane " + row->pane_id +
                     " input is refused because its human-owned mode is active; "
                     "capture pane content and wait for the mode to end"});
    }
    if (caller.relation == PaneInputCallerRelation::selected &&
        row->pane_id == caller.pane_id) {
      return libtmux::unexpected(
          ToolError{false, "pane " + row->pane_id +
                               " input is refused because it belongs to the caller"});
    }
    if (attended.contains(row->pane_id)) {
      return libtmux::unexpected(ToolError{
          false, "pane " + row->pane_id +
                     " input is refused because a terminal client attends it"});
    }
    result.configured_pane_ids.push_back(row->pane_id);
  }
  const std::set<std::string, std::less<>> configured_ids{
      result.configured_pane_ids.begin(), result.configured_pane_ids.end()};
  for (const PaneInputRow& row : rows) {
    if (configured_ids.contains(row.pane_id)) {
      result.configured_state.push_back(PaneInputMemberState{
          .pane_id = row.pane_id,
          .window_id = row.window_id,
          .window_index = row.window_index,
          .session_id = row.session_id,
          .synchronized = row.synchronized,
          .mode = row.mode,
          .dead = row.dead,
          .input_off = row.input_off,
      });
    }
  }
  std::ranges::sort(result.configured_state);
  if (scope == PaneInputScope::singular_posix_shell) {
    if (result.configured_pane_ids.size() != 1U) {
      std::string ids;
      for (const std::string& id : result.configured_pane_ids) {
        ids += ids.empty() ? id : ", " + id;
      }
      return libtmux::unexpected(ToolError{
          false,
          "run_shell_command requires one configured pane target; configured IDs "
          "are " +
              ids +
              "; disable synchronize-panes or use a source pane whose effective "
              "synchronize-panes state is off"});
    }
    if (!supported_posix_shell(result.foreground_command)) {
      return libtmux::unexpected(ToolError{
          false, "run_shell_command requires a supported POSIX shell in pane " +
                     std::string{source_pane_id}});
    }
  }
  return result;
}

[[nodiscard]] static std::optional<std::string>
process_generation(std::uint64_t process_id) noexcept;

[[nodiscard]] static libtmux::expected<std::string, ToolError>
retained_pane_input_endpoint(const Pane& pane) {
  const auto session = pane.session();
  if (!session.has_value()) {
    return libtmux::unexpected(tmux_error(session.error()));
  }
  const auto endpoint = session->attach_command();
  if (!endpoint.has_value()) {
    return libtmux::unexpected(tmux_error(endpoint.error()));
  }
  const std::vector<std::string>& route = endpoint->argv();
  if (route.size() < 3U || route[1] != "-S" ||
      !std::filesystem::path{route[2]}.is_absolute() ||
      contains_control_byte(route[2])) {
    return libtmux::unexpected(
        ToolError{false, "pane input requires a retained POSIX tmux endpoint"});
  }
  return route[2];
}

libtmux::expected<PaneInputPreflight, ToolError>
preflight_pane_input(const Server& server, std::string_view source_pane_id,
                     PaneInputScope scope) {
  const auto pane = server.pane(source_pane_id);
  if (!pane.has_value()) {
    return libtmux::unexpected(tmux_error(pane.error()));
  }
  const auto endpoint = retained_pane_input_endpoint(*pane);
  if (!endpoint.has_value()) {
    return libtmux::unexpected(endpoint.error());
  }
  const auto panes =
      server.run({"list-panes", "-a", "-F", format_request(kPaneInputFields)});
  if (!panes.has_value()) {
    return libtmux::unexpected(tmux_error(panes.error()));
  }
  const auto clients =
      server.run({"list-clients", "-F", format_request(kPaneInputClientFields)});
  if (!clients.has_value()) {
    return libtmux::unexpected(tmux_error(clients.error()));
  }
  auto caller = parse_pane_input_caller(environment_value("TMUX"),
                                        environment_value("TMUX_PANE"), *endpoint);
  if (!caller.has_value()) {
    return libtmux::unexpected(caller.error());
  }
  auto parsed = parse_pane_input_snapshot(source_pane_id, *panes, scope, *clients,
                                          std::move(*caller));
  if (!parsed.has_value()) {
    return parsed;
  }
  if (auto generation = process_generation(parsed->server_pid);
      generation.has_value()) {
    parsed->server_process_generation = std::move(*generation);
  }
  parsed->endpoint_path = *endpoint;
  return parsed;
}

[[nodiscard]] static libtmux::expected<PaneInputLease, ToolError>
reserve_pane_input(const PaneInputPreflight& preflight, PaneInputReservationKind kind,
                   std::string_view tool_name) {
  return detail::reserve_pane_input(preflight.endpoint_path, preflight.server_pid,
                                    preflight.server_start_time,
                                    preflight.configured_pane_ids, kind, tool_name);
}

bool same_pane_input_route(const PaneInputPreflight& initial,
                           const PaneInputPreflight& final) {
  return initial.configured_pane_ids == final.configured_pane_ids &&
         initial.configured_state == final.configured_state &&
         initial.terminal_clients == final.terminal_clients &&
         initial.caller == final.caller && initial.window_id == final.window_id &&
         initial.server_pid == final.server_pid &&
         initial.server_start_time == final.server_start_time &&
         initial.server_process_generation == final.server_process_generation;
}

[[nodiscard]] static bool same_shell_input_route(const PaneInputPreflight& initial,
                                                 const PaneInputPreflight& final) {
  return same_pane_input_route(initial, final) &&
         initial.foreground_command == final.foreground_command;
}

[[nodiscard]] static ToolError changed_pane_input_route(std::string_view tool_name) {
  return ToolError{false, std::string{tool_name} +
                              " refuses because the pane input route changed before "
                              "dispatch; no input was sent"};
}

[[nodiscard]] static bool pane_identity_settled(std::string raw,
                                                std::string_view pane_id,
                                                std::uint64_t server_pid,
                                                std::uint64_t server_start_time) {
  constexpr std::array<std::string_view, 4> fields{"pane_id", "pid", "start_time",
                                                   "pane_dead"};
  if (raw.empty() || raw.back() != '\n' || raw.front() == '\n' ||
      raw.find("\n\n") != std::string::npos) {
    return false;
  }
  const auto snapshot = Snapshot::from_recording(fields, std::move(raw));
  if (snapshot == nullptr || snapshot->rows().empty()) {
    return false;
  }
  std::optional<bool> matching_dead;
  for (const auto& values : snapshot->rows()) {
    if (!canonical_id(values[0], '%') || !canonical_number(values[1]) ||
        values[1] == "0" || !canonical_number(values[2]) || values[2] == "0" ||
        (values[3] != "0" && values[3] != "1")) {
      return false;
    }
    std::uint64_t row_pid = 0U;
    const auto parsed =
        std::from_chars(values[1].data(), values[1].data() + values[1].size(), row_pid);
    std::uint64_t row_start_time = 0U;
    const auto parsed_start = std::from_chars(
        values[2].data(), values[2].data() + values[2].size(), row_start_time);
    if (parsed.ec != std::errc{} || parsed.ptr != values[1].data() + values[1].size()) {
      return false;
    }
    if (parsed_start.ec != std::errc{} ||
        parsed_start.ptr != values[2].data() + values[2].size()) {
      return false;
    }
    if (row_pid != server_pid || row_start_time != server_start_time) {
      return false;
    }
    if (values[0] == pane_id) {
      const bool dead = values[3] == "1";
      if (matching_dead.has_value() && *matching_dead != dead) {
        return false;
      }
      matching_dead = dead;
    }
  }
  return !matching_dead.has_value() || *matching_dead;
}

[[nodiscard]] static std::optional<std::string>
process_generation(std::uint64_t process_id) noexcept {
#if defined(_WIN32)
  static_cast<void>(process_id);
  return std::nullopt;
#else
  if (process_id == 0U ||
      process_id > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
    return std::nullopt;
  }
  try {
#if defined(__linux__)
    std::ifstream process{"/proc/" + std::to_string(process_id) + "/stat"};
    std::string stat;
    if (!std::getline(process, stat)) {
      return std::nullopt;
    }
    return parse_linux_process_generation(stat);
#elif defined(__APPLE__)
    struct kinfo_proc process {};
    std::size_t size = sizeof(process);
    int query[4]{CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(process_id)};
    if (::sysctl(query, 4U, &process, &size, nullptr, 0U) != 0 ||
        size != sizeof(process) || process.kp_proc.p_starttime.tv_sec <= 0) {
      return std::nullopt;
    }
    return std::to_string(process.kp_proc.p_starttime.tv_sec) + ":" +
           std::to_string(process.kp_proc.p_starttime.tv_usec);
#else
    static_cast<void>(process_id);
    return std::nullopt;
#endif
  } catch (...) {
    return std::nullopt;
  }
#endif
}

[[nodiscard]] static bool
process_identity_absent(std::uint64_t process_id, std::uint64_t server_start_time,
                        std::string_view captured_generation) noexcept {
#if defined(_WIN32)
  static_cast<void>(process_id);
  static_cast<void>(server_start_time);
  static_cast<void>(captured_generation);
  return false;
#else
  if (process_id == 0U || server_start_time == 0U ||
      process_id > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
    return false;
  }
  errno = 0;
  if (::kill(static_cast<pid_t>(process_id), 0) == -1) {
    return errno == ESRCH;
  }
  if (captured_generation.empty()) {
    return false;
  }
  const auto current_generation = process_generation(process_id);
  return current_generation.has_value() && *current_generation != captured_generation;
#endif
}

static void
retain_run_until_proven_complete(PaneInputLease lease, Server server, Pane pane,
                                 std::string marker,
                                 const PaneInputPreflight& preflight) noexcept {
  class Settlement final {
  public:
    explicit Settlement(PaneInputLease lease) : lease_{std::move(lease)} {}

    ~Settlement() {
      if (!settled_.load(std::memory_order_acquire)) {
        lease_.abandon();
      }
    }

    [[nodiscard]] bool settled() const noexcept {
      return settled_.load(std::memory_order_acquire);
    }

    void settle() {
      bool expected = false;
      if (settled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        lease_.release();
      }
    }

  private:
    PaneInputLease lease_;
    std::atomic_bool settled_{};
  };

  try {
    auto settlement = std::make_shared<Settlement>(std::move(lease));
    const std::string pane_id{pane.id()};
    const std::string endpoint = preflight.endpoint_path;
    const auto retained_endpoint = pane_input_endpoint_identity(endpoint);
    const auto same_endpoint = [endpoint, retained_endpoint] {
      return retained_endpoint.has_value() &&
             pane_input_endpoint_identity(endpoint) == retained_endpoint;
    };
    const auto watch = [settlement](auto prove) mutable {
      try {
        if (prove_pane_input_settlement([&] { return settlement->settled(); },
                                        std::move(prove),
                                        [](std::chrono::milliseconds delay) {
                                          std::this_thread::sleep_for(delay);
                                        })) {
          settlement->settle();
        }
      } catch (...) {
      }
    };
    const auto launch = [&](auto prove) {
      try {
        std::thread{watch, std::move(prove)}.detach();
      } catch (...) {
      }
    };

    launch([server, pane_id, marker = std::move(marker), same_endpoint] {
      if (!same_endpoint()) {
        return false;
      }
      const auto captured =
          server.run({"capture-pane", "-p", "-t", pane_id, "-S", "-", "-J"},
                     kPaneInputSettlementProofTimeout);
      return captured.has_value() && same_endpoint() &&
             shell_command_completion(*captured, marker).has_value();
    });
    launch([server, pane_id, server_pid = preflight.server_pid,
            server_start_time = preflight.server_start_time, same_endpoint] {
      if (!same_endpoint()) {
        return false;
      }
      constexpr std::array<std::string_view, 4> fields{"pane_id", "pid", "start_time",
                                                       "pane_dead"};
      const auto panes = server.run({"list-panes", "-a", "-F", format_request(fields)},
                                    kPaneInputSettlementProofTimeout);
      return panes.has_value() && same_endpoint() &&
             pane_identity_settled(*panes, pane_id, server_pid, server_start_time);
    });
    launch([server_pid = preflight.server_pid,
            server_start_time = preflight.server_start_time,
            process_generation = preflight.server_process_generation] {
      return process_identity_absent(server_pid, server_start_time, process_generation);
    });
  } catch (...) {
    lease.abandon();
  }
}

StructuredValue session_value(const Session& session) {
  return StructuredValue::Object{
      {"attached", session.attached()},
      {"client_count", session.client_count()},
      {"id", session.id()},
      {"name", session.name()},
      {"path", session.path()},
      {"window_count", session.window_count()},
  };
}

StructuredValue window_value(const Window& window) {
  return StructuredValue::Object{
      {"active", window.active()},
      {"height", window.height()},
      {"id", window.id()},
      {"index", window.index()},
      {"layout", window.layout()},
      {"name", window.name()},
      {"pane_count", window.pane_count()},
      {"session_id", window.session_id()},
      {"width", window.width()},
  };
}

StructuredValue pane_value(const Pane& pane) {
  return StructuredValue::Object{
      {"active", pane.active()},
      {"command", pane.command()},
      {"dead", pane.dead()},
      {"height", pane.height()},
      {"id", pane.id()},
      {"index", pane.index()},
      {"path", pane.path()},
      {"pid", pane.pid()},
      {"session_id", pane.session_id()},
      {"title", pane.title()},
      {"width", pane.width()},
      {"window_id", pane.window_id()},
  };
}

namespace {
[[nodiscard]] ToolError cancelled() { return ToolError{false, "request cancelled"}; }
} // namespace

libtmux::expected<BoundedRegex, std::string>
BoundedRegex::compile(std::string_view pattern) {
  std::vector<Atom> atoms;
  bool escaped = false;
  for (const char value : pattern) {
    if (escaped) {
      atoms.push_back(Atom{.value = value});
      escaped = false;
      continue;
    }
    if (value == '\\') {
      escaped = true;
      continue;
    }
    if (value == '*') {
      if (atoms.empty() || atoms.back().repeated) {
        return libtmux::unexpected(std::string{"misplaced regex repetition"});
      }
      atoms.back().repeated = true;
      continue;
    }
    if (value == '.') {
      atoms.push_back(Atom{.any = true});
      continue;
    }
    if (std::string_view{"+?[](){}|^$"}.find(value) != std::string_view::npos) {
      return libtmux::unexpected(
          std::string{"unsupported regex operator; escape it for a literal"});
    }
    atoms.push_back(Atom{.value = value});
  }
  if (escaped || atoms.empty()) {
    return libtmux::unexpected(std::string{"incomplete or empty regex"});
  }
  return BoundedRegex{std::move(atoms)};
}

libtmux::expected<bool, std::string>
BoundedRegex::search(std::string_view text, std::size_t& remaining_work) const {
  const auto charge = [&remaining_work](std::size_t work) {
    if (work > remaining_work) {
      return false;
    }
    remaining_work -= work;
    return true;
  };
  if (!charge(atoms_.size())) {
    return libtmux::unexpected(std::string{"search matching work limit exceeded"});
  }
  std::vector<bool> previous(atoms_.size() + 1U);
  std::vector<bool> next(atoms_.size() + 1U);
  previous[0] = true;
  for (std::size_t index = 1U; index <= atoms_.size(); ++index) {
    previous[index] = atoms_[index - 1U].repeated && previous[index - 1U];
  }
  if (previous.back()) {
    return true;
  }
  for (const char character : text) {
    if (!charge(atoms_.size())) {
      return libtmux::unexpected(std::string{"search matching work limit exceeded"});
    }
    next.assign(atoms_.size() + 1U, false);
    next[0] = true;
    for (std::size_t index = 1U; index <= atoms_.size(); ++index) {
      const Atom& atom = atoms_[index - 1U];
      const bool matches = atom.any || atom.value == character;
      next[index] = atom.repeated ? next[index - 1U] || (matches && previous[index])
                                  : matches && previous[index - 1U];
    }
    if (next.back()) {
      return true;
    }
    previous.swap(next);
  }
  return false;
}
} // namespace libtmux::mcp::detail

namespace libtmux::mcp {
namespace {

using namespace std::chrono_literals;

struct Field {
  Parameter parameter;
  std::set<Sink> sinks;
};

void append_json_string(std::string_view value, std::string& output) {
  constexpr std::string_view hex = "0123456789abcdef";
  output.push_back('"');
  for (const char byte : value) {
    const auto character = static_cast<unsigned char>(byte);
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\b':
      output += "\\b";
      break;
    case '\f':
      output += "\\f";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20U) {
        output += "\\u00";
        output.push_back(hex[character >> 4U]);
        output.push_back(hex[character & 0x0FU]);
      } else {
        output.push_back(static_cast<char>(character));
      }
    }
  }
  output.push_back('"');
}

void append_json(const StructuredValue& value, std::string& output) {
  std::visit(
      [&](const auto& item) {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::same_as<Item, std::nullptr_t>) {
          output += "null";
        } else if constexpr (std::same_as<Item, bool>) {
          output += item ? "true" : "false";
        } else if constexpr (std::same_as<Item, std::int64_t>) {
          std::array<char, 32> digits{};
          const auto converted =
              std::to_chars(digits.data(), digits.data() + digits.size(), item);
          output.append(digits.data(), converted.ptr);
        } else if constexpr (std::same_as<Item, std::string>) {
          append_json_string(item, output);
        } else if constexpr (std::same_as<Item, StructuredValue::Array>) {
          output.push_back('[');
          bool first = true;
          for (const StructuredValue& nested : item) {
            if (!first) {
              output.push_back(',');
            }
            first = false;
            append_json(nested, output);
          }
          output.push_back(']');
        } else {
          output.push_back('{');
          bool first = true;
          for (const auto& [key, nested] : item) {
            if (!first) {
              output.push_back(',');
            }
            first = false;
            append_json_string(key, output);
            output.push_back(':');
            append_json(nested, output);
          }
          output.push_back('}');
        }
      },
      value.value);
}

[[nodiscard]] std::string serialize_json(const StructuredValue& value) {
  std::string output;
  append_json(value, output);
  return output;
}

[[nodiscard]] StructuredValue nested_result(const ToolOutput& answer) {
  StructuredValue structured{answer.structured};
  const std::string text = serialize_json(structured);
  return StructuredValue::Object{
      {"content", StructuredValue::Array{StructuredValue::Object{{"text", text},
                                                                 {"type", "text"}}}},
      {"isError", false},
      {"structuredContent", std::move(structured)},
  };
}

[[nodiscard]] StructuredValue nested_error(std::string_view message) {
  return StructuredValue::Object{
      {"content", StructuredValue::Array{StructuredValue::Object{{"text", message},
                                                                 {"type", "text"}}}},
      {"isError", true},
  };
}

[[nodiscard]] Field
field(std::string name, std::string description, InputSink type, bool required = true,
      ArgumentType argument_type = ArgumentType::string,
      std::optional<long long> minimum = {}, std::optional<long long> maximum = {},
      std::optional<std::size_t> maximum_length = {},
      NestedAuthority nested = NestedAuthority::none,
      InputControl control = InputControl::none,
      std::vector<std::string> allowed_values = {}, bool allow_empty = false) {
  std::set<Sink> sinks{Sink{type, nested, control}};
  if (type == InputSink::tmux_format && control == InputControl::double_hash_once) {
    sinks.insert(
        Sink{InputSink::tmux_state, NestedAuthority::none, InputControl::none});
  } else if (type == InputSink::tmux_format &&
             control == InputControl::validated_variable_name) {
    sinks.insert(
        Sink{InputSink::tmux_lookup, NestedAuthority::none, InputControl::none});
  }
  return Field{.parameter = {.name = std::move(name),
                             .description = std::move(description),
                             .type = argument_type,
                             .required = required,
                             .minimum = minimum,
                             .maximum = maximum,
                             .maximum_length = maximum_length,
                             .allowed_values = std::move(allowed_values),
                             .allow_empty = allow_empty},
               .sinks = std::move(sinks)};
}

[[nodiscard]] Field send_key_operations_field() {
  return Field{
      .parameter = {.name = "operations",
                    .description = "One to sixty-four ordered pane-input operations.",
                    .type = ArgumentType::send_key_operations,
                    .required = true,
                    .minimum = std::nullopt,
                    .maximum = std::nullopt,
                    .maximum_length = std::nullopt,
                    .allowed_values = {}},
      .sinks = {Sink{InputSink::tmux_lookup, NestedAuthority::none},
                Sink{InputSink::pane_input, NestedAuthority::unrestricted}},
  };
}

[[nodiscard]] ToolDefinition
make_tool(std::string name, std::string title, Toolset toolset, ProcessReach reach,
          std::set<Effect> effects, std::set<OutputClass> outputs, bool secrets,
          bool untrusted, ToolAnnotations annotations, std::vector<Field> fields,
          OutputShape output_shape, Handler handler, std::string_view description,
          bool amplifies_future_input = false,
          std::set<std::string, std::less<>> nested_tools = {}) {
  ToolDefinition tool{.name = std::move(name),
                      .title = std::move(title),
                      .description = {},
                      .toolset = toolset,
                      .authority = {.process_reach = reach,
                                    .effects = std::move(effects),
                                    .output_classes = std::move(outputs),
                                    .may_expose_secrets = secrets,
                                    .may_return_untrusted_content = untrusted,
                                    .amplifies_future_input = amplifies_future_input,
                                    .input_sinks = {},
                                    .nested_tools = std::move(nested_tools)},
                      .annotations = annotations,
                      .schema = {.input = {}, .output = output_shape},
                      .handler = std::move(handler)};
  tool.description = std::string{
      detail::controlled_opener(tool.toolset, tool.authority.process_reach,
                                tool.authority.output_classes, tool.authority.effects)};
  if (!description.empty()) {
    tool.description.push_back(' ');
    tool.description.append(description);
  }
  for (Field& item : fields) {
    tool.authority.input_sinks.emplace(item.parameter.name, std::move(item.sinks));
    tool.schema.input.push_back(std::move(item.parameter));
  }
  return tool;
}

[[nodiscard]] const std::string& required(const FlatArguments& arguments,
                                          std::string_view name) {
  return arguments.find(name)->second;
}

[[nodiscard]] std::string optional(const FlatArguments& arguments,
                                   std::string_view name) {
  const auto found = arguments.find(name);
  return found == arguments.end() ? std::string{} : found->second;
}

[[nodiscard]] long long integer(const FlatArguments& arguments, std::string_view name,
                                long long fallback = 0) {
  const auto found = arguments.find(name);
  if (found == arguments.end()) {
    return fallback;
  }
  long long value = fallback;
  static_cast<void>(std::from_chars(
      found->second.data(), found->second.data() + found->second.size(), value));
  return value;
}

[[nodiscard]] bool boolean(const FlatArguments& arguments, std::string_view name,
                           bool fallback = false) {
  const auto found = arguments.find(name);
  return found == arguments.end() ? fallback : found->second == "true";
}

[[nodiscard]] ToolResult failure(const CommandFailure& error) {
  return libtmux::unexpected(detail::tmux_error(error));
}

[[nodiscard]] ToolResult changed(std::string_view key, std::string_view id) {
  return detail::output({{std::string{key}, id}, {"changed", StructuredValue{true}}});
}

[[nodiscard]] StructuredValue::Array
pane_target_ids(const std::vector<std::string>& pane_ids) {
  StructuredValue::Array targets;
  targets.reserve(pane_ids.size());
  for (const std::string& pane_id : pane_ids) {
    targets.emplace_back(pane_id);
  }
  return targets;
}

[[nodiscard]] std::string shell_command_nonce() {
  constexpr std::string_view digits{"0123456789abcdef"};
  std::random_device entropy;
  std::string nonce;
  nonce.reserve(32U);
  for (std::size_t index = 0; index < 16U; ++index) {
    const auto value = entropy();
    nonce += digits[(value >> 4U) & 0x0fU];
    nonce += digits[value & 0x0fU];
  }
  return nonce;
}

[[nodiscard]] libtmux::expected<std::string, ToolError>
private_paste_buffer_name(const Server& server) {
  const auto buffers = server.buffers();
  if (!buffers.has_value()) {
    return libtmux::unexpected(detail::tmux_error(buffers.error()));
  }
  static std::atomic_uint64_t sequence{0U};
  for (std::size_t attempt = 0U; attempt < 32U; ++attempt) {
    const std::string name =
        "libtmux-mcp-paste-" + shell_command_nonce() + "-" + std::to_string(++sequence);
    if (std::ranges::none_of(
            *buffers, [&](const Buffer& buffer) { return buffer.name() == name; })) {
      return name;
    }
  }
  return libtmux::unexpected(
      ToolError{false, "could not allocate a private paste buffer name"});
}

[[nodiscard]] std::optional<ToolError>
remove_private_paste_buffer(const Server& server, std::string_view name) {
  const auto removed = server.run({"delete-buffer", "-b", std::string{name}});
  const auto buffers = server.buffers();
  if (buffers.has_value() && std::ranges::none_of(*buffers, [&](const Buffer& buffer) {
        return buffer.name() == name;
      })) {
    return std::nullopt;
  }
  std::string message{"temporary paste buffer cleanup could not be confirmed"};
  if (!removed.has_value()) {
    message += ": " + removed.error().diagnostic;
  }
  if (!buffers.has_value()) {
    message += "; verification failed: " + buffers.error().diagnostic;
  } else {
    message += "; the private buffer remains present";
  }
  return ToolError{false, std::move(message)};
}

[[nodiscard]] ToolError with_paste_cleanup_error(ToolError primary,
                                                 const ToolError& cleanup) {
  primary.caller_error = false;
  primary.message += "; " + cleanup.message;
  return primary;
}

[[nodiscard]] libtmux::expected<std::string, ToolError> resolve_tmux_executable() {
#if defined(_WIN32)
  return libtmux::unexpected(
      ToolError{false, "run_shell_command requires a POSIX tmux endpoint"});
#else
  std::string search_path;
  if (const char* const configured = std::getenv("PATH"); configured != nullptr) {
    search_path = configured;
  } else {
    const std::size_t size = ::confstr(_CS_PATH, nullptr, 0U);
    if (size == 0U) {
      return libtmux::unexpected(
          ToolError{false, "the POSIX executable search path is unavailable"});
    }
    std::string fallback(size, '\0');
    const std::size_t written = ::confstr(_CS_PATH, fallback.data(), fallback.size());
    if (written == 0U || written > fallback.size()) {
      return libtmux::unexpected(
          ToolError{false, "the POSIX executable search path is unavailable"});
    }
    fallback.resize(written - 1U);
    search_path = std::move(fallback);
  }
  std::error_code error;
  const std::filesystem::path current = std::filesystem::current_path(error);
  if (error) {
    return libtmux::unexpected(
        ToolError{false, "the current executable search directory is unavailable"});
  }
  return detail::resolve_executable(search_path, current);
#endif
}

[[nodiscard]] StructuredValue option_value(const OptionEntry& option) {
  StructuredValue::Object result{
      {"inherited", option.inherited}, {"name", option.name}, {"value", option.value}};
  if (option.index.has_value()) {
    result.emplace("index", static_cast<long long>(*option.index));
  }
  return result;
}

[[nodiscard]] std::vector<std::string_view> lines(std::string_view value) {
  std::vector<std::string_view> result;
  std::size_t offset = 0;
  while (offset <= value.size()) {
    const std::size_t end = value.find('\n', offset);
    const std::string_view line = value.substr(
        offset, end == std::string_view::npos ? std::string_view::npos : end - offset);
    if (!line.empty()) {
      result.push_back(line);
    }
    if (end == std::string_view::npos) {
      break;
    }
    offset = end + 1U;
  }
  return result;
}

[[nodiscard]] bool valid_variable(std::string_view name) {
  return !name.empty() && std::ranges::all_of(name, [](char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
  });
}

[[nodiscard]] ToolResult list_sessions_impl(const Server& server) {
  const auto sessions = server.sessions();
  if (!sessions.has_value()) {
    if (sessions.error().kind == FailureKind::missing) {
      return detail::output({{"sessions", StructuredValue::Array{}}});
    }
    return failure(sessions.error());
  }
  StructuredValue::Array rows;
  for (const Session& session : *sessions) {
    rows.push_back(detail::session_value(session));
  }
  return detail::output({{"sessions", StructuredValue{std::move(rows)}}});
}

[[nodiscard]] ToolResult list_panes_impl(const Server& server) {
  const auto panes = server.panes();
  if (!panes.has_value()) {
    if (panes.error().kind == FailureKind::missing) {
      return detail::output({{"panes", StructuredValue::Array{}}});
    }
    return failure(panes.error());
  }
  StructuredValue::Array rows;
  for (const Pane& pane : *panes) {
    rows.push_back(detail::pane_value(pane));
  }
  return detail::output({{"panes", StructuredValue{std::move(rows)}}});
}

[[nodiscard]] std::vector<ToolDefinition> definitions() {
  std::vector<ToolDefinition> tools;
  tools.reserve(45U);
  const std::set<std::string, std::less<>> read_batch_tools{
      "list_sessions",      "list_windows",     "list_panes",
      "get_server_info",    "get_session_info", "get_window_info",
      "get_pane_info",      "capture_pane",     "capture_since",
      "snapshot_pane",      "search_panes",     "find_pane_by_position",
      "get_tmux_variables", "show_option",      "show_environment",
      "show_hooks"};
  const auto add = [&tools](ToolDefinition tool) { tools.push_back(std::move(tool)); };

  add(make_tool(
      "list_sessions", "List tmux sessions", Toolset::inspect, ProcessReach::none,
      {Effect::observe}, {OutputClass::tmux_metadata}, false, false,
      kConservativeAnnotations, {}, OutputShape::sessions,
      [](const Server& server, const Arguments&, const CallContext&) {
        return list_sessions_impl(server);
      },
      "List stable session IDs, names, paths, and client counts."));

  add(make_tool(
      "list_windows", "List tmux windows", Toolset::inspect, ProcessReach::none,
      {Effect::observe}, {OutputClass::tmux_metadata}, false, false,
      kConservativeAnnotations,
      {field("session", "Stable session ID or name.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      OutputShape::windows,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto session = server.session(required(arguments, "session"));
        if (!session.has_value()) {
          return failure(session.error());
        }
        const auto windows = session->windows();
        if (!windows.has_value()) {
          return failure(windows.error());
        }
        StructuredValue::Array rows;
        for (const Window& window : *windows) {
          rows.push_back(detail::window_value(window));
        }
        return detail::output({{"windows", StructuredValue{std::move(rows)}}});
      },
      "List windows through an exact owning session."));

  add(make_tool(
      "list_panes", "List tmux panes", Toolset::inspect, ProcessReach::none,
      {Effect::observe}, {OutputClass::tmux_metadata}, false, false,
      kConservativeAnnotations, {}, OutputShape::panes,
      [](const Server& server, const Arguments&, const CallContext&) {
        return list_panes_impl(server);
      },
      "List every pane with stable pane, window, and session IDs."));

  const auto inspect_metadata = [&](std::string name, std::string title,
                                    std::vector<Field> fields, OutputShape shape,
                                    Handler handler, std::string description) {
    add(make_tool(std::move(name), std::move(title), Toolset::inspect,
                  ProcessReach::none, {Effect::observe}, {OutputClass::tmux_metadata},
                  false, false, kConservativeAnnotations, std::move(fields), shape,
                  std::move(handler), std::move(description)));
  };
  const auto inspect_terminal =
      [&](std::string name, std::string title, std::vector<Field> fields,
          OutputShape shape, Handler handler, std::string description,
          std::set<OutputClass> outputs = {OutputClass::tmux_metadata,
                                           OutputClass::terminal_content}) {
        add(make_tool(std::move(name), std::move(title), Toolset::inspect,
                      ProcessReach::none, {Effect::observe}, std::move(outputs), true,
                      true, kConservativeAnnotations, std::move(fields), shape,
                      std::move(handler), std::move(description)));
      };

  inspect_metadata(
      "get_server_info", "Get tmux server information", {}, OutputShape::object,
      [](const Server& server, const Arguments&, const CallContext&) -> ToolResult {
        const bool running = server.is_alive();
        StructuredValue::Object result{{"running", running}};
        const auto version = server.tmux_version();
        if (version.has_value()) {
          std::string text = version->unbounded ? std::string{"master"}
                                                : std::to_string(version->major) + "." +
                                                      std::to_string(version->minor);
          if (version->revision != 0U) {
            text += "." + std::to_string(version->revision);
          }
          result.emplace("version", std::move(text));
        }
        if (running) {
          // The configured path, not `#{socket_path}`: tmux escapes a
          // non-printable byte in the path it stores, and would report a path
          // that names no file.
          if (const std::string_view socket = server.socket_path(); !socket.empty()) {
            result.emplace("socket_path", std::string{socket});
          }
        }
        return detail::output(std::move(result));
      },
      "Report liveness, tmux version, and the resolved socket when running.");

  inspect_metadata(
      "get_session_info", "Get tmux session information",
      {field("session", "Stable session ID or name.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto session = server.session(required(arguments, "session"));
        return session.has_value()
                   ? detail::output({{"session", detail::session_value(*session)}})
                   : failure(session.error());
      },
      "Return one session by stable ID or name.");

  inspect_metadata(
      "get_window_info", "Get tmux window information",
      {field("windowId", "Stable window ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto window = server.window(required(arguments, "windowId"));
        return window.has_value()
                   ? detail::output({{"window", detail::window_value(*window)}})
                   : failure(window.error());
      },
      "Return one window by stable ID.");

  inspect_metadata(
      "get_pane_info", "Get tmux pane information",
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        return pane.has_value() ? detail::output({{"pane", detail::pane_value(*pane)}})
                                : failure(pane.error());
      },
      "Return one pane by stable ID.");

  inspect_terminal(
      "capture_pane", "Capture a tmux pane",
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("history", "Capture retained history as well as the visible pane.",
             InputSink::none, false, ArgumentType::boolean)},
      OutputShape::pane_text,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        CaptureOptions options;
        options.whole_history = boolean(arguments, "history");
        const auto captured = pane->capture(options);
        return captured.has_value()
                   ? detail::output({{"pane_id", pane->id()}, {"text", *captured}})
                   : failure(captured.error());
      },
      "Return visible text, or retained history when requested.");

  inspect_terminal(
      "capture_since", "Capture new pane output",
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("cursor", "Previously returned byte cursor.", InputSink::none, false,
             ArgumentType::integer, 0)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        CaptureOptions options;
        options.whole_history = true;
        const auto captured = pane->capture(options);
        if (!captured.has_value()) {
          return failure(captured.error());
        }
        std::size_t cursor = static_cast<std::size_t>(integer(arguments, "cursor"));
        if (cursor > captured->size()) {
          cursor = 0U;
        }
        return detail::output({{"cursor", static_cast<long long>(captured->size())},
                               {"pane_id", pane->id()},
                               {"text", captured->substr(cursor)}});
      },
      "Return bytes after a bounded client cursor and a replacement cursor.");

  inspect_terminal("snapshot_pane", "Snapshot a tmux pane",
                   {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
                          ArgumentType::string, {}, {}, detail::kTargetCharacters)},
                   OutputShape::object,
                   [](const Server& server, const Arguments& arguments,
                      const CallContext&) -> ToolResult {
                     const auto pane = server.pane(required(arguments, "paneId"));
                     if (!pane.has_value()) {
                       return failure(pane.error());
                     }
                     const auto captured = pane->capture();
                     return captured.has_value()
                                ? detail::output({{"pane", detail::pane_value(*pane)},
                                                  {"text", *captured}})
                                : failure(captured.error());
                   },
                   "Return pane metadata and visible terminal content from one call.",
                   {OutputClass::tmux_metadata, OutputClass::terminal_content});

  inspect_terminal(
      "search_panes", "Search tmux panes",
      {field("pattern", "Bounded regular expression.", InputSink::regex, true,
             ArgumentType::string, {}, {}, 256U)},
      OutputShape::matches,
      [](const Server& server, const Arguments& arguments,
         const CallContext& context) -> ToolResult {
        const auto pattern =
            detail::BoundedRegex::compile(required(arguments, "pattern"));
        if (!pattern.has_value()) {
          return libtmux::unexpected(ToolError{true, pattern.error()});
        }
        const auto panes = server.panes();
        if (!panes.has_value()) {
          return failure(panes.error());
        }
        StructuredValue::Array matches;
        std::size_t remaining_work = detail::kRegexWorkUnits;
        for (const Pane& pane : *panes) {
          if (context.cancelled()) {
            return libtmux::unexpected(detail::cancelled());
          }
          const auto captured = pane.capture();
          if (!captured.has_value()) {
            return failure(captured.error());
          }
          for (const std::string_view line : libtmux::capture_lines(*captured)) {
            const auto matched = pattern->search(line, remaining_work);
            if (!matched.has_value()) {
              return libtmux::unexpected(ToolError{false, matched.error()});
            }
            if (*matched) {
              if (matches.size() == detail::kSearchMatchLimit) {
                return libtmux::unexpected(
                    ToolError{false, "search match limit exceeded"});
              }
              matches.push_back(
                  StructuredValue::Object{{"line", line}, {"pane_id", pane.id()}});
            }
          }
        }
        return detail::output({{"matches", StructuredValue{std::move(matches)}}});
      },
      "Search captured lines using a length-limited expression.");

  inspect_metadata(
      "find_pane_by_position", "Find a pane by position",
      {field("row", "Zero-based terminal row.", InputSink::none, true,
             ArgumentType::integer, 0),
       field("column", "Zero-based terminal column.", InputSink::none, true,
             ArgumentType::integer, 0),
       field("windowId", "Optional stable window ID.", InputSink::tmux_lookup, false,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto reply = server.run({"list-panes", "-a", "-F",
                                       "#{pane_id}\t#{window_id}\t#{pane_left}\t"
                                       "#{pane_top}\t#{pane_width}\t#{pane_height}"});
        if (!reply.has_value()) {
          return failure(reply.error());
        }
        const long long row = integer(arguments, "row");
        const long long column = integer(arguments, "column");
        const std::string window = optional(arguments, "windowId");
        for (const std::string_view line : lines(*reply)) {
          std::vector<std::string_view> parts;
          std::size_t offset = 0U;
          while (offset <= line.size()) {
            const auto end = line.find('\t', offset);
            parts.push_back(line.substr(offset, end == std::string_view::npos
                                                    ? std::string_view::npos
                                                    : end - offset));
            if (end == std::string_view::npos) {
              break;
            }
            offset = end + 1U;
          }
          if (parts.size() != 6U || (!window.empty() && parts[1] != window)) {
            continue;
          }
          const auto number = [](std::string_view text) {
            long long result = 0;
            static_cast<void>(
                std::from_chars(text.data(), text.data() + text.size(), result));
            return result;
          };
          const long long left = number(parts[2]);
          const long long top = number(parts[3]);
          const long long width = number(parts[4]);
          const long long height = number(parts[5]);
          if (column >= left && column < left + width && row >= top &&
              row < top + height) {
            return detail::output({{"pane_id", parts[0]}, {"window_id", parts[1]}});
          }
        }
        return libtmux::unexpected(ToolError{true, "no pane contains that position"});
      },
      "Resolve coordinates using fixed tmux layout fields.");

  add(make_tool("wait_for_text", "Wait for pane text", Toolset::inspect,
                ProcessReach::none, {Effect::observe},
                {OutputClass::tmux_metadata, OutputClass::terminal_content}, true, true,
                kConservativeAnnotations,
                {field("target", "Pane ID or pane target.", InputSink::tmux_lookup,
                       true, ArgumentType::string, {}, {}, detail::kTargetCharacters),
                 field("text", "Literal text to wait for.", InputSink::none, true,
                       ArgumentType::string, {}, {}, detail::kSearchCharacters),
                 field("timeout_ms", "Bounded wait in milliseconds.", InputSink::none,
                       false, ArgumentType::integer, 1, 60000)},
                OutputShape::wait, detail::wait_for_text,
                "Poll within one deadline and report a match or timeout."));

  add(make_tool(
      "get_tmux_variables", "Get tmux variables", Toolset::inspect, ProcessReach::none,
      {Effect::observe}, {OutputClass::tmux_metadata, OutputClass::configured_command},
      false, true, kConservativeAnnotations,
      {field("names", "One to thirty-two validated tmux variable names.",
             InputSink::tmux_format, true, ArgumentType::string_array, 1, 32, 128U,
             NestedAuthority::controlled, InputControl::validated_variable_name),
       field("paneId", "Optional pane context.", InputSink::tmux_lookup, false,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto& names = arguments.string_arrays.at("names");
        if (names.empty() || !std::ranges::all_of(names, valid_variable)) {
          return libtmux::unexpected(
              ToolError{true, "names must contain valid tmux variable names"});
        }
        std::string format;
        for (const std::string& name : names) {
          if (!format.empty()) {
            format.push_back('\t');
          }
          format += libtmux::variable(name);
        }
        libtmux::expected<std::string, CommandFailure> expanded = server.expand(format);
        if (const std::string pane_id = optional(arguments, "paneId");
            !pane_id.empty()) {
          const auto pane = server.pane(pane_id);
          if (!pane.has_value()) {
            return failure(pane.error());
          }
          expanded = pane->expand(format);
        }
        if (!expanded.has_value()) {
          return failure(expanded.error());
        }
        StructuredValue::Object values;
        std::size_t offset = 0U;
        for (const std::string& name : names) {
          const auto end = expanded->find('\t', offset);
          values.emplace(name, expanded->substr(offset, end == std::string::npos
                                                            ? std::string::npos
                                                            : end - offset));
          offset = end == std::string::npos ? expanded->size() : end + 1U;
        }
        return detail::output({{"values", StructuredValue{std::move(values)}}});
      },
      "Expand only validated variable identifiers, never a caller format."));

  add(make_tool(
      "show_option", "Show a tmux option", Toolset::inspect, ProcessReach::none,
      {Effect::observe}, {OutputClass::tmux_metadata, OutputClass::configured_command},
      false, true, kConservativeAnnotations,
      {field("name", "Exact option name.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, 256U),
       field("target", "Optional session, window, or pane target.",
             InputSink::tmux_lookup, false, ArgumentType::string, {}, {},
             detail::kTargetCharacters)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto options = server.options(optional(arguments, "target"));
        if (!options.has_value()) {
          return failure(options.error());
        }
        const auto found = std::ranges::find(*options, required(arguments, "name"),
                                             &OptionEntry::name);
        return found == options->end()
                   ? ToolResult{libtmux::unexpected(
                         ToolError{true, "tmux option is not set"})}
                   : ToolResult{detail::output({{"option", option_value(*found)}})};
      },
      "Read one exact option without accepting an option value."));

  add(make_tool(
      "show_environment", "Show the tmux environment", Toolset::inspect,
      ProcessReach::none, {Effect::observe}, {OutputClass::process_environment}, true,
      false, kConservativeAnnotations,
      {field("session", "Optional session target.", InputSink::tmux_lookup, false,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("name", "Optional exact environment name.", InputSink::tmux_lookup, false,
             ArgumentType::string, {}, {}, 256U)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        CommandRequest request{"show-environment"};
        if (const std::string session = optional(arguments, "session");
            !session.empty()) {
          request.emplace_back("-t");
          request.emplace_back(session);
        } else {
          request.emplace_back("-g");
        }
        if (const std::string name = optional(arguments, "name"); !name.empty()) {
          request.emplace_back(name);
        }
        const auto reply = server.run(request);
        if (!reply.has_value()) {
          return failure(reply.error());
        }
        StructuredValue::Object values;
        for (const std::string_view line : lines(*reply)) {
          const auto equal = line.find('=');
          if (equal == std::string_view::npos) {
            values.emplace(std::string{line}, StructuredValue{});
          } else {
            values.emplace(std::string{line.substr(0, equal)}, line.substr(equal + 1U));
          }
        }
        return detail::output({{"environment", StructuredValue{std::move(values)}}});
      },
      "Read global or session environment values."));

  add(make_tool(
      "show_hooks", "Show tmux hooks", Toolset::inspect, ProcessReach::none,
      {Effect::observe}, {OutputClass::configured_command}, false, true,
      kConservativeAnnotations,
      {field("target", "Optional tmux target.", InputSink::tmux_lookup, false,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("name", "Optional exact hook name.", InputSink::tmux_lookup, false,
             ArgumentType::string, {}, {}, 256U)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto hooks = server.hooks(optional(arguments, "target"));
        if (!hooks.has_value()) {
          return failure(hooks.error());
        }
        const std::string wanted = optional(arguments, "name");
        StructuredValue::Array rows;
        for (const OptionEntry& hook : *hooks) {
          if (wanted.empty() || hook.name == wanted) {
            rows.push_back(option_value(hook));
          }
        }
        return detail::output({{"hooks", StructuredValue{std::move(rows)}}});
      },
      "Read configured hooks, optionally filtered by exact name."));

  add(make_tool(
      "call_read_tools_batch", "Call read tools as a batch", Toolset::inspect,
      ProcessReach::none, {Effect::observe},
      {OutputClass::tmux_metadata, OutputClass::terminal_content,
       OutputClass::process_environment, OutputClass::configured_command},
      true, true, kConservativeAnnotations,
      {field("operations", "One to sixteen typed inspect-tool operations.",
             InputSink::nested_tool, true, ArgumentType::read_calls, {}, {}, {},
             NestedAuthority::controlled),
       field("onError", "Stop after the first failed operation or continue.",
             InputSink::none, false, ArgumentType::string, {}, {}, {},
             NestedAuthority::none, InputControl::none, {"continue", "stop"})},
      OutputShape::object,
      [](const Server&, const Arguments& arguments,
         const CallContext& context) -> ToolResult {
        if (!context.call_tool || !context.can_call_read_tool) {
          return libtmux::unexpected(
              ToolError{false, "nested tool dispatch is unavailable"});
        }
        if (arguments.read_calls.empty() || arguments.read_calls.size() > 16U) {
          return libtmux::unexpected(ToolError{
              true, "operations must contain between one and sixteen operations"});
        }
        std::string on_error = optional(arguments, "onError");
        if (on_error.empty()) {
          on_error = "stop";
        }
        if (on_error != "stop" && on_error != "continue") {
          return libtmux::unexpected(
              ToolError{true, "onError must be stop or continue"});
        }
        StructuredValue::Array results;
        std::size_t succeeded = 0U;
        std::size_t failed = 0U;
        std::optional<std::size_t> stopped_at;
        for (std::size_t index = 0U; index < arguments.read_calls.size(); ++index) {
          const ReadToolCall& call = arguments.read_calls[index];
          if (!context.can_call_read_tool(call.tool)) {
            ++failed;
            const std::string message = "tool is not batch eligible: " + call.tool;
            results.push_back(
                StructuredValue::Object{{"error", message},
                                        {"index", static_cast<long long>(index)},
                                        {"result", nested_error(message)},
                                        {"resultTruncated", false},
                                        {"success", false},
                                        {"tool", call.tool}});
            if (on_error == "stop") {
              stopped_at = index;
              break;
            }
            continue;
          }
          ToolResult result = context.call_tool(call.tool, Arguments{call.arguments});
          if (!result.has_value()) {
            ++failed;
            results.push_back(StructuredValue::Object{
                {"error", result.error().message},
                {"index", static_cast<long long>(index)},
                {"result", nested_error(result.error().message)},
                {"resultTruncated", false},
                {"success", false},
                {"tool", call.tool}});
            if (on_error == "stop") {
              stopped_at = index;
              break;
            }
            continue;
          }
          ++succeeded;
          results.push_back(
              StructuredValue::Object{{"error", StructuredValue{}},
                                      {"index", static_cast<long long>(index)},
                                      {"result", nested_result(*result)},
                                      {"resultTruncated", false},
                                      {"success", true},
                                      {"tool", call.tool}});
        }
        return detail::output(
            {{"failed", static_cast<long long>(failed)},
             {"onError", on_error},
             {"results", StructuredValue{std::move(results)}},
             {"stoppedAt", stopped_at.has_value()
                               ? StructuredValue{static_cast<long long>(*stopped_at)}
                               : StructuredValue{}},
             {"succeeded", static_cast<long long>(succeeded)},
             {"truncated", false},
             {"truncatedBytes", 0}},
            detail::kReadBatchResponseBytes);
      },
      "Run up to sixteen declared inspect operations serially under this one call; "
      "inner operations receive no separate approval.",
      false, read_batch_tools));
  const auto manage = [&](std::string name, std::string title,
                          std::vector<Field> fields, Handler handler,
                          std::string description) {
    const bool state_only = name == "wait_for_channel" || name == "signal_channel" ||
                            name == "set_mouse_enabled" || name == "set_history_limit";
    add(make_tool(std::move(name), std::move(title), Toolset::manage,
                  ProcessReach::none,
                  state_only ? std::set{Effect::change}
                             : std::set{Effect::observe, Effect::change},
                  {OutputClass::tmux_metadata}, false, false, kConservativeAnnotations,
                  std::move(fields), OutputShape::object, std::move(handler),
                  std::move(description)));
  };

  manage(
      "rename_session", "Rename a tmux session",
      {field("session", "Stable session ID or name.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("name", "Literal replacement session name.", InputSink::tmux_format, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters,
             NestedAuthority::controlled, InputControl::double_hash_once)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto session = server.session(required(arguments, "session"));
        if (!session.has_value()) {
          return failure(session.error());
        }
        const auto answer = session->rename(required(arguments, "name"));
        return answer.has_value() ? changed("session_id", session->id())
                                  : failure(answer.error());
      },
      "Replace one session name.");

  manage(
      "rename_window", "Rename a tmux window",
      {field("windowId", "Stable window ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("name", "Literal replacement window name.", InputSink::tmux_format, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters,
             NestedAuthority::controlled, InputControl::double_hash_once)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto window = server.window(required(arguments, "windowId"));
        if (!window.has_value()) {
          return failure(window.error());
        }
        const auto answer = window->rename(required(arguments, "name"));
        return answer.has_value() ? changed("window_id", window->id())
                                  : failure(answer.error());
      },
      "Replace one window name.");

  manage(
      "select_window", "Select a tmux window",
      {field("windowId", "Stable window ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto window = server.window(required(arguments, "windowId"));
        if (!window.has_value()) {
          return failure(window.error());
        }
        const auto answer = window->select();
        return answer.has_value() ? changed("window_id", window->id())
                                  : failure(answer.error());
      },
      "Make one window active.");

  manage(
      "select_pane", "Select a tmux pane",
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        const auto answer = pane->select();
        return answer.has_value() ? changed("pane_id", pane->id())
                                  : failure(answer.error());
      },
      "Make one pane active.");

  manage(
      "select_layout", "Select a tmux layout",
      {field("windowId", "Stable window ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("layout", "Named or saved tmux layout.", InputSink::tmux_state, true,
             ArgumentType::string, {}, {}, 4096U)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto window = server.window(required(arguments, "windowId"));
        if (!window.has_value()) {
          return failure(window.error());
        }
        const auto answer = window->select_layout(required(arguments, "layout"));
        return answer.has_value() ? changed("window_id", window->id())
                                  : failure(answer.error());
      },
      "Replace the pane layout of one window.");

  manage(
      "resize_window", "Resize a tmux window",
      {field("windowId", "Stable window ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("width", "Positive window width.", InputSink::tmux_state, true,
             ArgumentType::integer, 1, 100000),
       field("height", "Positive window height.", InputSink::tmux_state, true,
             ArgumentType::integer, 1, 100000)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto window = server.window(required(arguments, "windowId"));
        if (!window.has_value()) {
          return failure(window.error());
        }
        const auto answer =
            window->resize(integer(arguments, "width"), integer(arguments, "height"));
        return answer.has_value() ? changed("window_id", window->id())
                                  : failure(answer.error());
      },
      "Replace one window's dimensions.");

  manage(
      "resize_pane", "Resize a tmux pane",
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("width", "Optional positive pane width.", InputSink::tmux_state, false,
             ArgumentType::integer, 1, 100000),
       field("height", "Optional positive pane height.", InputSink::tmux_state, false,
             ArgumentType::integer, 1, 100000)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        if (!arguments.contains("width") && !arguments.contains("height")) {
          return libtmux::unexpected(
              ToolError{true, "resize_pane requires width or height"});
        }
        if (arguments.contains("width")) {
          const auto answer = pane->set_width(integer(arguments, "width"));
          if (!answer.has_value()) {
            return failure(answer.error());
          }
        }
        if (arguments.contains("height")) {
          const auto answer = pane->set_height(integer(arguments, "height"));
          if (!answer.has_value()) {
            return failure(answer.error());
          }
        }
        return changed("pane_id", pane->id());
      },
      "Replace one or both pane dimensions.");

  manage(
      "move_window", "Move a tmux window",
      {field("windowId", "Stable window ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("index", "Non-negative destination index.", InputSink::tmux_state, true,
             ArgumentType::integer, 0, 100000)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto window = server.window(required(arguments, "windowId"));
        if (!window.has_value()) {
          return failure(window.error());
        }
        const auto answer = window->move_to(integer(arguments, "index"));
        return answer.has_value() ? changed("window_id", window->id())
                                  : failure(answer.error());
      },
      "Move a window to an exact index in its owning session.");

  manage(
      "swap_pane", "Swap two tmux panes",
      {field("sourcePaneId", "Stable source pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("targetPaneId", "Stable target pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto source = server.pane(required(arguments, "sourcePaneId"));
        if (!source.has_value()) {
          return failure(source.error());
        }
        const auto target = server.pane(required(arguments, "targetPaneId"));
        if (!target.has_value()) {
          return failure(target.error());
        }
        const auto answer = source->swap_with(*target);
        return answer.has_value() ? changed("pane_id", source->id())
                                  : failure(answer.error());
      },
      "Exchange two pane positions without changing their IDs.");

  manage(
      "set_pane_title", "Set a tmux pane title",
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("title", "Literal replacement pane title.", InputSink::tmux_format, true,
             ArgumentType::string, {}, {}, 4096U, NestedAuthority::controlled,
             InputControl::double_hash_once)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        const auto answer = pane->set_title(required(arguments, "title"));
        return answer.has_value() ? changed("pane_id", pane->id())
                                  : failure(answer.error());
      },
      "Replace one pane title.");

  manage(
      "wait_for_channel", "Wait for a tmux channel",
      {field("channel", "Bounded channel name.", InputSink::tmux_state, true,
             ArgumentType::string, {}, {}, 256U),
       field("timeoutMs", "Optional bounded wait in milliseconds.", InputSink::none,
             false, ArgumentType::integer, 1, 60000)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        std::optional<std::chrono::milliseconds> timeout;
        if (arguments.contains("timeoutMs")) {
          timeout = std::chrono::milliseconds{integer(arguments, "timeoutMs")};
        }
        const auto answer = server.wait_for(required(arguments, "channel"), timeout);
        return answer.has_value()
                   ? detail::output({{"channel", required(arguments, "channel")},
                                     {"signalled", true}})
                   : failure(answer.error());
      },
      "Wait for a named server-side channel with an optional deadline.");

  manage(
      "signal_channel", "Signal a tmux channel",
      {field("channel", "Bounded channel name.", InputSink::tmux_state, true,
             ArgumentType::string, {}, {}, 256U)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto answer = server.signal(required(arguments, "channel"));
        return answer.has_value()
                   ? detail::output({{"channel", required(arguments, "channel")},
                                     {"signalled", true}})
                   : failure(answer.error());
      },
      "Release or latch one named server-side channel.");

  manage(
      "set_mouse_enabled", "Set tmux mouse handling",
      {field("enabled", "Whether mouse handling is enabled.", InputSink::tmux_state,
             true, ArgumentType::boolean)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const bool enabled = boolean(arguments, "enabled");
        const auto answer = server.set_global_option("mouse", enabled ? "on" : "off");
        return answer.has_value() ? detail::output({{"enabled", enabled}})
                                  : failure(answer.error());
      },
      "Replace the global mouse option with a typed boolean.");

  manage(
      "set_history_limit", "Set tmux history limit",
      {field("session", "Stable session ID or name.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("limit", "Retained history line limit.", InputSink::tmux_state, true,
             ArgumentType::integer, 0, 10000000)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto session = server.session(required(arguments, "session"));
        if (!session.has_value()) {
          return failure(session.error());
        }
        const auto answer = session->set_option(
            "history-limit", std::to_string(integer(arguments, "limit")));
        return answer.has_value()
                   ? detail::output({{"limit", integer(arguments, "limit")},
                                     {"session_id", session->id()}})
                   : failure(answer.error());
      },
      "Replace retained-history bounds with a non-negative integer.");
  add(make_tool(
      "create_session", "Create a tmux session", Toolset::execute,
      ProcessReach::configured_process, {Effect::observe, Effect::change},
      {OutputClass::tmux_metadata}, false, false, kConservativeAnnotations,
      {field("height", "Optional detached height.", InputSink::tmux_state, false,
             ArgumentType::integer, 1, 100000),
       field("name", "Optional literal unique session name.", InputSink::tmux_format,
             false, ArgumentType::string, {}, {}, detail::kTargetCharacters,
             NestedAuthority::controlled, InputControl::double_hash_once),
       field("startDirectory", "Literal pane start directory.", InputSink::tmux_format,
             false, ArgumentType::string, {}, {}, 4096U, NestedAuthority::controlled,
             InputControl::double_hash_once),
       field("width", "Optional detached width.", InputSink::tmux_state, false,
             ArgumentType::integer, 1, 100000),
       field("windowName", "Optional literal first window name.",
             InputSink::tmux_format, false, ArgumentType::string, {}, {},
             detail::kTargetCharacters, NestedAuthority::controlled,
             InputControl::double_hash_once)},
      OutputShape::session_id,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        static std::atomic_uint64_t sequence{0U};
        NewSessionOptions options;
        options.name = optional(arguments, "name");
        if (options.name.empty()) {
          options.name = "libtmux-mcp-" + std::to_string(++sequence);
        }
        options.first_window_name = optional(arguments, "windowName");
        options.start_directory = optional(arguments, "startDirectory");
        if (arguments.contains("width")) {
          options.width = static_cast<int>(integer(arguments, "width"));
        }
        if (arguments.contains("height")) {
          options.height = static_cast<int>(integer(arguments, "height"));
        }
        const auto session = server.new_session(std::move(options));
        return session.has_value() ? detail::output({{"name", session->name()},
                                                     {"session_id", session->id()}})
                                   : failure(session.error());
      },
      "Create a detached session; names and path data are literalized before tmux "
      "expansion."));

  add(make_tool(
      "create_window", "Create a tmux window", Toolset::execute,
      ProcessReach::configured_process, {Effect::observe, Effect::change},
      {OutputClass::tmux_metadata}, false, false, kConservativeAnnotations,
      {field("name", "Optional literal window name.", InputSink::tmux_format, false,
             ArgumentType::string, {}, {}, detail::kTargetCharacters,
             NestedAuthority::controlled, InputControl::double_hash_once),
       field("session", "Stable owning session ID or name.", InputSink::tmux_lookup,
             true, ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("startDirectory", "Literal pane start directory.", InputSink::tmux_format,
             false, ArgumentType::string, {}, {}, 4096U, NestedAuthority::controlled,
             InputControl::double_hash_once)},
      OutputShape::window_id,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        static std::atomic_uint64_t sequence{0U};
        const auto session = server.session(required(arguments, "session"));
        if (!session.has_value()) {
          return failure(session.error());
        }
        NewWindowOptions options;
        options.name = optional(arguments, "name");
        if (options.name.empty()) {
          options.name = "libtmux-mcp-" + std::to_string(++sequence);
        }
        options.start_directory = optional(arguments, "startDirectory");
        const auto window = session->new_window(std::move(options));
        return window.has_value()
                   ? detail::output({{"session_id", window->session_id()},
                                     {"window_id", window->id()}})
                   : failure(window.error());
      },
      "Create a detached window; name and path data are literalized before tmux "
      "expansion."));

  add(make_tool(
      "split_window", "Split a tmux window", Toolset::execute,
      ProcessReach::configured_process, {Effect::observe, Effect::change},
      {OutputClass::tmux_metadata}, false, false, kConservativeAnnotations,
      {field("direction", "vertical or horizontal.", InputSink::tmux_state, false,
             ArgumentType::string, {}, {}, 16U),
       field("paneId", "Stable pane to split.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("startDirectory", "Literal pane start directory.", InputSink::tmux_format,
             false, ArgumentType::string, {}, {}, 4096U, NestedAuthority::controlled,
             InputControl::double_hash_once)},
      OutputShape::pane_id,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        const std::string direction = optional(arguments, "direction");
        if (!direction.empty() && direction != "vertical" &&
            direction != "horizontal") {
          return libtmux::unexpected(
              ToolError{true, "direction must be vertical or horizontal"});
        }
        SplitOptions options;
        options.horizontal = direction == "horizontal";
        options.start_directory = optional(arguments, "startDirectory");
        const auto created = pane->split(std::move(options));
        if (!created.has_value()) {
          return failure(created.error());
        }
        return detail::output({{"pane_id", created->id()}});
      },
      "Create a configured-process pane; path data is literalized before tmux "
      "expansion."));

  add(make_tool(
      "respawn_pane", "Respawn a tmux pane", Toolset::execute,
      ProcessReach::configured_process,
      {Effect::observe, Effect::change, Effect::delete_}, {OutputClass::tmux_metadata},
      false, false, kConservativeAnnotations,
      {field("force", "Permit replacing a running pane process.", InputSink::none,
             false, ArgumentType::boolean),
       field("killFirst", "Kill a running pane process before respawn.",
             InputSink::none, false, ArgumentType::boolean),
       field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("startDirectory", "Literal replacement start directory.",
             InputSink::tmux_format, false, ArgumentType::string, {}, {}, 4096U,
             NestedAuthority::controlled, InputControl::double_hash_once)},
      OutputShape::pane_id,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        RespawnOptions options;
        options.replace_running =
            boolean(arguments, "force") || boolean(arguments, "killFirst");
        options.start_directory = optional(arguments, "startDirectory");
        const auto answer = pane->respawn(std::move(options));
        return answer.has_value() ? changed("pane_id", pane->id())
                                  : failure(answer.error());
      },
      "Restart only the pane's configured process; no caller command is accepted."));

  add(make_tool(
      "run_shell_command", "Run a shell command in a tmux pane", Toolset::execute,
      ProcessReach::pane_command, {Effect::observe, Effect::change},
      {OutputClass::tmux_metadata, OutputClass::terminal_content}, true, true,
      kConservativeAnnotations,
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("command", "Shell command sent to the pane.", InputSink::shell_command,
             true, ArgumentType::string, {}, {}, 1024U * 1024U,
             NestedAuthority::unrestricted),
       field("timeoutMs", "Completion timeout in milliseconds.", InputSink::none, false,
             ArgumentType::integer, 1, 60000)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext& context) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        const auto tmux_executable = resolve_tmux_executable();
        if (!tmux_executable.has_value()) {
          return libtmux::unexpected(tmux_executable.error());
        }
        const auto initial = detail::preflight_pane_input(
            server, pane->id(), detail::PaneInputScope::singular_posix_shell);
        if (!initial.has_value()) {
          return libtmux::unexpected(initial.error());
        }
        const auto payload = detail::shell_command_payload(
            required(arguments, "command"), *tmux_executable, initial->endpoint_path,
            initial->foreground_command, shell_command_nonce);
        if (!payload.has_value()) {
          return libtmux::unexpected(payload.error());
        }
        auto reserved = detail::reserve_pane_input(
            *initial, detail::PaneInputReservationKind::run, "run_shell_command");
        if (!reserved.has_value()) {
          return libtmux::unexpected(reserved.error());
        }
        detail::PaneInputLease lease = std::move(*reserved);
        Chain dispatch;
        dispatch.send_text(pane->id(), payload->text);
        if (!dispatch.valid()) {
          return libtmux::unexpected(ToolError{true, dispatch.error()});
        }
        const auto final = detail::preflight_pane_input(
            server, pane->id(), detail::PaneInputScope::singular_posix_shell);
        if (!final.has_value()) {
          return libtmux::unexpected(final.error());
        }
        if (context.cancelled()) {
          return libtmux::unexpected(detail::cancelled());
        }
        if (!detail::same_shell_input_route(*initial, *final) ||
            !lease.covers(final->endpoint_path, final->server_pid,
                          final->server_start_time, final->configured_pane_ids)) {
          return libtmux::unexpected(
              detail::changed_pane_input_route("run_shell_command"));
        }
        const auto sent = server.run_chain(dispatch);
        if (!sent.has_value()) {
          if (sent.error().delivery != DeliveryStatus::not_started) {
            detail::retain_run_until_proven_complete(std::move(lease), server, *pane,
                                                     payload->marker, *initial);
          }
          return failure(sent.error());
        }
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds{integer(arguments, "timeoutMs", 30000)};
        while (std::chrono::steady_clock::now() < deadline) {
          if (context.cancelled()) {
            detail::retain_run_until_proven_complete(std::move(lease), server, *pane,
                                                     payload->marker, *initial);
            return libtmux::unexpected(detail::cancelled());
          }
          CaptureOptions options;
          options.whole_history = true;
          options.join_wrapped = true;
          const auto captured = pane->capture(options);
          if (!captured.has_value()) {
            detail::retain_run_until_proven_complete(std::move(lease), server, *pane,
                                                     payload->marker, *initial);
            return failure(captured.error());
          }
          const auto completed =
              detail::shell_command_completion(*captured, payload->marker);
          if (completed.has_value()) {
            return detail::output(
                {{"exit_code", completed->exit_code},
                 {"pane_id", pane->id()},
                 {"text",
                  captured->substr(completed->text_begin,
                                   completed->record_begin - completed->text_begin)}});
          }
          std::this_thread::sleep_for(20ms);
        }
        detail::retain_run_until_proven_complete(std::move(lease), server, *pane,
                                                 payload->marker, *initial);
        return libtmux::unexpected(ToolError{false, "shell command timed out"});
      },
      "Require one live configured pane, outside human-owned mode and running a "
      "supported POSIX foreground shell, then send one command and return output "
      "after its private completion boundary."));

  add(make_tool(
      "send_keys", "Send keys to a tmux pane", Toolset::execute,
      ProcessReach::pane_input, {Effect::observe, Effect::change},
      {OutputClass::tmux_metadata}, false, true, kConservativeAnnotations,
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("keys", "One tmux key name.", InputSink::pane_input, true,
             ArgumentType::string, {}, {}, 256U, NestedAuthority::unrestricted)},
      OutputShape::pane_targets,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        Chain dispatch;
        dispatch.send_key(pane->id(), required(arguments, "keys"));
        if (!dispatch.valid()) {
          return libtmux::unexpected(ToolError{true, dispatch.error()});
        }
        const auto preflight = detail::preflight_pane_input(
            server, pane->id(), detail::PaneInputScope::effective_cohort);
        if (!preflight.has_value()) {
          return libtmux::unexpected(preflight.error());
        }
        auto reserved = detail::reserve_pane_input(
            *preflight, detail::PaneInputReservationKind::input, "send_keys");
        if (!reserved.has_value()) {
          return libtmux::unexpected(reserved.error());
        }
        detail::PaneInputLease lease = std::move(*reserved);
        const auto final = detail::preflight_pane_input(
            server, pane->id(), detail::PaneInputScope::effective_cohort);
        if (!final.has_value()) {
          return libtmux::unexpected(final.error());
        }
        if (!detail::same_pane_input_route(*preflight, *final) ||
            !lease.covers(final->endpoint_path, final->server_pid,
                          final->server_start_time, final->configured_pane_ids)) {
          return libtmux::unexpected(detail::changed_pane_input_route("send_keys"));
        }
        const auto answer = server.run_chain(dispatch);
        return answer.has_value()
                   ? detail::output(
                         {{"pane_id", pane->id()},
                          {"target_pane_ids", StructuredValue{pane_target_ids(
                                                  preflight->configured_pane_ids)}}})
                   : failure(answer.error());
      },
      "Require every configured synchronized pane to be live and outside "
      "human-owned mode, then send one validated tmux key name."));

  add(make_tool(
      "send_keys_batch", "Send a key sequence to a tmux pane", Toolset::execute,
      ProcessReach::pane_input, {Effect::observe, Effect::change},
      {OutputClass::tmux_metadata}, false, true, kConservativeAnnotations,
      {field("onError", "Stop after the first failed operation or continue.",
             InputSink::none, false, ArgumentType::string, {}, {}, {},
             NestedAuthority::none, InputControl::none, {"continue", "stop"}),
       send_key_operations_field()},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext& context) -> ToolResult {
        if (arguments.send_key_operations.empty() ||
            arguments.send_key_operations.size() > 64U) {
          return libtmux::unexpected(ToolError{
              true, "operations must contain between one and sixty-four operations"});
        }
        std::string on_error = optional(arguments, "onError");
        if (on_error.empty()) {
          on_error = "stop";
        }
        if (on_error != "stop" && on_error != "continue") {
          return libtmux::unexpected(
              ToolError{true, "onError must be stop or continue"});
        }
        StructuredValue::Array failures;
        StructuredValue::Array targets;
        std::size_t completed = 0U;
        for (std::size_t index = 0U; index < arguments.send_key_operations.size();
             ++index) {
          if (context.cancelled()) {
            return libtmux::unexpected(detail::cancelled());
          }
          const FlatArguments& operation = arguments.send_key_operations[index];
          std::string error;
          const auto pane = server.pane(required(operation, "paneId"));
          StructuredValue::Array resolved;
          if (!pane.has_value()) {
            error = pane.error().diagnostic;
          } else {
            Chain dispatch;
            if (boolean(operation, "literal")) {
              dispatch.send_text(pane->id(), required(operation, "keys"));
            } else {
              dispatch.send_key(pane->id(), required(operation, "keys"));
            }
            if (boolean(operation, "enter")) {
              dispatch.send_key(pane->id(), "Enter");
            }
            if (!dispatch.valid()) {
              error = dispatch.error();
            } else {
              const auto preflight = detail::preflight_pane_input(
                  server, pane->id(), detail::PaneInputScope::effective_cohort);
              if (!preflight.has_value()) {
                error = preflight.error().message;
              } else {
                auto reserved = detail::reserve_pane_input(
                    *preflight, detail::PaneInputReservationKind::input,
                    "send_keys_batch");
                if (!reserved.has_value()) {
                  error = reserved.error().message;
                } else {
                  detail::PaneInputLease lease = std::move(*reserved);
                  const auto final = detail::preflight_pane_input(
                      server, pane->id(), detail::PaneInputScope::effective_cohort);
                  if (!final.has_value()) {
                    error = final.error().message;
                  } else if (!detail::same_pane_input_route(*preflight, *final) ||
                             !lease.covers(final->endpoint_path, final->server_pid,
                                           final->server_start_time,
                                           final->configured_pane_ids)) {
                    error = detail::changed_pane_input_route("send_keys_batch").message;
                  } else {
                    const auto sent = server.run_chain(dispatch);
                    if (!sent.has_value()) {
                      error = sent.error().diagnostic;
                    } else {
                      resolved = pane_target_ids(final->configured_pane_ids);
                    }
                  }
                }
              }
            }
          }
          if (!error.empty()) {
            failures.push_back(StructuredValue::Object{
                {"index", static_cast<long long>(index)}, {"reason", error}});
            if (on_error == "stop") {
              break;
            }
            continue;
          }
          ++completed;
          targets.push_back(StructuredValue::Object{
              {"index", static_cast<long long>(index)},
              {"resolvedPaneIds", StructuredValue{std::move(resolved)}}});
        }
        return detail::output({{"completed", static_cast<long long>(completed)},
                               {"failures", StructuredValue{std::move(failures)}},
                               {"targets", StructuredValue{std::move(targets)}}});
      },
      "Preflight each row's configured synchronized pane cohort independently, "
      "requiring every pane to be live and outside human-owned mode before its text "
      "or key and optional Enter."));

  add(make_tool(
      "paste_text", "Paste text into a tmux pane", Toolset::execute,
      ProcessReach::pane_input, {Effect::observe, Effect::change},
      {OutputClass::tmux_metadata}, false, true, kConservativeAnnotations,
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("text", "Literal text to paste.", InputSink::pane_input, true,
             ArgumentType::string, {}, {}, 1024U * 1024U, NestedAuthority::unrestricted,
             InputControl::none, {}, true),
       field("enter", "Append Enter to the same private paste buffer.",
             InputSink::pane_input, false, ArgumentType::boolean, {}, {}, {},
             NestedAuthority::unrestricted)},
      OutputShape::pane_id,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        const auto initial = detail::preflight_pane_input(
            server, pane->id(), detail::PaneInputScope::target_only);
        if (!initial.has_value()) {
          return libtmux::unexpected(initial.error());
        }
        auto reserved = detail::reserve_pane_input(
            *initial, detail::PaneInputReservationKind::input, "paste_text");
        if (!reserved.has_value()) {
          return libtmux::unexpected(reserved.error());
        }
        detail::PaneInputLease lease = std::move(*reserved);
        std::string payload = required(arguments, "text");
        if (boolean(arguments, "enter")) {
          payload.push_back('\n');
        }
        if (payload.empty()) {
          return detail::output(
              {{"pane_id", pane->id()}, {"changed", StructuredValue{false}}});
        }
        const auto name = private_paste_buffer_name(server);
        if (!name.has_value()) {
          return libtmux::unexpected(name.error());
        }
        const auto fail_after_cleanup = [&](ToolError primary) -> ToolResult {
          const auto cleanup = remove_private_paste_buffer(server, *name);
          return libtmux::unexpected(
              cleanup.has_value()
                  ? with_paste_cleanup_error(std::move(primary), *cleanup)
                  : std::move(primary));
        };
        const auto staged = server.set_buffer(*name, payload);
        if (!staged.has_value()) {
          return fail_after_cleanup(detail::tmux_error(staged.error()));
        }
        const auto buffers = server.buffers();
        if (!buffers.has_value()) {
          return fail_after_cleanup(detail::tmux_error(buffers.error()));
        }
        const auto buffer = std::ranges::find(*buffers, *name, &Buffer::name);
        if (buffer == buffers->end()) {
          return fail_after_cleanup(
              ToolError{false, "temporary paste buffer disappeared"});
        }
        const auto final = detail::preflight_pane_input(
            server, pane->id(), detail::PaneInputScope::target_only);
        if (!final.has_value()) {
          return fail_after_cleanup(final.error());
        }
        if (!detail::same_pane_input_route(*initial, *final) ||
            !lease.covers(final->endpoint_path, final->server_pid,
                          final->server_start_time, final->configured_pane_ids)) {
          return fail_after_cleanup(detail::changed_pane_input_route("paste_text"));
        }
        const auto answer = pane->paste(*buffer, true);
        if (!answer.has_value()) {
          return fail_after_cleanup(detail::tmux_error(answer.error()));
        }
        const auto cleanup = remove_private_paste_buffer(server, *name);
        if (cleanup.has_value()) {
          return libtmux::unexpected(
              ToolError{false, "paste completed; " + cleanup->message});
        }
        return changed("pane_id", pane->id());
      },
      "Require the target pane to be live and outside human-owned mode, then stage "
      "a private target-only buffer, optionally append Enter, paste it once, and "
      "verify cleanup. Empty text without Enter is a guarded buffer-free no-op."));

  add(make_tool(
      "set_synchronize_panes", "Set synchronized pane input", Toolset::execute,
      ProcessReach::none, {Effect::change}, {OutputClass::tmux_metadata}, false, false,
      kConservativeAnnotations,
      {field("windowId", "Stable window ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters),
       field("enabled", "Whether later pane input is duplicated.",
             InputSink::tmux_state, true, ArgumentType::boolean)},
      OutputShape::object,
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto window = server.window(required(arguments, "windowId"));
        if (!window.has_value()) {
          return failure(window.error());
        }
        const bool enabled = boolean(arguments, "enabled");
        const auto answer =
            window->set_option("synchronize-panes", enabled ? "on" : "off");
        return answer.has_value()
                   ? detail::output({{"enabled", enabled}, {"window_id", window->id()}})
                   : failure(answer.error());
      },
      "Set the inherited window synchronize-panes default; pane-level overrides "
      "determine effective synchronized input membership.",
      true));
  const auto teardown = [&](std::string name, std::string title,
                            std::vector<Field> fields, Handler handler,
                            std::string description) {
    const bool observes = name != "clear_pane_scrollback";
    add(make_tool(std::move(name), std::move(title), Toolset::teardown,
                  ProcessReach::none,
                  observes ? std::set{Effect::observe, Effect::delete_}
                           : std::set{Effect::delete_},
                  {OutputClass::tmux_metadata}, false, false, kConservativeAnnotations,
                  std::move(fields), OutputShape::object, std::move(handler),
                  std::move(description)));
  };

  teardown(
      "clear_pane_scrollback", "Clear tmux pane scrollback",
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        const auto answer = pane->clear_history();
        return answer.has_value() ? changed("pane_id", pane->id())
                                  : failure(answer.error());
      },
      "Irreversibly discard retained scrollback for one pane.");

  teardown(
      "kill_pane", "Kill a tmux pane",
      {field("paneId", "Stable pane ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto pane = server.pane(required(arguments, "paneId"));
        if (!pane.has_value()) {
          return failure(pane.error());
        }
        const std::string id{pane->id()};
        const auto answer = pane->kill();
        return answer.has_value() ? detail::output({{"pane_id", id}, {"deleted", true}})
                                  : failure(answer.error());
      },
      "Delete one pane and its running process.");

  teardown(
      "kill_window", "Kill a tmux window",
      {field("windowId", "Stable window ID.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto window = server.window(required(arguments, "windowId"));
        if (!window.has_value()) {
          return failure(window.error());
        }
        const std::string id{window->id()};
        const auto answer = window->kill();
        return answer.has_value()
                   ? detail::output({{"window_id", id}, {"deleted", true}})
                   : failure(answer.error());
      },
      "Delete one window and every pane it owns.");

  teardown(
      "kill_session", "Kill a tmux session",
      {field("session", "Stable session ID or name.", InputSink::tmux_lookup, true,
             ArgumentType::string, {}, {}, detail::kTargetCharacters)},
      [](const Server& server, const Arguments& arguments,
         const CallContext&) -> ToolResult {
        const auto session = server.session(required(arguments, "session"));
        if (!session.has_value()) {
          return failure(session.error());
        }
        const std::string id{session->id()};
        const auto answer = session->kill();
        return answer.has_value()
                   ? detail::output({{"session_id", id}, {"deleted", true}})
                   : failure(answer.error());
      },
      "Delete one session and every window it owns.");
  return tools;
}

} // namespace

libtmux::expected<ToolRegistry, std::string> default_tools() {
  return default_tools(ToolSelection::all());
}

libtmux::expected<ToolRegistry, std::string>
default_tools(const ToolSelection& selection) {
  return ToolRegistry::create(definitions(), selection);
}

} // namespace libtmux::mcp
