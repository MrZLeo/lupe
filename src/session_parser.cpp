#include "lupe/session_parser.hpp"

#include "lupe/format_detector.hpp"
#include "lupe/log_format.hpp"
#include "lupe/log_parser.hpp"

#include "lupe/session_ir.hpp"
#include "session_parser_internal.hpp"
#include "zstd_decode.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace lupe {
namespace detail {

void add_diagnostic(SessionParseResult &result, DiagnosticSeverity severity,
                    DiagnosticCode code, std::string message,
                    std::size_t source_line) {
  result.diagnostics.push_back(Diagnostic{
      .severity = severity,
      .code = code,
      .message = std::move(message),
      .source_line = source_line,
  });
}

RecordIR make_invalid_record(std::size_t sequence, std::size_t source_line,
                             std::string_view raw) {
  return RecordIR{
      .sequence = sequence,
      .source_line = source_line,
      .native_sequence = std::nullopt,
      .native_type = "invalid_json",
      .native_id = {},
      .native_parent_id = std::nullopt,
      .navigation_parent_id = std::nullopt,
      .timestamp = {},
      .raw_json = std::string{raw},
      .events = {},
  };
}

namespace {
Role generic_role(std::string_view value) {
  if (value == "user") {
    return Role::User;
  }
  if (value == "assistant") {
    return Role::Assistant;
  }
  if (value == "system") {
    return Role::System;
  }
  if (value == "developer") {
    return Role::Developer;
  }
  if (value == "agent") {
    return Role::Agent;
  }
  return Role::Unknown;
}

} // namespace

SessionParseResult parse_generic_session(std::string_view content) {
  SessionParseResult result;
  result.session.format = LogFormat::Generic;

  const ParseResult legacy = parse_log_content(content);
  for (const auto &error : legacy.errors) {
    add_diagnostic(result, DiagnosticSeverity::Error,
                   DiagnosticCode::InvalidJson, error);
  }

  for (const auto &message : legacy.messages) {
    RecordIR record{
        .sequence = result.session.records.size(),
        .source_line = message.source_line,
        .native_sequence = std::nullopt,
        .native_type = message.raw_type,
        .native_id = {},
        .native_parent_id = std::nullopt,
        .navigation_parent_id = std::nullopt,
        .timestamp = message.timestamp,
        .raw_json = {},
        .events = {},
    };

    MessageEvent event{
        .role = generic_role(message.role),
        .raw_role = message.role,
        .provider = {},
        .model = {},
        .phase = {},
        .content = {TextContent{.text = message.content}},
    };
    append_event(record, std::move(event));

    for (const auto &annotation : message.annotations) {
      append_event(record, MetadataEvent{
                               .name = "display_annotation",
                               .value = annotation,
                           });
    }
    result.session.records.push_back(std::move(record));
  }

  return result;
}

} // namespace detail

namespace {

struct DiagnosticIdentity {
  DiagnosticCode code;
  std::size_t source_line;
  std::string message;

  bool operator==(const DiagnosticIdentity &) const = default;
};

struct DiagnosticIdentityHash {
  std::size_t operator()(const DiagnosticIdentity &identity) const {
    std::size_t hash = std::hash<int>{}(static_cast<int>(identity.code));
    hash ^= std::hash<std::size_t>{}(identity.source_line)
          + 0x9e3779b9U
          + (hash << 6U)
          + (hash >> 2U);
    hash ^= std::hash<std::string>{}(identity.message)
          + 0x9e3779b9U
          + (hash << 6U)
          + (hash >> 2U);
    return hash;
  }
};

DiagnosticIdentity diagnostic_identity(const Diagnostic &diagnostic) {
  return DiagnosticIdentity{
      .code = diagnostic.code,
      .source_line = diagnostic.source_line,
      .message = diagnostic.message,
  };
}

void append_validation(SessionParseResult &result) {
  auto validation = validate_session(result.session);
  std::unordered_set<DiagnosticIdentity, DiagnosticIdentityHash>
      reported_diagnostics;
  reported_diagnostics.reserve(result.diagnostics.size() + validation.size());
  for (const auto &diagnostic : result.diagnostics) {
    reported_diagnostics.insert(diagnostic_identity(diagnostic));
  }
  for (auto &diagnostic : validation) {
    if (reported_diagnostics.insert(diagnostic_identity(diagnostic)).second) {
      result.diagnostics.push_back(std::move(diagnostic));
    }
  }
}

} // namespace

SessionParseResult
parse_session_content(std::string_view content, LogFormat format) {
  SessionParseResult result;
  bool detection_succeeded = false;
  if (format == LogFormat::Auto) {
    const std::optional<LogFormat> detected = detect_log_format(content);
    if (detected.has_value()) {
      format = *detected;
      detection_succeeded = true;
    } else {
      result.session.format = LogFormat::Auto;
      detail::add_diagnostic(
          result, DiagnosticSeverity::Fatal, DiagnosticCode::FormatMismatch,
          "could not detect the log format; specify it with --format "
          "(pi, codex, codex-exec, claudecode, deepseek-harness, or generic)");
    }
  }
  switch (format) {
  case LogFormat::Pi:
    result = detail::parse_pi_session(content);
    break;
  case LogFormat::Codex:
    result = detail::parse_codex_rollout(content);
    break;
  case LogFormat::CodexExec:
    result = detail::parse_codex_exec_stream(content);
    break;
  case LogFormat::ClaudeCode:
    result = detail::parse_claudecode_transcript(content);
    break;
  case LogFormat::DeepseekHarness:
    result = detail::parse_deepseek_harness_session(content);
    break;
  case LogFormat::Generic:
    result = detail::parse_generic_session(content);
    break;
  case LogFormat::Auto:
    break;
  }
  if (detection_succeeded && result.has_fatal_error()) {
    detail::add_diagnostic(
        result, DiagnosticSeverity::Info, DiagnosticCode::FormatMismatch,
        "content was auto-detected; pass --format to override");
  }

  append_validation(result);
  return result;
}

SessionParseResult
parse_session_file(const std::filesystem::path &path, LogFormat format) {
  const std::ifstream input{path, std::ios::binary};
  if (!input) {
    SessionParseResult result;
    result.session.format = format;
    result.session.source_path = path;
    detail::add_diagnostic(result, DiagnosticSeverity::Fatal,
                           DiagnosticCode::IoError,
                           "failed to open " + path.string());
    return result;
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  if (input.bad()) {
    SessionParseResult result;
    result.session.format = format;
    result.session.source_path = path;
    detail::add_diagnostic(result, DiagnosticSeverity::Fatal,
                           DiagnosticCode::IoError,
                           "failed while reading " + path.string());
    return result;
  }

  std::string content = buffer.str();
  bool dropped_torn_tail = false;
  if (detail::looks_like_zstd(content)) {
    // Session logs can be stored as concatenated Zstandard frames (for
    // example deepseek-harness session.jsonl.zstd).
    constexpr std::uint64_t kMaxDecompressedBytes =
        std::uint64_t{512} * 1024 * 1024;
    detail::ZstdDecodeResult decoded =
        detail::decompress_zstd_frames(content, kMaxDecompressedBytes);
    if (!decoded.error.empty()) {
      SessionParseResult result;
      result.session.format = format;
      result.session.source_path = path;
      detail::add_diagnostic(
          result, DiagnosticSeverity::Fatal, DiagnosticCode::IoError,
          "failed to decompress " + path.string() + ": " + decoded.error);
      return result;
    }
    dropped_torn_tail = decoded.dropped_torn_tail;
    content = std::move(decoded.text);
  }

  SessionParseResult result;
  const bool codex = format == LogFormat::Codex
                  || (format == LogFormat::Auto
                      && detect_log_format(content) == LogFormat::Codex);
  if (codex) {
    result = detail::parse_codex_rollout_file(path, content);
    append_validation(result);
  } else {
    result = parse_session_content(content, format);
  }
  if (dropped_torn_tail) {
    detail::add_diagnostic(
        result, DiagnosticSeverity::Warning, DiagnosticCode::IncompleteStream,
        "ignored the incomplete final zstd frame (torn append) in "
            + path.string());
  }
  result.session.source_path = path;
  return result;
}

} // namespace lupe
