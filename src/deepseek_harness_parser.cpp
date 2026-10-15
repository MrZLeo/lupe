#include "lupe/log_format.hpp"
#include "lupe/session_ir.hpp"
#include "session_parser_internal.hpp"

#include "json_helpers.hpp"
#include "jsonl_reader.hpp"

#include <cstddef>

#include <array>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <optional>
#include <simdjson.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// Parser for DeepSeek Harness session logs (one JSONL event stream per
// session, format version 0). Physical layout:
//   - a `session` header row, always first
//   - ordinary event rows `{type, seq, time, data, ...}`
//   - packed streaming rows (`text-chunks`, `reasoning-chunks`,
//     `tool-call-chunks`) that expand into `assistant/chunk` events
// The default on-disk file is Zstandard-compressed (`session.jsonl.zstd`);
// this parser reads the decompressed JSONL content.

namespace lupe::detail {
namespace {

using simdjson::dom::element;
using simdjson::dom::element_type;

// ---------------------------------------------------------------------------
// Timestamps: deepseek-harness records Unix epoch milliseconds; the IR keeps
// ISO 8601 strings like the other parsers.
// ---------------------------------------------------------------------------

std::string format_epoch_ms(std::uint64_t epoch_ms) {
  const std::uint64_t days = epoch_ms / 86400000ULL;
  const std::uint64_t day_ms = epoch_ms % 86400000ULL;
  const std::uint64_t hours = day_ms / 3600000ULL;
  const std::uint64_t minutes = (day_ms / 60000ULL) % 60ULL;
  const std::uint64_t seconds = (day_ms / 1000ULL) % 60ULL;
  const std::uint64_t millis = day_ms % 1000ULL;

  // Howard Hinnant's civil_from_days (public domain).
  std::int64_t z = static_cast<std::int64_t>(days) + 719468LL;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::uint64_t day_of_era = static_cast<std::uint64_t>(z - era * 146097);
  const std::uint64_t year_of_era = (day_of_era
                                     - day_of_era / 1460
                                     + day_of_era / 36524
                                     - day_of_era / 146096)
                                  / 365;
  std::int64_t year = static_cast<std::int64_t>(year_of_era) + era * 400;
  const std::int64_t day_of_year = static_cast<std::int64_t>(
      day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100));
  const std::int64_t month_prime = (5 * day_of_year + 2) / 153;
  const std::int64_t day = day_of_year - (153 * month_prime + 2) / 5 + 1;
  const std::int64_t month = month_prime + (month_prime < 10 ? 3 : -9);
  year += month <= 2;

  char buffer[40];
  std::snprintf(buffer, sizeof(buffer),
                "%04lld-%02lld-%02lldT%02llu:%02llu:%02llu.%03lluZ",
                static_cast<long long>(year), static_cast<long long>(month),
                static_cast<long long>(day),
                static_cast<unsigned long long>(hours),
                static_cast<unsigned long long>(minutes),
                static_cast<unsigned long long>(seconds),
                static_cast<unsigned long long>(millis));
  return buffer;
}

// ---------------------------------------------------------------------------
// Small JSON helpers (same flavor as the other native parsers).
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Parser state shared across records.
// ---------------------------------------------------------------------------

struct HarnessState {
  std::unordered_map<std::string, std::string> tool_names;
  // Usage chunks stream ahead of the assembled assistant message. The
  // committed message usage wins; chunk usage is flushed at `step/end`
  // (a failed request can leave a usage chunk without any message).
  std::map<std::pair<std::uint64_t, std::uint64_t>, UsageEvent>
      pending_chunk_usage;
  std::uint64_t expected_seq = 0;
  bool saw_header = false;
};

void
check_seq(SessionParseResult &result, HarnessState &state,
          const std::optional<std::uint64_t> &seq, std::size_t source_line) {
  if (!seq) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "deepseek-harness event is missing `seq`", source_line);
    return;
  }
  if (*seq != state.expected_seq) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::InvalidLifecycle,
                   "deepseek-harness event seq is not contiguous (expected "
                       + std::to_string(state.expected_seq)
                       + ", got "
                       + std::to_string(*seq)
                       + ")",
                   source_line);
    state.expected_seq = *seq;
  }
  ++state.expected_seq;
}

// ---------------------------------------------------------------------------
// Usage.
// ---------------------------------------------------------------------------

UsageEvent make_usage(element usage) {
  // Counts are disjoint: inputTokens excludes cached input, and
  // reasoningTokens is accounting detail of the output. No total is
  // synthesized; the annotation shows exactly what was recorded.
  return UsageEvent{
      .scope = UsageScope::Message,
      .input_tokens = uint_at(usage, "/inputTokens"),
      .cached_input_tokens = uint_at(usage, "/cacheReadTokens"),
      .cache_write_tokens = uint_at(usage, "/cacheWriteTokens"),
      .output_tokens = uint_at(usage, "/outputTokens"),
      .reasoning_tokens = uint_at(usage, "/reasoningTokens"),
      .total_tokens = std::nullopt,
      .cost = std::nullopt,
  };
}

void append_usage_if_present(RecordIR &record, element usage) {
  UsageEvent event = make_usage(usage);
  if (event.input_tokens
      || event.cached_input_tokens
      || event.cache_write_tokens
      || event.output_tokens
      || event.reasoning_tokens) {
    append_event(record, std::move(event));
  }
}

// ---------------------------------------------------------------------------
// Messages and content blocks.
// ---------------------------------------------------------------------------

Role message_role(std::string_view role) {
  if (role == "system") {
    return Role::System;
  }
  if (role == "user") {
    return Role::User;
  }
  if (role == "assistant") {
    return Role::Assistant;
  }
  return Role::Unknown;
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

void append_message_block(std::optional<MessageEvent> &pending, Role role,
                          std::string_view raw_role, std::string_view provider,
                          std::string_view model, ContentBlock block) {
  if (!pending) {
    pending = make_message(role, raw_role, provider, model);
  }
  pending->content.push_back(std::move(block));
}

ImageContent parse_image_block(element block) {
  // Image bytes live out-of-line in the attachment store; the block keeps
  // an opaque `sha256:` reference plus verified metadata.
  ImageContent image;
  image.inline_data = false;

  element attachment;
  if (element_at(block, "/attachment", attachment)) {
    if (attachment.type() == element_type::STRING) {
      std::string_view text;
      if (!attachment.get_string().get(text)) {
        image.url = std::string{text};
      }
    } else if (attachment.type() == element_type::OBJECT) {
      image.url = first_string_at(attachment, {"/ref", "/uri", "/id", "/url"})
                      .value_or("");
      image.mime_type =
          first_string_at(attachment, {"/mediaType", "/mimeType", "/media_type",
                                       "/mime_type"})
              .value_or("");
    }
  }
  if (image.mime_type.empty()) {
    image.mime_type =
        first_string_at(block, {"/mediaType", "/mimeType"}).value_or("");
  }
  return image;
}

void append_result_content(std::vector<ContentBlock> &output, element content) {
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
      append_result_content(output, block);
    }
    return;
  }
  case element_type::OBJECT: {
    const std::string block_type = string_at(content, "/type").value_or("");
    if (block_type == "text") {
      output.emplace_back(TextContent{
          .text = string_at(content, "/text").value_or(""),
      });
    } else if (block_type == "image") {
      output.emplace_back(parse_image_block(content));
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

ToolResultEvent parse_tool_result_block(element block, HarnessState &state) {
  ToolResultEvent tool_result{
      .call_id = string_at(block, "/toolCallId").value_or(""),
      .name = {},
      .output = {},
      .is_error = bool_at(block, "/isError").value_or(false),
      .exit_code = std::nullopt,
  };
  if (const auto iterator = state.tool_names.find(tool_result.call_id);
      iterator != state.tool_names.end()) {
    tool_result.name = iterator->second;
  }

  element content;
  if (element_at(block, "/content", content)) {
    append_result_content(tool_result.output, content);
  }
  return tool_result;
}

void
parse_tool_call_block(element block, RecordIR &record, HarnessState &state) {
  const std::string call_id = string_at(block, "/id").value_or("");
  const std::string name = string_at(block, "/name").value_or("");
  if (!call_id.empty() && !name.empty()) {
    state.tool_names.insert_or_assign(call_id, name);
  }

  // `arguments` is the raw JSON string generated by the model; tolerate a
  // non-conforming object/array payload by re-serializing it.
  std::string input;
  bool input_is_json = false;
  if (const auto arguments = string_at(block, "/arguments")) {
    input = *arguments;
    input_is_json = true;
  } else {
    element arguments_field;
    if (element_at(block, "/arguments", arguments_field)) {
      input = json_text(arguments_field);
      input_is_json = true;
    }
  }

  append_event(record, ToolCallEvent{
                           .call_id = call_id,
                           .name = name,
                           .name_space = {},
                           .input = std::move(input),
                           .input_is_json = input_is_json,
                       });
}

// Parses a deepseek-harness Message (`user/message` data, or the `message`
// member of assistant/message and tool/result data).
void parse_harness_message(element message, RecordIR &record,
                           SessionParseResult &result, HarnessState &state) {
  const std::string raw_role = string_at(message, "/role").value_or("");
  const Role role = message_role(raw_role);

  std::string provider;
  std::string model;
  if (string_at(message, "/source/kind").value_or("") == "model") {
    provider = string_at(message, "/source/provider").value_or("");
    model = string_at(message, "/source/model").value_or("");
  }

  element content;
  if (!element_at(message, "/content", content)) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "deepseek-harness message is missing content",
                   record.source_line);
    append_event(record, make_message(role, raw_role, provider, model));
    return;
  }
  if (content.type() != element_type::ARRAY) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::InvalidFieldType,
                   "deepseek-harness message content must be an array",
                   record.source_line);
    auto event = make_message(role, raw_role, provider, model);
    event.content.emplace_back(UnknownContent{
        .native_type = "content",
        .json = json_text(content),
    });
    append_event(record, std::move(event));
    return;
  }

  simdjson::dom::array blocks;
  if (content.get_array().get(blocks)) {
    append_event(record, UnknownEvent{.native_type = record.native_type});
    return;
  }

  std::optional<MessageEvent> pending;
  bool appended_message = false;
  const auto flush = [&]() {
    if (!pending) {
      return;
    }
    append_event(record, std::move(*pending));
    pending.reset();
    appended_message = true;
  };

  for (const auto block : blocks) {
    if (block.type() != element_type::OBJECT) {
      append_message_block(
          pending, role, raw_role, provider, model,
          UnknownContent{.native_type = "content", .json = json_text(block)});
      continue;
    }

    const std::string block_type = string_at(block, "/type").value_or("");
    if (block_type == "text") {
      append_message_block(pending, role, raw_role, provider, model,
                           TextContent{
                               .text = string_at(block, "/text").value_or(""),
                           });
      continue;
    }
    if (block_type == "image") {
      append_message_block(pending, role, raw_role, provider, model,
                           parse_image_block(block));
      continue;
    }
    if (block_type == "reasoning") {
      flush();
      append_event(record,
                   ReasoningEvent{
                       .summary = {},
                       .content = string_at(block, "/text").value_or(""),
                       .encrypted = false,
                   });
      continue;
    }
    if (block_type == "tool-call") {
      flush();
      parse_tool_call_block(block, record, state);
      continue;
    }
    if (block_type == "tool-result") {
      flush();
      append_event(record, parse_tool_result_block(block, state));
      continue;
    }

    append_message_block(pending, role, raw_role, provider, model,
                         UnknownContent{
                             .native_type = block_type.empty()
                                              ? std::string{"unknown"}
                                              : block_type,
                             .json = json_text(block),
                         });
  }

  flush();
  if (!appended_message && role != Role::User) {
    // An assistant message carrying only reasoning/tool blocks (or nothing)
    // still surfaces its role and provenance; user messages consisting
    // solely of tool results need no empty bubble.
    append_event(record, make_message(role, raw_role, provider, model));
  }
}

// ---------------------------------------------------------------------------
// Compaction.
// ---------------------------------------------------------------------------

std::string extract_blocks_text(element blocks) {
  if (blocks.type() != element_type::ARRAY) {
    return {};
  }
  simdjson::dom::array array;
  if (blocks.get_array().get(array)) {
    return {};
  }
  std::string result;
  for (const auto block : array) {
    if (block.type() != element_type::OBJECT) {
      continue;
    }
    const std::string block_type = string_at(block, "/type").value_or("");
    if (block_type != "text" && block_type != "reasoning") {
      continue;
    }
    const std::string text = string_at(block, "/text").value_or("");
    if (text.empty()) {
      continue;
    }
    if (!result.empty()) {
      result.push_back('\n');
    }
    result += text;
  }
  return result;
}

std::string shadowed_context_json(element data) {
  return std::string{"{\"shadowedRange\":"}
       + json_at(data, "/shadowedRange")
       + ",\"shadowedSeqs\":"
       + json_at(data, "/shadowedSeqs")
       + "}";
}

void parse_compaction_summary(element data, RecordIR &record) {
  std::string summary;
  element summary_blocks;
  if (element_at(data, "/summary", summary_blocks)) {
    summary = extract_blocks_text(summary_blocks);
  }
  if (summary.empty()) {
    element raw_output;
    if (element_at(data, "/rawOutput", raw_output)) {
      summary = extract_blocks_text(raw_output);
    }
  }

  append_event(record,
               CompactionEvent{
                   .summary = std::move(summary),
                   .tokens_before = uint_at(data, "/shadowedTokenCount"),
                   .retained_from_record = std::nullopt,
                   .replacement_context_json = shadowed_context_json(data),
                   .trigger = "compaction/summary",
               });

  element usage;
  if (element_at(data, "/usage", usage)
      && usage.type() == element_type::OBJECT) {
    append_usage_if_present(record, usage);
  }
}

void parse_compaction_prune(element data, RecordIR &record) {
  append_event(record,
               CompactionEvent{
                   .summary = {},
                   .tokens_before = uint_at(data, "/shadowedTokenCount"),
                   .retained_from_record = std::nullopt,
                   .replacement_context_json = shadowed_context_json(data),
                   .trigger = "compaction/prune",
               });
}

// ---------------------------------------------------------------------------
// Streaming chunks (ordinary `assistant/chunk` rows and packed chunk rows).
// ---------------------------------------------------------------------------

void append_chunk_event(element data, RecordIR &record,
                        SessionParseResult &result, HarnessState &state) {
  element chunk;
  if (!element_at(data, "/chunk", chunk)
      || chunk.type() != element_type::OBJECT) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "deepseek-harness assistant/chunk is missing an object "
                   "`chunk`",
                   record.source_line);
    append_event(record, UnknownEvent{.native_type = record.native_type});
    return;
  }

  const std::string chunk_type = string_at(chunk, "/type").value_or("");
  if (chunk_type == "usage") {
    element usage;
    if (element_at(chunk, "/usage", usage)
        && usage.type() == element_type::OBJECT) {
      const auto turn = uint_at(data, "/turn");
      const auto step = uint_at(data, "/step");
      if (turn && step) {
        state.pending_chunk_usage[{*turn, *step}] = make_usage(usage);
      }
    }
    return;
  }

  const std::uint64_t turn = uint_at(data, "/turn").value_or(0);
  const std::uint64_t step = uint_at(data, "/step").value_or(0);
  append_event(
      record,
      ExecutionEvent{
          .subject = ExecutionSubject::Stream,
          .phase = ExecutionPhase::Updated,
          .correlation_id = std::to_string(turn) + ":" + std::to_string(step),
          .native_id = {},
          .native_type = "assistant/chunk",
          .status = chunk_type,
          .message = {},
          .terminal = false,
      });
}

std::vector<std::string> string_array(element array) {
  std::vector<std::string> values;
  if (array.type() != element_type::ARRAY) {
    return values;
  }
  simdjson::dom::array elements;
  if (array.get_array().get(elements)) {
    return values;
  }
  for (const auto value : elements) {
    std::string_view text;
    if (!value.get_string().get(text)) {
      values.emplace_back(text);
    }
  }
  return values;
}

std::vector<std::int64_t> delta_array(element array) {
  std::vector<std::int64_t> values;
  if (array.type() != element_type::ARRAY) {
    return values;
  }
  simdjson::dom::array elements;
  if (array.get_array().get(elements)) {
    return values;
  }
  for (const auto value : elements) {
    std::int64_t number = 0;
    if (!value.get_int64().get(number)) {
      values.push_back(number);
    }
  }
  return values;
}

// Expands a packed chunk row into one decoded `assistant/chunk` record per
// member. Packing only applies to runs of same-turn/step/index deltas.
void decode_chunk_row(std::string_view row_type, element document,
                      const JsonlLine &line, SessionParseResult &result,
                      HarnessState &state) {
  const auto seq0 = uint_at(document, "/seq0");
  const auto time0 = uint_at(document, "/time0");
  if (!seq0 || !time0) {
    RecordIR record =
        make_invalid_record(line.sequence, line.source_line, line.raw);
    record.native_type = std::string{row_type};
    result.session.records.push_back(std::move(record));
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "deepseek-harness packed chunk row is missing `seq0` or "
                   "`time0`",
                   line.source_line);
    return;
  }

  element data;
  if (!element_at(document, "/data", data)
      || data.type() != element_type::OBJECT) {
    RecordIR record =
        make_invalid_record(line.sequence, line.source_line, line.raw);
    record.native_type = std::string{row_type};
    result.session.records.push_back(std::move(record));
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "deepseek-harness packed chunk row is missing object "
                   "`data`",
                   line.source_line);
    return;
  }

  const std::uint64_t turn = uint_at(data, "/turn").value_or(0);
  const std::uint64_t step = uint_at(data, "/step").value_or(0);
  const std::uint64_t index = uint_at(data, "/index").value_or(0);
  const bool is_tool_call = row_type == "tool-call-chunks";
  const std::string delta_type = row_type == "text-chunks" ? "text-delta"
                               : row_type == "reasoning-chunks"
                                   ? "reasoning-delta"
                                   : "tool-call-delta";

  element fragments_field;
  const std::string fragments_pointer = is_tool_call ? "/args" : "/texts";
  const std::vector<std::string> fragments =
      element_at(data, fragments_pointer == "/args" ? "/args" : "/texts",
                 fragments_field)
          ? string_array(fragments_field)
          : std::vector<std::string>{};
  element dt_field;
  const std::vector<std::int64_t> deltas = element_at(data, "/dt", dt_field)
                                             ? delta_array(dt_field)
                                             : std::vector<std::int64_t>{};

  if (fragments.empty()) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "deepseek-harness packed chunk row has no fragments",
                   line.source_line);
    return;
  }
  if (deltas.size() + 1 != fragments.size()) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::InvalidLifecycle,
                   "deepseek-harness packed chunk row `dt` must have one "
                   "fewer entry than its fragments",
                   line.source_line);
  }

  const std::string call_id =
      is_tool_call ? string_at(data, "/id").value_or("") : std::string{};
  const std::string call_name =
      is_tool_call ? string_at(data, "/name").value_or("") : std::string{};
  if (is_tool_call && !call_id.empty() && !call_name.empty()) {
    state.tool_names.insert_or_assign(call_id, call_name);
  }

  std::uint64_t time = *time0;
  for (std::size_t k = 0; k < fragments.size(); ++k) {
    if (k > 0 && k - 1 < deltas.size()) {
      // Negative dt is allowed: wall-clock time need not be monotonic.
      time = static_cast<std::uint64_t>(static_cast<std::int64_t>(time)
                                        + deltas[k - 1]);
    }
    const std::uint64_t seq = *seq0 + k;

    std::string chunk_json = "{\"type\":"
                           + json_quote(delta_type)
                           + ",\"index\":"
                           + std::to_string(index);
    if (is_tool_call) {
      chunk_json += ",\"id\":" + json_quote(call_id);
      // The run's name belongs to the first delta of the run.
      if (k == 0 && !call_name.empty()) {
        chunk_json += ",\"name\":" + json_quote(call_name);
      }
      chunk_json += ",\"argumentsDelta\":" + json_quote(fragments[k]);
    } else {
      chunk_json += ",\"text\":" + json_quote(fragments[k]);
    }
    chunk_json.push_back('}');

    std::string decoded = std::string{"{\"type\":\"assistant/chunk\",\"seq\":"}
                        + std::to_string(seq)
                        + ",\"time\":"
                        + std::to_string(time)
                        + ",\"data\":{\"turn\":"
                        + std::to_string(turn)
                        + ",\"step\":"
                        + std::to_string(step)
                        + ",\"chunk\":"
                        + chunk_json
                        + "}}";

    RecordIR record{
        .sequence = result.session.records.size(),
        .source_line = line.source_line,
        .native_sequence = seq,
        .native_type = "assistant/chunk",
        .native_id = {},
        .native_parent_id = std::nullopt,
        .navigation_parent_id = std::nullopt,
        .timestamp = format_epoch_ms(time),
        .raw_json = std::move(decoded),
        .events = {},
    };
    check_seq(result, state, seq, line.source_line);

    append_event(
        record,
        ExecutionEvent{
            .subject = ExecutionSubject::Stream,
            .phase = ExecutionPhase::Updated,
            .correlation_id = std::to_string(turn) + ":" + std::to_string(step),
            .native_id = {},
            .native_type = "assistant/chunk",
            .status = delta_type,
            .message = {},
            .terminal = false,
        });
    result.session.records.push_back(std::move(record));
  }
}

// ---------------------------------------------------------------------------
// Turn/step lifecycle.
// ---------------------------------------------------------------------------

void flush_pending_usage(HarnessState &state, std::uint64_t turn,
                         std::uint64_t step, RecordIR &record) {
  const auto iterator = state.pending_chunk_usage.find({turn, step});
  if (iterator == state.pending_chunk_usage.end()) {
    return;
  }
  append_event(record, std::move(iterator->second));
  state.pending_chunk_usage.erase(iterator);
}

ExecutionPhase turn_end_phase(std::string_view kind) {
  if (kind == "completed") {
    return ExecutionPhase::Completed;
  }
  if (kind == "aborted" || kind == "interrupted") {
    return ExecutionPhase::Interrupted;
  }
  if (kind == "blocked" || kind == "error" || kind == "max-tokens") {
    return ExecutionPhase::Failed;
  }
  return ExecutionPhase::Unknown;
}

void parse_turn_end(element data, RecordIR &record) {
  const std::uint64_t turn = uint_at(data, "/turn").value_or(0);
  const std::string kind = string_at(data, "/reason/kind").value_or("");
  append_event(
      record,
      ExecutionEvent{
          .subject = ExecutionSubject::Turn,
          .phase = turn_end_phase(kind),
          .correlation_id = std::to_string(turn),
          .native_id = {},
          .native_type = "turn/end",
          .status = kind,
          .message = string_at(data, "/reason/error/message").value_or(""),
          .terminal = true,
      });
}

void parse_llm_retry(element data, RecordIR &record) {
  const std::uint64_t turn = uint_at(data, "/turn").value_or(0);
  const std::uint64_t step = uint_at(data, "/step").value_or(0);
  append_event(
      record,
      ExecutionEvent{
          .subject = ExecutionSubject::Stream,
          .phase = ExecutionPhase::Error,
          .correlation_id = std::to_string(turn) + ":" + std::to_string(step),
          .native_id = {},
          .native_type = "llm/retry",
          .status = string_at(data, "/failure/code").value_or(""),
          .message = string_at(data, "/failure/message").value_or(""),
          .terminal = false,
      });
}

// ---------------------------------------------------------------------------
// Metadata bucket.
// ---------------------------------------------------------------------------

bool is_metadata_event_type(std::string_view type) {
  static constexpr std::array<std::string_view, 28> kTypes{
      "agent/inbox/spliced",
      "agent-preset/selected",
      "approval/asked",
      "approval/decided",
      "approval/policy",
      "command/done",
      "command/run",
      "compaction/end",
      "compaction/start",
      "feedback/record",
      "goal/change",
      "hook/invoked",
      "hook/result",
      "permission/preset",
      "plan/mode",
      "request/context",
      "request/header",
      "sandbox/mode",
      "schedule/change",
      "session/end-seed",
      "session/title-llm-request",
      "subagent/descriptor",
      "todo/write",
      "tool-workflow/agent-end",
      "tool-workflow/agent-start",
      "tool-workflow/run-end",
      "tool-workflow/run-start",
  };
  for (const auto known : kTypes) {
    if (type == known) {
      return true;
    }
  }
  return false;
}

void append_metadata(RecordIR &record, element document, std::string name) {
  element data;
  append_event(record, MetadataEvent{
                           .name = std::move(name),
                           .value = element_at(document, "/data", data)
                                      ? json_text(data)
                                      : json_text(document),
                       });
}

void append_surface_op_metadata(RecordIR &record, element document) {
  // Surface replacements (compaction snapshots) keep the full placement in
  // metadata; the raw timeline still shows both the replaced nodes and the
  // replacement in chronological order.
  element surface_op;
  if (!element_at(document, "/surfaceOp", surface_op)
      || surface_op.type() != element_type::OBJECT) {
    return;
  }
  append_event(record, MetadataEvent{
                           .name = "surface_op",
                           .value = json_text(surface_op),
                       });
}

// ---------------------------------------------------------------------------
// Header.
// ---------------------------------------------------------------------------

void
parse_header(SessionParseResult &result, RecordIR &record, element document) {
  if (record.sequence != 0) {
    add_diagnostic(result, DiagnosticSeverity::Error,
                   DiagnosticCode::FormatMismatch,
                   "deepseek-harness session header must be the first JSONL "
                   "record",
                   record.source_line);
  }

  simdjson::dom::element version_field;
  const bool has_version = element_at(document, "/version", version_field);
  const auto parsed_version = uint_at(document, "/version");
  if (has_version && !parsed_version) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::UnsupportedVersion,
                   "deepseek-harness session header has an invalid `version`",
                   record.source_line);
  }
  const std::uint64_t version = parsed_version.value_or(0);
  result.session.source_version = std::to_string(version);
  if (version != 0) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::UnsupportedVersion,
                   "deepseek-harness session version "
                       + std::to_string(version)
                       + " is not the supported v0 schema",
                   record.source_line);
  }

  result.session.session_id = string_at(document, "/id").value_or("");
  result.session.cwd = string_at(document, "/cwd").value_or("");
  result.session.parent_session_ref = string_at(document, "/parentSession");

  if (const auto created_at = uint_at(document, "/createdAt")) {
    result.session.created_at = format_epoch_ms(*created_at);
    if (record.timestamp.empty()) {
      record.timestamp = result.session.created_at;
    }
  } else {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::MissingRequiredField,
                   "deepseek-harness session header is missing `createdAt`",
                   record.source_line);
  }
  if (result.session.session_id.empty()) {
    add_diagnostic(
        result, DiagnosticSeverity::Error, DiagnosticCode::MissingRequiredField,
        "deepseek-harness session header is missing `id`", record.source_line);
  }
}

// ---------------------------------------------------------------------------
// Ordinary event rows.
// ---------------------------------------------------------------------------

void parse_event(SessionParseResult &result, RecordIR &record, element document,
                 HarnessState &state) {
  element data;
  const bool has_data = element_at(document, "/data", data)
                     && data.type() == element_type::OBJECT;

  const std::string_view type = record.native_type;
  if (type == "user/message") {
    if (!has_data) {
      add_diagnostic(result, DiagnosticSeverity::Warning,
                     DiagnosticCode::MissingRequiredField,
                     "deepseek-harness user/message is missing object `data`",
                     record.source_line);
      append_event(record, UnknownEvent{.native_type = record.native_type});
      return;
    }
    parse_harness_message(data, record, result, state);
    append_surface_op_metadata(record, document);
    return;
  }

  if (type == "assistant/message") {
    element message;
    if (!has_data
        || !element_at(data, "/message", message)
        || message.type() != element_type::OBJECT) {
      add_diagnostic(result, DiagnosticSeverity::Warning,
                     DiagnosticCode::MissingRequiredField,
                     "deepseek-harness assistant/message is missing an object "
                     "`message`",
                     record.source_line);
      append_event(record, UnknownEvent{.native_type = record.native_type});
      return;
    }
    parse_harness_message(message, record, result, state);

    const auto turn = uint_at(data, "/turn");
    const auto step = uint_at(data, "/step");
    element usage;
    if (element_at(data, "/usage", usage)
        && usage.type() == element_type::OBJECT) {
      // Committed usage supersedes the streamed usage chunk for the step.
      append_usage_if_present(record, usage);
      if (turn && step) {
        state.pending_chunk_usage.erase({*turn, *step});
      }
    }
    append_surface_op_metadata(record, document);
    return;
  }

  if (type == "tool/result") {
    element message;
    if (!has_data
        || !element_at(data, "/message", message)
        || message.type() != element_type::OBJECT) {
      add_diagnostic(result, DiagnosticSeverity::Warning,
                     DiagnosticCode::MissingRequiredField,
                     "deepseek-harness tool/result is missing an object "
                     "`message`",
                     record.source_line);
      append_event(record, UnknownEvent{.native_type = record.native_type});
      return;
    }
    parse_harness_message(message, record, result, state);

    element error;
    if (element_at(data, "/error", error)
        && error.type() == element_type::OBJECT) {
      append_event(record, MetadataEvent{
                               .name = "tool_error",
                               .value = json_text(error),
                           });
    }
    element meta;
    if (element_at(data, "/meta", meta)) {
      append_event(record, MetadataEvent{
                               .name = "tool_meta",
                               .value = json_text(meta),
                           });
    }
    append_surface_op_metadata(record, document);
    return;
  }

  if (type == "tool/call") {
    // Audit copy of the model-requested call; the assembled tool-call block
    // on assistant/message is the conversation node. Register the name so
    // tool results can resolve it, and keep the raw record as metadata.
    if (has_data) {
      const std::string call_id = string_at(data, "/callId").value_or("");
      const std::string name = string_at(data, "/name").value_or("");
      if (!call_id.empty() && !name.empty()) {
        state.tool_names.insert_or_assign(call_id, name);
      }
    }
    append_metadata(record, document, "tool/call");
    return;
  }

  if (type == "assistant/chunk") {
    if (!has_data) {
      add_diagnostic(result, DiagnosticSeverity::Warning,
                     DiagnosticCode::MissingRequiredField,
                     "deepseek-harness assistant/chunk is missing object "
                     "`data`",
                     record.source_line);
      append_event(record, UnknownEvent{.native_type = record.native_type});
      return;
    }
    append_chunk_event(data, record, result, state);
    return;
  }

  if (type == "turn/start") {
    const std::uint64_t turn =
        has_data ? uint_at(data, "/turn").value_or(0) : 0;
    append_event(record, ExecutionEvent{
                             .subject = ExecutionSubject::Turn,
                             .phase = ExecutionPhase::Started,
                             .correlation_id = std::to_string(turn),
                             .native_id = {},
                             .native_type = "turn/start",
                             .status = {},
                             .message = {},
                             .terminal = false,
                         });
    return;
  }
  if (type == "turn/end") {
    if (has_data) {
      parse_turn_end(data, record);
    } else {
      append_event(record, ExecutionEvent{
                               .subject = ExecutionSubject::Turn,
                               .phase = ExecutionPhase::Unknown,
                               .correlation_id = {},
                               .native_id = {},
                               .native_type = "turn/end",
                               .status = {},
                               .message = {},
                               .terminal = true,
                           });
    }
    return;
  }
  if (type == "step/start" || type == "step/end") {
    const std::uint64_t turn =
        has_data ? uint_at(data, "/turn").value_or(0) : 0;
    const std::uint64_t step =
        has_data ? uint_at(data, "/step").value_or(0) : 0;
    const bool is_end = type == "step/end";
    append_event(
        record,
        ExecutionEvent{
            .subject = ExecutionSubject::Item,
            .phase =
                is_end ? ExecutionPhase::Completed : ExecutionPhase::Started,
            .correlation_id = std::to_string(turn) + ":" + std::to_string(step),
            .native_id = {},
            .native_type = std::string{type},
            .status = {},
            .message = {},
            .terminal = is_end,
        });
    if (is_end) {
      flush_pending_usage(state, turn, step, record);
    }
    return;
  }

  if (type == "compaction/summary") {
    if (has_data) {
      parse_compaction_summary(data, record);
    } else {
      append_event(record, UnknownEvent{.native_type = record.native_type});
    }
    return;
  }
  if (type == "compaction/prune") {
    if (has_data) {
      parse_compaction_prune(data, record);
    } else {
      append_event(record, UnknownEvent{.native_type = record.native_type});
    }
    return;
  }

  if (type == "llm/retry") {
    if (has_data) {
      parse_llm_retry(data, record);
    } else {
      append_event(record, UnknownEvent{.native_type = record.native_type});
    }
    return;
  }
  if (type == "llm/retry-started") {
    append_event(record, ExecutionEvent{
                             .subject = ExecutionSubject::Stream,
                             .phase = ExecutionPhase::Updated,
                             .correlation_id = {},
                             .native_id = {},
                             .native_type = "llm/retry-started",
                             .status = {},
                             .message = {},
                             .terminal = false,
                         });
    return;
  }

  if (type == "session/title") {
    append_event(
        record,
        MetadataEvent{
            .name = "session/title",
            .value = has_data
                       ? string_at(data, "/title").value_or(json_text(data))
                       : json_text(document),
        });
    return;
  }

  if (is_metadata_event_type(type)) {
    append_metadata(record, document, record.native_type);
    return;
  }

  // Plugin-extended unions can add event types at any time. `ignorable`
  // marks events an unknown reader may skip; everything else is a required
  // record this build does not understand.
  append_event(record, UnknownEvent{.native_type = record.native_type.empty()
                                                     ? std::string{"unknown"}
                                                     : record.native_type});
  if (!bool_at(document, "/ignorable").value_or(false)) {
    add_diagnostic(result, DiagnosticSeverity::Error,
                   DiagnosticCode::FormatMismatch,
                   "unsupported required deepseek-harness event type `"
                       + record.native_type
                       + "`",
                   record.source_line);
  }
}

} // namespace

SessionParseResult parse_deepseek_harness_session(std::string_view content) {
  SessionParseResult result;
  result.session.format = LogFormat::DeepseekHarness;

  JsonlReader reader{content};
  JsonlLine line;
  simdjson::dom::parser parser;
  HarnessState state;

  while (reader.next(line)) {
    element document;
    const simdjson::error_code parse_error =
        parser.parse(line.json).get(document);
    if (parse_error) {
      // The harness loader discards a torn tail row; surface it instead.
      result.session.records.push_back(
          make_invalid_record(line.sequence, line.source_line, line.raw));
      add_diagnostic(result, DiagnosticSeverity::Error,
                     DiagnosticCode::InvalidJson,
                     "invalid JSON in deepseek-harness session: "
                         + std::string{simdjson::error_message(parse_error)},
                     line.source_line);
      continue;
    }

    if (document.type() != element_type::OBJECT) {
      RecordIR record =
          make_invalid_record(line.sequence, line.source_line, line.raw);
      record.native_type = "invalid_record";
      result.session.records.push_back(std::move(record));
      add_diagnostic(
          result, DiagnosticSeverity::Error, DiagnosticCode::ExpectedObject,
          "deepseek-harness JSONL record must be an object", line.source_line);
      continue;
    }

    const std::string native_type =
        string_at(document, "/type").value_or("unknown");
    if (native_type == "unknown") {
      add_diagnostic(result, DiagnosticSeverity::Error,
                     DiagnosticCode::MissingRequiredField,
                     "deepseek-harness JSONL record is missing `type`",
                     line.source_line);
    }

    if (native_type == "text-chunks"
        || native_type == "reasoning-chunks"
        || native_type == "tool-call-chunks") {
      decode_chunk_row(native_type, document, line, result, state);
      continue;
    }

    const auto seq = uint_at(document, "/seq");
    const auto time = uint_at(document, "/time");
    // sequence is the IR record index: packed rows expand to several
    // records per physical line, so line.sequence is not usable here.
    RecordIR record{
        .sequence = result.session.records.size(),
        .source_line = line.source_line,
        .native_sequence = seq,
        .native_type = native_type,
        .native_id = {},
        .native_parent_id = std::nullopt,
        .navigation_parent_id = std::nullopt,
        .timestamp = time ? format_epoch_ms(*time) : std::string{},
        .raw_json = std::string{line.raw},
        .events = {},
    };

    if (native_type == "session") {
      if (state.saw_header) {
        add_diagnostic(result, DiagnosticSeverity::Error,
                       DiagnosticCode::FormatMismatch,
                       "deepseek-harness session contains more than one "
                       "header",
                       line.source_line);
      } else {
        state.saw_header = true;
        parse_header(result, record, document);
      }
    } else {
      if (native_type != "unknown") {
        check_seq(result, state, seq, line.source_line);
        if (!time) {
          add_diagnostic(result, DiagnosticSeverity::Warning,
                         DiagnosticCode::MissingRequiredField,
                         "deepseek-harness event is missing `time`",
                         line.source_line);
        }
      }
      parse_event(result, record, document, state);
    }

    result.session.records.push_back(std::move(record));
  }

  // A torn tail can leave streamed usage without its step/end; keep it
  // visible as an annotation on the last intact record rather than
  // dropping it.
  if (!state.pending_chunk_usage.empty()) {
    auto target = result.session.records.end();
    for (auto iterator = result.session.records.end();
         iterator != result.session.records.begin();) {
      --iterator;
      if (iterator->native_type != "invalid_json"
          && iterator->native_type != "invalid_record") {
        target = iterator;
        break;
      }
    }
    for (auto &[unused_key, usage] : state.pending_chunk_usage) {
      static_cast<void>(unused_key);
      if (target != result.session.records.end()) {
        append_event(*target, std::move(usage));
      }
    }
    state.pending_chunk_usage.clear();
  }

  // Resolve tool result names against calls seen later in the stream.
  for (auto &record : result.session.records) {
    for (auto &event : record.events) {
      auto *tool_result = std::get_if<ToolResultEvent>(&event.payload);
      if (tool_result == nullptr || !tool_result->name.empty()) {
        continue;
      }
      if (const auto iterator = state.tool_names.find(tool_result->call_id);
          iterator != state.tool_names.end()) {
        tool_result->name = iterator->second;
      }
    }
  }

  if (!result.session.records.empty() && !state.saw_header) {
    add_diagnostic(result, DiagnosticSeverity::Fatal,
                   DiagnosticCode::FormatMismatch,
                   "input is not a deepseek-harness session: missing "
                   "`session` header");
  }

  return result;
}

} // namespace lupe::detail
