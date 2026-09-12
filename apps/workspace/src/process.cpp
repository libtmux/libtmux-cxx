#include "services.hpp"

#include "process.hpp"

namespace libtmux::workspace::cli {
ChildOutput run_child(const std::vector<std::string>& arguments) {
  if (arguments.empty())
    throw Failure{2, "USAGE", "child command is empty"};
  libtmux::detail::ProcessRequest request;
  request.executable = arguments.front();
  request.timeout = std::chrono::seconds{5};
  for (auto argument = arguments.begin() + 1; argument != arguments.end(); ++argument)
    request.arguments.push_back({*argument});
  const auto reply = libtmux::detail::run_process(request);
  if (!reply)
    throw Failure{1, "PROCESS_FAILED", reply.error().diagnostic};
  if (reply->output_truncated)
    throw Failure{1, "OUTPUT_LIMIT", "child output exceeds 1 MiB"};
  const int status =
      std::holds_alternative<libtmux::detail::Exited>(reply->termination)
          ? std::get<libtmux::detail::Exited>(reply->termination).code
          : 128 + std::get<libtmux::detail::Signaled>(reply->termination).signal;
  const auto text = [](const std::vector<std::byte>& bytes) {
    if (bytes.empty())
      return std::string{};
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
  };
  return {status, text(reply->stdout_bytes), text(reply->stderr_bytes)};
}
} // namespace libtmux::workspace::cli
