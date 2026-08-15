// The `field__lookup=value` spelling a caller brings across from Python.
//
// It is an edge, and the point of an edge is that nothing behind it knows a
// string was involved: what it produces has to be the same `FilterExpr` the
// typed fields produce, and behave identically when a range is filtered by
// it. Every case here therefore checks the parse against the typed spelling
// rather than against itself.

#include "libtmux/legacy_lookup.hpp"

#include <array>
#include <initializer_list>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/cardinality.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/filter_expr.hpp"
#include "libtmux/snapshot.hpp"

namespace {

using libtmux::LookupParseError;
using libtmux::parse_lookup;
using libtmux::StringFieldHandle;
using libtmux::Window;
namespace window = libtmux::window;

// The fields a caller is allowed to name, keyed by the tmux token — which is
// also what Python calls the attribute, so a recorded `window_name__contains`
// crosses unchanged. Supplied by the caller rather than discovered, which is
// what keeps this an edge: a string cannot reach a field nobody offered.
constexpr std::array kWindowFields{window::id, window::name};

std::vector<Window> two_windows() {
  const auto recording = [](std::initializer_list<std::string_view> values) {
    EXPECT_EQ(values.size(), Window::kFields.size());
    std::string line;
    for (const std::string_view value : values) {
      line += value;
      line += libtmux::kFormatSeparator;
    }
    return line + "\n";
  };
  static const std::string output = recording({"@0", "Editor", "1", "$0", "0", "1",
                                               "80", "24", "", "0", "0", "0", "1"}) +
                                    recording({"@1", "logs-tail", "0", "$0", "1", "1",
                                               "80", "24", "", "0", "0", "0", "1"});
  static const auto recorded =
      libtmux::Snapshot::from_recording(Window::kFields, output);
  EXPECT_NE(recorded, nullptr);
  return {Window{recorded, 0}, Window{recorded, 1}};
}

// Which windows a filter keeps, by name, so two filters can be compared by
// what they do rather than by how they were spelled.
std::vector<std::string> kept(const std::vector<Window>& windows,
                              const libtmux::FilterExpr<Window>& filter) {
  std::vector<std::string> names;
  for (const Window& window : windows | libtmux::matching(filter)) {
    names.emplace_back(window.name());
  }
  return names;
}

TEST(LegacyLookup, EveryLookupParsesToWhatTheTypedFieldWouldBuild) {
  const auto windows = two_windows();
  const std::pair<std::string, libtmux::FilterExpr<Window>> cases[]{
      {"window_name=Editor", window::name == "Editor"},
      {"window_name__eq=Editor", window::name == "Editor"},
      {"window_name__exact=Editor", window::name == "Editor"},
      {"window_name__iexact=editor", window::name.iequals("editor")},
      {"window_name__contains=dit", window::name.contains("dit")},
      {"window_name__startswith=logs", window::name.starts_with("logs")},
      {"window_name__endswith=tail", window::name.ends_with("tail")},
  };

  for (const auto& [term, typed] : cases) {
    SCOPED_TRACE(term);
    const auto parsed = parse_lookup<Window>(term, kWindowFields);
    ASSERT_TRUE(parsed.has_value());
    // Compared by what they keep: two filters that select the same windows
    // are the same filter as far as anything downstream is concerned.
    EXPECT_EQ(kept(windows, *parsed), kept(windows, typed));
    EXPECT_FALSE(kept(windows, typed).empty()) << "the case proves nothing";
  }
}

TEST(LegacyLookup, ABareFieldIsEqualityJustAsPythonReadsIt) {
  const auto windows = two_windows();

  const auto parsed = parse_lookup<Window>("window_name=Editor", kWindowFields);
  ASSERT_TRUE(parsed.has_value());

  // Only the one, so the implicit lookup is equality and not something
  // looser that happens to match.
  EXPECT_EQ(kept(windows, *parsed), (std::vector<std::string>{"Editor"}));
}

TEST(LegacyLookup, OnlyTheKeyIsSplitSoAValueSurvivesWhole) {
  const auto windows = two_windows();

  // A value carrying the two characters the key is split on. Splitting the
  // whole term rather than the key would lose part of it, and the filter
  // would quietly match something else.
  const auto parsed =
      parse_lookup<Window>("window_name__contains=a=b__c", kWindowFields);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(kept(windows, *parsed), kept(windows, window::name.contains("a=b__c")));
  EXPECT_TRUE(kept(windows, *parsed).empty());

  // And the same value under equality, so the check above is about the value
  // rather than about `contains` finding nothing either way.
  const auto exact = parse_lookup<Window>("window_name=a=b", kWindowFields);
  ASSERT_TRUE(exact.has_value());
  EXPECT_EQ(kept(windows, *exact), kept(windows, window::name == "a=b"));
}

TEST(LegacyLookup, AnEmptyValueIsAValueRatherThanAMissingOne) {
  const auto windows = two_windows();

  const auto parsed = parse_lookup<Window>("window_name=", kWindowFields);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(kept(windows, *parsed), kept(windows, window::name == ""));
}

TEST(LegacyLookup, EachWayOfBeingWrongIsSaidSeparately) {
  // No `=` at all: nothing was asked for.
  const auto valueless = parse_lookup<Window>("name__contains", kWindowFields);
  ASSERT_FALSE(valueless.has_value());
  EXPECT_EQ(valueless.error(), LookupParseError::missing_value);

  // A field the caller did not offer. Refused rather than matched against
  // whatever the entity happens to carry.
  const auto absent = parse_lookup<Window>("nosuchfield=x", kWindowFields);
  ASSERT_FALSE(absent.has_value());
  EXPECT_EQ(absent.error(), LookupParseError::unknown_field);

  // A lookup that is not one. Python's `regex` and `gt` are the tempting
  // ones, and neither is offered here.
  for (const auto* term : {"window_name__regex=x", "window_name__gt=x", "name__=x"}) {
    SCOPED_TRACE(term);
    const auto parsed = parse_lookup<Window>(term, kWindowFields);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error(), LookupParseError::unknown_lookup);
  }
}

TEST(LegacyLookup, TheLastSeparatorSplitsSoAFieldMayCarryOne) {
  // `rfind` rather than `find`: a field whose own name holds the separator
  // would otherwise be read as a lookup nobody offered.
  const auto parsed = parse_lookup<Window>("odd__field__contains=x", kWindowFields);
  ASSERT_FALSE(parsed.has_value());
  // The lookup resolved; it is the field that did not exist.
  EXPECT_EQ(parsed.error(), LookupParseError::unknown_field);
}

TEST(LegacyLookup, ATermIsOneFilterAndCombiningIsTheCallersToDo) {
  const auto windows = two_windows();

  // Deliberately no `&` or `,` grammar: a term is a term, and the typed
  // operators are how two of them are joined. That keeps the edge small and
  // the typed spelling the only way to write anything compound.
  const auto left = parse_lookup<Window>("window_name__contains=o", kWindowFields);
  const auto right = parse_lookup<Window>("window_name__endswith=tail", kWindowFields);
  ASSERT_TRUE(left.has_value());
  ASSERT_TRUE(right.has_value());

  EXPECT_EQ(kept(windows, *left && *right), (std::vector<std::string>{"logs-tail"}));
  EXPECT_EQ(kept(windows, !*right), (std::vector<std::string>{"Editor"}));
}

} // namespace
