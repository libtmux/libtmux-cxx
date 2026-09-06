#include "storage.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

namespace libtmux::mcp_swap::detail {
namespace {

constexpr std::array<std::uint32_t, 64> sha_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

[[noreturn]] void system_failure(std::string_view action,
                                 const std::filesystem::path& path) {
  throw Error{std::string{action} + " " + path.string() + ": " + std::strerror(errno)};
}

Identity identity(const struct stat& details) {
  return {static_cast<std::uint64_t>(details.st_dev),
          static_cast<std::uint64_t>(details.st_ino)};
}

unsigned permission_mode(const struct stat& details) {
  return static_cast<unsigned>(details.st_mode & 0777);
}

void write_all(int descriptor, std::string_view bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto written =
        ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      system_failure("could not write", "file");
    }
    offset += static_cast<std::size_t>(written);
  }
}

void sync_descriptor(int descriptor, const std::filesystem::path& path) {
  if (::fsync(descriptor) != 0) {
    system_failure("could not sync directory", path);
  }
}

int open_directory(const std::filesystem::path& path) {
  const int descriptor =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  if (descriptor < 0) {
    system_failure("could not open directory", path);
  }
  return descriptor;
}

Identity descriptor_identity(int descriptor, const std::filesystem::path& path) {
  struct stat details {};
  if (::fstat(descriptor, &details) != 0) {
    system_failure("could not inspect directory", path);
  }
  if (!S_ISDIR(details.st_mode)) {
    throw Error{"path is not a directory: " + path.string()};
  }
  return identity(details);
}

void validate_directory_entry(int directory, const std::filesystem::path& path,
                              const std::optional<Identity>& expected) {
  struct stat details {};
  if (::fstatat(directory, path.filename().c_str(), &details, AT_SYMLINK_NOFOLLOW) ==
      0) {
    if (!expected.has_value() || !S_ISREG(details.st_mode) ||
        identity(details) != *expected) {
      throw Error{"destination entry changed before publication: " + path.string()};
    }
    return;
  }
  if (errno == ENOENT && !expected.has_value()) {
    return;
  }
  system_failure("could not authenticate destination entry", path);
}

void validate_private_file(const struct stat& details,
                           const std::filesystem::path& path) {
  if (!S_ISREG(details.st_mode)) {
    throw Error{"private recovery path is not a regular file: " + path.string()};
  }
  if (details.st_uid != ::geteuid()) {
    throw Error{"private recovery path is not owned by this user: " + path.string()};
  }
  if (details.st_nlink != 1) {
    throw Error{"private recovery path has multiple hard links: " + path.string()};
  }
  if (permission_mode(details) != 0600U) {
    throw Error{"private recovery path is not mode 0600: " + path.string()};
  }
}

std::uint32_t load_word(std::span<const std::uint8_t, 64> block, std::size_t offset) {
  return (static_cast<std::uint32_t>(block[offset]) << 24U) |
         (static_cast<std::uint32_t>(block[offset + 1]) << 16U) |
         (static_cast<std::uint32_t>(block[offset + 2]) << 8U) |
         static_cast<std::uint32_t>(block[offset + 3]);
}

} // namespace

std::string sha256(std::string_view bytes) {
  std::vector<std::uint8_t> message(bytes.begin(), bytes.end());
  const auto bit_length = static_cast<std::uint64_t>(message.size()) * 8U;
  message.push_back(0x80U);
  while (message.size() % 64U != 56U) {
    message.push_back(0U);
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(
        static_cast<std::uint8_t>(bit_length >> static_cast<unsigned>(shift)));
  }

  std::array<std::uint32_t, 8> hash{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  for (std::size_t base = 0; base < message.size(); base += 64U) {
    std::array<std::uint32_t, 64> words{};
    const std::span<const std::uint8_t, 64> block{message.data() + base, 64U};
    for (std::size_t index = 0; index < 16U; ++index) {
      words[index] = load_word(block, index * 4U);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const auto first = std::rotr(words[index - 15], 7) ^
                         std::rotr(words[index - 15], 18) ^ (words[index - 15] >> 3U);
      const auto second = std::rotr(words[index - 2], 17) ^
                          std::rotr(words[index - 2], 19) ^ (words[index - 2] >> 10U);
      words[index] = words[index - 16] + first + words[index - 7] + second;
    }
    auto [a, b, c, d, e, f, g, h] = hash;
    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto upper = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const auto choose = (e & f) ^ (~e & g);
      const auto first = h + upper + choose + sha_constants[index] + words[index];
      const auto lower = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto second = lower + majority;
      h = g;
      g = f;
      f = e;
      e = d + first;
      d = c;
      c = b;
      b = a;
      a = first + second;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
  }

  std::ostringstream result;
  result << std::hex << std::setfill('0');
  for (const auto word : hash) {
    result << std::setw(8) << word;
  }
  return result.str();
}

std::filesystem::path absolute_logical(const std::filesystem::path& path) {
  return (path.is_absolute() ? path : std::filesystem::absolute(path))
      .lexically_normal();
}

bool lexists(const std::filesystem::path& path) {
  struct stat details {};
  if (::lstat(path.c_str(), &details) == 0) {
    return true;
  }
  if (errno == ENOENT || errno == ENOTDIR) {
    return false;
  }
  system_failure("could not inspect", path);
}

FileBinding capture_binding(const std::filesystem::path& path) {
  const auto absolute = absolute_logical(path);
  std::vector<RouteNode> nodes;
  auto current = absolute.root_path();
  std::size_t position = 0;
  for (const auto& part : absolute.relative_path()) {
    ++position;
    current /= part;
    struct stat details {};
    if (::lstat(current.c_str(), &details) != 0) {
      system_failure("could not inspect", current);
    }
    const bool last = current == absolute;
    if (last && S_ISREG(details.st_mode)) {
      continue;
    }
    if (S_ISLNK(details.st_mode)) {
      nodes.push_back({position, "symlink",
                       std::filesystem::read_symlink(current).string(),
                       identity(details)});
    } else if (S_ISDIR(details.st_mode)) {
      nodes.push_back({position, "directory", std::nullopt, identity(details)});
    } else {
      throw Error{"destination topology contains a non-directory: " + current.string()};
    }
  }

  const auto resolved = std::filesystem::canonical(absolute);
  const int parent = open_directory(resolved.parent_path());
  Identity parent_identity;
  try {
    parent_identity = descriptor_identity(parent, resolved.parent_path());
  } catch (...) {
    static_cast<void>(::close(parent));
    throw;
  }
  static_cast<void>(::close(parent));
  struct stat details {};
  if (::stat(resolved.c_str(), &details) != 0) {
    system_failure("could not inspect", resolved);
  }
  if (!S_ISREG(details.st_mode)) {
    throw Error{"destination is not a regular file: " + path.string()};
  }
  return {resolved, std::move(nodes), parent_identity, identity(details),
          permission_mode(details)};
}

BoundFile read_bound(const std::filesystem::path& path, TransactionLock* lock) {
  if (lock != nullptr) {
    lock->validate();
  }
  const auto before = capture_binding(path);
  if (lock != nullptr && before.target == lock->binding_.target) {
    throw Error{"destination aliases the held transaction lock: " + path.string()};
  }
  int descriptor = ::open(before.resolved.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    system_failure("could not open", path);
  }
  struct stat opened {};
  if (::fstat(descriptor, &opened) != 0) {
    const int saved = errno;
    if (lock != nullptr) {
      lock->retained_alias_descriptor_ = descriptor;
      descriptor = -1;
    }
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
    }
    errno = saved;
    system_failure("could not inspect open file", path);
  }
  if (lock != nullptr && identity(opened) == lock->binding_.target) {
    lock->retained_alias_descriptor_ = descriptor;
    descriptor = -1;
    throw Error{"destination changed to an alias of the held transaction lock: " +
                path.string()};
  }
  if (identity(opened) != before.target || permission_mode(opened) != before.mode) {
    static_cast<void>(::close(descriptor));
    throw Error{"destination changed while it was opened: " + path.string()};
  }
  std::string bytes;
  std::array<char, 8192> buffer{};
  for (;;) {
    const auto count = ::read(descriptor, buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int saved = errno;
      static_cast<void>(::close(descriptor));
      errno = saved;
      system_failure("could not read", path);
    }
    if (count == 0) {
      break;
    }
    bytes.append(buffer.data(), static_cast<std::size_t>(count));
  }
  static_cast<void>(::close(descriptor));
  const auto after = capture_binding(path);
  if (lock != nullptr) {
    lock->validate();
    if (after.target == lock->binding_.target) {
      throw Error{"destination changed to an alias of the held transaction lock: " +
                  path.string()};
    }
  }
  if (after != before) {
    throw Error{"destination changed while it was read: " + path.string()};
  }
  return {std::move(bytes), before};
}

void validate_bound(const std::filesystem::path& path, std::string_view bytes,
                    const FileBinding& binding, TransactionLock* lock) {
  const auto current = read_bound(path, lock);
  if (current.binding != binding) {
    throw Error{"destination identity changed after planning: " + path.string()};
  }
  if (current.bytes != bytes) {
    throw Error{"destination content changed after planning: " + path.string()};
  }
}

MissingBinding capture_missing(const std::filesystem::path& path) {
  const auto absolute = absolute_logical(path);
  if (lexists(absolute)) {
    throw Error{"destination already exists: " + path.string()};
  }
  auto ancestor = absolute.parent_path();
  std::vector<std::filesystem::path> tail{absolute.filename()};
  while (!lexists(ancestor)) {
    tail.push_back(ancestor.filename());
    ancestor = ancestor.parent_path();
  }
  std::ranges::reverse(tail);
  const auto parent = std::filesystem::canonical(ancestor);
  struct stat details {};
  if (::stat(parent.c_str(), &details) != 0) {
    system_failure("could not inspect", parent);
  }
  if (!S_ISDIR(details.st_mode) || ::access(parent.c_str(), W_OK | X_OK) != 0) {
    throw Error{"destination parent is not writable: " + ancestor.string()};
  }
  auto resolved = parent;
  for (const auto& part : tail) {
    resolved /= part;
  }
  return {resolved.lexically_normal(), parent, identity(details)};
}

void validate_missing(const std::filesystem::path& path,
                      const MissingBinding& binding) {
  if (capture_missing(path) != binding) {
    throw Error{"destination parent changed after planning: " + path.string()};
  }
}

void validate_distinct(std::span<const PathClaim> claims) {
  for (std::size_t index = 0; index < claims.size(); ++index) {
    for (std::size_t previous = 0; previous < index; ++previous) {
      const bool same_target = claims[index].target.has_value() &&
                               claims[previous].target.has_value() &&
                               claims[index].target == claims[previous].target;
      if (claims[index].resolved == claims[previous].resolved || same_target) {
        throw Error{claims[previous].owner + " " + claims[previous].kind + " and " +
                    claims[index].owner + " " + claims[index].kind +
                    " select the same physical path"};
      }
    }
  }
}

void atomic_write(const std::filesystem::path& path, std::string_view bytes,
                  unsigned mode, std::optional<Identity> expected_target,
                  Identity expected_parent) {
  const auto parent = path.parent_path();
  const int directory = open_directory(parent);
  try {
    if (descriptor_identity(directory, parent) != expected_parent) {
      throw Error{"destination parent changed before publication: " + path.string()};
    }
  } catch (...) {
    static_cast<void>(::close(directory));
    throw;
  }
  static std::atomic<std::uint64_t> sequence{};
  std::string temporary_name;
  int descriptor = -1;
  for (int attempt = 0; attempt < 128; ++attempt) {
    temporary_name = "." + path.filename().string() + ".mcp-swap-" +
                     std::to_string(::getpid()) + "-" +
                     std::to_string(sequence.fetch_add(1));
    descriptor = ::openat(directory, temporary_name.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor >= 0) {
      break;
    }
    if (errno != EEXIST) {
      const int saved = errno;
      static_cast<void>(::close(directory));
      errno = saved;
      system_failure("could not create temporary file beside", path);
    }
  }
  if (descriptor < 0) {
    static_cast<void>(::close(directory));
    throw Error{"could not reserve a temporary file beside " + path.string()};
  }
  try {
    if (::fchmod(descriptor, static_cast<mode_t>(mode)) != 0) {
      system_failure("could not set temporary file mode for", path);
    }
    write_all(descriptor, bytes);
    if (::fsync(descriptor) != 0) {
      system_failure("could not sync temporary file for", path);
    }
    if (::close(descriptor) != 0) {
      descriptor = -1;
      system_failure("could not close temporary file for", path);
    }
    descriptor = -1;
    validate_directory_entry(directory, path, expected_target);
    if (::renameat(directory, temporary_name.c_str(), directory,
                   path.filename().c_str()) != 0) {
      system_failure("could not publish", path);
    }
    temporary_name.clear();
    sync_descriptor(directory, parent);
    static_cast<void>(::close(directory));
  } catch (...) {
    const int saved = errno;
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
    }
    if (!temporary_name.empty()) {
      static_cast<void>(::unlinkat(directory, temporary_name.c_str(), 0));
    }
    static_cast<void>(::close(directory));
    errno = saved;
    throw;
  }
}

FileBinding create_private_file(const std::filesystem::path& path,
                                std::string_view bytes,
                                const MissingBinding& expected) {
  const int directory = open_directory(expected.parent);
  int descriptor = -1;
  try {
    if (descriptor_identity(directory, expected.parent) != expected.parent_identity ||
        expected.resolved.parent_path() != expected.parent ||
        expected.resolved.filename() != path.filename()) {
      throw Error{"private recovery parent changed: " + path.string()};
    }
    validate_directory_entry(directory, expected.resolved, std::nullopt);
    descriptor = ::openat(directory, path.filename().c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) {
      system_failure("could not create private recovery file", path);
    }
    write_all(descriptor, bytes);
    if (::fsync(descriptor) != 0) {
      system_failure("could not sync private recovery file", path);
    }
    if (::close(descriptor) != 0) {
      descriptor = -1;
      system_failure("could not close private recovery file", path);
    }
    descriptor = -1;
    sync_descriptor(directory, path.parent_path());
    static_cast<void>(::close(directory));
  } catch (...) {
    const int saved = errno;
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
    }
    static_cast<void>(::unlinkat(directory, path.filename().c_str(), 0));
    static_cast<void>(::close(directory));
    errno = saved;
    throw;
  }
  const auto binding = capture_binding(path);
  struct stat details {};
  if (::stat(binding.resolved.c_str(), &details) != 0) {
    system_failure("could not authenticate private recovery file", path);
  }
  validate_private_file(details, path);
  return binding;
}

void remove_bound(const std::filesystem::path& path, std::string_view bytes,
                  const FileBinding& binding, TransactionLock* lock) {
  const int directory = open_directory(binding.resolved.parent_path());
  try {
    if (descriptor_identity(directory, binding.resolved.parent_path()) !=
        binding.parent_identity) {
      throw Error{"destination parent changed before removal: " + path.string()};
    }
    validate_bound(path, bytes, binding, lock);
    validate_directory_entry(directory, binding.resolved, binding.target);
    if (::unlinkat(directory, binding.resolved.filename().c_str(), 0) != 0) {
      system_failure("could not remove", path);
    }
    sync_descriptor(directory, binding.resolved.parent_path());
    static_cast<void>(::close(directory));
  } catch (...) {
    const int saved = errno;
    static_cast<void>(::close(directory));
    errno = saved;
    throw;
  }
}

void ensure_private_directory(const std::filesystem::path& path) {
  if (!lexists(path)) {
    const auto parent = path.parent_path();
    if (!parent.empty() && !lexists(parent)) {
      ensure_private_directory(parent);
    }
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
      system_failure("could not create private directory", path);
    }
  }
  struct stat details {};
  if (::lstat(path.c_str(), &details) != 0) {
    system_failure("could not inspect private directory", path);
  }
  if (!S_ISDIR(details.st_mode) || details.st_uid != ::geteuid() ||
      (permission_mode(details) & 0077U) != 0U) {
    throw Error{"recovery directory is not private and owned: " + path.string()};
  }
}

void validate_private_path(const std::filesystem::path& path,
                           const FileBinding& binding) {
  struct stat logical {};
  if (::lstat(path.c_str(), &logical) != 0) {
    system_failure("could not inspect private recovery path", path);
  }
  if (!S_ISREG(logical.st_mode)) {
    throw Error{"private recovery path is indirect: " + path.string()};
  }
  struct stat resolved {};
  if (::stat(binding.resolved.c_str(), &resolved) != 0) {
    system_failure("could not inspect private recovery path", path);
  }
  validate_private_file(resolved, path);
  if (capture_binding(path) != binding) {
    throw Error{"private recovery path changed: " + path.string()};
  }
}

nlohmann::json to_json(const Identity& value) {
  return {{"device", value.device}, {"inode", value.inode}};
}

nlohmann::json to_json(const RouteNode& node) {
  return {{"position", node.position},
          {"kind", node.kind},
          {"link", node.link.has_value() ? nlohmann::json(*node.link)
                                         : nlohmann::json(nullptr)},
          {"identity", to_json(node.identity)}};
}

nlohmann::json to_json(const FileBinding& binding) {
  auto nodes = nlohmann::json::array();
  for (const auto& node : binding.nodes) {
    nodes.push_back(to_json(node));
  }
  return {{"resolved", binding.resolved.string()},
          {"nodes", std::move(nodes)},
          {"parent", to_json(binding.parent_identity)},
          {"target", to_json(binding.target)},
          {"mode", binding.mode}};
}

Identity identity_from_json(const nlohmann::json& value) {
  if (!value.is_object() || value.size() != 2 || !value.contains("device") ||
      !value.contains("inode") || !value["device"].is_number_unsigned() ||
      !value["inode"].is_number_unsigned()) {
    throw Error{"recovery identity is malformed"};
  }
  return {value["device"].get<std::uint64_t>(), value["inode"].get<std::uint64_t>()};
}

FileBinding binding_from_json(const nlohmann::json& value) {
  if (!value.is_object() || value.size() != 5 || !value.contains("resolved") ||
      !value.contains("nodes") || !value.contains("parent") ||
      !value.contains("target") || !value.contains("mode") ||
      !value["resolved"].is_string() || !value["nodes"].is_array() ||
      !value["mode"].is_number_unsigned()) {
    throw Error{"recovery path binding is malformed"};
  }
  FileBinding result;
  result.resolved = value["resolved"].get<std::string>();
  if (!result.resolved.is_absolute() ||
      result.resolved != result.resolved.lexically_normal() ||
      result.resolved == result.resolved.root_path()) {
    throw Error{"recovery path binding is not an absolute normalized file path"};
  }
  result.parent_identity = identity_from_json(value["parent"]);
  result.target = identity_from_json(value["target"]);
  result.mode = value["mode"].get<unsigned>();
  if (result.mode > 0777U) {
    throw Error{"recovery path binding has an invalid mode"};
  }
  std::size_t expected_position = 1;
  for (const auto& item : value["nodes"]) {
    if (!item.is_object() || item.size() != 4 || !item.contains("position") ||
        !item.contains("kind") || !item.contains("link") ||
        !item.contains("identity") || !item["position"].is_number_unsigned() ||
        !item["kind"].is_string() ||
        !(item["link"].is_null() || item["link"].is_string())) {
      throw Error{"recovery topology node is malformed"};
    }
    RouteNode node;
    node.position = item["position"].get<std::size_t>();
    node.kind = item["kind"].get<std::string>();
    if (item["link"].is_string()) {
      node.link = item["link"].get<std::string>();
    }
    node.identity = identity_from_json(item["identity"]);
    if (node.position != expected_position++ ||
        (node.kind == "directory") != !node.link.has_value() ||
        (node.kind != "directory" && node.kind != "symlink") ||
        (node.link.has_value() && node.link->empty())) {
      throw Error{"recovery topology node is inconsistent"};
    }
    result.nodes.push_back(std::move(node));
  }
  return result;
}

TransactionLock::TransactionLock(const std::filesystem::path& state_directory)
    : path_(state_directory / "state.lock") {
  ensure_private_directory(state_directory);
  bool created = true;
  descriptor_ =
      ::open(path_.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (descriptor_ < 0 && errno == EEXIST) {
    created = false;
    descriptor_ = ::open(path_.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  }
  if (descriptor_ < 0) {
    system_failure("could not open transaction lock", path_);
  }
  try {
    if (created && ::fchmod(descriptor_, 0600) != 0) {
      const int saved = errno;
      static_cast<void>(::close(descriptor_));
      descriptor_ = -1;
      errno = saved;
      system_failure("could not set transaction lock mode", path_);
    }
    struct stat opened {};
    if (::fstat(descriptor_, &opened) != 0) {
      const int saved = errno;
      static_cast<void>(::close(descriptor_));
      descriptor_ = -1;
      errno = saved;
      system_failure("could not inspect transaction lock", path_);
    }
    validate_private_file(opened, path_);
    binding_ = capture_binding(path_);
    if (identity(opened) != binding_.target) {
      static_cast<void>(::close(descriptor_));
      descriptor_ = -1;
      throw Error{"transaction lock changed while it was opened: " + path_.string()};
    }
    struct flock record_lock {};
    record_lock.l_type = F_WRLCK;
    record_lock.l_whence = SEEK_SET;
    record_lock.l_start = 0;
    record_lock.l_len = 0;
    int locked;
    do {
      locked = ::fcntl(descriptor_, F_SETLKW, &record_lock);
    } while (locked != 0 && errno == EINTR);
    if (locked != 0) {
      const int saved = errno;
      static_cast<void>(::close(descriptor_));
      descriptor_ = -1;
      errno = saved;
      system_failure("could not acquire transaction lock", path_);
    }
    validate();
  } catch (...) {
    if (descriptor_ >= 0) {
      struct flock unlock {};
      unlock.l_type = F_UNLCK;
      unlock.l_whence = SEEK_SET;
      unlock.l_start = 0;
      unlock.l_len = 0;
      static_cast<void>(::fcntl(descriptor_, F_SETLK, &unlock));
      static_cast<void>(::close(descriptor_));
      descriptor_ = -1;
    }
    throw;
  }
}

TransactionLock::~TransactionLock() {
  if (descriptor_ >= 0) {
    struct flock unlock {};
    unlock.l_type = F_UNLCK;
    unlock.l_whence = SEEK_SET;
    unlock.l_start = 0;
    unlock.l_len = 0;
    static_cast<void>(::fcntl(descriptor_, F_SETLK, &unlock));
    if (retained_alias_descriptor_ >= 0) {
      static_cast<void>(::close(retained_alias_descriptor_));
    }
    static_cast<void>(::close(descriptor_));
  }
}

void TransactionLock::validate() const {
  if (retained_alias_descriptor_ >= 0) {
    throw Error{"transaction lock encountered a physical alias: " + path_.string()};
  }
  struct stat opened {};
  if (::fstat(descriptor_, &opened) != 0) {
    system_failure("could not inspect transaction lock", path_);
  }
  validate_private_file(opened, path_);
  const auto current = capture_binding(path_);
  if (current != binding_ || identity(opened) != binding_.target) {
    throw Error{"transaction lock changed while held: " + path_.string()};
  }
}

} // namespace libtmux::mcp_swap::detail
