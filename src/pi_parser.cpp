#include "lupe/log_format.hpp"
#include "lupe/session_ir.hpp"
#include "session_parser_internal.hpp"

#include "json_helpers.hpp"
#include "jsonl_reader.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <simdjson.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lupe::detail {
namespace {

constexpr std::uint64_t kCurrentPiSessionVersion = 3;

Role pi_role(std::string_view role) {
  if (role == "user") {
    return Role::User;
  }
  if (role == "assistant") {
    return Role::Assistant;
  }
  if (role == "system") {
    return Role::System;
  }
  if (role == "developer") {
    return Role::Developer;
  }
  if (role == "agent") {
    return Role::Agent;
  }
  return Role::Unknown;
}

std::optional<int>
int_at(simdjson::dom::element value, std::string_view pointer) {
  simdjson::dom::element field;
  if (!element_at(value, pointer, field)) {
    return std::nullopt;
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
  return std::nullopt;
}

std::string
message_timestamp(simdjson::dom::element message, const RecordIR &record) {
  if (!record.timestamp.empty()) {
    return record.timestamp;
  }
  return scalar_text_at(message, "/timestamp");
}

RecordIR make_record(const JsonlLine &line, simdjson::dom::element document,
                     std::string native_type) {
  RecordIR record;
  record.sequence = line.sequence;
  record.source_line = line.source_line;
  record.native_type = std::move(native_type);
  record.native_id = string_at(document, "/id").value_or("");
  record.timestamp = string_at(document, "/timestamp").value_or("");
  record.raw_json = std::string{line.raw};

  if (const auto parent_id = string_at(document, "/parentId")) {
    record.native_parent_id = *parent_id;
  }
  return record;
}

void validate_entry_parent_id(SessionParseResult &result,
                              simdjson::dom::element document,
                              const RecordIR &record,
                              std::uint64_t session_version) {
  if (session_version < 2) {
    return;
  }

  simdjson::dom::element parent_id;
  if (!element_at(document, "/parentId", parent_id)) {
    add_diagnostic(
        result, DiagnosticSeverity::Error, DiagnosticCode::MissingRequiredField,
        "Pi v2/v3 entry is missing required `parentId`", record.source_line);
    return;
  }

  const auto parent_type = parent_id.type();
  if (parent_type != simdjson::dom::element_type::NULL_VALUE
      && parent_type != simdjson::dom::element_type::STRING) {
    add_diagnostic(result, DiagnosticSeverity::Error,
                   DiagnosticCode::FormatMismatch,
                   "Pi v2/v3 entry `parentId` must be null or a string",
                   record.source_line);
  }
}

std::optional<ContentBlock>
parse_basic_content_block(simdjson::dom::element block) {
  if (block.type() != simdjson::dom::element_type::OBJECT) {
    return UnknownContent{
        .native_type = "unknown",
        .json = json_text(block),
    };
  }

  const std::string type = string_at(block, "/type").value_or("unknown");
  if (type == "text") {
    return TextContent{
        .text = string_at(block, "/text").value_or(""),
    };
  }

  if (type == "image") {
    simdjson::dom::element data;
    auto base64_data = string_at(block, "/data");
    return ImageContent{
        .mime_type = string_at(block, "/mimeType").value_or(""),
        .url = string_at(block, "/url").value_or(""),
        .inline_data = element_at(block, "/data", data),
        .base64_data = base64_data ? std::make_shared<const std::string>(
                                         std::move(*base64_data))
                                   : nullptr,
    };
  }

  return UnknownContent{
      .native_type = type,
      .json = json_text(block),
  };
}

void
append_basic_content(simdjson::dom::element owner, std::string_view pointer,
                     std::vector<ContentBlock> &output) {
  simdjson::dom::element content;
  if (!element_at(owner, pointer, content)) {
    return;
  }

  std::string_view text;
  if (!content.get_string().get(text)) {
    output.emplace_back(TextContent{.text = std::string{text}});
    return;
  }

  simdjson::dom::array blocks;
  if (!content.get_array().get(blocks)) {
    for (const simdjson::dom::element block : blocks) {
      if (auto parsed = parse_basic_content_block(block)) {
        output.push_back(std::move(*parsed));
      }
    }
    return;
  }

  if (content.type() != simdjson::dom::element_type::NULL_VALUE) {
    output.emplace_back(UnknownContent{
        .native_type = "content",
        .json = json_text(content),
    });
  }
}

void append_usage(RecordIR &record, simdjson::dom::element owner,
                  std::string_view pointer, UsageScope scope,
                  const std::string &timestamp) {
  simdjson::dom::element usage;
  if (!element_at(owner, pointer, usage)
      || usage.type() != simdjson::dom::element_type::OBJECT) {
    return;
  }

  append_event(record,
               UsageEvent{
                   .scope = scope,
                   .input_tokens = uint_at(usage, "/input"),
                   .cached_input_tokens = uint_at(usage, "/cacheRead"),
                   .cache_write_tokens = uint_at(usage, "/cacheWrite"),
                   .output_tokens = uint_at(usage, "/output"),
                   .reasoning_tokens = uint_at(usage, "/reasoning"),
                   .total_tokens = uint_at(usage, "/totalTokens"),
                   .cost = double_at(usage, "/cost/total"),
               },
               timestamp);
}

MessageEvent
make_message(simdjson::dom::element message, Role role, std::string raw_role) {
  MessageEvent event;
  event.role = role;
  event.raw_role = std::move(raw_role);
  event.provider = string_at(message, "/provider").value_or("");
  event.model = string_at(message, "/model").value_or("");
  return event;
}

void flush_message(RecordIR &record, std::optional<MessageEvent> &message,
                   const std::string &timestamp) {
  if (!message) {
    return;
  }
  append_event(record, std::move(*message), timestamp);
  message.reset();
}

void append_assistant_message(RecordIR &record, simdjson::dom::element message,
                              const std::string &timestamp) {
  const MessageEvent prototype =
      make_message(message, Role::Assistant, "assistant");
  std::optional<MessageEvent> pending;

  simdjson::dom::element content;
  if (!element_at(message, "/content", content)) {
    append_event(record, prototype, timestamp);
    append_usage(record, message, "/usage", UsageScope::Message, timestamp);
    return;
  }

  std::string_view text;
  if (!content.get_string().get(text)) {
    MessageEvent event = prototype;
    event.content.emplace_back(TextContent{.text = std::string{text}});
    append_event(record, std::move(event), timestamp);
    append_usage(record, message, "/usage", UsageScope::Message, timestamp);
    return;
  }

  simdjson::dom::array blocks;
  if (content.get_array().get(blocks)) {
    if (content.type() == simdjson::dom::element_type::NULL_VALUE) {
      append_event(record, prototype, timestamp);
    } else {
      MessageEvent event = prototype;
      event.content.emplace_back(UnknownContent{
          .native_type = "content",
          .json = json_text(content),
      });
      append_event(record, std::move(event), timestamp);
    }
    append_usage(record, message, "/usage", UsageScope::Message, timestamp);
    return;
  }

  bool saw_block = false;
  for (const simdjson::dom::element block : blocks) {
    saw_block = true;
    const std::string block_type =
        string_at(block, "/type").value_or("unknown");
    if (block_type == "thinking") {
      flush_message(record, pending, timestamp);
      append_event(record,
                   ReasoningEvent{
                       .summary = "",
                       .content = string_at(block, "/thinking").value_or(""),
                       .encrypted = bool_at(block, "/redacted").value_or(false),
                   },
                   timestamp);
      continue;
    }

    if (block_type == "toolCall") {
      flush_message(record, pending, timestamp);

      simdjson::dom::element arguments;
      std::string input = "{}";
      if (element_at(block, "/arguments", arguments)) {
        input = json_text(arguments);
      }

      append_event(record,
                   ToolCallEvent{
                       .call_id = string_at(block, "/id").value_or(""),
                       .name = string_at(block, "/name").value_or(""),
                       .name_space = "pi",
                       .input = std::move(input),
                       .input_is_json = true,
                   },
                   timestamp);
      continue;
    }

    if (!pending) {
      pending = prototype;
    }
    if (auto parsed = parse_basic_content_block(block)) {
      pending->content.push_back(std::move(*parsed));
    }
  }

  if (!saw_block) {
    pending = prototype;
  }
  flush_message(record, pending, timestamp);
  append_usage(record, message, "/usage", UsageScope::Message, timestamp);
}

void append_tool_result(RecordIR &record, simdjson::dom::element message,
                        const std::string &timestamp) {
  ToolResultEvent event{
      .call_id = string_at(message, "/toolCallId").value_or(""),
      .name = string_at(message, "/toolName").value_or(""),
      .output = {},
      .is_error = bool_at(message, "/isError").value_or(false),
      .exit_code = std::nullopt,
  };
  append_basic_content(message, "/content", event.output);
  append_event(record, std::move(event), timestamp);
  append_usage(record, message, "/usage", UsageScope::Message, timestamp);
}

void append_bash_execution(RecordIR &record, simdjson::dom::element message,
                           const std::string &timestamp) {
  const std::string call_id =
      record.native_id.empty()
          ? "pi:bash:line:" + std::to_string(record.source_line)
          : "pi:bash:" + record.native_id;

  const std::string command_json = json_at(message, "/command");
  append_event(record,
               ToolCallEvent{
                   .call_id = call_id,
                   .name = "bash",
                   .name_space = "pi",
                   .input = command_json.empty()
                              ? "{}"
                              : "{\"command\":" + command_json + "}",
                   .input_is_json = true,
               },
               timestamp);

  const std::optional<int> exit_code = int_at(message, "/exitCode");
  const bool cancelled = bool_at(message, "/cancelled").value_or(false);
  ToolResultEvent result{
      .call_id = call_id,
      .name = "bash",
      .output = {TextContent{
          .text = string_at(message, "/output").value_or(""),
      }},
      .is_error =
          cancelled || (exit_code.has_value() && exit_code.value() != 0),
      .exit_code = exit_code,
  };
  append_event(record, std::move(result), timestamp);
}

void append_compaction(RecordIR &record, simdjson::dom::element owner,
                       const std::string &timestamp, std::string trigger = {}) {
  if (trigger.empty()) {
    trigger = bool_at(owner, "/fromHook").value_or(false) ? "extension" : "pi";
  }

  append_event(
      record,
      CompactionEvent{
          .summary = string_at(owner, "/summary").value_or(""),
          .tokens_before = uint_at(owner, "/tokensBefore"),
          .retained_from_record = string_at(owner, "/firstKeptEntryId"),
          .replacement_context_json = json_at(owner, "/retainedTail"),
          .trigger = std::move(trigger),
      },
      timestamp);
  append_usage(record, owner, "/usage", UsageScope::Message, timestamp);
}

void append_plain_message(RecordIR &record, simdjson::dom::element message,
                          Role role, std::string raw_role,
                          const std::string &timestamp) {
  MessageEvent event = make_message(message, role, std::move(raw_role));
  append_basic_content(message, "/content", event.content);
  append_event(record, std::move(event), timestamp);
  append_usage(record, message, "/usage", UsageScope::Message, timestamp);
}

void append_branch_summary(RecordIR &record, simdjson::dom::element owner,
                           const std::string &timestamp) {
  append_event(record,
               MessageEvent{
                   .role = Role::System,
                   .raw_role = "branchSummary",
                   .provider = {},
                   .model = {},
                   .phase = {},
                   .content = {TextContent{
                       .text = string_at(owner, "/summary").value_or(""),
                   }},
               },
               timestamp);
}

void append_custom_message(RecordIR &record, simdjson::dom::element owner,
                           std::string raw_role, const std::string &timestamp) {
  const std::string custom_type = string_at(owner, "/customType").value_or("");
  MessageEvent event{
      .role = Role::Unknown,
      .raw_role = std::move(raw_role),
      .provider = "",
      .model = "",
      .phase = custom_type,
      .content = {},
  };
  append_basic_content(owner, "/content", event.content);
  append_event(record, std::move(event), timestamp);

  if (const auto display = bool_at(owner, "/display")) {
    append_event(record,
                 MetadataEvent{
                     .name = "custom_message.display",
                     .value = *display ? "true" : "false",
                 },
                 timestamp);
  }
}

void parse_message_entry(SessionParseResult &result, RecordIR &record,
                         simdjson::dom::element document) {
  simdjson::dom::element message;
  if (!element_at(document, "/message", message)
      || message.type() != simdjson::dom::element_type::OBJECT) {
    add_diagnostic(result, DiagnosticSeverity::Error,
                   DiagnosticCode::MissingRequiredField,
                   "Pi message record is missing an object `message` field",
                   record.source_line);
    append_event(record, UnknownEvent{.native_type = "message"});
    return;
  }

  const std::string role = string_at(message, "/role").value_or("");
  const std::string timestamp = message_timestamp(message, record);
  if (role.empty()) {
    add_diagnostic(result, DiagnosticSeverity::Error,
                   DiagnosticCode::MissingRequiredField,
                   "Pi message is missing `role`", record.source_line);
    append_event(record, UnknownEvent{.native_type = "message"});
    return;
  }

  if (role == "assistant") {
    append_assistant_message(record, message, timestamp);
    return;
  }
  if (role == "toolResult") {
    append_tool_result(record, message, timestamp);
    return;
  }
  if (role == "bashExecution") {
    append_bash_execution(record, message, timestamp);
    return;
  }
  if (role == "compactionSummary") {
    append_compaction(record, message, timestamp, "message");
    return;
  }
  if (role == "branchSummary") {
    append_branch_summary(record, message, timestamp);
    return;
  }
  if (role == "custom" || role == "hookMessage") {
    append_custom_message(record, message, role, timestamp);
    return;
  }

  append_plain_message(record, message, pi_role(role), role, timestamp);
}

void append_metadata(RecordIR &record, simdjson::dom::element document,
                     std::string name) {
  append_event(record,
               MetadataEvent{
                   .name = std::move(name),
                   .value = json_text(document),
               },
               record.timestamp);
}

void parse_pi_entry(SessionParseResult &result, RecordIR &record,
                    simdjson::dom::element document) {
  if (record.native_type == "message") {
    parse_message_entry(result, record, document);
    return;
  }
  if (record.native_type == "compaction") {
    append_compaction(record, document, record.timestamp);
    return;
  }
  if (record.native_type == "branch_summary") {
    append_branch_summary(record, document, record.timestamp);
    append_usage(record, document, "/usage", UsageScope::Message,
                 record.timestamp);
    return;
  }
  if (record.native_type == "custom_message") {
    append_custom_message(record, document, "custom", record.timestamp);
    return;
  }
  if (record.native_type == "custom") {
    const std::string custom_type =
        string_at(document, "/customType").value_or("");
    append_metadata(record, document,
                    custom_type.empty() ? "custom" : "custom:" + custom_type);
    return;
  }
  if (record.native_type == "model_change"
      || record.native_type == "thinking_level_change"
      || record.native_type == "active_tools_change"
      || record.native_type == "label"
      || record.native_type == "session_info"
      || record.native_type == "leaf") {
    append_metadata(record, document, record.native_type);
    return;
  }

  append_event(record, UnknownEvent{.native_type = record.native_type});
}

void parse_header(SessionParseResult &result, const RecordIR &record,
                  simdjson::dom::element document, std::uint64_t &version) {
  if (record.sequence != 0) {
    add_diagnostic(
        result, DiagnosticSeverity::Error, DiagnosticCode::FormatMismatch,
        "Pi session header must be the first JSONL record", record.source_line);
  }

  simdjson::dom::element version_field;
  const bool has_version = element_at(document, "/version", version_field);
  const auto parsed_version = uint_at(document, "/version");
  if (has_version && !parsed_version) {
    add_diagnostic(
        result, DiagnosticSeverity::Warning, DiagnosticCode::UnsupportedVersion,
        "Pi session header has an invalid `version`", record.source_line);
  }
  version = parsed_version.value_or(1);
  result.session.source_version = std::to_string(version);
  if (version == 0 || version > kCurrentPiSessionVersion) {
    const std::string reason = version == 0
                                 ? " is invalid; supported versions are v1-v3"
                                 : " is newer than the supported v3 schema";
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::UnsupportedVersion,
                   "Pi session version " + std::to_string(version) + reason,
                   record.source_line);
  }

  result.session.session_id = string_at(document, "/id").value_or("");
  result.session.cwd = string_at(document, "/cwd").value_or("");
  result.session.created_at = string_at(document, "/timestamp").value_or("");
  result.session.parent_session_ref = string_at(document, "/parentSession");

  if (result.session.session_id.empty()) {
    add_diagnostic(result, DiagnosticSeverity::Error,
                   DiagnosticCode::MissingRequiredField,
                   "Pi session header is missing `id`", record.source_line);
  }
  if (result.session.cwd.empty()) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "Pi session header is missing `cwd`", record.source_line);
  }
  if (result.session.created_at.empty()) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "Pi session header is missing `timestamp`",
                   record.source_line);
  }
}

} // namespace

SessionParseResult parse_pi_session(std::string_view content) {
  SessionParseResult result;
  result.session.format = LogFormat::Pi;

  JsonlReader reader{content};
  simdjson::dom::parser parser;
  JsonlLine line;
  bool saw_header = false;
  std::uint64_t session_version = 1;

  while (reader.next(line)) {
    simdjson::dom::element document;
    const simdjson::error_code parse_error =
        parser.parse(line.json).get(document);
    if (parse_error) {
      result.session.records.push_back(
          make_invalid_record(line.sequence, line.source_line, line.raw));
      add_diagnostic(result, DiagnosticSeverity::Error,
                     DiagnosticCode::InvalidJson,
                     "invalid JSON in Pi session: "
                         + std::string{simdjson::error_message(parse_error)},
                     line.source_line);
      continue;
    }

    if (document.type() != simdjson::dom::element_type::OBJECT) {
      RecordIR record =
          make_invalid_record(line.sequence, line.source_line, line.raw);
      record.native_type = "invalid_record";
      result.session.records.push_back(std::move(record));
      add_diagnostic(result, DiagnosticSeverity::Error,
                     DiagnosticCode::ExpectedObject,
                     "Pi JSONL record must be an object", line.source_line);
      continue;
    }

    const std::string native_type =
        string_at(document, "/type").value_or("unknown");
    RecordIR record = make_record(line, document, native_type);

    if (native_type == "unknown") {
      add_diagnostic(result, DiagnosticSeverity::Error,
                     DiagnosticCode::MissingRequiredField,
                     "Pi JSONL record is missing `type`", line.source_line);
    }

    if (native_type == "session") {
      // The header `id` identifies the session, not a node in the entry tree.
      record.native_id.clear();
      if (saw_header) {
        add_diagnostic(
            result, DiagnosticSeverity::Error, DiagnosticCode::FormatMismatch,
            "Pi session contains more than one header", line.source_line);
      } else {
        saw_header = true;
        parse_header(result, record, document, session_version);
      }
    } else {
      if (session_version >= 2 && record.native_id.empty()) {
        add_diagnostic(result, DiagnosticSeverity::Warning,
                       DiagnosticCode::MissingRequiredField,
                       "Pi v2/v3 entry is missing `id`", line.source_line);
      }
      validate_entry_parent_id(result, document, record, session_version);

      parse_pi_entry(result, record, document);

      if (native_type == "leaf") {
        simdjson::dom::element target_id_field;
        if (!element_at(document, "/targetId", target_id_field)) {
          add_diagnostic(result, DiagnosticSeverity::Error,
                         DiagnosticCode::MissingRequiredField,
                         "Pi leaf entry is missing required `targetId`",
                         line.source_line);
        } else if (const auto target_id = string_at(document, "/targetId");
                   !target_id) {
          add_diagnostic(result, DiagnosticSeverity::Error,
                         DiagnosticCode::FormatMismatch,
                         "Pi leaf entry `targetId` must be a non-empty string",
                         line.source_line);
        } else if (target_id->empty()) {
          add_diagnostic(result, DiagnosticSeverity::Error,
                         DiagnosticCode::MissingRequiredField,
                         "Pi leaf entry `targetId` must not be empty",
                         line.source_line);
        } else {
          result.session.active_leaf_id = *target_id;
        }
      } else if (!record.native_id.empty()) {
        result.session.active_leaf_id = record.native_id;
      }
    }

    result.session.records.push_back(std::move(record));
  }

  if (!result.session.records.empty() && !saw_header) {
    add_diagnostic(result, DiagnosticSeverity::Fatal,
                   DiagnosticCode::FormatMismatch,
                   "input is not a Pi session: missing `session` header");
  }

  return result;
}

} // namespace lupe::detail
