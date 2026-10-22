#include "lupe/message_projection.hpp"

#include "lupe/log_message.hpp"
#include "lupe/session_ir.hpp"
#include "lupe/session_parser.hpp"
#include "lupe/structured_text.hpp"

#include "json_helpers.hpp"

#include <cstddef>

#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <simdjson.h>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace lupe {
namespace {

std::string content_text(const ContentBlock &block, bool show_unknown) {
  return std::visit(
      [show_unknown](const auto &content) -> std::string {
        using Content = std::decay_t<decltype(content)>;
        if constexpr (std::is_same_v<Content, TextContent>) {
          return content.text;
        } else if constexpr (std::is_same_v<Content, ImageContent>) {
          std::string rendered = "[image";
          if (!content.mime_type.empty()) {
            rendered += " ";
            rendered += content.mime_type;
          }
          if (!content.url.empty()) {
            rendered += ": ";
            rendered += content.url;
          } else if (content.inline_data) {
            rendered += ": inline data";
          }
          rendered += "]";
          return rendered;
        } else {
          return show_unknown ? content.json : std::string{};
        }
      },
      block);
}

std::string
join_content(const std::vector<ContentBlock> &blocks, bool show_unknown) {
  std::string joined;
  for (const auto &block : blocks) {
    const std::string part = content_text(block, show_unknown);
    if (part.empty()) {
      continue;
    }
    if (!joined.empty()) {
      joined += '\n';
    }
    joined += std::move(part);
  }
  return joined;
}

std::vector<DisplayImage>
content_images(const std::vector<ContentBlock> &blocks) {
  std::vector<DisplayImage> images;
  for (const auto &block : blocks) {
    const auto *image = std::get_if<ImageContent>(&block);
    if (image == nullptr) {
      continue;
    }
    images.push_back(DisplayImage{
        .mime_type = image->mime_type,
        .url = image->url,
        .inline_data = image->inline_data,
        .base64_data = image->base64_data,
    });
  }
  return images;
}

std::string tool_call_annotation(const ToolCallEvent &call) {
  std::string rendered = "call";
  if (!call.name_space.empty()) {
    rendered += " ";
    rendered += call.name_space;
    rendered += "::";
    rendered += call.name;
  } else if (!call.name.empty()) {
    rendered += " ";
    rendered += call.name;
  }

  if (!call.input.empty() && call.input_is_json) {
    rendered += " ";
    rendered += format_structured_text(call.input);
  }
  if (!call.call_id.empty()) {
    rendered += " [";
    rendered += call.call_id;
    rendered += "]";
  }
  if (!call.input.empty() && !call.input_is_json) {
    // Freeform scripts belong on their own lines, without JSON escaping or
    // structured-text normalization (which would corrupt literal \n).
    rendered += '\n';
    rendered += call.input;
  }
  return rendered;
}

std::string nested_call_annotation(const NestedToolCall &nested) {
  ToolCallEvent label = nested.call;
  label.input.clear();
  std::string rendered = "nested " + tool_call_annotation(label);
  if (!nested.status.empty()) {
    rendered += " status=" + nested.status;
  }
  if (nested.duration_ms) {
    rendered += std::format(" duration={}ms", *nested.duration_ms);
  }
  if (nested.cost) {
    rendered += std::format(" cost={}", *nested.cost);
  }
  if (nested.omitted_input_bytes) {
    rendered += "\nfull arguments omitted ("
              + std::to_string(*nested.omitted_input_bytes)
              + " bytes)";
  }
  if (!nested.call.input.empty()) {
    rendered += '\n';
    rendered += nested.call.input_is_json
                  ? format_structured_text(nested.call.input)
                  : nested.call.input;
  }
  if (!nested.error.empty()) {
    rendered += "\nerror: " + nested.error;
  }
  return rendered;
}

std::string usage_annotation(const UsageEvent &usage) {
  std::string rendered = "usage";
  auto append = [&](std::string_view label,
                    const std::optional<std::uint64_t> &value) {
    if (!value) {
      return;
    }
    rendered += " ";
    rendered += label;
    rendered += "=";
    rendered += std::to_string(*value);
  };

  append("input", usage.input_tokens);
  append("cached", usage.cached_input_tokens);
  append("cache-write", usage.cache_write_tokens);
  append("output", usage.output_tokens);
  append("reasoning", usage.reasoning_tokens);
  append("total", usage.total_tokens);
  if (usage.cost) {
    rendered += " cost=";
    rendered += std::to_string(*usage.cost);
  }
  switch (usage.scope) {
  case UsageScope::Message:
    rendered += " scope=message";
    break;
  case UsageScope::Turn:
    rendered += " scope=turn";
    break;
  case UsageScope::Session:
    rendered += " scope=session";
    break;
  case UsageScope::Unknown:
    break;
  }
  return rendered;
}

std::string event_timestamp(const EventIR &event, const RecordIR &record) {
  return event.timestamp.empty() ? record.timestamp : event.timestamp;
}

bool execution_event_visible(const ExecutionEvent &execution) {
  return (execution.subject == ExecutionSubject::Stream
          && execution.phase == ExecutionPhase::Error)
      || (execution.subject == ExecutionSubject::Turn
          && execution.phase == ExecutionPhase::Failed)
      || (execution.subject == ExecutionSubject::Item
          && execution.phase == ExecutionPhase::Completed
          && execution.native_type == "error");
}

std::string execution_event_message(const ExecutionEvent &execution) {
  if (!execution.message.empty()) {
    return execution.message;
  }
  if (!execution.status.empty()) {
    return execution.status;
  }
  if (execution.subject == ExecutionSubject::Turn
      && execution.phase == ExecutionPhase::Failed) {
    return "Codex Exec turn failed";
  }
  if (execution.subject == ExecutionSubject::Item
      && execution.native_type == "error") {
    return "Codex Exec item error";
  }
  return "Codex Exec stream error";
}

bool record_messages_hidden(const RecordIR &record) {
  for (const auto &event : record.events) {
    const auto *metadata = std::get_if<MetadataEvent>(&event.payload);
    if (metadata != nullptr
        && metadata->name == "custom_message.display"
        && metadata->value == "false") {
      return true;
    }
  }
  return false;
}

LogMessage make_message(const MessageEvent &message, const EventIR &event,
                        const RecordIR &record, bool show_unknown) {
  std::string role{role_name(message.role)};
  if (message.role == Role::Unknown && !message.raw_role.empty()) {
    role = message.raw_role;
  }

  return LogMessage{
      .role = std::move(role),
      .content = join_content(message.content, show_unknown),
      .annotations = {},
      .timestamp = event_timestamp(event, record),
      .raw_type = record.native_type,
      .source_line = record.source_line,
      .images = content_images(message.content),
  };
}

std::string todo_list_checklist(const std::string &json) {
  simdjson::dom::parser parser;
  const simdjson::padded_string padded{json};
  simdjson::dom::element root;
  if (parser.parse(padded).get(root)) {
    return json;
  }

  simdjson::dom::array items;
  if (root.get_array().get(items)) {
    return json;
  }

  std::string checklist;
  for (const simdjson::dom::element item : items) {
    if (item.type() != simdjson::dom::element_type::OBJECT) {
      return json;
    }
    const std::string text = detail::string_at(item, "/text").value_or("");
    const bool completed = detail::bool_at(item, "/completed").value_or(false);
    if (!checklist.empty()) {
      checklist.push_back('\n');
    }
    checklist += completed ? "[x] " : "[ ] ";
    checklist += text;
  }
  return checklist;
}

} // namespace

std::string severity_name(DiagnosticSeverity severity) {
  switch (severity) {
  case DiagnosticSeverity::Info:
    return "info";
  case DiagnosticSeverity::Warning:
    return "warning";
  case DiagnosticSeverity::Error:
    return "error";
  case DiagnosticSeverity::Fatal:
    return "fatal";
  }
  return "warning";
}

std::vector<LogMessage>
make_display_messages(const SessionIR &session, const DisplayOptions &options) {
  std::vector<LogMessage> messages;
  std::optional<std::size_t> last_message;
  std::map<std::pair<std::size_t, UsageScope>, std::size_t>
      usage_annotation_indices;
  const auto record_indices = select_conversation_records(
      session, options.leaf_id
                   ? std::optional<std::string_view>{*options.leaf_id}
                   : std::nullopt);

  for (const std::size_t record_index : record_indices) {
    if (record_index >= session.records.size()) {
      continue;
    }
    const auto &record = session.records[record_index];
    const bool hide_record_messages = record_messages_hidden(record);
    std::optional<std::size_t> last_assistant;

    for (const auto &event : record.events) {
      if (const auto *message = std::get_if<MessageEvent>(&event.payload)) {
        if (hide_record_messages) {
          continue;
        }
        messages.push_back(
            make_message(*message, event, record, options.show_unknown));
        last_message = messages.size() - 1;
        if (message->role == Role::Assistant || message->role == Role::Agent) {
          last_assistant = messages.size() - 1;
        }
        continue;
      }

      if (const auto *call = std::get_if<ToolCallEvent>(&event.payload)) {
        if (!last_assistant) {
          messages.push_back(LogMessage{
              .role = "assistant",
              .content = {},
              .annotations = {},
              .timestamp = event_timestamp(event, record),
              .raw_type = record.native_type,
              .source_line = record.source_line,
          });
          last_assistant = messages.size() - 1;
          last_message = last_assistant;
        }
        messages[*last_assistant].annotations.push_back(
            tool_call_annotation(*call));
        continue;
      }

      if (const auto *tool_result =
              std::get_if<ToolResultEvent>(&event.payload)) {
        std::string annotation = "result";
        if (!tool_result->name.empty()) {
          annotation += " for ";
          annotation += tool_result->name;
        }
        if (!tool_result->call_id.empty()) {
          annotation += " [";
          annotation += tool_result->call_id;
          annotation += "]";
        }
        if (tool_result->is_error) {
          annotation += " error";
        }
        if (tool_result->exit_code) {
          annotation += " exit=";
          annotation += std::to_string(*tool_result->exit_code);
        }

        std::vector<std::string> annotations{std::move(annotation)};
        if (tool_result->nested_calls_complete == false) {
          annotations.emplace_back("nested calls: incomplete");
        }
        for (const auto &nested : tool_result->nested_calls) {
          annotations.push_back(nested_call_annotation(nested));
        }
        if (!tool_result->full_output_path.empty()) {
          annotations.push_back("full output: "
                                + tool_result->full_output_path);
        }

        messages.push_back(LogMessage{
            .role = "tool",
            .content = join_content(tool_result->output, options.show_unknown),
            .annotations = std::move(annotations),
            .timestamp = event_timestamp(event, record),
            .raw_type = record.native_type,
            .source_line = record.source_line,
            .images = content_images(tool_result->output),
        });
        last_message = messages.size() - 1;
        continue;
      }

      if (const auto *reasoning = std::get_if<ReasoningEvent>(&event.payload)) {
        if (!options.show_reasoning) {
          continue;
        }
        std::string content = reasoning->summary;
        if (!reasoning->content.empty()) {
          if (!content.empty()) {
            content += '\n';
          }
          content += reasoning->content;
        }
        if (reasoning->encrypted && reasoning->content.empty()) {
          if (!content.empty()) {
            content += '\n';
          }
          content += "[encrypted reasoning]";
        }
        messages.push_back(LogMessage{
            .role = "assistant",
            .content = std::move(content),
            .annotations = {},
            .timestamp = event_timestamp(event, record),
            .raw_type = "reasoning",
            .source_line = record.source_line,
        });
        last_message = messages.size() - 1;
        continue;
      }

      if (const auto *compaction =
              std::get_if<CompactionEvent>(&event.payload)) {
        if (!options.show_compaction) {
          continue;
        }
        LogMessage message{
            .role = "system",
            .content = compaction->summary.empty()
                         ? std::string{"Conversation compacted"}
                         : compaction->summary,
            .annotations = {},
            .timestamp = event_timestamp(event, record),
            .raw_type = "compaction",
            .source_line = record.source_line,
        };
        if (compaction->tokens_before) {
          message.annotations.push_back(
              "tokens before=" + std::to_string(*compaction->tokens_before));
        }
        if (!compaction->trigger.empty()) {
          message.annotations.push_back("trigger=" + compaction->trigger);
        }
        if (compaction->retained_from_record) {
          message.annotations.push_back("retained from="
                                        + *compaction->retained_from_record);
        }
        messages.push_back(std::move(message));
        last_message = messages.size() - 1;
        continue;
      }

      if (const auto *usage = std::get_if<UsageEvent>(&event.payload)) {
        if (last_message) {
          auto &annotations = messages[*last_message].annotations;
          const auto key = std::pair{*last_message, usage->scope};
          const auto location = usage_annotation_indices.find(key);
          if (location == usage_annotation_indices.end()) {
            const std::size_t annotation_index = annotations.size();
            annotations.push_back(usage_annotation(*usage));
            usage_annotation_indices.emplace(key, annotation_index);
          } else {
            annotations[location->second] = usage_annotation(*usage);
          }
        }
        continue;
      }

      if (const auto *metadata = std::get_if<MetadataEvent>(&event.payload)) {
        if (metadata->name == "display_annotation" && last_message) {
          messages[*last_message].annotations.push_back(metadata->value);
          continue;
        }
        if (metadata->name == "codex_exec.todo_list") {
          messages.push_back(LogMessage{
              .role = "system",
              .content = todo_list_checklist(metadata->value),
              .annotations = {},
              .timestamp = event_timestamp(event, record),
              .raw_type = "todo_list",
              .source_line = record.source_line,
          });
          last_message = messages.size() - 1;
          continue;
        }
        if (!options.show_metadata) {
          continue;
        }
        messages.push_back(LogMessage{
            .role = "system",
            .content = metadata->value,
            .annotations = {},
            .timestamp = event_timestamp(event, record),
            .raw_type = metadata->name,
            .source_line = record.source_line,
        });
        last_message = messages.size() - 1;
        continue;
      }

      if (const auto *execution = std::get_if<ExecutionEvent>(&event.payload)) {
        if (!execution_event_visible(*execution)) {
          continue;
        }
        std::string content = execution_event_message(*execution);
        std::vector<std::string> annotations;
        if (!execution->status.empty() && execution->status != content) {
          annotations.push_back("status=" + execution->status);
        }
        if (!execution->native_id.empty()) {
          annotations.push_back("id=" + execution->native_id);
        }
        messages.push_back(LogMessage{
            .role = "system",
            .content = std::move(content),
            .annotations = std::move(annotations),
            .timestamp = event_timestamp(event, record),
            .raw_type = execution->native_type.empty() ? record.native_type
                                                       : execution->native_type,
            .source_line = record.source_line,
        });
        continue;
      }

      if (const auto *unknown = std::get_if<UnknownEvent>(&event.payload);
          unknown != nullptr && options.show_unknown) {
        messages.push_back(LogMessage{
            .role = "unknown",
            .content = record.raw_json,
            .annotations = {},
            .timestamp = event_timestamp(event, record),
            .raw_type = unknown->native_type,
            .source_line = record.source_line,
        });
        last_message = messages.size() - 1;
      }
    }
  }

  return messages;
}

std::string format_diagnostic(const Diagnostic &diagnostic) {
  std::string rendered = severity_name(diagnostic.severity);
  if (diagnostic.source_line > 0) {
    rendered += " at line ";
    rendered += std::to_string(diagnostic.source_line);
  }
  rendered += ": ";
  rendered += diagnostic.message;
  return rendered;
}

} // namespace lupe
