#include "lupe/log_format.hpp"
#include <optional>
#include <string_view>

namespace lupe {

std::optional<LogFormat> parse_log_format(std::string_view value) {
  if (value == "pi") {
    return LogFormat::Pi;
  }
  if (value == "codex") {
    return LogFormat::Codex;
  }
  if (value == "codex-exec") {
    return LogFormat::CodexExec;
  }
  if (value == "claudecode" || value == "claude-code") {
    return LogFormat::ClaudeCode;
  }
  if (value == "generic") {
    return LogFormat::Generic;
  }
  if (value == "deepseek-harness" || value == "dsh") {
    return LogFormat::DeepseekHarness;
  }
  if (value == "auto") {
    return LogFormat::Auto;
  }
  return std::nullopt;
}

std::string_view log_format_name(LogFormat format) {
  switch (format) {
  case LogFormat::Pi:
    return "pi";
  case LogFormat::Codex:
    return "codex";
  case LogFormat::CodexExec:
    return "codex-exec";
  case LogFormat::ClaudeCode:
    return "claudecode";
  case LogFormat::Generic:
    return "generic";
  case LogFormat::DeepseekHarness:
    return "deepseek-harness";
  case LogFormat::Auto:
    return "auto";
  }
  return "unknown";
}

} // namespace lupe
