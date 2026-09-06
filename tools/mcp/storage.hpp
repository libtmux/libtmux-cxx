#pragma once

#include "mcp_swap.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace libtmux::mcp_swap::detail {

struct Identity {
  std::uint64_t device{};
  std::uint64_t inode{};

  auto operator<=>(const Identity&) const = default;
};

struct RouteNode {
  std::size_t position{};
  std::string kind;
  std::optional<std::string> link;
  Identity identity;

  auto operator<=>(const RouteNode&) const = default;
};

struct FileBinding {
  std::filesystem::path resolved;
  std::vector<RouteNode> nodes;
  Identity parent_identity;
  Identity target;
  unsigned mode{};

  auto operator<=>(const FileBinding&) const = default;
};

struct MissingBinding {
  std::filesystem::path resolved;
  std::filesystem::path parent;
  Identity parent_identity;

  auto operator<=>(const MissingBinding&) const = default;
};

struct BoundFile {
  std::string bytes;
  FileBinding binding;
};

struct PathClaim {
  std::string owner;
  std::string kind;
  std::filesystem::path resolved;
  std::optional<Identity> target;
};

class TransactionLock;

[[nodiscard]] std::string sha256(std::string_view bytes);
[[nodiscard]] std::filesystem::path absolute_logical(const std::filesystem::path& path);
[[nodiscard]] bool lexists(const std::filesystem::path& path);
[[nodiscard]] FileBinding capture_binding(const std::filesystem::path& path);
[[nodiscard]] BoundFile read_bound(const std::filesystem::path& path,
                                   TransactionLock* lock = nullptr);
void validate_bound(const std::filesystem::path& path, std::string_view bytes,
                    const FileBinding& binding, TransactionLock* lock = nullptr);
[[nodiscard]] MissingBinding capture_missing(const std::filesystem::path& path);
void validate_missing(const std::filesystem::path& path, const MissingBinding& binding);
void validate_distinct(std::span<const PathClaim> claims);
void atomic_write(const std::filesystem::path& path, std::string_view bytes,
                  unsigned mode, std::optional<Identity> expected_target,
                  Identity expected_parent);
[[nodiscard]] FileBinding create_private_file(const std::filesystem::path& path,
                                              std::string_view bytes,
                                              const MissingBinding& expected);
void remove_bound(const std::filesystem::path& path, std::string_view bytes,
                  const FileBinding& binding, TransactionLock* lock = nullptr);
void ensure_private_directory(const std::filesystem::path& path);
void validate_private_path(const std::filesystem::path& path,
                           const FileBinding& binding);

[[nodiscard]] nlohmann::json to_json(const Identity& identity);
[[nodiscard]] nlohmann::json to_json(const RouteNode& node);
[[nodiscard]] nlohmann::json to_json(const FileBinding& binding);
[[nodiscard]] Identity identity_from_json(const nlohmann::json& value);
[[nodiscard]] FileBinding binding_from_json(const nlohmann::json& value);

class TransactionLock final {
public:
  explicit TransactionLock(const std::filesystem::path& state_directory);
  ~TransactionLock();

  TransactionLock(const TransactionLock&) = delete;
  TransactionLock& operator=(const TransactionLock&) = delete;
  TransactionLock(TransactionLock&&) = delete;
  TransactionLock& operator=(TransactionLock&&) = delete;

  void validate() const;
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
  [[nodiscard]] const FileBinding& binding() const { return binding_; }

private:
  friend BoundFile read_bound(const std::filesystem::path&, TransactionLock*);

  std::filesystem::path path_;
  int descriptor_{-1};
  int retained_alias_descriptor_{-1};
  FileBinding binding_;
};

} // namespace libtmux::mcp_swap::detail
