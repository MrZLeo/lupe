#include "lupe/session_ir.hpp"

#include <algorithm>
#include <string_view>

namespace lupe {

std::string_view role_name(Role role) {
  switch (role) {
  case Role::User:
    return "user";
  case Role::Assistant:
    return "assistant";
  case Role::System:
    return "system";
  case Role::Developer:
    return "developer";
  case Role::Agent:
    return "agent";
  case Role::Unknown:
    return "unknown";
  }
  return "unknown";
}

bool SessionParseResult::has_fatal_error() const {
  return std::ranges::any_of(diagnostics, [](const Diagnostic &diagnostic) {
    return diagnostic.severity == DiagnosticSeverity::Fatal;
  });
}

} // namespace lupe
