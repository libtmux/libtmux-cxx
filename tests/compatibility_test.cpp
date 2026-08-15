// The compatibility floor for what entities ask tmux for.
//
// tmux answers a format token it does not recognise with the empty string and
// a zero exit status. A field added for a newer tmux would therefore read as
// present-and-empty on an older server rather than failing, and no other test
// would notice. This one compares every requested field against the token
// table of the oldest supported release.

#include <algorithm>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/entities.hpp"
#include "libtmux/keys.hpp"

namespace {

using libtmux::Client;
using libtmux::Pane;
using libtmux::Session;
using libtmux::Window;

std::vector<std::string> supported_tokens() {
  std::ifstream file{LIBTMUX_FORMAT_TOKENS_PATH};
  EXPECT_TRUE(file.is_open()) << "cannot read " << LIBTMUX_FORMAT_TOKENS_PATH;
  std::vector<std::string> tokens;
  for (std::string line; std::getline(file, line);) {
    if (!line.empty() && line.front() != '#') {
      tokens.push_back(line);
    }
  }
  return tokens;
}

template <typename Entity> void require_fields_are_supported() {
  const std::vector<std::string> tokens = supported_tokens();
  // A path typo would otherwise pass this test by comparing against nothing.
  ASSERT_GT(tokens.size(), 100U) << "token table looks truncated";
  for (const std::string_view field : Entity::kFields) {
    EXPECT_NE(std::ranges::find(tokens, field), tokens.end())
        << Entity::kNoun << " asks for " << field
        << ", which tmux 3.2a does not register";
  }
}

TEST(Compatibility, EveryRequestedFieldExistsInTheOldestSupportedTmux) {
  require_fields_are_supported<Session>();
  require_fields_are_supported<Window>();
  require_fields_are_supported<Pane>();
  require_fields_are_supported<Client>();
}

TEST(Compatibility, NoEntityAsksForTheSameFieldTwice) {
  // Positional accessors read by index, so a duplicated token is a field that
  // silently costs bytes and can never be reached by the second accessor.
  const auto unique = [](auto fields) {
    std::vector<std::string_view> sorted{fields.begin(), fields.end()};
    std::ranges::sort(sorted);
    return std::ranges::adjacent_find(sorted) == sorted.end();
  };
  EXPECT_TRUE(unique(Session::kFields));
  EXPECT_TRUE(unique(Window::kFields));
  EXPECT_TRUE(unique(Pane::kFields));
  EXPECT_TRUE(unique(Client::kFields));
}

TEST(Compatibility, EveryKeyNameTmuxKnowsIsAccepted) {
  // Taken from the key table of the oldest supported release. A name missing
  // here is a key `send_key` refuses and tmux would have delivered.
  std::ifstream file{LIBTMUX_KEY_NAMES_PATH};
  ASSERT_TRUE(file.is_open()) << "cannot read " << LIBTMUX_KEY_NAMES_PATH;
  std::size_t checked = 0;
  for (std::string name; std::getline(file, name);) {
    if (name.empty() || name.front() == '#') {
      continue;
    }
    ++checked;
    EXPECT_TRUE(libtmux::is_key_name(name)) << name << " is a key tmux accepts";
  }
  EXPECT_GT(checked, 40U) << "the key table looks truncated";
}

TEST(Compatibility, ModifiersStackAndStillHaveToNameAKey) {
  // tmux takes any number of C-, M- and S- prefixes, so the check strips
  // them all before deciding. What is left still has to be a key: a prefix
  // is not one on its own, and stacking them cannot make a typo valid.
  EXPECT_TRUE(libtmux::is_key_name("C-Up"));
  EXPECT_TRUE(libtmux::is_key_name("M-x"));
  EXPECT_TRUE(libtmux::is_key_name("C-M-S-Home"));
  EXPECT_TRUE(libtmux::is_key_name("C-F12"));

  EXPECT_FALSE(libtmux::is_key_name("C-"));
  EXPECT_FALSE(libtmux::is_key_name("C-M-"));
  EXPECT_FALSE(libtmux::is_key_name("C-Nope"));
  // Not a modifier, so this is a two-character name and no key is called it.
  EXPECT_FALSE(libtmux::is_key_name("X-Up"));
}

TEST(Compatibility, FunctionKeysStopWhereTmuxStops) {
  for (int number = 1; number <= 12; ++number) {
    const auto name = "F" + std::to_string(number);
    EXPECT_TRUE(libtmux::is_key_name(name)) << name;
  }
  EXPECT_FALSE(libtmux::is_key_name("F0"));
  EXPECT_FALSE(libtmux::is_key_name("F13"));
  EXPECT_FALSE(libtmux::is_key_name("F99"));
  // A digit count the accumulator must reject before multiplying, which is
  // what the two-digit cap in the parser is for.
  EXPECT_FALSE(libtmux::is_key_name("F4294967297"));
  EXPECT_FALSE(libtmux::is_key_name("Fx"));
  EXPECT_FALSE(libtmux::is_key_name("F1x"));
  // One character, so it is the printable character F rather than a
  // function key with no number.
  EXPECT_TRUE(libtmux::is_key_name("F"));
}

TEST(Compatibility, LiteralTextRefusesTheEmptyString) {
  // An empty `send-keys -l` reaches tmux and delivers nothing, so a caller
  // that built one from an empty variable would see success and no effect.
  const auto empty = libtmux::literal_arguments("");
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error(), libtmux::KeyError::empty);
  EXPECT_FALSE(libtmux::to_string(empty.error()).empty());

  const auto text = libtmux::literal_arguments("echo hi");
  ASSERT_TRUE(text.has_value());
  EXPECT_EQ(*text, (std::vector<std::string>{"-l", "echo hi"}));
}

TEST(Compatibility, AControlByteIsNotAKeyName) {
  // tmux delivers nothing for one, so accepting it would send input that
  // never arrives.
  EXPECT_FALSE(libtmux::is_key_name(std::string(1, '\x01')));
  EXPECT_FALSE(libtmux::is_key_name(std::string(1, '\x1f')));
  EXPECT_TRUE(libtmux::is_key_name("a"));
  EXPECT_TRUE(libtmux::is_key_name(" "));
}

} // namespace
