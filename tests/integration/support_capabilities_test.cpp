// The capability registry and the manifest the documentation site reads.
//
// `support::capabilities` is the table the gates refuse by, and the root
// `capabilities.json` is written from it. Nothing here needs a daemon.
//
// To rewrite the manifest after changing the registry:
//
//   LIBTMUX_EMIT_CAPABILITIES=1 ctest --preset cxx-gcc -R TheCommittedManifest

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/version.hpp"
#include "support_capabilities.hpp"
#include "support_commands.hpp"

namespace {

namespace caps = libtmux::support::capabilities;

const std::filesystem::path kRoot{LIBTMUX_TEST_SOURCE_ROOT};

std::string slurp(const std::filesystem::path& path) {
  std::ifstream in{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

libtmux::Version parsed(std::string_view text) {
  const auto version = libtmux::parse_version("tmux " + std::string{text});
  EXPECT_TRUE(version.has_value()) << text;
  return version.value_or(libtmux::Version{});
}

bool kebab(std::string_view id) {
  if (id.empty() || !std::islower(static_cast<unsigned char>(id.front()))) {
    return false;
  }
  return std::ranges::all_of(id,
                             [](char c) {
                               return std::islower(static_cast<unsigned char>(c)) ||
                                      std::isdigit(static_cast<unsigned char>(c)) ||
                                      c == '-';
                             }) &&
         id.back() != '-' && id.find("--") == std::string_view::npos;
}

bool word_in(const std::string& text, std::string_view word) {
  for (std::size_t at = text.find(word); at != std::string::npos;
       at = text.find(word, at + 1)) {
    const auto ident = [](char c) {
      return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    const bool left = at == 0 || !ident(text[at - 1]);
    const bool right =
        at + word.size() >= text.size() || !ident(text[at + word.size()]);
    if (left && right) {
      return true;
    }
  }
  return false;
}

bool identifier_character(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The text between the braces of the `class` or `struct` that defines `type`.
std::optional<std::string> body_of(const std::string& text, const std::string& type) {
  for (const char* keyword : {"class ", "struct "}) {
    const std::string needle = keyword + type;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + 1)) {
      const std::size_t end = at + needle.size();
      if (end < text.size() && identifier_character(text[end])) {
        continue;
      }
      const std::size_t open = text.find_first_of("{;", end);
      if (open == std::string::npos || text[open] == ';') {
        continue;
      }
      int depth = 0;
      for (std::size_t i = open; i < text.size(); ++i) {
        depth += text[i] == '{' ? 1 : text[i] == '}' ? -1 : 0;
        if (depth == 0) {
          return text.substr(open + 1, i - open - 1);
        }
      }
    }
  }
  return std::nullopt;
}

// The error type of every `expected<T, E>` that `method` is declared to return.
std::vector<std::string> returned_errors(const std::string& body,
                                         const std::string& method) {
  const std::regex declaration{R"(expected<((?:[^<>;{}]|<[^<>]*>)*)>\s+)" + method +
                               R"(\s*\()"};
  std::vector<std::string> errors;
  for (std::sregex_iterator it{body.begin(), body.end(), declaration}, end; it != end;
       ++it) {
    const std::string arguments = (*it)[1];
    int depth = 0;
    std::size_t comma = std::string::npos;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
      depth += arguments[i] == '<' ? 1 : arguments[i] == '>' ? -1 : 0;
      if (arguments[i] == ',' && depth == 0) {
        comma = i;
      }
    }
    std::string error = arguments.substr(comma + 1);
    error.erase(0, error.find_first_not_of(" \n"));
    errors.push_back(error.substr(0, error.find_last_not_of(" \n") + 1));
  }
  return errors;
}

TEST(CapabilitiesManifest, TheCommittedManifestIsTheRegistry) {
  const auto path = kRoot / "capabilities.json";
  const std::string registry = caps::manifest_json();
  if (std::getenv("LIBTMUX_EMIT_CAPABILITIES") != nullptr) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << registry;
  }
  EXPECT_EQ(slurp(path), registry)
      << path << " differs from support::capabilities; regenerate it with "
      << "LIBTMUX_EMIT_CAPABILITIES=1";
}

TEST(CapabilitiesManifest, TheManifestEndsInOneNewline) {
  const std::string manifest = caps::manifest_json();
  ASSERT_GE(manifest.size(), 2U);
  EXPECT_EQ(manifest.back(), '\n');
  EXPECT_NE(manifest[manifest.size() - 2U], '\n');
}

TEST(CapabilitiesRegistry, IdsAreKebabCaseAndUnique) {
  std::set<std::string> seen;
  const auto check = [&](std::string_view id) {
    EXPECT_TRUE(kebab(id)) << id;
    EXPECT_TRUE(seen.emplace(id).second) << id << " appears twice";
  };
  for (const auto& entry : caps::all_capabilities()) {
    check(entry.id);
  }
  for (const auto& entry : caps::all_workarounds()) {
    check(entry.id);
  }
}

TEST(CapabilitiesRegistry, EveryKeyFindsItsOwnRecord) {
  for (const auto& entry : caps::all_capabilities()) {
    EXPECT_EQ(caps::capability(entry.key).id, entry.id);
  }
  for (const auto& entry : caps::all_workarounds()) {
    EXPECT_EQ(caps::workaround(entry.key).id, entry.id);
  }
}

TEST(CapabilitiesRegistry, ReleasesAreSpelledTheWayTmuxPrintsThem) {
  for (const auto& entry : caps::all_capabilities()) {
    EXPECT_EQ(parsed(entry.since.text), entry.since.version) << entry.id;
  }
  for (const auto& entry : caps::all_workarounds()) {
    EXPECT_FALSE(entry.releases.empty()) << entry.id;
    for (const auto& release : entry.releases) {
      EXPECT_EQ(parsed(release.text), release.version) << entry.id;
    }
  }
}

TEST(CapabilitiesRegistry, ARefusalNamesItsError) {
  for (const auto& entry : caps::all_capabilities()) {
    EXPECT_EQ(entry.handling == caps::Handling::refuse, !entry.error.empty())
        << entry.id;
  }
  for (const auto& entry : caps::all_workarounds()) {
    EXPECT_NE(entry.handling, caps::Handling::fallback) << entry.id;
  }
}

TEST(CapabilitiesRegistry, EveryCapabilityIsAtOrAboveTheFloor) {
  for (const auto& entry : caps::all_capabilities()) {
    EXPECT_GE(entry.since.version, libtmux::kMinimumSupported) << entry.id;
  }
}

TEST(CapabilitiesRegistry, AWorkaroundAffectsOnlyItsListedReleases) {
  using caps::WorkaroundId;
  EXPECT_TRUE(caps::affects(WorkaroundId::name_separators_rejected, parsed("3.7")));
  EXPECT_FALSE(caps::affects(WorkaroundId::name_separators_rejected, parsed("3.7a")));
  EXPECT_FALSE(caps::affects(WorkaroundId::name_separators_rejected, parsed("3.6b")));
}

// A refusal is published with the error type its APIs report. The type may
// also be the one a reporting struct carries, as `LayoutFailure` carries the
// `CommandFailure` of the layout it stopped at.
TEST(CapabilitiesRegistry, ARefusalNamesTheErrorItsApisReturn) {
  std::string headers;
  for (const auto& file :
       std::filesystem::directory_iterator{kRoot / "include/libtmux"}) {
    if (file.path().extension() == ".hpp") {
      headers += slurp(file.path());
      headers.push_back('\n');
    }
  }
  for (const auto& entry : caps::all_capabilities()) {
    ASSERT_TRUE(entry.error.starts_with("libtmux::")) << entry.id;
    const std::string error{entry.error.substr(9)};
    for (const std::string_view api : entry.apis) {
      const auto parts_end = api.rfind("::");
      const std::string method{api.substr(parts_end + 2)};
      const std::string type{api.substr(9, api.find("::", 9) - 9)};
      const auto body = body_of(headers, type);
      ASSERT_TRUE(body.has_value()) << entry.id << " " << api << ": no type " << type;
      const auto returned = returned_errors(*body, method);
      EXPECT_FALSE(returned.empty()) << entry.id << " " << api << ": no declaration";
      for (const std::string& each : returned) {
        const auto carrier = body_of(headers, each);
        const bool carries = carrier.has_value() && word_in(*carrier, error);
        EXPECT_TRUE(each == error || carries)
            << entry.id << " " << api << " returns " << each << ", not " << error;
      }
    }
  }
}

TEST(CapabilitiesRegistry, EveryListedCommandIsATmuxCommand) {
  const auto check = [](std::string_view id, std::span<const caps::CommandUse> uses) {
    for (const auto& use : uses) {
      EXPECT_EQ(libtmux::support::commands::resolve(use.command), use.command)
          << id << " lists " << use.command;
      for (const std::string_view flag : use.flags) {
        EXPECT_EQ(flag.size(), 2U) << id << " " << flag;
        EXPECT_EQ(flag.front(), '-') << id << " " << flag;
      }
    }
  };
  for (const auto& entry : caps::all_capabilities()) {
    check(entry.id, entry.commands);
  }
  for (const auto& entry : caps::all_workarounds()) {
    check(entry.id, entry.commands);
  }
}

TEST(CapabilitiesRegistry, EveryApiNameIsDeclaredInAPublicHeader) {
  std::string headers;
  for (const auto& file :
       std::filesystem::directory_iterator{kRoot / "include/libtmux"}) {
    if (file.path().extension() == ".hpp") {
      headers += slurp(file.path());
      headers.push_back('\n');
    }
  }
  const auto check = [&](std::string_view where, std::string_view name) {
    ASSERT_TRUE(name.starts_with("libtmux::")) << where << " " << name;
    std::vector<std::string_view> parts;
    for (std::string_view rest = name.substr(9); !rest.empty();) {
      const auto at = rest.find("::");
      parts.push_back(rest.substr(0, at));
      rest = at == std::string_view::npos ? std::string_view{} : rest.substr(at + 2);
    }
    ASSERT_FALSE(parts.empty()) << where << " " << name;
    const std::string type{parts.front()};
    EXPECT_TRUE(word_in(headers, "struct " + type) ||
                word_in(headers, "class " + type) ||
                word_in(headers, "enum class " + type))
        << where << " " << name << ": no public type " << type;
    for (std::size_t i = 1; i < parts.size(); ++i) {
      EXPECT_TRUE(word_in(headers, parts[i])) << where << " " << name;
    }
  };
  for (const auto& entry : caps::all_capabilities()) {
    for (const auto name : entry.apis) {
      check(entry.id, name);
    }
    if (!entry.error.empty()) {
      check(entry.id, entry.error);
    }
  }
  for (const auto& entry : caps::all_workarounds()) {
    for (const auto name : entry.apis) {
      check(entry.id, name);
    }
  }
}

} // namespace
