#include "lupe/log_format.hpp"
#include "lupe/session_ir.hpp"
#include "session_parser_internal.hpp"

#include "json_helpers.hpp"
#include "jsonl_reader.hpp"

#include <cstddef>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <simdjson.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace lupe::detail {
namespace {

using simdjson::dom::element;
using simdjson::dom::element_type;

constexpr std::string_view kThreadStarted = "thread.started";
constexpr std::string_view kTurnStarted = "turn.started";
constexpr std::string_view kTurnCompleted = "turn.completed";
constexpr std::string_view kTurnFailed = "turn.failed";
constexpr std::string_view kItemStarted = "item.started";
constexpr std::string_view kItemUpdated = "item.updated";
constexpr std::string_view kItemCompleted = "item.completed";
constexpr std::string_view kError = "error";

bool is_known_record_type(std::string_view type) {
  return type == kThreadStarted
      || type == kTurnStarted
      || type == kTurnCompleted
      || type == kTurnFailed
      || type == kItemStarted
      || type == kItemUpdated
      || type == kItemCompleted
      || type == kError;
}

bool is_known_item_type(std::string_view type) {
  return type == "agent_message"
      || type == "reasoning"
      || type == "command_execution"
      || type == "file_change"
      || type == "mcp_tool_call"
      || type == "collab_tool_call"
      || type == "web_search"
      || type == "todo_list"
      || type == "error";
}

bool is_tool_item(std::string_view type) {
  return type == "command_execution"
      || type == "file_change"
      || type == "mcp_tool_call"
      || type == "collab_tool_call"
      || type == "web_search";
}

bool is_completion_only_item(std::string_view type) {
  return type == "agent_message" || type == "reasoning" || type == "error";
}

bool failed_status(std::string_view status) {
  return status == "failed"
      || status == "declined"
      || status == "error"
      || status == "cancelled"
      || status == "canceled";
}

std::string json_quote(std::string_view value) {
  std::string output;
  output.reserve(value.size() + 2);
  output.push_back('"');
  constexpr char kHex[] = "0123456789abcdef";
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\b':
      output += "\\b";
      break;
    case '\f':
      output += "\\f";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20U) {
        output += "\\u00";
        output.push_back(kHex[(character >> 4U) & 0x0fU]);
        output.push_back(kHex[character & 0x0fU]);
      } else {
        output.push_back(static_cast<char>(character));
      }
      break;
    }
  }
  output.push_back('"');
  return output;
}

void invalid_field_type(SessionParseResult &result, const RecordIR &record,
                        std::string_view field, std::string_view expected) {
  add_diagnostic(result, DiagnosticSeverity::Warning,
                 DiagnosticCode::InvalidFieldType,
                 "Codex Exec field `"
                     + std::string{field}
                     + "` must be "
                     + std::string{expected},
                 record.source_line);
}

void missing_field(SessionParseResult &result, const RecordIR &record,
                   std::string_view field) {
  add_diagnostic(
      result, DiagnosticSeverity::Warning, DiagnosticCode::MissingRequiredField,
      "Codex Exec record is missing required `" + std::string{field} + "`",
      record.source_line);
}

std::optional<std::string>
required_string(element owner, std::string_view pointer,
                std::string_view field_name, SessionParseResult &result,
                const RecordIR &record) {
  element field;
  if (!element_at(owner, pointer, field)) {
    missing_field(result, record, field_name);
    return std::nullopt;
  }

  std::string_view value;
  if (field.get_string().get(value)) {
    invalid_field_type(result, record, field_name, "a string");
    return std::nullopt;
  }
  if (value.empty()) {
    missing_field(result, record, field_name);
  }
  return std::string{value};
}

void merge_string_field(element owner, std::string_view pointer,
                        std::string_view field_name,
                        std::optional<std::string> &destination,
                        SessionParseResult &result, const RecordIR &record) {
  element field;
  if (!element_at(owner, pointer, field)) {
    return;
  }
  if (field.type() == element_type::NULL_VALUE) {
    destination.reset();
    return;
  }

  std::string_view value;
  if (field.get_string().get(value)) {
    invalid_field_type(result, record, field_name, "a string or null");
    return;
  }
  destination = std::string{value};
}

void merge_json_field(element owner, std::string_view pointer,
                      std::string_view field_name,
                      std::optional<element_type> expected_type,
                      std::optional<std::string> &destination,
                      SessionParseResult &result, const RecordIR &record) {
  element field;
  if (!element_at(owner, pointer, field)) {
    return;
  }
  if (field.type() == element_type::NULL_VALUE) {
    destination.reset();
    return;
  }
  if (expected_type && field.type() != *expected_type) {
    const std::string expected = *expected_type == element_type::ARRAY
                                   ? "an array or null"
                                   : "an object or null";
    invalid_field_type(result, record, field_name, expected);
    return;
  }
  destination = json_text(field);
}

void merge_json_value_field(element owner, std::string_view pointer,
                            std::optional<std::string> &destination) {
  element field;
  if (element_at(owner, pointer, field)) {
    // JSON null is a valid MCP argument value, not an absent optional field.
    destination = json_text(field);
  }
}

void
merge_int_field(element owner, std::string_view pointer,
                std::string_view field_name, std::optional<int> &destination,
                SessionParseResult &result, const RecordIR &record) {
  element field;
  if (!element_at(owner, pointer, field)) {
    return;
  }
  if (field.type() == element_type::NULL_VALUE) {
    destination.reset();
    return;
  }

  std::int64_t signed_value = 0;
  if (!field.get_int64().get(signed_value)
      && signed_value >= std::numeric_limits<int>::min()
      && signed_value <= std::numeric_limits<int>::max()) {
    destination = static_cast<int>(signed_value);
    return;
  }

  std::uint64_t unsigned_value = 0;
  if (!field.get_uint64().get(unsigned_value)
      && unsigned_value
             <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    destination = static_cast<int>(unsigned_value);
    return;
  }

  invalid_field_type(result, record, field_name,
                     "an integer in the supported range or null");
}

std::optional<std::uint64_t>
checked_uint(element owner, std::string_view pointer,
             std::string_view field_name, SessionParseResult &result,
             const RecordIR &record) {
  element field;
  if (!element_at(owner, pointer, field)) {
    return std::nullopt;
  }
  if (field.type() == element_type::NULL_VALUE) {
    return std::nullopt;
  }

  if (const auto value = uint_at(owner, pointer)) {
    return value;
  }
  invalid_field_type(result, record, field_name,
                     "a non-negative integer or null");
  return std::nullopt;
}

std::optional<std::uint64_t>
required_uint(element owner, std::string_view pointer,
              std::string_view field_name, SessionParseResult &result,
              const RecordIR &record) {
  element field;
  if (!element_at(owner, pointer, field)) {
    missing_field(result, record, field_name);
    return std::nullopt;
  }
  return checked_uint(owner, pointer, field_name, result, record);
}

struct ItemSnapshot {
  std::string native_id;
  std::string type;
  std::optional<std::string> status;
  std::optional<std::string> text;
  std::optional<std::string> message;
  std::optional<std::string> command;
  std::optional<std::string> aggregated_output;
  std::optional<int> exit_code;
  std::optional<std::string> changes_json;
  std::optional<std::string> server;
  std::optional<std::string> tool;
  std::optional<std::string> arguments_json;
  std::optional<std::string> result_json;
  std::optional<std::string> error_json;
  std::optional<std::string> sender_thread_id;
  std::optional<std::string> receiver_thread_ids_json;
  std::optional<std::string> prompt;
  std::optional<std::string> agents_states_json;
  std::optional<std::string> query;
  std::optional<std::string> action_json;
  std::optional<std::string> todo_items_json;
};

struct ItemState {
  std::string correlation_id;
  ItemSnapshot snapshot;
  bool saw_started{false};
  bool saw_updated{false};
  bool saw_completed{false};
  bool call_emitted{false};
  std::optional<std::size_t> call_record_index;
  std::optional<std::size_t> call_event_index;
  bool result_emitted{false};
  bool content_emitted{false};
  bool todo_emitted{false};
  std::string last_todo_value;
  bool incomplete_reported{false};
  std::size_t started_line{0};
};

struct TurnState {
  std::string correlation_id;
  bool active{false};
  bool terminal{false};
  bool inferred{false};
  bool incomplete_reported{false};
  std::size_t started_line{0};
  std::size_t next_item_index{0};
  std::unordered_map<std::string, ItemState> items;
};

struct ParserState {
  std::size_t run_index{0};
  std::size_t next_turn_index{0};
  std::size_t next_prelude_item_index{0};
  bool saw_thread{false};
  std::string thread_id;
  std::optional<TurnState> turn;
  std::unordered_set<std::string> reported_thread_ids;
};

std::string run_correlation(const ParserState &state) {
  return "codex-exec:r" + std::to_string(state.run_index);
}

std::string next_turn_correlation(ParserState &state) {
  return run_correlation(state)
       + ":t"
       + std::to_string(state.next_turn_index++);
}

std::string next_item_correlation(TurnState &turn) {
  return turn.correlation_id + ":i" + std::to_string(turn.next_item_index++);
}

RecordIR make_record(const JsonlLine &line, std::string native_type) {
  return RecordIR{
      .sequence = line.sequence,
      .source_line = line.source_line,
      .native_sequence = std::nullopt,
      .native_type = std::move(native_type),
      // Exec lifecycle IDs repeat across started/updated/completed records.
      // Keep them on events, not on RecordIR where IDs must be globally unique.
      .native_id = {},
      .native_parent_id = std::nullopt,
      .navigation_parent_id = std::nullopt,
      .timestamp = {},
      .raw_json = std::string{line.raw},
      .events = {},
  };
}

ExecutionPhase item_phase(std::string_view record_type) {
  if (record_type == kItemStarted) {
    return ExecutionPhase::Started;
  }
  if (record_type == kItemUpdated) {
    return ExecutionPhase::Updated;
  }
  if (record_type == kItemCompleted) {
    return ExecutionPhase::Completed;
  }
  return ExecutionPhase::Unknown;
}

void merge_item_snapshot(element item, ItemState &state,
                         const std::string &native_id,
                         const std::string &native_type,
                         SessionParseResult &result, const RecordIR &record) {
  if (!native_id.empty()) {
    state.snapshot.native_id = native_id;
  }
  if (!native_type.empty()) {
    if (!state.snapshot.type.empty() && state.snapshot.type != native_type) {
      add_diagnostic(result, DiagnosticSeverity::Warning,
                     DiagnosticCode::InvalidLifecycle,
                     "Codex Exec item `"
                         + state.snapshot.native_id
                         + "` changes type from `"
                         + state.snapshot.type
                         + "` to `"
                         + native_type
                         + "`",
                     record.source_line);
    } else {
      state.snapshot.type = native_type;
    }
  }

  const std::string_view type = state.snapshot.type;
  if (type == "agent_message" || type == "reasoning") {
    merge_string_field(item, "/text", "item.text", state.snapshot.text, result,
                       record);
    return;
  }
  if (type == "error") {
    merge_string_field(item, "/message", "item.message", state.snapshot.message,
                       result, record);
    return;
  }
  if (type == "command_execution") {
    merge_string_field(item, "/status", "item.status", state.snapshot.status,
                       result, record);
    merge_string_field(item, "/command", "item.command", state.snapshot.command,
                       result, record);
    merge_string_field(item, "/aggregated_output", "item.aggregated_output",
                       state.snapshot.aggregated_output, result, record);
    merge_int_field(item, "/exit_code", "item.exit_code",
                    state.snapshot.exit_code, result, record);
    return;
  }
  if (type == "file_change") {
    merge_string_field(item, "/status", "item.status", state.snapshot.status,
                       result, record);
    merge_json_field(item, "/changes", "item.changes", element_type::ARRAY,
                     state.snapshot.changes_json, result, record);
    return;
  }
  if (type == "mcp_tool_call") {
    merge_string_field(item, "/status", "item.status", state.snapshot.status,
                       result, record);
    merge_string_field(item, "/server", "item.server", state.snapshot.server,
                       result, record);
    merge_string_field(item, "/tool", "item.tool", state.snapshot.tool, result,
                       record);
    merge_json_value_field(item, "/arguments", state.snapshot.arguments_json);
    merge_json_field(item, "/result", "item.result", std::nullopt,
                     state.snapshot.result_json, result, record);
    merge_json_field(item, "/error", "item.error", std::nullopt,
                     state.snapshot.error_json, result, record);
    return;
  }
  if (type == "collab_tool_call") {
    merge_string_field(item, "/status", "item.status", state.snapshot.status,
                       result, record);
    merge_string_field(item, "/tool", "item.tool", state.snapshot.tool, result,
                       record);
    merge_string_field(item, "/sender_thread_id", "item.sender_thread_id",
                       state.snapshot.sender_thread_id, result, record);
    merge_json_field(item, "/receiver_thread_ids", "item.receiver_thread_ids",
                     element_type::ARRAY,
                     state.snapshot.receiver_thread_ids_json, result, record);
    merge_string_field(item, "/prompt", "item.prompt", state.snapshot.prompt,
                       result, record);
    merge_json_field(item, "/agents_states", "item.agents_states", std::nullopt,
                     state.snapshot.agents_states_json, result, record);
    return;
  }
  if (type == "web_search") {
    merge_string_field(item, "/query", "item.query", state.snapshot.query,
                       result, record);
    merge_json_field(item, "/action", "item.action", std::nullopt,
                     state.snapshot.action_json, result, record);
    return;
  }
  if (type == "todo_list") {
    merge_json_field(item, "/items", "item.items", element_type::ARRAY,
                     state.snapshot.todo_items_json, result, record);
  }
}

std::string item_call_id(const ItemState &state) {
  // Exec restarts its item counter for every invocation (including resume), so
  // the provider item ID alone is not a stable call correlation key.
  return state.correlation_id;
}

std::string collab_input(const ItemSnapshot &snapshot) {
  std::string input = "{";
  bool needs_comma = false;
  const auto append_member = [&](std::string_view name,
                                 const std::string &json_value,
                                 std::string &destination, bool &comma) {
    if (comma) {
      destination.push_back(',');
    }
    destination += json_quote(name);
    destination.push_back(':');
    destination += json_value;
    comma = true;
  };

  if (snapshot.sender_thread_id) {
    append_member("sender_thread_id", json_quote(*snapshot.sender_thread_id),
                  input, needs_comma);
  }
  if (snapshot.receiver_thread_ids_json) {
    append_member("receiver_thread_ids", *snapshot.receiver_thread_ids_json,
                  input, needs_comma);
  }
  if (snapshot.prompt) {
    append_member("prompt", json_quote(*snapshot.prompt), input, needs_comma);
  }
  input.push_back('}');
  return input;
}

std::string web_search_input(const ItemSnapshot &snapshot) {
  std::string input = "{";
  if (snapshot.query) {
    input += "\"query\":";
    input += json_quote(*snapshot.query);
  }
  if (snapshot.action_json) {
    if (snapshot.query) {
      input.push_back(',');
    }
    input += "\"action\":";
    input += *snapshot.action_json;
  }
  input.push_back('}');
  return input;
}

ToolCallEvent make_tool_call(const ItemState &state) {
  const ItemSnapshot &snapshot = state.snapshot;
  ToolCallEvent call{
      .call_id = item_call_id(state),
      .name = {},
      .name_space = {},
      .input = {},
      .input_is_json = false,
  };

  if (snapshot.type == "command_execution") {
    call.name = "command_execution";
    call.name_space = "codex";
    call.input = snapshot.command.value_or("");
  } else if (snapshot.type == "file_change") {
    call.name = "file_change";
    call.name_space = "codex";
    call.input = snapshot.changes_json.value_or("[]");
    call.input_is_json = true;
  } else if (snapshot.type == "mcp_tool_call") {
    call.name = snapshot.tool.value_or("");
    call.name_space = snapshot.server.value_or("");
    call.input = snapshot.arguments_json.value_or("{}");
    call.input_is_json = true;
  } else if (snapshot.type == "collab_tool_call") {
    call.name = snapshot.tool.value_or("");
    call.name_space = "codex.collab";
    call.input = collab_input(snapshot);
    call.input_is_json = true;
  } else if (snapshot.type == "web_search") {
    call.name = "web_search";
    call.name_space = "codex";
    if (snapshot.action_json) {
      call.input = web_search_input(snapshot);
      call.input_is_json = true;
    } else {
      call.input = snapshot.query.value_or("");
    }
  }

  return call;
}

void refresh_tool_call(SessionParseResult &result, const ItemState &state) {
  if (!state.call_record_index
      || !state.call_event_index
      || *state.call_record_index >= result.session.records.size()) {
    return;
  }
  auto &events = result.session.records[*state.call_record_index].events;
  if (*state.call_event_index >= events.size()) {
    return;
  }
  if (auto *call = std::get_if<ToolCallEvent>(
          &events[*state.call_event_index].payload)) {
    *call = make_tool_call(state);
  }
}

std::string file_change_summary(const std::string &json) {
  simdjson::dom::parser parser;
  const simdjson::padded_string padded{json};
  element root;
  if (parser.parse(padded).get(root) || root.type() != element_type::ARRAY) {
    return json;
  }

  simdjson::dom::array changes;
  if (root.get_array().get(changes)) {
    return json;
  }

  std::string summary;
  for (const element change : changes) {
    if (!summary.empty()) {
      summary.push_back('\n');
    }
    if (change.type() != element_type::OBJECT) {
      summary += json_text(change);
      continue;
    }
    const std::string kind = string_at(change, "/kind").value_or("update");
    const std::string path = string_at(change, "/path").value_or("");
    summary += kind;
    if (!path.empty()) {
      summary.push_back(' ');
      summary += path;
    }
  }
  return summary;
}

void append_mcp_content(element value, std::vector<ContentBlock> &output) {
  if (value.type() == element_type::STRING) {
    std::string_view text;
    if (!value.get_string().get(text)) {
      output.emplace_back(TextContent{.text = std::string{text}});
    }
    return;
  }

  if (value.type() == element_type::ARRAY) {
    simdjson::dom::array blocks;
    if (value.get_array().get(blocks)) {
      return;
    }
    for (const element block : blocks) {
      if (block.type() == element_type::OBJECT
          && string_at(block, "/type").value_or("") == "text") {
        output.emplace_back(TextContent{
            .text = string_at(block, "/text").value_or(""),
        });
      } else {
        output.emplace_back(UnknownContent{
            .native_type = block.type() == element_type::OBJECT
                             ? string_at(block, "/type").value_or("mcp_content")
                             : std::string{"mcp_content"},
            .json = json_text(block),
        });
      }
    }
    return;
  }

  output.emplace_back(UnknownContent{
      .native_type = "mcp_content",
      .json = json_text(value),
  });
}

void append_mcp_result_json(const std::string &json,
                            std::vector<ContentBlock> &output) {
  simdjson::dom::parser parser;
  const simdjson::padded_string padded{json};
  element root;
  if (parser.parse(padded).get(root)) {
    output.emplace_back(
        UnknownContent{.native_type = "mcp_result", .json = json});
    return;
  }

  if (root.type() != element_type::OBJECT) {
    append_mcp_content(root, output);
    return;
  }

  bool found = false;
  element content;
  if (element_at(root, "/content", content)
      && content.type() != element_type::NULL_VALUE) {
    append_mcp_content(content, output);
    found = true;
  }
  element structured_content;
  if (element_at(root, "/structured_content", structured_content)
      && structured_content.type() != element_type::NULL_VALUE) {
    output.emplace_back(UnknownContent{
        .native_type = "structured_content",
        .json = json_text(structured_content),
    });
    found = true;
  }
  if (!found) {
    output.emplace_back(UnknownContent{
        .native_type = "mcp_result",
        .json = json_text(root),
    });
  }
}

std::string error_message_json(const std::string &json) {
  simdjson::dom::parser parser;
  const simdjson::padded_string padded{json};
  element root;
  if (parser.parse(padded).get(root)) {
    return json;
  }
  if (root.type() == element_type::STRING) {
    std::string_view text;
    if (!root.get_string().get(text)) {
      return std::string{text};
    }
  }
  if (root.type() == element_type::OBJECT) {
    if (const auto message = string_at(root, "/message")) {
      return *message;
    }
  }
  return json_text(root);
}

ToolResultEvent make_tool_result(const ItemState &state) {
  const ItemSnapshot &snapshot = state.snapshot;
  ToolResultEvent result{
      .call_id = item_call_id(state),
      .name = {},
      .output = {},
      .is_error = failed_status(snapshot.status.value_or("")),
      .exit_code = std::nullopt,
  };

  if (snapshot.type == "command_execution") {
    result.name = "command_execution";
    result.output.emplace_back(
        TextContent{.text = snapshot.aggregated_output.value_or("")});
    result.exit_code = snapshot.exit_code;
    if (snapshot.exit_code && *snapshot.exit_code != 0) {
      result.is_error = true;
    }
  } else if (snapshot.type == "file_change") {
    result.name = "file_change";
    result.output.emplace_back(TextContent{
        .text = snapshot.changes_json
                  ? file_change_summary(*snapshot.changes_json)
                  : std::string{},
    });
  } else if (snapshot.type == "mcp_tool_call") {
    result.name = snapshot.tool.value_or("");
    if (snapshot.result_json) {
      append_mcp_result_json(*snapshot.result_json, result.output);
    }
    if (snapshot.error_json) {
      result.output.emplace_back(
          TextContent{.text = error_message_json(*snapshot.error_json)});
      result.is_error = true;
    }
  } else if (snapshot.type == "collab_tool_call") {
    result.name = snapshot.tool.value_or("");
    if (snapshot.agents_states_json) {
      result.output.emplace_back(
          TextContent{.text = *snapshot.agents_states_json});
    }
  }
  return result;
}

template <typename Value>
void
require_snapshot_field(const std::optional<Value> &value,
                       std::string_view field_name, SessionParseResult &result,
                       const RecordIR &record) {
  if (!value) {
    missing_field(result, record, field_name);
  }
}

void append_item_semantics(RecordIR &record, ItemState &state,
                           ExecutionPhase phase, SessionParseResult &result) {
  const std::string &type = state.snapshot.type;
  const bool completed = phase == ExecutionPhase::Completed;

  if (is_tool_item(type) && state.call_emitted) {
    refresh_tool_call(result, state);
  }
  if (is_tool_item(type) && !state.call_emitted) {
    if (type == "command_execution") {
      require_snapshot_field(state.snapshot.command, "item.command", result,
                             record);
      require_snapshot_field(state.snapshot.status, "item.status", result,
                             record);
    } else if (type == "file_change") {
      require_snapshot_field(state.snapshot.changes_json, "item.changes",
                             result, record);
      require_snapshot_field(state.snapshot.status, "item.status", result,
                             record);
    } else if (type == "mcp_tool_call") {
      require_snapshot_field(state.snapshot.server, "item.server", result,
                             record);
      require_snapshot_field(state.snapshot.tool, "item.tool", result, record);
      require_snapshot_field(state.snapshot.arguments_json, "item.arguments",
                             result, record);
      require_snapshot_field(state.snapshot.status, "item.status", result,
                             record);
    } else if (type == "collab_tool_call") {
      require_snapshot_field(state.snapshot.tool, "item.tool", result, record);
      require_snapshot_field(state.snapshot.sender_thread_id,
                             "item.sender_thread_id", result, record);
      require_snapshot_field(state.snapshot.receiver_thread_ids_json,
                             "item.receiver_thread_ids", result, record);
      require_snapshot_field(state.snapshot.status, "item.status", result,
                             record);
    } else if (type == "web_search") {
      require_snapshot_field(state.snapshot.query, "item.query", result,
                             record);
    }
    state.call_record_index = result.session.records.size();
    state.call_event_index = record.events.size();
    append_event(record, make_tool_call(state));
    state.call_emitted = true;
  }

  if (completed
      && type != "web_search"
      && (type == "command_execution"
          || type == "file_change"
          || type == "mcp_tool_call"
          || type == "collab_tool_call")
      && !state.result_emitted) {
    require_snapshot_field(state.snapshot.status, "item.status", result,
                           record);
    if (type == "command_execution") {
      require_snapshot_field(state.snapshot.aggregated_output,
                             "item.aggregated_output", result, record);
    } else if (type == "collab_tool_call") {
      require_snapshot_field(state.snapshot.agents_states_json,
                             "item.agents_states", result, record);
    }
    append_event(record, make_tool_result(state));
    state.result_emitted = true;
  }

  if (completed && type == "agent_message" && !state.content_emitted) {
    require_snapshot_field(state.snapshot.text, "item.text", result, record);
    append_event(record, MessageEvent{
                             .role = Role::Assistant,
                             .raw_role = "assistant",
                             .provider = {},
                             .model = {},
                             .phase = {},
                             .content = {TextContent{
                                 .text = state.snapshot.text.value_or(""),
                             }},
                         });
    state.content_emitted = true;
  } else if (completed && type == "reasoning" && !state.content_emitted) {
    require_snapshot_field(state.snapshot.text, "item.text", result, record);
    append_event(record, ReasoningEvent{
                             .summary = {},
                             .content = state.snapshot.text.value_or(""),
                             .encrypted = false,
                         });
    state.content_emitted = true;
  }

  if (type == "todo_list") {
    if (!state.todo_emitted) {
      require_snapshot_field(state.snapshot.todo_items_json, "item.items",
                             result, record);
    }
    const std::string value = state.snapshot.todo_items_json.value_or("null");
    if (!state.todo_emitted || state.last_todo_value != value) {
      append_event(record, MetadataEvent{
                               .name = "codex_exec.todo_list",
                               .value = value,
                           });
      state.todo_emitted = true;
      state.last_todo_value = value;
    }
  } else if (!is_known_item_type(type)) {
    append_event(
        record,
        UnknownEvent{
            .native_type = type.empty() ? std::string{"unknown_item"} : type,
        });
  }
}

void report_incomplete(SessionParseResult &result, TurnState &turn,
                       std::size_t fallback_line) {
  if (turn.active && !turn.incomplete_reported) {
    add_diagnostic(
        result, DiagnosticSeverity::Warning, DiagnosticCode::IncompleteStream,
        "Codex Exec stream ended or crossed a boundary with an open turn",
        turn.started_line == 0 ? fallback_line : turn.started_line);
    turn.incomplete_reported = true;
  }

  // `turn.items` is an unordered_map, so carry the item key along and use it
  // as the final tiebreaker to keep diagnostic order deterministic.
  std::vector<std::pair<std::string_view, ItemState *>> pending_items;
  pending_items.reserve(turn.items.size());
  for (auto &[key, item] : turn.items) {
    if (item.saw_started && !item.saw_completed && !item.incomplete_reported) {
      pending_items.emplace_back(key, &item);
    }
  }
  std::ranges::sort(pending_items, [](const auto &left, const auto &right) {
    const ItemState *left_item = left.second;
    const ItemState *right_item = right.second;
    if (left_item->started_line != right_item->started_line) {
      return left_item->started_line < right_item->started_line;
    }
    if (left_item->correlation_id != right_item->correlation_id) {
      return left_item->correlation_id < right_item->correlation_id;
    }
    return left.first < right.first;
  });

  for (const auto &entry : pending_items) {
    ItemState *item = entry.second;
    add_diagnostic(
        result, DiagnosticSeverity::Warning, DiagnosticCode::IncompleteStream,
        "Codex Exec stream ended or crossed a boundary before item `"
            + item->snapshot.native_id
            + "` completed",
        item->started_line == 0 ? fallback_line : item->started_line);
    item->incomplete_reported = true;
  }
}

TurnState &
start_turn(ParserState &state, bool inferred, std::size_t source_line) {
  state.turn = TurnState{
      .correlation_id = next_turn_correlation(state),
      .active = true,
      .terminal = false,
      .inferred = inferred,
      .incomplete_reported = false,
      .started_line = source_line,
      .next_item_index = 0,
      .items = {},
  };
  return *state.turn;
}

TurnState &turn_for_item(ParserState &state, SessionParseResult &result,
                         const RecordIR &record, std::string_view native_id,
                         std::string_view native_type) {
  if (state.turn && state.turn->active) {
    return *state.turn;
  }

  if (state.turn && state.turn->terminal && !native_id.empty()) {
    const auto iterator = state.turn->items.find(std::string{native_id});
    if (iterator != state.turn->items.end()
        && !iterator->second.saw_completed) {
      add_diagnostic(result, DiagnosticSeverity::Warning,
                     DiagnosticCode::InvalidLifecycle,
                     "Codex Exec item `"
                         + std::string{native_id}
                         + "` continues after its turn is terminal",
                     record.source_line);
      return *state.turn;
    }
  }

  if (state.turn) {
    report_incomplete(result, *state.turn, record.source_line);
  }
  add_diagnostic(result, DiagnosticSeverity::Warning,
                 DiagnosticCode::InvalidLifecycle,
                 "Codex Exec item `"
                     + std::string{native_type}
                     + "` appeared outside an active turn",
                 record.source_line);
  return start_turn(state, true, record.source_line);
}

ItemState &
select_item_state(TurnState &turn, std::string key,
                  const std::string &native_id, const std::string &native_type,
                  ExecutionPhase phase, SessionParseResult &result,
                  const RecordIR &record) {
  auto iterator = turn.items.find(key);

  if (phase == ExecutionPhase::Started
      && iterator != turn.items.end()
      && iterator->second.saw_completed) {
    ItemState replacement;
    replacement.correlation_id = next_item_correlation(turn);
    replacement.snapshot.native_id = native_id;
    replacement.snapshot.type = native_type;
    iterator->second = std::move(replacement);
    return iterator->second;
  }

  if (iterator == turn.items.end()) {
    ItemState fresh;
    fresh.correlation_id = next_item_correlation(turn);
    fresh.snapshot.native_id = native_id;
    fresh.snapshot.type = native_type;
    iterator = turn.items.emplace(std::move(key), std::move(fresh)).first;

    if (phase == ExecutionPhase::Updated && native_type != "todo_list") {
      add_diagnostic(result, DiagnosticSeverity::Warning,
                     DiagnosticCode::InvalidLifecycle,
                     "Codex Exec item `"
                         + native_id
                         + "` was updated before it was started",
                     record.source_line);
    }
    return iterator->second;
  }

  ItemState &item = iterator->second;
  if (phase == ExecutionPhase::Started
      && item.saw_started
      && !item.saw_completed) {
    add_diagnostic(
        result, DiagnosticSeverity::Warning, DiagnosticCode::InvalidLifecycle,
        "Codex Exec item `" + native_id + "` was started more than once",
        record.source_line);
  } else if (phase == ExecutionPhase::Updated && item.saw_completed) {
    add_diagnostic(
        result, DiagnosticSeverity::Warning, DiagnosticCode::InvalidLifecycle,
        "Codex Exec item `" + native_id + "` was updated after completion",
        record.source_line);
  } else if (phase == ExecutionPhase::Completed && item.saw_completed) {
    add_diagnostic(
        result, DiagnosticSeverity::Warning, DiagnosticCode::InvalidLifecycle,
        "Codex Exec item `" + native_id + "` completed more than once",
        record.source_line);
  }
  return item;
}

void append_item_execution(RecordIR &record, const ItemState &state,
                           ExecutionPhase phase) {
  std::string message = state.snapshot.message.value_or("");
  if (message.empty() && state.snapshot.error_json) {
    message = error_message_json(*state.snapshot.error_json);
  }
  append_event(record, ExecutionEvent{
                           .subject = ExecutionSubject::Item,
                           .phase = phase,
                           .correlation_id = state.correlation_id,
                           .native_id = state.snapshot.native_id,
                           .native_type = state.snapshot.type,
                           .status = state.snapshot.status.value_or(""),
                           .message = std::move(message),
                           .terminal = phase == ExecutionPhase::Completed,
                       });
}

void
append_rejected_item_execution(RecordIR &record, const ItemState &state,
                               element item, std::string_view native_type) {
  append_event(record, ExecutionEvent{
                           .subject = ExecutionSubject::Item,
                           .phase = ExecutionPhase::Unknown,
                           .correlation_id = state.correlation_id,
                           .native_id = state.snapshot.native_id,
                           .native_type = std::string{native_type},
                           .status = string_at(item, "/status").value_or(""),
                           .message = string_at(item, "/message").value_or(""),
                           .terminal = false,
                       });
}

void parse_item_record(element root, RecordIR &record,
                       SessionParseResult &result, ParserState &state) {
  const ExecutionPhase phase = item_phase(record.native_type);
  element item;
  if (!element_at(root, "/item", item)) {
    missing_field(result, record, "item");
    append_event(record, ExecutionEvent{
                             .subject = ExecutionSubject::Item,
                             .phase = phase,
                             .correlation_id = run_correlation(state)
                                             + ":unbound:i"
                                             + std::to_string(record.sequence),
                             .native_id = {},
                             .native_type = {},
                             .status = {},
                             .message = {},
                             .terminal = phase == ExecutionPhase::Completed,
                         });
    return;
  }
  if (item.type() != element_type::OBJECT) {
    invalid_field_type(result, record, "item", "an object");
    append_event(record, ExecutionEvent{
                             .subject = ExecutionSubject::Item,
                             .phase = phase,
                             .correlation_id = run_correlation(state)
                                             + ":unbound:i"
                                             + std::to_string(record.sequence),
                             .native_id = {},
                             .native_type = {},
                             .status = {},
                             .message = {},
                             .terminal = phase == ExecutionPhase::Completed,
                         });
    return;
  }

  const std::string native_id =
      required_string(item, "/id", "item.id", result, record).value_or("");
  const std::string native_type =
      required_string(item, "/type", "item.type", result, record).value_or("");
  if (is_completion_only_item(native_type)
      && phase != ExecutionPhase::Completed) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::InvalidLifecycle,
                   "Codex Exec item type `"
                       + native_type
                       + "` is only valid on item.completed",
                   record.source_line);
  }

  const bool prelude_error =
      native_type == "error" && (!state.turn || !state.turn->active);
  if (prelude_error) {
    ItemState prelude;
    prelude.correlation_id = run_correlation(state)
                           + ":prelude:i"
                           + std::to_string(state.next_prelude_item_index++);
    prelude.snapshot.native_id = native_id;
    prelude.snapshot.type = native_type;
    merge_item_snapshot(item, prelude, native_id, native_type, result, record);
    prelude.saw_started = phase == ExecutionPhase::Started;
    prelude.saw_updated = phase == ExecutionPhase::Updated;
    prelude.saw_completed = phase == ExecutionPhase::Completed;
    append_item_execution(record, prelude, phase);
    return;
  }

  TurnState &turn =
      turn_for_item(state, result, record, native_id, native_type);
  const std::string key = native_id.empty() || native_type.empty()
                            ? "\x1fmissing:" + std::to_string(record.sequence)
                            : native_id;
  ItemState &item_state = select_item_state(turn, key, native_id, native_type,
                                            phase, result, record);
  const bool type_conflict = !native_type.empty()
                          && !item_state.snapshot.type.empty()
                          && item_state.snapshot.type != native_type;
  const bool continues_after_completion =
      item_state.saw_completed && phase != ExecutionPhase::Started;
  if (type_conflict || continues_after_completion) {
    if (type_conflict) {
      add_diagnostic(result, DiagnosticSeverity::Warning,
                     DiagnosticCode::InvalidLifecycle,
                     "Codex Exec item `"
                         + native_id
                         + "` changes type from `"
                         + item_state.snapshot.type
                         + "` to `"
                         + native_type
                         + "`",
                     record.source_line);
    }
    append_rejected_item_execution(record, item_state, item, native_type);
    return;
  }
  merge_item_snapshot(item, item_state, native_id, native_type, result, record);

  if (phase == ExecutionPhase::Started) {
    item_state.saw_started = true;
    if (item_state.started_line == 0) {
      item_state.started_line = record.source_line;
    }
  } else if (phase == ExecutionPhase::Updated) {
    item_state.saw_updated = true;
  } else if (phase == ExecutionPhase::Completed) {
    item_state.saw_completed = true;
  }

  if (phase == ExecutionPhase::Completed
      && item_state.snapshot.type == "error") {
    require_snapshot_field(item_state.snapshot.message, "item.message", result,
                           record);
  }
  append_item_execution(record, item_state, phase);
  append_item_semantics(record, item_state, phase, result);
}

std::string record_error_message(element root, SessionParseResult &result,
                                 const RecordIR &record, bool nested_error) {
  const std::string_view pointer =
      nested_error ? std::string_view{"/error"} : std::string_view{"/message"};
  element error;
  if (!element_at(root, pointer, error)) {
    missing_field(result, record, nested_error ? "error" : "message");
    return {};
  }

  if (error.type() == element_type::STRING) {
    std::string_view value;
    if (!error.get_string().get(value)) {
      return std::string{value};
    }
  }
  if (nested_error && error.type() == element_type::OBJECT) {
    element message;
    if (!element_at(error, "/message", message)) {
      missing_field(result, record, "error.message");
      return {};
    }
    std::string_view value;
    if (!message.get_string().get(value)) {
      return std::string{value};
    }
    invalid_field_type(result, record, "error.message", "a string");
    return {};
  }

  invalid_field_type(result, record, nested_error ? "error" : "message",
                     nested_error ? "an object or string" : "a string");
  return {};
}

void
append_turn_usage(element root, RecordIR &record, SessionParseResult &result) {
  element usage;
  if (!element_at(root, "/usage", usage)) {
    missing_field(result, record, "usage");
    return;
  }
  if (usage.type() != element_type::OBJECT) {
    invalid_field_type(result, record, "usage", "an object");
    return;
  }

  UsageEvent event{
      // Exec currently reports cumulative totals, including after resume.
      .scope = UsageScope::Unknown,
      .input_tokens = required_uint(usage, "/input_tokens",
                                    "usage.input_tokens", result, record),
      .cached_input_tokens =
          required_uint(usage, "/cached_input_tokens",
                        "usage.cached_input_tokens", result, record),
      .cache_write_tokens =
          checked_uint(usage, "/cache_write_input_tokens",
                       "usage.cache_write_input_tokens", result, record),
      .output_tokens = required_uint(usage, "/output_tokens",
                                     "usage.output_tokens", result, record),
      .reasoning_tokens =
          required_uint(usage, "/reasoning_output_tokens",
                        "usage.reasoning_output_tokens", result, record),
      .total_tokens = std::nullopt,
      .cost = std::nullopt,
  };

  if (event.input_tokens
      || event.cached_input_tokens
      || event.cache_write_tokens
      || event.output_tokens
      || event.reasoning_tokens) {
    append_event(record, std::move(event));
  }
}

void start_thread(element root, RecordIR &record, SessionParseResult &result,
                  ParserState &state) {
  const std::string thread_id =
      required_string(root, "/thread_id", "thread_id", result, record)
          .value_or("");

  if (state.saw_thread || state.turn || state.next_prelude_item_index > 0) {
    // A leading fragment belongs to a different inferred invocation. Do not
    // reuse its turn or prelude correlations for the first native thread.
    if (state.turn) {
      report_incomplete(result, *state.turn, record.source_line);
    }
    ++state.run_index;
  }
  state.saw_thread = true;
  state.next_turn_index = 0;
  state.next_prelude_item_index = 0;
  state.turn.reset();
  state.thread_id = thread_id;

  if (!thread_id.empty()) {
    if (result.session.session_id.empty()) {
      result.session.session_id = thread_id;
    } else if (result.session.session_id != thread_id
               && state.reported_thread_ids.insert(thread_id).second) {
      add_diagnostic(result, DiagnosticSeverity::Warning,
                     DiagnosticCode::InconsistentSessionId,
                     "Codex Exec stream changes thread id from `"
                         + result.session.session_id
                         + "` to `"
                         + thread_id
                         + "`",
                     record.source_line);
    }
  }

  append_event(record, ExecutionEvent{
                           .subject = ExecutionSubject::Thread,
                           .phase = ExecutionPhase::Started,
                           .correlation_id = run_correlation(state),
                           .native_id = thread_id,
                           .native_type = record.native_type,
                           .status = {},
                           .message = {},
                           .terminal = false,
                       });
}

void start_native_turn(RecordIR &record, SessionParseResult &result,
                       ParserState &state) {
  if (state.turn) {
    report_incomplete(result, *state.turn, record.source_line);
    if (state.turn->active) {
      add_diagnostic(
          result, DiagnosticSeverity::Warning, DiagnosticCode::InvalidLifecycle,
          "Codex Exec turn started before the previous turn terminated",
          record.source_line);
    }
  }
  const TurnState &turn = start_turn(state, false, record.source_line);
  append_event(record, ExecutionEvent{
                           .subject = ExecutionSubject::Turn,
                           .phase = ExecutionPhase::Started,
                           .correlation_id = turn.correlation_id,
                           .native_id = {},
                           .native_type = record.native_type,
                           .status = {},
                           .message = {},
                           .terminal = false,
                       });
}

TurnState &turn_for_terminal(RecordIR &record, SessionParseResult &result,
                             ParserState &state) {
  if (!state.turn) {
    add_diagnostic(
        result, DiagnosticSeverity::Warning, DiagnosticCode::InvalidLifecycle,
        "Codex Exec turn terminated before it was started", record.source_line);
    return start_turn(state, true, record.source_line);
  }
  if (state.turn->terminal) {
    add_diagnostic(
        result, DiagnosticSeverity::Warning, DiagnosticCode::InvalidLifecycle,
        "Codex Exec turn terminated more than once", record.source_line);
  }
  return *state.turn;
}

void complete_turn(element root, RecordIR &record, SessionParseResult &result,
                   ParserState &state) {
  const bool duplicate_terminal = state.turn && state.turn->terminal;
  TurnState &turn = turn_for_terminal(record, result, state);
  if (duplicate_terminal) {
    append_event(record, ExecutionEvent{
                             .subject = ExecutionSubject::Turn,
                             .phase = ExecutionPhase::Unknown,
                             .correlation_id = turn.correlation_id,
                             .native_id = {},
                             .native_type = record.native_type,
                             .status = "completed",
                             .message = {},
                             .terminal = false,
                         });
    return;
  }
  append_event(record, ExecutionEvent{
                           .subject = ExecutionSubject::Turn,
                           .phase = ExecutionPhase::Completed,
                           .correlation_id = turn.correlation_id,
                           .native_id = {},
                           .native_type = record.native_type,
                           .status = "completed",
                           .message = {},
                           .terminal = true,
                       });
  append_turn_usage(root, record, result);
  turn.active = false;
  turn.terminal = true;
}

void fail_turn(element root, RecordIR &record, SessionParseResult &result,
               ParserState &state) {
  const bool duplicate_terminal = state.turn && state.turn->terminal;
  TurnState &turn = turn_for_terminal(record, result, state);
  const std::string message = record_error_message(root, result, record, true);
  if (duplicate_terminal) {
    append_event(record, ExecutionEvent{
                             .subject = ExecutionSubject::Turn,
                             .phase = ExecutionPhase::Unknown,
                             .correlation_id = turn.correlation_id,
                             .native_id = {},
                             .native_type = record.native_type,
                             .status = "failed",
                             .message = message,
                             .terminal = false,
                         });
    return;
  }
  append_event(record, ExecutionEvent{
                           .subject = ExecutionSubject::Turn,
                           .phase = ExecutionPhase::Failed,
                           .correlation_id = turn.correlation_id,
                           .native_id = {},
                           .native_type = record.native_type,
                           .status = "failed",
                           .message = message,
                           .terminal = true,
                       });
  turn.active = false;
  turn.terminal = true;
}

void append_stream_error(element root, RecordIR &record,
                         SessionParseResult &result, const ParserState &state) {
  const std::string message = record_error_message(root, result, record, false);
  append_event(record, ExecutionEvent{
                           .subject = ExecutionSubject::Stream,
                           .phase = ExecutionPhase::Error,
                           .correlation_id = run_correlation(state),
                           .native_id = {},
                           .native_type = record.native_type,
                           .status = "error",
                           .message = message,
                           .terminal = false,
                       });
}

} // namespace

SessionParseResult parse_codex_exec_stream(std::string_view content) {
  SessionParseResult result;
  result.session.format = LogFormat::CodexExec;

  JsonlReader reader{content};
  JsonlLine line;
  simdjson::dom::parser parser;
  ParserState state;
  std::size_t recognized_records = 0;
  std::size_t last_source_line = 0;

  while (reader.next(line)) {
    last_source_line = line.source_line;
    element root;
    const simdjson::error_code parse_error = parser.parse(line.json).get(root);
    if (parse_error) {
      result.session.records.push_back(
          make_invalid_record(line.sequence, line.source_line, line.raw));
      add_diagnostic(result, DiagnosticSeverity::Error,
                     DiagnosticCode::InvalidJson,
                     "invalid JSON in Codex Exec stream: "
                         + std::string{simdjson::error_message(parse_error)},
                     line.source_line);
      continue;
    }

    if (root.type() != element_type::OBJECT) {
      RecordIR record = make_record(line, "non_object");
      append_event(record, UnknownEvent{.native_type = "non_object"});
      result.session.records.push_back(std::move(record));
      add_diagnostic(
          result, DiagnosticSeverity::Error, DiagnosticCode::ExpectedObject,
          "Codex Exec JSONL record must be an object", line.source_line);
      continue;
    }

    element type_field;
    std::string native_type;
    if (!element_at(root, "/type", type_field)) {
      native_type = "unknown";
    } else {
      std::string_view type;
      if (type_field.get_string().get(type)) {
        native_type = "unknown";
      } else {
        native_type = std::string{type};
      }
    }

    RecordIR record = make_record(line, native_type);
    if (!element_at(root, "/type", type_field)) {
      missing_field(result, record, "type");
    } else if (type_field.type() != element_type::STRING) {
      invalid_field_type(result, record, "type", "a string");
    }

    if (!is_known_record_type(native_type)) {
      append_event(record, UnknownEvent{
                               .native_type = native_type.empty()
                                                ? std::string{"unknown"}
                                                : native_type,
                           });
      result.session.records.push_back(std::move(record));
      continue;
    }

    ++recognized_records;
    if (native_type == kThreadStarted) {
      start_thread(root, record, result, state);
    } else if (native_type == kTurnStarted) {
      start_native_turn(record, result, state);
    } else if (native_type == kTurnCompleted) {
      complete_turn(root, record, result, state);
    } else if (native_type == kTurnFailed) {
      fail_turn(root, record, result, state);
    } else if (native_type == kError) {
      append_stream_error(root, record, result, state);
    } else {
      parse_item_record(root, record, result, state);
    }
    result.session.records.push_back(std::move(record));
  }

  if (state.turn) {
    report_incomplete(result, *state.turn, last_source_line);
  }

  if (!result.session.records.empty() && recognized_records == 0) {
    add_diagnostic(result, DiagnosticSeverity::Fatal,
                   DiagnosticCode::FormatMismatch,
                   "input does not contain recognizable Codex Exec records");
  }

  return result;
}

} // namespace lupe::detail
