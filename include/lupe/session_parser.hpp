#ifndef LUPE_SESSION_PARSER_HPP_
#define LUPE_SESSION_PARSER_HPP_

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include "lupe/session_ir.hpp"

namespace lupe {

SessionParseResult
parse_session_content(std::string_view content, LogFormat format);
SessionParseResult
parse_session_file(const std::filesystem::path &path, LogFormat format);

std::vector<Diagnostic> validate_session(const SessionIR &session);

std::vector<std::size_t> select_conversation_records(
    const SessionIR &session,
    std::optional<std::string_view> leaf_id = std::nullopt);

} // namespace lupe

#endif // LUPE_SESSION_PARSER_HPP_
