#include "support_capabilities.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

LIBTMUX_NAMESPACE_BEGIN
namespace support::capabilities {

namespace {

constexpr std::string_view kSchema{"libtmux-capabilities/1"};
constexpr std::string_view kPort{"cxx"};
// Control mode is acquired through `Connection::connect`, which reports a
// `ProtocolError`; the rest refuse with the `CommandFailure` a command carries.
constexpr std::string_view kProtocolError{"libtmux::ProtocolError"};
constexpr std::string_view kCommandFailure{"libtmux::CommandFailure"};

constexpr Release kTmux34{Version{.major = 3, .minor = 4}, "3.4"};
constexpr Release kTmux35{Version{.major = 3, .minor = 5}, "3.5"};
constexpr Release kTmux37{Version{.major = 3, .minor = 7}, "3.7"};
constexpr Release kTmux38Rc2{Version{.major = 3, .minor = 8, .candidate = 2},
                             "3.8-rc2"};

constexpr std::array<std::string_view, 4> kControlApis{
    "libtmux::Connection::connect", "libtmux::Server::control",
    "libtmux::Server::control_with_options", "libtmux::Server::over_control"};
constexpr std::array<CommandUse, 1> kControlCommands{CommandUse{"attach-session", {}}};

constexpr std::array<std::string_view, 1> kBufferApis{"libtmux::Buffer::remove"};
constexpr std::array<std::string_view, 1> kDeleteBufferFlags{"-b"};
constexpr std::array<std::string_view, 2> kRenameBufferFlags{"-b", "-n"};
constexpr std::array<CommandUse, 2> kBufferCommands{
    CommandUse{"delete-buffer", kDeleteBufferFlags},
    CommandUse{"set-buffer", kRenameBufferFlags}};

constexpr std::array<std::string_view, 2> kLayoutApis{
    "libtmux::Server::validate_layouts", "libtmux::Window::select_layout"};
constexpr std::array<CommandUse, 1> kLayoutCommands{CommandUse{"select-layout", {}}};

constexpr std::array<std::string_view, 1> kClipboardFlags{"-l"};
constexpr std::array<CommandUse, 1> kClipboardCommands{
    CommandUse{"refresh-client", kClipboardFlags}};

constexpr std::array kCapabilities{
    Capability{
        .key = CapabilityId::control_mode,
        .id = "control-mode",
        .name = "Control mode",
        .summary = "Attach a library-managed control client and stream events.",
        .since = kTmux37,
        .handling = Handling::refuse,
        .error = kProtocolError,
        .apis = kControlApis,
        .commands = kControlCommands,
    },
    Capability{
        .key = CapabilityId::buffer_delete_named,
        .id = "buffer-delete-named",
        .name = "Named buffer deletion",
        .summary = "Delete or rename one paste buffer by name without risking "
                   "another buffer.",
        .since = kTmux34,
        .handling = Handling::refuse,
        .error = kCommandFailure,
        .apis = kBufferApis,
        .commands = kBufferCommands,
    },
    Capability{
        .key = CapabilityId::layout_mirrored_main,
        .id = "layout-mirrored-main",
        .name = "Mirrored main layouts",
        .summary = "Apply main-horizontal-mirrored and main-vertical-mirrored.",
        .since = kTmux35,
        .handling = Handling::refuse,
        .error = kCommandFailure,
        .apis = kLayoutApis,
        .commands = kLayoutCommands,
    },
    Capability{
        .key = CapabilityId::layout_json,
        .id = "layout-json",
        .name = "JSON layout strings",
        .summary = "Apply a layout saved in the JSON form tmux reports.",
        .since = kTmux38Rc2,
        .handling = Handling::refuse,
        .error = kCommandFailure,
        .apis = kLayoutApis,
        .commands = kLayoutCommands,
    },
    Capability{
        .key = CapabilityId::clipboard_query,
        .id = "clipboard-query",
        .name = "Clipboard query",
        .summary = "Ask an attached client for its clipboard with refresh-client -l.",
        .since = kTmux34,
        .handling = Handling::refuse,
        .error = kCommandFailure,
        .apis = {},
        .commands = kClipboardCommands,
    },
};

constexpr std::array<Release, 1> kOnlyTmux37{kTmux37};

constexpr std::array<std::string_view, 1> kBreakApis{"libtmux::Pane::break_out"};
constexpr std::array<CommandUse, 1> kBreakCommands{CommandUse{"break-pane", {}}};

constexpr std::array<std::string_view, 4> kNameApis{
    "libtmux::Server::new_session", "libtmux::Session::new_window",
    "libtmux::Session::rename", "libtmux::Window::rename"};
constexpr std::array<std::string_view, 2> kNameSessionFlags{"-s", "-n"};
constexpr std::array<std::string_view, 1> kNameWindowFlags{"-n"};
constexpr std::array<CommandUse, 4> kNameCommands{
    CommandUse{"new-session", kNameSessionFlags},
    CommandUse{"new-window", kNameWindowFlags},
    CommandUse{"rename-session", {}},
    CommandUse{"rename-window", {}},
};

constexpr std::array kWorkarounds{
    Workaround{
        .key = WorkaroundId::break_pane_misnames_windows,
        .id = "break-pane-misnames-windows",
        .name = "break-pane window naming",
        .summary = "break-pane gets the new window wrong on exactly tmux 3.7; "
                   "Pane::break_out names the window and repairs it.",
        .releases = kOnlyTmux37,
        .handling = Handling::repair,
        .apis = kBreakApis,
        .commands = kBreakCommands,
    },
    Workaround{
        .key = WorkaroundId::name_separators_rejected,
        .id = "name-separators-rejected",
        .name = "Colon and dot in names",
        .summary = "tmux 3.7 rejects ':' and '.' in session and window names "
                   "after partly running.",
        .releases = kOnlyTmux37,
        .handling = Handling::refuse,
        .apis = kNameApis,
        .commands = kNameCommands,
    },
};

std::string_view handling_text(Handling handling) noexcept {
  switch (handling) {
  case Handling::refuse:
    return "refuse";
  case Handling::fallback:
    return "fallback";
  case Handling::repair:
    return "repair";
  }
  return "refuse";
}

// The manifest is written the way serde_json prints a pretty document, with
// keys in sorted order, so every port's file diffs the same way.
class Writer {
public:
  std::string finish() {
    out_.push_back('\n');
    return std::move(out_);
  }

  void open(char bracket) {
    out_.push_back(bracket);
    ++depth_;
    first_ = true;
  }

  void close(char bracket) {
    --depth_;
    if (!first_) {
      newline();
    }
    out_.push_back(bracket);
    first_ = false;
  }

  void key(std::string_view name) {
    element();
    string(name);
    out_ += ": ";
    first_ = true;
  }

  void item() { element(); }

  void string(std::string_view text) {
    out_.push_back('"');
    for (const char c : text) {
      switch (c) {
      case '"':
        out_ += "\\\"";
        break;
      case '\\':
        out_ += "\\\\";
        break;
      case '\n':
        out_ += "\\n";
        break;
      default:
        out_.push_back(c);
      }
    }
    out_.push_back('"');
    first_ = false;
  }

private:
  void newline() {
    out_.push_back('\n');
    out_.append(depth_ * 2U, ' ');
  }

  void element() {
    if (!first_) {
      out_.push_back(',');
    }
    newline();
    first_ = false;
  }

  std::string out_;
  std::size_t depth_{0};
  bool first_{true};
};

// `key: value` pairs follow a `key` call, which leaves `first_` set so the
// value is not preceded by a comma.
void names(Writer& w, std::span<const std::string_view> list) {
  w.open('[');
  for (const std::string_view name : list) {
    w.item();
    w.string(name);
  }
  w.close(']');
}

void commands(Writer& w, std::span<const CommandUse> list) {
  w.open('[');
  for (const CommandUse& use : list) {
    w.item();
    w.open('{');
    w.key("command");
    w.string(use.command);
    if (!use.flags.empty()) {
      w.key("flags");
      names(w, use.flags);
    }
    w.close('}');
  }
  w.close(']');
}

} // namespace

std::span<const Capability> all_capabilities() noexcept { return kCapabilities; }

std::span<const Workaround> all_workarounds() noexcept { return kWorkarounds; }

const Capability& capability(CapabilityId id) noexcept {
  return *std::ranges::find(kCapabilities, id, &Capability::key);
}

const Workaround& workaround(WorkaroundId id) noexcept {
  return *std::ranges::find(kWorkarounds, id, &Workaround::key);
}

bool affects(WorkaroundId id, const Version& running) noexcept {
  const auto& releases = workaround(id).releases;
  return std::ranges::any_of(
      releases, [&](const Release& release) { return release.version == running; });
}

std::string manifest_json() {
  Writer w;
  w.open('{');
  w.key("capabilities");
  w.open('[');
  for (const Capability& c : kCapabilities) {
    w.item();
    w.open('{');
    w.key("apis");
    names(w, c.apis);
    w.key("commands");
    commands(w, c.commands);
    if (!c.error.empty()) {
      w.key("error");
      w.string(c.error);
    }
    w.key("handling");
    w.string(handling_text(c.handling));
    w.key("id");
    w.string(c.id);
    w.key("name");
    w.string(c.name);
    w.key("since");
    w.string(c.since.text);
    w.key("summary");
    w.string(c.summary);
    w.close('}');
  }
  w.close(']');
  w.key("floor");
  w.open('{');
  w.key("minimum");
  w.string(kMinimumSupportedText);
  w.close('}');
  w.key("port");
  w.string(kPort);
  w.key("schema");
  w.string(kSchema);
  w.key("workarounds");
  w.open('[');
  for (const Workaround& x : kWorkarounds) {
    w.item();
    w.open('{');
    w.key("apis");
    names(w, x.apis);
    w.key("commands");
    commands(w, x.commands);
    w.key("handling");
    w.string(handling_text(x.handling));
    w.key("id");
    w.string(x.id);
    w.key("name");
    w.string(x.name);
    w.key("releases");
    w.open('[');
    for (const Release& release : x.releases) {
      w.item();
      w.string(release.text);
    }
    w.close(']');
    w.key("summary");
    w.string(x.summary);
    w.close('}');
  }
  w.close(']');
  w.close('}');
  return w.finish();
}

} // namespace support::capabilities
LIBTMUX_NAMESPACE_END
