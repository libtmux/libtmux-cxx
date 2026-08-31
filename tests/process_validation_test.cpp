#include "process_validation.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {

using libtmux::detail::Argument;
using libtmux::detail::ProcessRequest;
using libtmux::detail::ProcessValidationFailure;
using libtmux::detail::ProcessValidationMode;
using libtmux::detail::validate_process_request;

ProcessRequest request() {
  ProcessRequest value;
  value.executable = "tmux";
  value.arguments = {Argument{"display-message"}};
  value.environment = {{"LIBTMUX_TEST", std::string{"value"}}};
  return value;
}

TEST(ProcessValidation, WindowsRejectsMalformedUtf8InEveryConvertedField) {
  using Mutation = std::function<void(ProcessRequest&)>;
  const std::vector<std::pair<std::string, Mutation>> cases{
      {"executable",
       [](ProcessRequest& value) {
#if defined(_WIN32)
         value.executable = std::filesystem::path{std::wstring(1U, wchar_t{0xd800})};
#else
         value.executable = std::filesystem::path{std::string{"\xff", 1U}};
#endif
       }},
      {"argument",
       [](ProcessRequest& value) { value.arguments.front().value = "\xc0\x80"; }},
      {"environment name",
       [](ProcessRequest& value) { value.environment.front().first = "\xed\xa0\x80"; }},
      {"environment value",
       [](ProcessRequest& value) {
         value.environment.front().second = "\xf4\x90\x80\x80";
       }},
  };

  for (const auto& [name, mutate] : cases) {
    SCOPED_TRACE(name);
    auto malformed = request();
    mutate(malformed);

    EXPECT_EQ(
        validate_process_request(malformed, ProcessValidationMode::windows).failure,
        ProcessValidationFailure::malformed_utf8);
  }
}

TEST(ProcessValidation, WindowsAcceptsValidMultibyteFields) {
  auto multibyte = request();
  const std::string unicode{"\xe9\x9b\xaa\xe2\x98\x83\xf0\x9f\x98\x80"};
#if defined(_WIN32)
  multibyte.executable = std::filesystem::path{L"\u96ea\u2603\U0001f600"};
#else
  multibyte.executable = std::filesystem::path{unicode};
#endif
  multibyte.arguments.front().value = unicode;
  multibyte.environment = {{unicode, unicode}};

  EXPECT_EQ(validate_process_request(multibyte, ProcessValidationMode::windows).failure,
            ProcessValidationFailure::none);
}

TEST(ProcessValidation, PosixAcceptsArbitraryNonNulBytes) {
  auto bytes = request();
  bytes.executable = std::filesystem::path{std::string{"\xff", 1U}};
  bytes.arguments.front().value = "\xc0\x80";
  bytes.environment = {{std::string{"\xed\xa0\x80"}, std::string{"\xf4\x90\x80\x80"}}};

  EXPECT_EQ(validate_process_request(bytes, ProcessValidationMode::posix).failure,
            ProcessValidationFailure::none);
}

TEST(ProcessValidation, MeasuresWindowsQuotingInUtf16CodeUnits) {
  auto measured = request();
  measured.arguments = {Argument{"a\\\"b"}, Argument{"\xf0\x9f\x98\x80"}};

  const auto validation =
      validate_process_request(measured, ProcessValidationMode::windows);

  EXPECT_EQ(validation.failure, ProcessValidationFailure::none);
  EXPECT_EQ(validation.windows_command_line_size, 20U);
}

TEST(ProcessValidation, EnforcesTheExactWindowsCommandLineLimit) {
  auto maximum = request();
  maximum.arguments = {Argument{std::string(32757U, 'a')}};
  const auto accepted =
      validate_process_request(maximum, ProcessValidationMode::windows);
  EXPECT_EQ(accepted.failure, ProcessValidationFailure::none);
  EXPECT_EQ(accepted.windows_command_line_size, 32766U);

  maximum.arguments.front().value.push_back('a');
  const auto refused =
      validate_process_request(maximum, ProcessValidationMode::windows);
  EXPECT_EQ(refused.failure, ProcessValidationFailure::command_line_too_long);
  EXPECT_EQ(refused.windows_command_line_size, 32767U);
}

} // namespace
