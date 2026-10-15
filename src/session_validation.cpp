#include "lupe/log_format.hpp"
#include "lupe/session_ir.hpp"
#include "lupe/session_parser.hpp"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace lupe {
namespace {

Diagnostic make_diagnostic(DiagnosticSeverity severity, DiagnosticCode code,
                           std::string message, std::size_t source_line = 0) {
  return Diagnostic{
      .severity = severity,
      .code = code,
      .message = std::move(message),
      .source_line = source_line,
  };
}

std::optional<std::string_view> navigation_parent(const RecordIR &record) {
  if (record.navigation_parent_id) {
    if (record.navigation_parent_id->empty()) {
      return std::nullopt;
    }
    return *record.navigation_parent_id;
  }
  if (record.native_parent_id && !record.native_parent_id->empty()) {
    return *record.native_parent_id;
  }
  return std::nullopt;
}

} // namespace

std::vector<Diagnostic> validate_session(const SessionIR &session) {
  std::vector<Diagnostic> diagnostics;
  if (session.records.empty()) {
    diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Fatal,
                                          DiagnosticCode::EmptyInput,
                                          "input contains no records"));
    return diagnostics;
  }

  std::unordered_map<std::string, std::size_t> ids;
  for (std::size_t index = 0; index < session.records.size(); ++index) {
    const auto &record = session.records[index];
    if (record.native_id.empty()) {
      continue;
    }
    const auto [iterator, inserted] = ids.emplace(record.native_id, index);
    if (!inserted) {
      diagnostics.push_back(make_diagnostic(
          DiagnosticSeverity::Error, DiagnosticCode::DuplicateNativeId,
          "duplicate native record id: " + record.native_id,
          record.source_line));
    }
  }

  for (const auto &record : session.records) {
    if (record.native_parent_id
        && !record.native_parent_id->empty()
        && !ids.contains(*record.native_parent_id)) {
      diagnostics.push_back(make_diagnostic(
          DiagnosticSeverity::Warning, DiagnosticCode::MissingParent,
          "record parent is not present: " + *record.native_parent_id,
          record.source_line));
    }
    if (record.navigation_parent_id
        && !record.navigation_parent_id->empty()
        && record.navigation_parent_id != record.native_parent_id
        && !ids.contains(*record.navigation_parent_id)) {
      diagnostics.push_back(make_diagnostic(
          DiagnosticSeverity::Warning, DiagnosticCode::MissingParent,
          "record navigation parent is not present: "
              + *record.navigation_parent_id,
          record.source_line));
    }

    for (const auto &event : record.events) {
      if (const auto *call = std::get_if<ToolCallEvent>(&event.payload);
          call != nullptr && call->call_id.empty()) {
        diagnostics.push_back(make_diagnostic(
            DiagnosticSeverity::Warning, DiagnosticCode::EmptyCallId,
            "tool call has an empty call id", record.source_line));
      }
      if (const auto *tool_result =
              std::get_if<ToolResultEvent>(&event.payload);
          tool_result != nullptr && tool_result->call_id.empty()) {
        diagnostics.push_back(make_diagnostic(
            DiagnosticSeverity::Warning, DiagnosticCode::EmptyCallId,
            "tool result has an empty call id", record.source_line));
      }
    }
  }

  if (session.active_leaf_id
      && !session.active_leaf_id->empty()
      && !ids.contains(*session.active_leaf_id)) {
    diagnostics.push_back(make_diagnostic(
        DiagnosticSeverity::Error, DiagnosticCode::MissingParent,
        "active conversation leaf is not present: " + *session.active_leaf_id));
  }

  std::unordered_map<std::string, int> states;
  for (const auto &[id, unused_index] : ids) {
    static_cast<void>(unused_index);
    if (states[id] == 2) {
      continue;
    }

    std::vector<std::string> path;
    std::string current = id;
    while (!current.empty() && ids.contains(current)) {
      if (states[current] == 1) {
        const auto &record = session.records[ids.at(current)];
        diagnostics.push_back(make_diagnostic(
            DiagnosticSeverity::Error, DiagnosticCode::ParentCycle,
            "record parent graph contains a cycle at: " + current,
            record.source_line));
        break;
      }
      if (states[current] == 2) {
        break;
      }

      states[current] = 1;
      path.push_back(current);
      const auto &record = session.records[ids.at(current)];
      const auto parent = navigation_parent(record);
      if (!parent) {
        break;
      }
      current = *parent;
    }

    for (const auto &visited : path) {
      states[visited] = 2;
    }
  }

  return diagnostics;
}

std::vector<std::size_t>
select_conversation_records(const SessionIR &session,
                            std::optional<std::string_view> leaf_id) {
  if (session.format == LogFormat::Codex
      || session.format == LogFormat::CodexExec
      || session.format == LogFormat::DeepseekHarness
      || session.format == LogFormat::Generic) {
    std::vector<std::size_t> all(session.records.size());
    // std::iota: libc++ only declares std::ranges::iota when targeting
    // macOS 13.3+, but the release build targets macOS 13.0.
    std::iota(all.begin(), all.end(), std::size_t{0});
    return all;
  }

  std::unordered_map<std::string, std::size_t> ids;
  for (std::size_t index = 0; index < session.records.size(); ++index) {
    if (!session.records[index].native_id.empty()) {
      ids.emplace(session.records[index].native_id, index);
    }
  }

  std::string selected_leaf;
  if (leaf_id && !leaf_id->empty()) {
    selected_leaf = std::string{*leaf_id};
  } else if (session.active_leaf_id) {
    selected_leaf = *session.active_leaf_id;
  } else {
    std::vector<std::size_t> all(session.records.size());
    std::iota(all.begin(), all.end(), std::size_t{0});
    return all;
  }

  if (!ids.contains(selected_leaf)) {
    return {};
  }

  std::vector<std::size_t> reversed;
  std::unordered_set<std::string> visited;
  std::string current = std::move(selected_leaf);
  while (!current.empty()
         && ids.contains(current)
         && visited.insert(current).second) {
    const std::size_t index = ids.at(current);
    reversed.push_back(index);
    const auto &record = session.records[index];
    const auto parent = navigation_parent(record);
    if (!parent) {
      break;
    }
    current = *parent;
  }

  std::ranges::reverse(reversed);
  return reversed;
}

} // namespace lupe
