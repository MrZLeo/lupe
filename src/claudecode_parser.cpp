#include "loupe/log_format.hpp"
#include "loupe/session_ir.hpp"
#include "session_parser_internal.hpp"

#include "json_helpers.hpp"
#include "jsonl_reader.hpp"

#include <cstddef>

#include <array>
#include <cstdint>
#include <initializer_list>
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

namespace loupe::detail {
namespace {

using simdjson::dom::element;
using simdjson::dom::element_type;

bool is_metadata_record(std::string_view type) {
  static constexpr std::array<std::string_view, 12> kMetadataTypes{
      "ai-title",           "attachment",
      "file-history-delta", "file-history-snapshot",
      "last-prompt",        "mode",
      "permission-mode",    "progress",
      "queue-operation",    "summary",
      "agent-name",         "rate-limit-event",
  };

  for (const auto known_type : kMetadataTypes) {
    if (type == known_type) {
      return true;
    }
  }
  return false;
}

std::optional<std::string>
first_string_at(element value,
                std::initializer_list<std::string_view> pointers) {
  for (const auto pointer : pointers) {
    if (auto text = string_at(value, pointer)) {
      return text;
    }
  }
  return std::nullopt;
}

std::optional<std::uint64_t>
first_uint_at(element value, std::initializer_list<std::string_view> pointers) {
  for (const auto pointer : pointers) {
    if (auto number = uint_at(value, pointer)) {
      return number;
    }
  }
  return std::nullopt;
}

std::optional<double>
first_double_at(element value,
                std::initializer_list<std::string_view> pointers) {
  for (const auto pointer : pointers) {
    if (auto number = double_at(value, pointer)) {
      return number;
    }
  }
  return std::nullopt;
}

std::optional<int>
first_int_at(element value, std::initializer_list<std::string_view> pointers) {
  for (const auto pointer : pointers) {
    element field;
    if (!element_at(value, pointer, field)) {
      continue;
    }

    std::int64_t signed_number = 0;
    if (!field.get_int64().get(signed_number)
        && signed_number >= std::numeric_limits<int>::min()
        && signed_number <= std::numeric_limits<int>::max()) {
      return static_cast<int>(signed_number);
    }

    std::uint64_t unsigned_number = 0;
    if (!field.get_uint64().get(unsigned_number)
        && unsigned_number
               <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
      return static_cast<int>(unsigned_number);
    }
  }
  return std::nullopt;
}

ImageContent parse_image_content(element block) {
  ImageContent image;
  image.mime_type =
      first_string_at(block, {"/source/media_type", "/mime_type", "/mimeType"})
          .value_or("");

  const std::string source_type = string_at(block, "/source/type").value_or("");
  element inline_data;
  const bool has_inline_data = element_at(block, "/source/data", inline_data)
                            || element_at(block, "/data", inline_data);
  image.inline_data = source_type == "base64" || has_inline_data;
  if (image.inline_data) {
    auto base64_data = first_string_at(block, {"/source/data", "/data"});
    if (base64_data) {
      image.base64_data =
          std::make_shared<const std::string>(std::move(*base64_data));
    }
  }

  if (!image.inline_data) {
    image.url = first_string_at(block, {"/source/url", "/url"}).value_or("");
  }
  return image;
}

void
append_tool_result_content(std::vector<ContentBlock> &output, element content) {
  switch (content.type()) {
  case element_type::STRING: {
    std::string_view text;
    if (!content.get_string().get(text)) {
      output.emplace_back(TextContent{.text = std::string{text}});
    }
    return;
  }
  case element_type::ARRAY: {
    simdjson::dom::array blocks;
    if (content.get_array().get(blocks)) {
      return;
    }
    for (const auto block : blocks) {
      append_tool_result_content(output, block);
    }
    return;
  }
  case element_type::OBJECT: {
    const std::string block_type = string_at(content, "/type").value_or("");
    if (block_type == "text") {
      output.emplace_back(TextContent{
          .text = first_string_at(content, {"/text", "/content"}).value_or(""),
      });
    } else if (block_type == "image") {
      output.emplace_back(parse_image_content(content));
    } else {
      output.emplace_back(UnknownContent{
          .native_type =
              block_type.empty() ? std::string{"unknown"} : block_type,
          .json = json_text(content),
      });
    }
    return;
  }
  case element_type::NULL_VALUE:
    return;
  case element_type::INT64:
  case element_type::UINT64:
  case element_type::DOUBLE:
  case element_type::BOOL:
  case element_type::BIGINT:
    output.emplace_back(TextContent{.text = json_text(content)});
    return;
  }
}

std::string extract_text_content(element content) {
  switch (content.type()) {
  case element_type::STRING: {
    std::string_view text;
    if (!content.get_string().get(text)) {
      return std::string{text};
    }
    return {};
  }
  case element_type::ARRAY: {
    simdjson::dom::array blocks;
    if (content.get_array().get(blocks)) {
      return {};
    }

    std::string result;
    for (const auto block : blocks) {
      const std::string part = extract_text_content(block);
      if (part.empty()) {
        continue;
      }
      if (!result.empty()) {
        result.push_back('\n');
      }
      result += part;
    }
    return result;
  }
  case element_type::OBJECT: {
    const std::string block_type = string_at(content, "/type").value_or("");
    if (block_type == "text"
        || block_type == "input_text"
        || block_type == "output_text"
        || block_type.empty()) {
      return first_string_at(content, {"/text", "/content"}).value_or("");
    }
    return {};
  }
  case element_type::NULL_VALUE:
  case element_type::INT64:
  case element_type::UINT64:
  case element_type::DOUBLE:
  case element_type::BOOL:
  case element_type::BIGINT:
    return {};
  }
  return {};
}

void
flush_message(RecordIR &record, std::optional<MessageEvent> &pending_message) {
  if (!pending_message) {
    return;
  }
  append_event(record, std::move(*pending_message));
  pending_message.reset();
}

MessageEvent make_message(Role role, std::string_view raw_role,
                          std::string_view provider, std::string_view model) {
  return MessageEvent{
      .role = role,
      .raw_role = std::string{raw_role},
      .provider = std::string{provider},
      .model = std::string{model},
      .phase = {},
      .content = {},
  };
}

void
append_message_block(std::optional<MessageEvent> &pending_message, Role role,
                     std::string_view raw_role, std::string_view provider,
                     std::string_view model, ContentBlock block) {
  if (!pending_message) {
    pending_message = make_message(role, raw_role, provider, model);
  }
  pending_message->content.push_back(std::move(block));
}

ToolResultEvent parse_tool_result(
    element block, element record,
    const std::unordered_map<std::string, std::string> &tool_names) {
  ToolResultEvent tool_result{
      .call_id =
          first_string_at(block, {"/tool_use_id", "/toolUseId"}).value_or(""),
      .name = first_string_at(block, {"/name", "/tool_name"}).value_or(""),
      .output = {},
      .is_error = false,
      .exit_code = std::nullopt,
  };
  if (tool_result.name.empty()) {
    if (const auto iterator = tool_names.find(tool_result.call_id);
        iterator != tool_names.end()) {
      tool_result.name = iterator->second;
    }
  }

  auto is_error = bool_at(block, "/is_error");
  if (!is_error) {
    is_error = bool_at(block, "/isError");
  }
  if (is_error) {
    tool_result.is_error = *is_error;
  } else {
    tool_result.is_error = false;
  }

  element content;
  if (element_at(block, "/content", content)) {
    append_tool_result_content(tool_result.output, content);
  }

  if (const auto exit_code = first_int_at(
          block, {"/exit_code", "/exitCode", "/toolUseResult/exitCode"})) {
    tool_result.exit_code = *exit_code;
  } else if (const auto record_exit_code =
                 first_int_at(record, {"/toolUseResult/exitCode",
                                       "/toolUseResult/exit_code"})) {
    tool_result.exit_code = *record_exit_code;
  }

  return tool_result;
}

void append_usage(RecordIR &record, element root, element message) {
  element usage;
  if (!element_at(message, "/usage", usage)
      || usage.type() != element_type::OBJECT) {
    return;
  }

  UsageEvent event{
      .scope = UsageScope::Message,
      .input_tokens = uint_at(usage, "/input_tokens"),
      .cached_input_tokens = first_uint_at(
          usage, {"/cache_read_input_tokens", "/cached_input_tokens"}),
      .cache_write_tokens = first_uint_at(
          usage, {"/cache_creation_input_tokens", "/cache_write_input_tokens"}),
      .output_tokens = uint_at(usage, "/output_tokens"),
      .reasoning_tokens =
          first_uint_at(usage, {"/reasoning_tokens", "/thinking_tokens"}),
      .total_tokens = uint_at(usage, "/total_tokens"),
      .cost = first_double_at(root, {"/costUSD", "/total_cost_usd"}),
  };

  if (event.input_tokens
      || event.cached_input_tokens
      || event.cache_write_tokens
      || event.output_tokens
      || event.reasoning_tokens
      || event.total_tokens
      || event.cost) {
    append_event(record, std::move(event));
  }
}

void
parse_message_record(element root, Role role, RecordIR &record,
                     SessionParseResult &result,
                     std::unordered_map<std::string, std::string> &tool_names) {
  element message;
  if (!element_at(root, "/message", message)
      || message.type() != element_type::OBJECT) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "Claude Code message record is missing an object message",
                   record.source_line);
    append_event(record, UnknownEvent{.native_type = record.native_type});
    return;
  }

  const std::string default_role =
      role == Role::Assistant ? "assistant" : "user";
  const std::string raw_role =
      string_at(message, "/role").value_or(default_role);
  const std::string provider = string_at(message, "/provider").value_or("");
  const std::string model = string_at(message, "/model").value_or("");

  element content;
  if (!element_at(message, "/content", content)) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "Claude Code message is missing content",
                   record.source_line);
    append_event(record, make_message(role, raw_role, provider, model));
    append_usage(record, root, message);
    return;
  }

  if (role == Role::User
      && bool_at(root, "/isCompactSummary").value_or(false)) {
    append_event(record, CompactionEvent{
                             .summary = extract_text_content(content),
                             .tokens_before = std::nullopt,
                             .retained_from_record = std::nullopt,
                             .replacement_context_json = {},
                             .trigger = "claudecode-summary",
                         });
    append_usage(record, root, message);
    return;
  }

  if (content.type() == element_type::STRING) {
    std::string_view text;
    if (!content.get_string().get(text)) {
      auto event = make_message(role, raw_role, provider, model);
      event.content.emplace_back(TextContent{.text = std::string{text}});
      append_event(record, std::move(event));
    }
    append_usage(record, root, message);
    return;
  }

  if (content.type() != element_type::ARRAY) {
    auto event = make_message(role, raw_role, provider, model);
    event.content.emplace_back(UnknownContent{
        .native_type = "content",
        .json = json_text(content),
    });
    append_event(record, std::move(event));
    append_usage(record, root, message);
    return;
  }

  simdjson::dom::array blocks;
  if (content.get_array().get(blocks)) {
    append_event(record, UnknownEvent{.native_type = record.native_type});
    return;
  }

  std::optional<MessageEvent> pending_message;
  bool saw_block = false;
  for (const auto block : blocks) {
    saw_block = true;
    if (block.type() != element_type::OBJECT) {
      append_message_block(
          pending_message, role, raw_role, provider, model,
          UnknownContent{.native_type = "content", .json = json_text(block)});
      continue;
    }

    const std::string block_type = string_at(block, "/type").value_or("");
    if (block_type == "text") {
      append_message_block(
          pending_message, role, raw_role, provider, model,
          TextContent{
              .text =
                  first_string_at(block, {"/text", "/content"}).value_or(""),
          });
      continue;
    }
    if (block_type == "image") {
      append_message_block(pending_message, role, raw_role, provider, model,
                           parse_image_content(block));
      continue;
    }

    if (block_type == "thinking") {
      flush_message(record, pending_message);
      append_event(record,
                   ReasoningEvent{
                       .summary = string_at(block, "/summary").value_or(""),
                       .content = string_at(block, "/thinking").value_or(""),
                       .encrypted = false,
                   });
      continue;
    }
    if (block_type == "redacted_thinking") {
      flush_message(record, pending_message);
      append_event(record,
                   ReasoningEvent{
                       .summary = string_at(block, "/summary").value_or(""),
                       // Ciphertext remains available in RecordIR::raw_json.
                       .content = {},
                       .encrypted = true,
                   });
      continue;
    }
    if (block_type == "tool_use") {
      flush_message(record, pending_message);

      element input;
      std::string input_json;
      const bool has_input = element_at(block, "/input", input);
      if (has_input) {
        input_json = json_text(input);
      }

      const std::string call_id = string_at(block, "/id").value_or("");
      const std::string name = string_at(block, "/name").value_or("");
      if (!call_id.empty() && !name.empty()) {
        tool_names.insert_or_assign(call_id, name);
      }
      append_event(record, ToolCallEvent{
                               .call_id = call_id,
                               .name = name,
                               .name_space = {},
                               .input = std::move(input_json),
                               .input_is_json = has_input,
                           });
      continue;
    }
    if (block_type == "tool_result") {
      flush_message(record, pending_message);
      append_event(record, parse_tool_result(block, root, tool_names));
      continue;
    }

    append_message_block(pending_message, role, raw_role, provider, model,
                         UnknownContent{
                             .native_type = block_type.empty()
                                              ? std::string{"unknown"}
                                              : block_type,
                             .json = json_text(block),
                         });
  }

  flush_message(record, pending_message);
  if (!saw_block) {
    append_event(record, make_message(role, raw_role, provider, model));
  }
  append_usage(record, root, message);
}

struct ClaudeSessionState {
  std::unordered_set<std::string> seen_session_ids;
  std::unordered_set<std::string> reported_inconsistent_ids;
  std::unordered_set<std::string> main_record_ids;
};

bool update_session_metadata(SessionParseResult &result, element root,
                             const RecordIR &record, bool is_sidechain,
                             ClaudeSessionState &state) {
  if (is_sidechain) {
    return false;
  }

  bool switched_session = false;
  bool belongs_to_current_session = true;
  const auto session_id = first_string_at(root, {"/sessionId", "/session_id"});
  if (session_id && !session_id->empty()) {
    if (result.session.session_id.empty()) {
      result.session.session_id = *session_id;
      state.seen_session_ids.insert(*session_id);
    } else if (result.session.session_id != *session_id) {
      if (state.seen_session_ids.contains(*session_id)) {
        belongs_to_current_session = false;
        if (state.reported_inconsistent_ids.insert(*session_id).second) {
          add_diagnostic(
              result, DiagnosticSeverity::Warning,
              DiagnosticCode::InconsistentSessionId,
              "Claude Code transcript returns to an earlier session id",
              record.source_line);
        }
      } else {
        const auto &parent = record.navigation_parent_id
                               ? record.navigation_parent_id
                               : record.native_parent_id;
        const bool continues_current_session =
            parent
            && !parent->empty()
            && state.main_record_ids.contains(*parent);
        if (continues_current_session) {
          result.session.parent_session_ref = result.session.session_id;
          result.session.session_id = *session_id;
          state.seen_session_ids.insert(*session_id);
          switched_session = true;
        } else {
          belongs_to_current_session = false;
          if (state.reported_inconsistent_ids.insert(*session_id).second) {
            add_diagnostic(
                result, DiagnosticSeverity::Warning,
                DiagnosticCode::InconsistentSessionId,
                "Claude Code session id changes without a parent link to the "
                "current conversation",
                record.source_line);
          }
        }
      }
    }
  }

  if (!belongs_to_current_session) {
    return false;
  }
  if (!record.native_id.empty()) {
    state.main_record_ids.insert(record.native_id);
  }

  if (switched_session) {
    result.session.source_version.clear();
    result.session.cwd.clear();
    result.session.created_at.clear();
  }

  const auto source_version = string_at(root, "/version");
  if (result.session.source_version.empty()) {
    result.session.source_version = source_version.value_or("");
  }
  const auto cwd = string_at(root, "/cwd");
  if (result.session.cwd.empty()) {
    result.session.cwd = cwd.value_or("");
  }
  if (result.session.created_at.empty() && !record.timestamp.empty()) {
    result.session.created_at = record.timestamp;
  }
  return true;
}

} // namespace

SessionParseResult parse_claudecode_transcript(std::string_view content) {
  SessionParseResult result;
  result.session.format = LogFormat::ClaudeCode;

  JsonlReader reader{content};
  JsonlLine line;
  simdjson::dom::parser parser;
  std::size_t recognized_records = 0;
  std::unordered_map<std::string, std::string> tool_names;
  ClaudeSessionState state;

  while (reader.next(line)) {
    element root;
    if (parser.parse(line.json).get(root)) {
      result.session.records.push_back(
          make_invalid_record(line.sequence, line.source_line, line.raw));
      add_diagnostic(
          result, DiagnosticSeverity::Error, DiagnosticCode::InvalidJson,
          "invalid JSON in Claude Code transcript", line.source_line);
      continue;
    }

    if (root.type() != element_type::OBJECT) {
      RecordIR record{
          .sequence = line.sequence,
          .source_line = line.source_line,
          .native_sequence = std::nullopt,
          .native_type = "non_object",
          .native_id = {},
          .native_parent_id = std::nullopt,
          .navigation_parent_id = std::nullopt,
          .timestamp = {},
          .raw_json = std::string{line.raw},
          .events = {},
      };
      append_event(record, UnknownEvent{.native_type = "non_object"});
      result.session.records.push_back(std::move(record));
      add_diagnostic(result, DiagnosticSeverity::Error,
                     DiagnosticCode::ExpectedObject,
                     "Claude Code transcript record must be a JSON object",
                     line.source_line);
      continue;
    }

    RecordIR record{
        .sequence = line.sequence,
        .source_line = line.source_line,
        .native_sequence = std::nullopt,
        .native_type = string_at(root, "/type").value_or(""),
        .native_id = string_at(root, "/uuid").value_or(""),
        .native_parent_id = string_at(root, "/parentUuid"),
        .navigation_parent_id = std::nullopt,
        .timestamp = string_at(root, "/timestamp").value_or(""),
        .raw_json = std::string{line.raw},
        .events = {},
    };

    if (record.native_type == "system"
        && string_at(root, "/subtype").value_or("") == "compact_boundary"
        && (!record.native_parent_id || record.native_parent_id->empty())) {
      record.navigation_parent_id = string_at(root, "/logicalParentUuid");
    }

    const bool is_sidechain = bool_at(root, "/isSidechain").value_or(false);
    const bool belongs_to_main_session =
        update_session_metadata(result, root, record, is_sidechain, state);

    if (record.native_type == "assistant") {
      ++recognized_records;
      parse_message_record(root, Role::Assistant, record, result, tool_names);
    } else if (record.native_type == "user") {
      ++recognized_records;
      parse_message_record(root, Role::User, record, result, tool_names);
    } else if (record.native_type == "system") {
      ++recognized_records;
      const std::string subtype = string_at(root, "/subtype").value_or("");
      if (subtype == "compact_boundary") {
        append_event(
            record,
            CompactionEvent{
                .summary = first_string_at(root, {"/content", "/summary"})
                               .value_or(""),
                .tokens_before = uint_at(root, "/compactMetadata/preTokens"),
                .retained_from_record = string_at(root, "/logicalParentUuid"),
                .replacement_context_json = {},
                .trigger =
                    string_at(root, "/compactMetadata/trigger").value_or(""),
            });
      } else {
        append_event(record, MetadataEvent{
                                 .name = subtype.empty() ? std::string{"system"}
                                                         : subtype,
                                 .value = json_text(root),
                             });
      }
    } else if (is_metadata_record(record.native_type)) {
      ++recognized_records;
      append_event(record, MetadataEvent{
                               .name = record.native_type,
                               .value = json_text(root),
                           });
    } else {
      append_event(record, UnknownEvent{
                               .native_type = record.native_type.empty()
                                                ? std::string{"unknown"}
                                                : record.native_type,
                           });
    }

    if (belongs_to_main_session && !record.native_id.empty()) {
      result.session.active_leaf_id = record.native_id;
    }
    result.session.records.push_back(std::move(record));
  }

  for (auto &record : result.session.records) {
    for (auto &event : record.events) {
      auto *tool_result = std::get_if<ToolResultEvent>(&event.payload);
      if (tool_result == nullptr || !tool_result->name.empty()) {
        continue;
      }
      if (const auto iterator = tool_names.find(tool_result->call_id);
          iterator != tool_names.end()) {
        tool_result->name = iterator->second;
      }
    }
  }

  if (!result.session.records.empty() && recognized_records == 0) {
    add_diagnostic(result, DiagnosticSeverity::Fatal,
                   DiagnosticCode::FormatMismatch,
                   "input does not contain recognizable Claude Code records");
  }

  return result;
}

} // namespace loupe::detail
