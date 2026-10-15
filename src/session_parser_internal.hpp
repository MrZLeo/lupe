#ifndef LUPE_SESSION_PARSER_INTERNAL_HPP_
#define LUPE_SESSION_PARSER_INTERNAL_HPP_

#include <span>
#include <string_view>

#include "lupe/session_ir.hpp"

namespace lupe::detail {

SessionParseResult parse_pi_session(std::string_view content);
struct CodexRolloutSource {
  std::string_view content;
  std::filesystem::path path;
};

SessionParseResult parse_codex_rollout(std::string_view content);
SessionParseResult
parse_codex_rollout_sources(std::span<const CodexRolloutSource> sources);
SessionParseResult parse_codex_rollout_file(const std::filesystem::path &path,
                                            std::string_view content);
SessionParseResult parse_codex_exec_stream(std::string_view content);
SessionParseResult parse_claudecode_transcript(std::string_view content);
SessionParseResult parse_deepseek_harness_session(std::string_view content);
SessionParseResult parse_generic_session(std::string_view content);

void add_diagnostic(SessionParseResult &result, DiagnosticSeverity severity,
                    DiagnosticCode code, std::string message,
                    std::size_t source_line = 0);

RecordIR make_invalid_record(std::size_t sequence, std::size_t source_line,
                             std::string_view raw);

} // namespace lupe::detail

#endif // LUPE_SESSION_PARSER_INTERNAL_HPP_
