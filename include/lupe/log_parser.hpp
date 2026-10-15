#ifndef PROJECT_LOG_PARSER_HPP_
#define PROJECT_LOG_PARSER_HPP_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "lupe/log_message.hpp"
#include "lupe/session_ir.hpp"

namespace lupe {
struct ParseResult {
  std::vector<LogMessage> messages;
  std::vector<std::string> errors;
  // Structured view of the same diagnostics flattened into `errors`, so
  // the UI can render severity and source line separately.
  std::vector<Diagnostic> diagnostics;
};

ParseResult parse_log_content(std::string_view content);
ParseResult parse_log_file(const std::filesystem::path &path);
} // namespace lupe

#endif // PROJECT_LOG_PARSER_HPP_
