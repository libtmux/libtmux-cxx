#include "pane_echo.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <tuple>
#include <utility>

namespace libtmux::mcp::detail {
namespace {

inline constexpr auto kRecentTtl = std::chrono::seconds{10};

inline constexpr std::size_t kRecentCap = 4U;
inline constexpr std::size_t kTextCap = 1024U * 1024U;
inline constexpr std::size_t kPaneCap = 256U;
inline constexpr auto kRecordTtl = std::chrono::minutes{2};

struct RecentEcho {
  std::chrono::steady_clock::time_point at;
  std::string text;
};

struct PaneEchoState {
  PaneServerIdentity identity;
  std::size_t in_flight{};
  std::chrono::steady_clock::time_point touched{};
  bool over_limit{};
  std::string pending;
  bool pending_captured{false};
  std::deque<RecentEcho> recent;
};

struct Ledger {
  std::mutex mutex;
  using Key = std::tuple<std::string, std::uint64_t, std::uint64_t, std::string>;
  std::map<Key, PaneEchoState> by_pane;
};

[[nodiscard]] Ledger::Key echo_key(const PaneServerIdentity& identity,
                                   std::string_view pane_id) {
  return {identity.socket_path, identity.server_pid, identity.server_start_time,
          std::string{pane_id}};
}

[[nodiscard]] Ledger& ledger() {
  static Ledger* const process_ledger = new Ledger;
  return *process_ledger;
}

void prune_locked(Ledger& owner, std::chrono::steady_clock::time_point at) {
  std::erase_if(owner.by_pane, [&](const auto& entry) {
    return entry.second.in_flight == 0U && at - entry.second.touched > kRecordTtl;
  });
  while (owner.by_pane.size() > kPaneCap) {
    auto oldest = owner.by_pane.end();
    for (auto entry = owner.by_pane.begin(); entry != owner.by_pane.end(); ++entry) {
      if (entry->second.in_flight == 0U &&
          (oldest == owner.by_pane.end() ||
           entry->second.touched < oldest->second.touched)) {
        oldest = entry;
      }
    }
    if (oldest == owner.by_pane.end()) {
      break;
    }
    owner.by_pane.erase(oldest);
  }
}

void append_text(PaneEchoState& state, std::string_view text) {
  if (state.over_limit) {
    return;
  }
  if (text.size() > kTextCap - state.pending.size()) {
    state.pending.clear();
    state.over_limit = true;
    return;
  }
  state.pending.append(text);
}

void prune_recent_locked(PaneEchoState& state,
                         std::chrono::steady_clock::time_point now) {
  while (!state.recent.empty() && now - state.recent.front().at > kRecentTtl) {
    state.recent.pop_front();
  }
}

void push_recent_locked(PaneEchoState& state, std::string text,
                        std::chrono::steady_clock::time_point now) {
  if (text.empty()) {
    return;
  }
  prune_recent_locked(state, now);
  state.recent.push_back(RecentEcho{.at = now, .text = std::move(text)});
  while (state.recent.size() > kRecentCap) {
    state.recent.pop_front();
  }
}

void end_line_locked(PaneEchoState& state, std::chrono::steady_clock::time_point now) {
  push_recent_locked(state, state.pending, now);
  state.pending.clear();
  state.pending_captured = false;
  state.over_limit = false;
}

void drop_last_code_point(std::string& text) {
  if (text.empty()) {
    return;
  }
  std::size_t cut = text.size() - 1U;
  while (cut > 0U && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) {
    --cut;
  }
  text.erase(cut);
}

enum class PaneKeyEffectKind { erase, kill, noop, submit, text, unknown };

struct PaneKeyEffect {
  PaneKeyEffectKind kind;
  std::string text;
};

[[nodiscard]] PaneKeyEffect classify_key(std::string_view key) {
  static constexpr std::array kSubmit{
      std::string_view{"C-m"}, std::string_view{"Enter"}, std::string_view{"KPEnter"}};
  static constexpr std::array kKill{std::string_view{"C-c"}, std::string_view{"C-u"}};
  static constexpr std::array kErase{std::string_view{"BSpace"},
                                     std::string_view{"C-h"}};
  static constexpr std::array kNoop{std::string_view{"DC"}, std::string_view{"Delete"}};

  if (std::ranges::find(kSubmit, key) != kSubmit.end()) {
    return {.kind = PaneKeyEffectKind::submit, .text = {}};
  }
  if (std::ranges::find(kKill, key) != kKill.end()) {
    return {.kind = PaneKeyEffectKind::kill, .text = {}};
  }
  if (std::ranges::find(kErase, key) != kErase.end()) {
    return {.kind = PaneKeyEffectKind::erase, .text = {}};
  }
  if (std::ranges::find(kNoop, key) != kNoop.end()) {
    return {.kind = PaneKeyEffectKind::noop, .text = {}};
  }
  if (key == "Space") {
    return {.kind = PaneKeyEffectKind::text, .text = " "};
  }
  if (key.size() == 1U) {
    return {.kind = PaneKeyEffectKind::text, .text = std::string{key}};
  }
  return {.kind = PaneKeyEffectKind::unknown, .text = {}};
}

void apply_effect_locked(PaneEchoState& state, const PaneKeyEffect& effect,
                         std::chrono::steady_clock::time_point now) {
  switch (effect.kind) {
  case PaneKeyEffectKind::submit:
  case PaneKeyEffectKind::kill:
    end_line_locked(state, now);
    return;
  case PaneKeyEffectKind::erase:
    // Shell redraws can leave the pre-edit line in buffered output.
    if (!state.pending_captured) {
      push_recent_locked(state, state.pending, now);
      state.pending_captured = true;
    }
    drop_last_code_point(state.pending);
    return;
  case PaneKeyEffectKind::noop:
    return;
  case PaneKeyEffectKind::unknown:
    // Stop masking a line whose cursor movement cannot be modelled.
    state.pending.clear();
    state.pending_captured = false;
    return;
  case PaneKeyEffectKind::text:
    append_text(state, effect.text);
    state.pending_captured = false;
    return;
  }
}

[[nodiscard]] PaneEchoState& state_for_locked(Ledger& owner, std::string_view pane_id,
                                              const PaneServerIdentity& identity) {
  auto [found, inserted] = owner.by_pane.try_emplace(echo_key(identity, pane_id));
  if (!inserted && found->second.identity == identity) {
    return found->second;
  }
  found->second = PaneEchoState{
      .identity = identity, .pending = {}, .pending_captured = false, .recent = {}};
  return found->second;
}

} // namespace

class PaneEchoEdit::Impl {
public:
  Impl(Ledger::Key key, PaneEchoState previous)
      : key_{std::move(key)}, previous_{std::move(previous)} {}

  void commit() noexcept {
    if (committed_) {
      return;
    }
    Ledger& owner = ledger();
    std::lock_guard lock{owner.mutex};
    const auto found = owner.by_pane.find(key_);
    if (found != owner.by_pane.end() && found->second.in_flight != 0U) {
      --found->second.in_flight;
    }
    prune_locked(owner, std::chrono::steady_clock::now());
    committed_ = true;
  }

  ~Impl() {
    if (committed_) {
      return;
    }
    Ledger& owner = ledger();
    std::lock_guard lock{owner.mutex};
    const auto found = owner.by_pane.find(key_);
    if (found != owner.by_pane.end()) {
      found->second = std::move(previous_);
    }
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

private:
  Ledger::Key key_;
  PaneEchoState previous_;
  bool committed_{false};
};

PaneEchoEdit::PaneEchoEdit(std::unique_ptr<Impl> implementation)
    : implementation_{std::move(implementation)} {}

PaneEchoEdit::PaneEchoEdit(PaneEchoEdit&&) noexcept = default;

PaneEchoEdit& PaneEchoEdit::operator=(PaneEchoEdit&& other) noexcept {
  if (this != &other) {
    implementation_ = std::move(other.implementation_);
  }
  return *this;
}

PaneEchoEdit::~PaneEchoEdit() = default;

void PaneEchoEdit::commit() noexcept {
  if (implementation_ != nullptr) {
    implementation_->commit();
  }
}

PaneEchoEdit note_key_dispatch(const PaneServerIdentity& identity,
                               std::string_view pane_id, std::string_view key,
                               bool enter) {
  Ledger& owner = ledger();
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock{owner.mutex};
  prune_locked(owner, now);
  PaneEchoState& state = state_for_locked(owner, pane_id, identity);
  PaneEchoState previous = state;
  ++state.in_flight;
  state.touched = now;
  apply_effect_locked(state, classify_key(key), now);
  if (enter) {
    end_line_locked(state, now);
  }
  return PaneEchoEdit{std::make_unique<PaneEchoEdit::Impl>(echo_key(identity, pane_id),
                                                           std::move(previous))};
}

PaneEchoEdit note_literal_write(const PaneServerIdentity& identity,
                                std::string_view pane_id, std::string_view text,
                                bool enter) {
  std::string whole{text};
  if (enter) {
    whole.push_back('\n');
  }
  Ledger& owner = ledger();
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock{owner.mutex};
  prune_locked(owner, now);
  PaneEchoState& state = state_for_locked(owner, pane_id, identity);
  PaneEchoState previous = state;
  ++state.in_flight;
  state.touched = now;
  state.pending_captured = false;
  std::string_view remaining = whole;
  for (;;) {
    const auto end = remaining.find_first_of("\r\n");
    append_text(state, remaining.substr(0, end));
    if (end == std::string_view::npos) {
      break;
    }
    end_line_locked(state, now);
    remaining.remove_prefix(end + 1U);
  }
  return PaneEchoEdit{std::make_unique<PaneEchoEdit::Impl>(echo_key(identity, pane_id),
                                                           std::move(previous))};
}

LiveEcho live_echo(const PaneServerIdentity& identity, std::string_view pane_id,
                   std::chrono::steady_clock::time_point at) {
  Ledger& owner = ledger();
  std::lock_guard lock{owner.mutex};
  prune_locked(owner, at);
  const auto found = owner.by_pane.find(echo_key(identity, pane_id));
  if (found == owner.by_pane.end() || !(found->second.identity == identity)) {
    return {};
  }
  PaneEchoState& state = found->second;
  prune_recent_locked(state, at);
  LiveEcho result;
  result.input_pending = state.in_flight != 0U || !state.pending.empty();
  result.pending = state.pending;
  result.recent.reserve(state.recent.size());
  for (const RecentEcho& entry : state.recent) {
    result.recent.push_back(entry.text);
  }
  return result;
}

void prune_dead_panes(const PaneServerIdentity& identity,
                      const std::set<std::string>& live_pane_ids) {
  Ledger& owner = ledger();
  std::lock_guard lock{owner.mutex};
  std::erase_if(owner.by_pane, [&](const auto& entry) {
    return std::get<0>(entry.first) == identity.socket_path &&
           (!(entry.second.identity == identity) ||
            !live_pane_ids.contains(std::get<3>(entry.first)));
  });
}

} // namespace libtmux::mcp::detail
