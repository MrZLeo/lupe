#ifndef LUPE_LOG_FORMAT_HPP_
#define LUPE_LOG_FORMAT_HPP_

#include <optional>
#include <string_view>

namespace lupe {

enum class LogFormat {
  Pi,
  Codex,
  ClaudeCode,
  Generic,
  CodexExec,
  DeepseekHarness,
  // Not an input format: ask the detector to classify the content.
  Auto,
};

std::optional<LogFormat> parse_log_format(std::string_view value);
std::string_view log_format_name(LogFormat format);

} // namespace lupe

#endif // LUPE_LOG_FORMAT_HPP_
