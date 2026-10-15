#ifndef LUPE_MESSAGE_PROJECTION_HPP_
#define LUPE_MESSAGE_PROJECTION_HPP_

#include <optional>
#include <string>
#include <vector>

#include "lupe/log_message.hpp"
#include "lupe/session_ir.hpp"

namespace lupe {

struct DisplayOptions {
  std::optional<std::string> leaf_id;
  bool show_reasoning{false};
  bool show_metadata{false};
  bool show_unknown{false};
  bool show_compaction{true};
};

std::vector<LogMessage>
make_display_messages(const SessionIR &session,
                      const DisplayOptions &options = {});

std::string format_diagnostic(const Diagnostic &diagnostic);

std::string severity_name(DiagnosticSeverity severity);

} // namespace lupe

#endif // LUPE_MESSAGE_PROJECTION_HPP_
