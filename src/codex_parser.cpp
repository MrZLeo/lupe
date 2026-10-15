#include "lupe/log_format.hpp"
#include "lupe/session_ir.hpp"
#include "session_parser_internal.hpp"

#include "json_helpers.hpp"
#include "jsonl_reader.hpp"

#include <array>
#include <cstddef>
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

namespace lupe::detail {
namespace {

using simdjson::dom::array;
using simdjson::dom::element;
using simdjson::dom::element_type;

struct PendingLegacyMessage {
  std::size_t record_index{0};
  std::size_t turn_index{0};
  MessageEvent message;
};

struct CanonicalMessage {
  std::size_t record_index{0};
  std::size_t turn_index{0};
};

struct CompletedItem {
  std::size_t record_index{0};
  std::size_t turn_index{0};
  std::string id;
};

struct CodexParserState {
  bool saw_session_meta{false};
  std::size_t recognized_records{0};
  std::size_t turn_index{0};
  std::string turn_id;
  std::string provider;
  std::string model;
  std::unordered_map<std::string, std::string> tool_names;
  std::vector<PendingLegacyMessage> legacy_messages;
  std::vector<CanonicalMessage> canonical_messages;
  std::vector<CanonicalMessage> response_records;
  std::vector<CompletedItem> completed_items;
};

void update_turn(element payload, CodexParserState &state, bool boundary) {
  const auto id = string_at(payload, "/turn_id");
  if (id && !id->empty()) {
    if (*id != state.turn_id) {
      // A truncated/older rollout may first reveal the current turn ID in a
      // completion, after its raw response records have already been read.
      if (boundary || !state.turn_id.empty()) {
        ++state.turn_index;
      }
      state.turn_id = *id;
    }
  } else if (boundary) {
    ++state.turn_index;
    state.turn_id.clear();
  }
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

std::optional<bool>
first_bool_at(element value, std::initializer_list<std::string_view> pointers) {
  for (const auto pointer : pointers) {
    if (auto boolean = bool_at(value, pointer)) {
      return boolean;
    }
  }
  return std::nullopt;
}

Role codex_role(std::string_view role) {
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

std::string join_text(const std::vector<std::string> &parts) {
  std::string joined;
  for (const auto &part : parts) {
    if (part.empty()) {
      continue;
    }
    if (!joined.empty()) {
      joined.push_back('\n');
    }
    joined += part;
  }
  return joined;
}

std::string inline_mime_type(std::string_view value) {
  constexpr std::string_view prefix{"data:"};
  if (!value.starts_with(prefix)) {
    return {};
  }

  value.remove_prefix(prefix.size());
  const std::size_t separator = value.find(';');
  if (separator == std::string_view::npos) {
    return {};
  }
  return std::string{value.substr(0, separator)};
}

std::shared_ptr<const std::string> inline_base64_data(std::string_view value) {
  constexpr std::string_view prefix{"data:"};
  if (!value.starts_with(prefix)) {
    return nullptr;
  }

  const std::size_t separator = value.find(',');
  if (separator == std::string_view::npos
      || !value.substr(0, separator).ends_with(";base64")) {
    return nullptr;
  }
  return std::make_shared<const std::string>(value.substr(separator + 1));
}

ContentBlock parse_content_block(element value) {
  if (value.type() == element_type::STRING) {
    std::string_view text;
    if (!value.get_string().get(text)) {
      return TextContent{.text = std::string{text}};
    }
  }

  const std::string type = string_at(value, "/type").value_or("unknown");
  if (type == "input_text"
      || type == "output_text"
      || type == "text"
      || type == "Text"
      || type == "inputText") {
    return TextContent{.text = string_at(value, "/text").value_or("")};
  }

  if (type == "input_image"
      || type == "image"
      || type == "inputImage"
      || type == "local_image") {
    std::string url =
        first_string_at(value, {"/image_url", "/imageUrl", "/url", "/path"})
            .value_or("");
    // File-backed image references have no URL; keep the file ID inspectable.
    if (url.empty() && string_at(value, "/file_id")) {
      return UnknownContent{.native_type = type, .json = json_text(value)};
    }
    const bool inline_data = std::string_view{url}.starts_with("data:");
    const auto base64_data = inline_base64_data(url);
    std::string mime_type =
        first_string_at(value, {"/mime_type", "/mimeType"}).value_or("");
    if (mime_type.empty() && inline_data) {
      mime_type = inline_mime_type(url);
    }
    if (inline_data) {
      url.clear();
    }
    return ImageContent{
        .mime_type = std::move(mime_type),
        .url = std::move(url),
        .inline_data = inline_data,
        .base64_data = base64_data,
    };
  }

  return UnknownContent{
      .native_type = type,
      .json = json_text(value),
  };
}

bool append_content_value(element value, std::vector<ContentBlock> &content) {
  if (value.type() == element_type::ARRAY) {
    array values;
    if (value.get_array().get(values)) {
      return false;
    }
    for (const auto item : values) {
      content.push_back(parse_content_block(item));
    }
    return true;
  }

  if (value.type() == element_type::STRING
      || value.type() == element_type::OBJECT) {
    content.push_back(parse_content_block(value));
    return true;
  }

  content.emplace_back(UnknownContent{
      .native_type = "content",
      .json = json_text(value),
  });
  return true;
}

bool append_content_at(element owner, std::string_view pointer,
                       std::vector<ContentBlock> &content) {
  element value;
  if (!element_at(owner, pointer, value)) {
    return false;
  }
  return append_content_value(value, content);
}

void parse_response_message(element payload, RecordIR &record,
                            SessionParseResult &result, CodexParserState &state,
                            std::size_t record_index) {
  const std::string raw_role = string_at(payload, "/role").value_or("unknown");
  MessageEvent message{
      .role = codex_role(raw_role),
      .raw_role = raw_role,
      .provider = state.provider,
      .model = state.model,
      .phase = string_at(payload, "/phase").value_or(""),
      .content = {},
  };

  if (!append_content_at(payload, "/content", message.content)) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "Codex response message is missing content",
                   record.source_line);
  }
  append_event(record, std::move(message));
  state.canonical_messages.push_back(CanonicalMessage{
      .record_index = record_index,
      .turn_index = state.turn_index,
  });
}

std::vector<std::string>
text_items_at(element owner, std::string_view pointer) {
  element value;
  if (!element_at(owner, pointer, value)) {
    return {};
  }

  if (value.type() == element_type::STRING) {
    std::string_view text;
    if (!value.get_string().get(text)) {
      return {std::string{text}};
    }
    return {};
  }

  array values;
  if (value.get_array().get(values)) {
    return {};
  }

  std::vector<std::string> parts;
  for (const auto item : values) {
    std::string_view text;
    if (item.get_string().get(text) == simdjson::SUCCESS) {
      parts.emplace_back(text);
    } else if (const auto nested_text = string_at(item, "/text")) {
      parts.push_back(*nested_text);
    }
  }
  return parts;
}

void parse_response_reasoning(element payload, RecordIR &record) {
  const std::vector<std::string> summaries = text_items_at(payload, "/summary");
  const std::vector<std::string> contents = text_items_at(payload, "/content");
  const auto encrypted = string_at(payload, "/encrypted_content");

  append_event(record,
               ReasoningEvent{
                   .summary = join_text(summaries),
                   .content = join_text(contents),
                   .encrypted = encrypted.has_value() && !encrypted->empty(),
               });
}

std::pair<std::string, bool> normalize_json(std::string_view value) {
  simdjson::dom::parser parser;
  const simdjson::padded_string padded{value};
  element parsed;
  if (parser.parse(padded).get(parsed)) {
    return {std::string{value}, false};
  }
  return {json_text(parsed), true};
}

std::pair<std::string, bool>
structured_input_at(element payload, std::string_view pointer,
                    bool parse_string_as_json) {
  element input;
  if (!element_at(payload, pointer, input)) {
    return {};
  }

  if (input.type() == element_type::STRING) {
    std::string_view text;
    if (input.get_string().get(text)) {
      return {};
    }
    if (parse_string_as_json) {
      return normalize_json(text);
    }
    return {std::string{text}, false};
  }

  return {json_text(input), true};
}

void
parse_tool_call(element payload, std::string_view subtype, RecordIR &record,
                SessionParseResult &result, CodexParserState &state) {
  ToolCallEvent call;
  call.call_id = first_string_at(payload, {"/call_id", "/id"}).value_or("");

  if (subtype == "local_shell_call") {
    call.name = "local_shell";
    call.name_space = "codex";
    auto [input, input_is_json] =
        structured_input_at(payload, "/action", false);
    call.input = std::move(input);
    call.input_is_json = input_is_json;
  } else if (subtype == "custom_tool_call") {
    call.name = string_at(payload, "/name").value_or("");
    call.name_space = string_at(payload, "/namespace").value_or("");
    auto [input, input_is_json] = structured_input_at(payload, "/input", false);
    call.input = std::move(input);
    call.input_is_json = input_is_json;
  } else {
    call.name = string_at(payload, "/name").value_or("");
    call.name_space = string_at(payload, "/namespace").value_or("");
    auto [input, input_is_json] =
        structured_input_at(payload, "/arguments", true);
    call.input = std::move(input);
    call.input_is_json = input_is_json;
    if (!call.input.empty() && !call.input_is_json) {
      add_diagnostic(result, DiagnosticSeverity::Warning,
                     DiagnosticCode::InvalidJson,
                     "Codex function-call arguments are not valid JSON",
                     record.source_line);
    }
  }

  if (!call.call_id.empty() && !call.name.empty()) {
    state.tool_names.insert_or_assign(call.call_id, call.name);
  }
  append_event(record, std::move(call));
}

void append_tool_output(element value, std::vector<ContentBlock> &output) {
  if (value.type() == element_type::OBJECT) {
    element nested;
    if (element_at(value, "/content", nested)) {
      static_cast<void>(append_content_value(nested, output));
      return;
    }
  }
  static_cast<void>(append_content_value(value, output));
}

void
parse_tool_result(element payload, std::string_view subtype, RecordIR &record,
                  SessionParseResult &result, const CodexParserState &state) {
  ToolResultEvent tool_result;
  tool_result.call_id = string_at(payload, "/call_id").value_or("");
  tool_result.name = string_at(payload, "/name").value_or("");
  if (tool_result.name.empty()) {
    if (const auto iterator = state.tool_names.find(tool_result.call_id);
        iterator != state.tool_names.end()) {
      tool_result.name = iterator->second;
    } else if (subtype == "local_shell_call_output") {
      tool_result.name = "local_shell";
    }
  }

  element output;
  if (element_at(payload, "/output", output)) {
    append_tool_output(output, tool_result.output);
    tool_result.is_error =
        first_bool_at(payload, {"/is_error", "/output/is_error"})
            .value_or(false);
    if (const auto success = bool_at(payload, "/success"); success) {
      tool_result.is_error = !*success;
    }
  } else {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "Codex tool result is missing output", record.source_line);
  }

  if (const auto exit_code = uint_at(payload, "/exit_code");
      exit_code
      && *exit_code
             <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    tool_result.exit_code = static_cast<int>(*exit_code);
  }
  append_event(record, std::move(tool_result));
}

std::string response_item_call_id(element payload, std::string_view name,
                                  const RecordIR &record) {
  if (const auto call_id = string_at(payload, "/call_id");
      call_id && !call_id->empty()) {
    return *call_id;
  }
  if (const auto id = string_at(payload, "/id"); id && !id->empty()) {
    return *id;
  }
  return "codex:" + std::string{name} + ":" + std::to_string(record.sequence);
}

bool failed_status(element payload) {
  const std::string status = string_at(payload, "/status").value_or("");
  return status == "failed"
      || status == "error"
      || status == "incomplete"
      || status == "declined"
      || status == "interrupted"
      || status == "cancelled"
      || status == "canceled";
}

void parse_web_search_call(element payload, RecordIR &record,
                           CodexParserState &state) {
  ToolCallEvent call{
      .call_id = response_item_call_id(payload, "web_search", record),
      .name = "web_search",
      .name_space = "codex",
      .input = {},
      .input_is_json = false,
  };
  auto [input, input_is_json] = structured_input_at(payload, "/action", false);
  call.input = std::move(input);
  call.input_is_json = input_is_json;
  state.tool_names.insert_or_assign(call.call_id, call.name);
  append_event(record, std::move(call));
}

void parse_tool_search_call(element payload, RecordIR &record,
                            CodexParserState &state) {
  ToolCallEvent call{
      .call_id = response_item_call_id(payload, "tool_search", record),
      .name = "tool_search",
      .name_space = "codex",
      .input = {},
      .input_is_json = false,
  };
  auto [input, input_is_json] =
      structured_input_at(payload, "/arguments", false);
  call.input = std::move(input);
  call.input_is_json = input_is_json;
  state.tool_names.insert_or_assign(call.call_id, call.name);
  append_event(record, std::move(call));
}

void parse_tool_search_output(element payload, RecordIR &record,
                              const CodexParserState &state) {
  ToolResultEvent result{
      .call_id = response_item_call_id(payload, "tool_search", record),
      .name = "tool_search",
      .output = {},
      .is_error = failed_status(payload),
      .exit_code = std::nullopt,
  };
  if (const auto iterator = state.tool_names.find(result.call_id);
      iterator != state.tool_names.end()) {
    result.name = iterator->second;
  }

  const std::string tools = json_at(payload, "/tools");
  result.output.emplace_back(UnknownContent{
      .native_type = "tool_search_results",
      .json = tools.empty() ? json_text(payload) : tools,
  });
  append_event(record, std::move(result));
}

ImageContent image_generation_result(std::string result) {
  if (std::string_view{result}.starts_with("data:")) {
    return ImageContent{
        .mime_type = inline_mime_type(result),
        .url = {},
        .inline_data = true,
        .base64_data = inline_base64_data(result),
    };
  }

  const bool is_url = std::string_view{result}.starts_with("http://")
                   || std::string_view{result}.starts_with("https://")
                   || std::string_view{result}.starts_with("file://");
  if (is_url) {
    return ImageContent{
        .mime_type = {},
        .url = std::move(result),
        .inline_data = false,
        .base64_data = nullptr,
    };
  }

  return ImageContent{
      .mime_type = {},
      .url = {},
      .inline_data = !result.empty(),
      .base64_data = result.empty()
                       ? nullptr
                       : std::make_shared<const std::string>(std::move(result)),
  };
}

void parse_image_generation_call(element payload, RecordIR &record,
                                 CodexParserState &state) {
  const std::string call_id =
      response_item_call_id(payload, "image_generation", record);
  ToolCallEvent call{
      .call_id = call_id,
      .name = "image_generation",
      .name_space = "codex",
      .input = string_at(payload, "/revised_prompt").value_or(""),
      .input_is_json = false,
  };
  state.tool_names.insert_or_assign(call.call_id, call.name);
  append_event(record, std::move(call));

  if (const auto generated = string_at(payload, "/result"); generated) {
    ToolResultEvent result{
        .call_id = call_id,
        .name = "image_generation",
        .output = {image_generation_result(*generated)},
        .is_error = failed_status(payload),
        .exit_code = std::nullopt,
    };
    append_event(record, std::move(result));
  }
}

void parse_response_compaction(element payload, std::string_view subtype,
                               RecordIR &record) {
  append_event(
      record,
      CompactionEvent{
          .summary = string_at(payload, "/message").value_or(""),
          .tokens_before = uint_at(payload, "/tokens_before"),
          .retained_from_record = std::nullopt,
          .replacement_context_json = json_at(payload, "/replacement_history"),
          .trigger = std::string{subtype},
      });
}

bool append_usage(element owner, std::string_view pointer, UsageScope scope,
                  RecordIR &record) {
  element value;
  if (!element_at(owner, pointer, value)
      || value.type() != element_type::OBJECT) {
    return false;
  }

  UsageEvent usage{
      .scope = scope,
      .input_tokens = uint_at(value, "/input_tokens"),
      .cached_input_tokens = uint_at(value, "/cached_input_tokens"),
      .cache_write_tokens = uint_at(value, "/cache_write_input_tokens"),
      .output_tokens = uint_at(value, "/output_tokens"),
      .reasoning_tokens = uint_at(value, "/reasoning_output_tokens"),
      .total_tokens = uint_at(value, "/total_tokens"),
      .cost = std::nullopt,
  };

  if (!usage.input_tokens
      && !usage.cached_input_tokens
      && !usage.cache_write_tokens
      && !usage.output_tokens
      && !usage.reasoning_tokens
      && !usage.total_tokens) {
    return false;
  }
  append_event(record, std::move(usage));
  return true;
}

void parse_token_count(element payload, RecordIR &record) {
  const bool added_turn =
      append_usage(payload, "/info/last_token_usage", UsageScope::Turn, record);
  const bool added_session = append_usage(payload, "/info/total_token_usage",
                                          UsageScope::Session, record);

  if (!added_turn && !added_session) {
    static_cast<void>(
        append_usage(payload, "/info", UsageScope::Unknown, record));
  }

  element rate_limits;
  if (element_at(payload, "/rate_limits", rate_limits)
      && rate_limits.type() != element_type::NULL_VALUE) {
    append_event(record, MetadataEvent{
                             .name = "rate_limits",
                             .value = json_text(rate_limits),
                         });
  }
}

void append_legacy_images(element payload, std::string_view pointer,
                          std::vector<ContentBlock> &content) {
  element value;
  if (!element_at(payload, pointer, value)) {
    return;
  }

  array images;
  if (value.get_array().get(images)) {
    return;
  }
  for (const auto image : images) {
    std::string_view url;
    if (!image.get_string().get(url)) {
      const bool inline_data = url.starts_with("data:");
      content.emplace_back(ImageContent{
          .mime_type = inline_data ? inline_mime_type(url) : std::string{},
          .url = inline_data ? std::string{} : std::string{url},
          .inline_data = inline_data,
          .base64_data = inline_base64_data(url),
      });
    }
  }
}

void save_legacy_message(element payload, std::string_view subtype,
                         std::size_t record_index, CodexParserState &state) {
  const bool is_agent = subtype == "agent_message";
  MessageEvent message{
      .role = is_agent ? Role::Assistant : Role::User,
      .raw_role = is_agent ? "assistant" : "user",
      .provider = state.provider,
      .model = state.model,
      .phase = string_at(payload, "/phase").value_or(""),
      .content = {},
  };

  if (const auto text = string_at(payload, "/message"); text) {
    message.content.emplace_back(TextContent{.text = *text});
  }
  if (!is_agent) {
    append_legacy_images(payload, "/images", message.content);
    append_legacy_images(payload, "/local_images", message.content);
  }
  state.legacy_messages.push_back(PendingLegacyMessage{
      .record_index = record_index,
      .turn_index = state.turn_index,
      .message = std::move(message),
  });
}

// TurnItem uses PascalCase tags (unlike ResponseItem and codex exec --json).
// Most fields are snake_case, but McpToolCall and dynamic output blocks use
// camelCase. Keep these schemas separate rather than lowercasing arbitrary
// JSON.
void parse_completed_item(element payload, std::size_t record_index,
                          RecordIR &record, SessionParseResult &result,
                          CodexParserState &state) {
  element item;
  if (!element_at(payload, "/item", item)
      || item.type() != element_type::OBJECT
      || !string_at(item, "/type")) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "Codex item_completed requires an object item with a type",
                   record.source_line);
    append_event(record,
                 UnknownEvent{.native_type = "event_msg.item_completed"});
    return;
  }

  const std::string type = *string_at(item, "/type");
  const std::string id = string_at(item, "/id").value_or("");
  if (id.empty()) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "Codex completed item is missing id", record.source_line);
  }
  // item.id identifies a semantic item, not a unique rollout record. In
  // particular it can equal a response_item ID; do not copy it to native_id.
  state.completed_items.push_back({
      .record_index = record_index,
      .turn_index = state.turn_index,
      .id = id,
  });

  if (type == "UserMessage" || type == "AgentMessage") {
    MessageEvent message{
        .role = type == "UserMessage" ? Role::User : Role::Assistant,
        .raw_role = type == "UserMessage" ? "user" : "assistant",
        .provider = state.provider,
        .model = state.model,
        .phase = string_at(item, "/phase").value_or(""),
        .content = {},
    };
    element content;
    if (!element_at(item, "/content", content)
        || content.type() != element_type::ARRAY) {
      add_diagnostic(result, DiagnosticSeverity::Warning,
                     DiagnosticCode::InvalidFieldType,
                     "Codex completed message content must be an array",
                     record.source_line);
      append_event(record,
                   UnknownEvent{.native_type = "item_completed." + type});
      return;
    }
    append_content_value(content, message.content);
    append_event(record, std::move(message));
    return;
  }
  if (type == "Reasoning") {
    append_event(record,
                 ReasoningEvent{
                     .summary = join_text(text_items_at(item, "/summary_text")),
                     .content = join_text(text_items_at(item, "/raw_content")),
                     .encrypted = false,
                 });
    return;
  }
  if (type == "Plan"
      || type == "HookPrompt"
      || type == "EnteredReviewMode"
      || type == "ExitedReviewMode") {
    std::string text;
    Role role = Role::Assistant;
    if (type == "HookPrompt") {
      text = join_text(text_items_at(item, "/fragments"));
      role = Role::Developer;
    } else if (type == "EnteredReviewMode") {
      text = string_at(item, "/user_facing_hint").value_or("");
      role = Role::System;
    } else if (type == "ExitedReviewMode") {
      text = json_at(item, "/review_output");
      role = Role::System;
    } else {
      text = string_at(item, "/text").value_or("");
    }
    append_event(record, MessageEvent{
                             .role = role,
                             .raw_role = std::string{role_name(role)},
                             .provider = state.provider,
                             .model = state.model,
                             .phase = type == "Plan" ? "plan" : "",
                             .content = {TextContent{.text = std::move(text)}},
                         });
    return;
  }
  if (type == "ContextCompaction") {
    append_event(record, CompactionEvent{
                             .summary = {},
                             .tokens_before = std::nullopt,
                             .retained_from_record = std::nullopt,
                             .replacement_context_json = {},
                             .trigger = "context_compaction",
                         });
    return;
  }
  if (type == "ImageGeneration") {
    parse_image_generation_call(item, record, state);
    return;
  }
  if (type == "SubAgentActivity") {
    append_event(record, MetadataEvent{
                             .name = "codex.SubAgentActivity",
                             .value = json_text(item),
                         });
    return;
  }

  const bool is_tool = type == "CommandExecution"
                    || type == "FileChange"
                    || type == "McpToolCall"
                    || type == "DynamicToolCall"
                    || type == "CollabAgentToolCall"
                    || type == "WebSearch"
                    || type == "ImageView"
                    || type == "FunctionCallOutput";
  if (!is_tool) {
    append_event(record, UnknownEvent{.native_type = "item_completed." + type});
    return;
  }

  const std::string call_id = response_item_call_id(item, type, record);
  ToolCallEvent call{
      .call_id = call_id,
      .name = {},
      .name_space = "codex",
      .input = {},
      .input_is_json = false,
  };
  ToolResultEvent output{
      .call_id = call_id,
      .name = {},
      .output = {},
      .is_error = failed_status(item),
      .exit_code = std::nullopt,
  };
  bool has_output = false;
  auto input_at = [&](std::string_view pointer) {
    auto [input, is_json] = structured_input_at(item, pointer, false);
    call.input = std::move(input);
    call.input_is_json = is_json;
  };
  auto output_at = [&](std::string_view pointer) {
    element field;
    if (element_at(item, pointer, field)
        && field.type() != element_type::NULL_VALUE) {
      append_tool_output(field, output.output);
      has_output = true;
    }
  };
  auto text_output_at = [&](std::string_view pointer) {
    element field;
    if (element_at(item, pointer, field)
        && field.type() != element_type::NULL_VALUE) {
      output.output.emplace_back(TextContent{
          .text = string_at(item, pointer).value_or(json_text(field)),
      });
      has_output = true;
    }
  };

  if (type == "CommandExecution") {
    call.name = "command_execution";
    input_at("/command");
    output_at("/aggregated_output");
    element code;
    std::int64_t exit = 0;
    if (element_at(item, "/exit_code", code)
        && code.get_int64().get(exit) == simdjson::SUCCESS
        && exit >= std::numeric_limits<int>::min()
        && exit <= std::numeric_limits<int>::max()) {
      output.exit_code = static_cast<int>(exit);
      output.is_error = output.is_error || exit != 0;
      has_output = true;
    }
  } else if (type == "FileChange") {
    call.name = "apply_patch";
    input_at("/changes");
    text_output_at("/changes");
    output_at("/stdout");
    output_at("/stderr");
  } else if (type == "McpToolCall" || type == "DynamicToolCall") {
    call.name = string_at(item, "/tool").value_or("");
    call.name_space =
        string_at(item, type == "McpToolCall" ? "/server" : "/namespace")
            .value_or("");
    input_at("/arguments");
    output_at(type == "McpToolCall" ? "/result" : "/content_items");
    if (type == "McpToolCall") {
      text_output_at("/result/structuredContent");
      output.is_error =
          output.is_error
          || first_bool_at(item, {"/result/isError", "/result/is_error"})
                 .value_or(false);
    }
    const auto error = first_string_at(item, {"/error/message", "/error"});
    if (error) {
      output.output.emplace_back(TextContent{.text = *error});
      output.is_error = true;
      has_output = true;
    }
    if (const auto success = bool_at(item, "/success")) {
      output.is_error = output.is_error || !*success;
      has_output = true;
    }
  } else if (type == "CollabAgentToolCall") {
    call.name = string_at(item, "/tool").value_or("collab");
    call.name_space = "codex.collab";
    call.input = json_text(item);
    call.input_is_json = true;
    text_output_at("/agents_states");
  } else if (type == "WebSearch") {
    call.name = "web_search";
    input_at("/action");
    if (call.input.empty()) {
      input_at("/query");
    }
    text_output_at("/results");
  } else if (type == "ImageView") {
    call.name = "view_image";
    input_at("/path");
    if (const auto path = string_at(item, "/path")) {
      output.output.emplace_back(ImageContent{
          .mime_type = {},
          .url = *path,
          .inline_data = false,
          .base64_data = nullptr,
      });
      has_output = true;
    }
  } else {
    call.name = string_at(item, "/name").value_or("");
    output_at("/output");
  }

  output.name = call.name;
  if (type != "FunctionCallOutput") {
    const auto [name, inserted] =
        state.tool_names.try_emplace(call_id, call.name);
    static_cast<void>(inserted);
    output.name = name->second;
    append_event(record, std::move(call));
  }
  if (has_output || output.is_error) {
    append_event(record, std::move(output));
  }
}

void
parse_event_message(element payload, std::size_t record_index, RecordIR &record,
                    SessionParseResult &result, CodexParserState &state) {
  const std::string subtype = string_at(payload, "/type").value_or("unknown");
  if (subtype == "task_started" || subtype == "turn_started") {
    update_turn(payload, state, true);
  }
  if (subtype == "item_completed") {
    update_turn(payload, state, false);
    parse_completed_item(payload, record_index, record, result, state);
    return;
  }
  if (subtype == "token_count") {
    parse_token_count(payload, record);
    return;
  }
  if (subtype == "agent_message" || subtype == "user_message") {
    save_legacy_message(payload, subtype, record_index, state);
    return;
  }

  append_event(record, MetadataEvent{
                           .name = subtype,
                           .value = json_text(payload),
                       });
}

void parse_session_meta(element payload, RecordIR &record,
                        SessionParseResult &result, CodexParserState &state) {
  if (state.saw_session_meta) {
    return;
  }
  state.saw_session_meta = true;

  result.session.session_id =
      first_string_at(payload, {"/id", "/session_id"}).value_or("");
  result.session.source_version =
      string_at(payload, "/cli_version").value_or("");
  result.session.cwd = string_at(payload, "/cwd").value_or("");
  result.session.created_at =
      string_at(payload, "/timestamp").value_or(record.timestamp);
  result.session.parent_session_ref =
      first_string_at(payload, {"/forked_from_id", "/parent_thread_id"});
  state.provider = string_at(payload, "/model_provider").value_or("");

  if (const auto mode = string_at(payload, "/history_mode")) {
    append_event(record, MetadataEvent{
                             .name = "codex.history_mode",
                             .value = *mode,
                         });
    if (*mode != "legacy" && *mode != "paginated") {
      add_diagnostic(result, DiagnosticSeverity::Warning,
                     DiagnosticCode::UnsupportedVersion,
                     "unknown Codex history_mode: " + *mode,
                     record.source_line);
    }
  }
  element base;
  if (element_at(payload, "/history_base", base)
      && base.type() != element_type::NULL_VALUE) {
    append_event(record, MetadataEvent{
                             .name = "codex.history_base",
                             .value = json_text(base),
                         });
  }

  if (result.session.session_id.empty()) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "Codex session metadata is missing id", record.source_line);
  }
}

void parse_compacted(element payload, RecordIR &record) {
  append_event(
      record,
      CompactionEvent{
          .summary = string_at(payload, "/message").value_or(""),
          .tokens_before = uint_at(payload, "/tokens_before"),
          .retained_from_record = std::nullopt,
          .replacement_context_json = json_at(payload, "/replacement_history"),
          .trigger = string_at(payload, "/trigger").value_or(""),
      });
}

void parse_response_item(element payload, RecordIR &record,
                         SessionParseResult &result, CodexParserState &state,
                         std::size_t record_index) {
  const std::string subtype = string_at(payload, "/type").value_or("unknown");
  if (subtype == "message") {
    parse_response_message(payload, record, result, state, record_index);
    return;
  }
  if (subtype == "agent_message") {
    MessageEvent message{
        .role = Role::Agent,
        .raw_role = "agent",
        .provider = state.provider,
        .model = state.model,
        .phase = {},
        .content = {},
    };
    static_cast<void>(append_content_at(payload, "/content", message.content));
    append_event(record, std::move(message));
    state.canonical_messages.push_back(CanonicalMessage{
        .record_index = record_index,
        .turn_index = state.turn_index,
    });
    return;
  }
  if (subtype == "reasoning") {
    parse_response_reasoning(payload, record);
    return;
  }
  if (subtype == "function_call"
      || subtype == "custom_tool_call"
      || subtype == "local_shell_call") {
    parse_tool_call(payload, subtype, record, result, state);
    return;
  }
  if (subtype == "function_call_output"
      || subtype == "custom_tool_call_output"
      || subtype == "local_shell_call_output") {
    parse_tool_result(payload, subtype, record, result, state);
    return;
  }
  if (subtype == "web_search_call") {
    parse_web_search_call(payload, record, state);
    return;
  }
  if (subtype == "tool_search_call") {
    parse_tool_search_call(payload, record, state);
    return;
  }
  if (subtype == "tool_search_output") {
    parse_tool_search_output(payload, record, state);
    return;
  }
  if (subtype == "image_generation_call") {
    parse_image_generation_call(payload, record, state);
    return;
  }
  if (subtype == "compaction"
      || subtype == "compaction_summary"
      || subtype == "context_compaction"
      || subtype == "compaction_trigger") {
    parse_response_compaction(payload, subtype, record);
    return;
  }

  append_event(record, UnknownEvent{
                           .native_type = "response_item." + subtype,
                       });
}

bool requires_object_payload(std::string_view type) {
  return type == "session_meta"
      || type == "response_item"
      || type == "event_msg"
      || type == "compacted"
      || type == "turn_context"
      || type == "world_state";
}

bool is_response_item_type(std::string_view type) {
  return type == "message"
      || type == "agent_message"
      || type == "reasoning"
      || type == "function_call"
      || type == "custom_tool_call"
      || type == "local_shell_call"
      || type == "function_call_output"
      || type == "custom_tool_call_output"
      || type == "local_shell_call_output"
      || type == "web_search_call"
      || type == "tool_search_call"
      || type == "tool_search_output"
      || type == "image_generation_call"
      || type == "compaction"
      || type == "compaction_summary"
      || type == "context_compaction"
      || type == "compaction_trigger"
      || type == "ghost_snapshot";
}

bool is_legacy_session_meta(element document) {
  if (!string_at(document, "/id") || !string_at(document, "/timestamp")) {
    return false;
  }

  element marker;
  return element_at(document, "/git", marker)
      || element_at(document, "/instructions", marker);
}

void parse_rollout_record(element document, RecordIR &record,
                          SessionParseResult &result, CodexParserState &state,
                          std::size_t record_index) {
  const auto type = string_at(document, "/type");
  if (!type) {
    if (const auto record_type = string_at(document, "/record_type")) {
      record.native_type = *record_type;
      ++state.recognized_records;
      append_event(record, UnknownEvent{.native_type = *record_type});
      return;
    }
    if (is_legacy_session_meta(document)) {
      record.native_type = "session_meta";
      ++state.recognized_records;
      parse_session_meta(document, record, result, state);
      return;
    }

    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "Codex rollout record is missing type", record.source_line);
    append_event(record, UnknownEvent{.native_type = "unknown"});
    return;
  }
  record.native_type = *type;

  if (is_response_item_type(*type)) {
    ++state.recognized_records;
    record.native_id = string_at(document, "/id").value_or("");
    parse_response_item(document, record, result, state, record_index);
    state.response_records.push_back({
        .record_index = record_index,
        .turn_index = state.turn_index,
    });
    return;
  }

  if (!requires_object_payload(*type)) {
    append_event(record, UnknownEvent{.native_type = *type});
    return;
  }
  ++state.recognized_records;

  element payload;
  if (!element_at(document, "/payload", payload)
      || payload.type() != element_type::OBJECT) {
    add_diagnostic(
        result, DiagnosticSeverity::Error, DiagnosticCode::MissingRequiredField,
        "Codex rollout record has no object payload", record.source_line);
    append_event(record, UnknownEvent{.native_type = *type});
    return;
  }
  if (*type != "session_meta") {
    record.native_id = string_at(payload, "/id").value_or("");
  }

  if (*type == "session_meta") {
    parse_session_meta(payload, record, result, state);
  } else if (*type == "response_item") {
    parse_response_item(payload, record, result, state, record_index);
    state.response_records.push_back({
        .record_index = record_index,
        .turn_index = state.turn_index,
    });
  } else if (*type == "event_msg") {
    parse_event_message(payload, record_index, record, result, state);
  } else if (*type == "compacted") {
    parse_compacted(payload, record);
    state.response_records.push_back({
        .record_index = record_index,
        .turn_index = state.turn_index,
    });
  } else if (*type == "turn_context") {
    update_turn(payload, state, true);
    state.model = string_at(payload, "/model").value_or(state.model);
    if (result.session.cwd.empty()) {
      result.session.cwd = string_at(payload, "/cwd").value_or("");
    }
    append_event(record, MetadataEvent{
                             .name = "turn_context",
                             .value = json_text(payload),
                         });
  } else if (*type == "world_state") {
    append_event(record, MetadataEvent{
                             .name = "world_state",
                             .value = json_text(payload),
                         });
  }
}

std::string comparable_message_content(const MessageEvent &message) {
  std::string content;
  for (const auto &block : message.content) {
    if (const auto *text = std::get_if<TextContent>(&block)) {
      content += "text:";
      content += std::to_string(text->text.size());
      content += ":";
      content += text->text;
    } else if (const auto *image = std::get_if<ImageContent>(&block)) {
      content += "image-mime:";
      content += std::to_string(image->mime_type.size());
      content += ":";
      content += image->mime_type;
      content += ":url:";
      content += std::to_string(image->url.size());
      content += ":";
      content += image->url;
      content += ":inline:";
      content += image->inline_data ? "1;" : "0;";
    } else if (const auto *unknown = std::get_if<UnknownContent>(&block)) {
      content += "unknown:";
      content += std::to_string(unknown->json.size());
      content += ":";
      content += unknown->json;
    }
  }
  return content;
}

bool same_message(const MessageEvent &left, const MessageEvent &right) {
  if (left.role != right.role
      || left.phase != right.phase
      || left.content.size() != right.content.size()
      || comparable_message_content(left)
             != comparable_message_content(right)) {
    return false;
  }
  // Inline URLs are normalized away. Compare their retained payloads too,
  // without copying large base64 strings into the comparison key.
  for (std::size_t index = 0; index < left.content.size(); ++index) {
    const auto *left_image = std::get_if<ImageContent>(&left.content[index]);
    const auto *right_image = std::get_if<ImageContent>(&right.content[index]);
    if (left_image == nullptr || right_image == nullptr) {
      continue;
    }
    const auto &left_data = left_image->base64_data;
    const auto &right_data = right_image->base64_data;
    if (left_data != right_data
        && (!left_data || !right_data || *left_data != *right_data)) {
      return false;
    }
  }
  return true;
}

const MessageEvent *message_event(const RecordIR &record) {
  for (const auto &event : record.events) {
    if (const auto *message = std::get_if<MessageEvent>(&event.payload)) {
      return message;
    }
  }
  return nullptr;
}

bool is_same_turn_duplicate(
    const PendingLegacyMessage &pending, const SessionIR &session,
    const std::vector<CanonicalMessage> &canonical_messages,
    std::unordered_set<std::size_t> &matched_canonical_records) {
  for (const auto &canonical_message : canonical_messages) {
    const std::size_t record_index = canonical_message.record_index;
    if (record_index >= session.records.size()
        || canonical_message.turn_index != pending.turn_index
        || matched_canonical_records.contains(record_index)) {
      continue;
    }

    const auto *canonical = message_event(session.records[record_index]);
    if (canonical != nullptr && same_message(pending.message, *canonical)) {
      matched_canonical_records.insert(record_index);
      return true;
    }
  }
  return false;
}

bool
same_completed_event(const EventIR &completed, EventIR &raw, bool same_id) {
  if (const auto *message = std::get_if<MessageEvent>(&completed.payload)) {
    const auto *other = std::get_if<MessageEvent>(&raw.payload);
    return other != nullptr
        && (same_message(*message, *other)
            || (same_id
                && message->role == other->role
                && message->phase == other->phase));
  }
  if (const auto *reasoning = std::get_if<ReasoningEvent>(&completed.payload)) {
    const auto *other = std::get_if<ReasoningEvent>(&raw.payload);
    return other != nullptr
        && (same_id
            || ((!reasoning->summary.empty() || !reasoning->content.empty())
                && reasoning->summary == other->summary
                && reasoning->content == other->content));
  }
  if (const auto *call = std::get_if<ToolCallEvent>(&completed.payload)) {
    const auto *other = std::get_if<ToolCallEvent>(&raw.payload);
    return other != nullptr
        && !call->call_id.empty()
        && call->call_id == other->call_id;
  }
  if (const auto *output = std::get_if<ToolResultEvent>(&completed.payload)) {
    auto *other = std::get_if<ToolResultEvent>(&raw.payload);
    if (other == nullptr
        || output->call_id.empty()
        || output->call_id != other->call_id) {
      return false;
    }
    // Raw outputs can be longer than the persistence-truncated TurnItem;
    // retain them, but enrich their otherwise missing completion status.
    other->is_error = other->is_error || output->is_error;
    if (!other->exit_code) {
      other->exit_code = output->exit_code;
    }
    return true;
  }
  return std::holds_alternative<CompactionEvent>(completed.payload)
      && std::holds_alternative<CompactionEvent>(raw.payload);
}

void deduplicate_completed_items(SessionIR &session, CodexParserState &state) {
  std::unordered_map<std::size_t, std::vector<std::size_t>> responses_by_turn;
  for (const auto &response : state.response_records) {
    responses_by_turn[response.turn_index].push_back(response.record_index);
  }
  std::unordered_map<std::size_t, std::unordered_set<std::size_t>> matched;
  for (const auto &item : state.completed_items) {
    auto &record = session.records.at(item.record_index);
    const auto candidates = responses_by_turn.find(item.turn_index);
    std::erase_if(record.events, [&](const EventIR &event) {
      if (candidates == responses_by_turn.end()) {
        return false;
      }
      for (const auto index : candidates->second) {
        auto &response = session.records.at(index);
        for (auto &raw : response.events) {
          if (matched[index].contains(raw.fragment_index)) {
            continue;
          }
          if (same_completed_event(event, raw,
                                   !item.id.empty()
                                       && item.id == response.native_id)) {
            matched[index].insert(raw.fragment_index);
            return true;
          }
        }
      }
      return false;
    });
    std::size_t fragment_index = 0;
    for (auto &event : record.events) {
      event.fragment_index = fragment_index++;
    }
    if (message_event(record) != nullptr) {
      state.canonical_messages.push_back({
          .record_index = item.record_index,
          .turn_index = item.turn_index,
      });
    }
  }
}

} // namespace

SessionParseResult parse_codex_rollout(std::string_view content) {
  const std::array sources{CodexRolloutSource{.content = content, .path = {}}};
  auto result = parse_codex_rollout_sources(sources);
  for (const auto &record : result.session.records) {
    for (const auto &event : record.events) {
      const auto *metadata = std::get_if<MetadataEvent>(&event.payload);
      if (metadata != nullptr && metadata->name == "codex.history_base") {
        add_diagnostic(result, DiagnosticSeverity::Warning,
                       DiagnosticCode::IncompleteStream,
                       "Codex history_base is unresolved when parsing content; "
                       "open the rollout with parse_session_file to load "
                       "inherited history",
                       record.source_line);
      }
    }
  }
  return result;
}

SessionParseResult
parse_codex_rollout_sources(std::span<const CodexRolloutSource> sources) {
  SessionParseResult result;
  result.session.format = LogFormat::Codex;

  CodexParserState state;
  simdjson::dom::parser parser;
  for (const auto &source : sources) {
    // Each file starts with metadata, but the requested (last) file owns the
    // session identity. Keep model/tool/turn context across inherited prefixes.
    state.saw_session_meta = false;
    const auto first_record = result.session.records.size();
    const auto first_diagnostic = result.diagnostics.size();
    JsonlReader reader{source.content};
    JsonlLine line;
    while (reader.next(line)) {
      const simdjson::padded_string padded{line.json};
      element document;
      const auto error = parser.parse(padded).get(document);
      if (error) {
        result.session.records.push_back(make_invalid_record(
            result.session.records.size(), line.source_line, line.raw));
        add_diagnostic(
            result, DiagnosticSeverity::Error, DiagnosticCode::InvalidJson,
            "invalid JSON: " + std::string{simdjson::error_message(error)},
            line.source_line);
        continue;
      }

      RecordIR record{
          .sequence = result.session.records.size(),
          .source_line = line.source_line,
          .native_sequence = uint_at(document, "/ordinal"),
          .native_type = {},
          .native_id = {},
          .native_parent_id = std::nullopt,
          .navigation_parent_id = std::nullopt,
          .timestamp = string_at(document, "/timestamp").value_or(""),
          .raw_json = std::string{line.raw},
          .events = {},
      };

      if (document.type() != element_type::OBJECT) {
        record.native_type = "non_object";
        append_event(record, UnknownEvent{.native_type = "non_object"});
        result.session.records.push_back(std::move(record));
        add_diagnostic(
            result, DiagnosticSeverity::Error, DiagnosticCode::ExpectedObject,
            "Codex rollout record must be a JSON object", line.source_line);
        continue;
      }

      const std::size_t record_index = result.session.records.size();
      parse_rollout_record(document, record, result, state, record_index);
      result.session.records.push_back(std::move(record));
    }
    for (std::size_t index = first_record;
         index < result.session.records.size(); ++index) {
      result.session.records.at(index).source_path = source.path;
    }
    for (std::size_t index = first_diagnostic;
         index < result.diagnostics.size(); ++index) {
      if (!source.path.empty()) {
        auto &diagnostic = result.diagnostics.at(index);
        diagnostic.message = source.path.string() + ": " + diagnostic.message;
      }
    }
  }

  deduplicate_completed_items(result.session, state);
  std::unordered_set<std::size_t> matched_canonical_records;
  for (auto &pending : state.legacy_messages) {
    if (pending.record_index < result.session.records.size()
        && !is_same_turn_duplicate(pending, result.session,
                                   state.canonical_messages,
                                   matched_canonical_records)) {
      append_event(result.session.records[pending.record_index],
                   std::move(pending.message));
    }
  }

  if (!result.session.records.empty() && state.recognized_records == 0) {
    add_diagnostic(result, DiagnosticSeverity::Fatal,
                   DiagnosticCode::FormatMismatch,
                   "input does not contain Codex rollout records");
  } else if (!result.session.records.empty() && !state.saw_session_meta) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "Codex rollout is missing session metadata");
  }

  return result;
}

} // namespace lupe::detail
