#ifndef LUPE_SESSION_IR_HPP_
#define LUPE_SESSION_IR_HPP_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "lupe/log_format.hpp"

namespace lupe {

enum class Role {
  User,
  Assistant,
  System,
  Developer,
  Agent,
  Unknown,
};

std::string_view role_name(Role role);

struct TextContent {
  std::string text;
};

struct ImageContent {
  std::string mime_type;
  std::string url;
  bool inline_data{false};
  // Shared with the display projection so large inline images are not copied
  // again when the TUI renders them inline.
  std::shared_ptr<const std::string> base64_data;
};

struct UnknownContent {
  std::string native_type;
  std::string json;
};

using ContentBlock = std::variant<TextContent, ImageContent, UnknownContent>;

struct MessageEvent {
  Role role{Role::Unknown};
  std::string raw_role;
  std::string provider;
  std::string model;
  std::string phase;
  std::vector<ContentBlock> content;
};

struct ReasoningEvent {
  std::string summary;
  std::string content;
  bool encrypted{false};
};

struct ToolCallEvent {
  std::string call_id;
  std::string name;
  std::string name_space;
  std::string input;
  bool input_is_json{false};
};

struct ToolResultEvent {
  std::string call_id;
  std::string name;
  std::vector<ContentBlock> output;
  bool is_error{false};
  std::optional<int> exit_code;
};

struct CompactionEvent {
  std::string summary;
  std::optional<std::uint64_t> tokens_before;
  std::optional<std::string> retained_from_record;
  std::string replacement_context_json;
  std::string trigger;
};

enum class UsageScope {
  Message,
  Turn,
  Session,
  Unknown,
};

struct UsageEvent {
  UsageScope scope{UsageScope::Unknown};
  std::optional<std::uint64_t> input_tokens;
  std::optional<std::uint64_t> cached_input_tokens;
  std::optional<std::uint64_t> cache_write_tokens;
  std::optional<std::uint64_t> output_tokens;
  std::optional<std::uint64_t> reasoning_tokens;
  std::optional<std::uint64_t> total_tokens;
  std::optional<double> cost;
};

struct MetadataEvent {
  std::string name;
  std::string value;
};

enum class ExecutionSubject {
  Stream,
  Thread,
  Turn,
  Item,
};

enum class ExecutionPhase {
  Started,
  Updated,
  Completed,
  Failed,
  Error,
  Interrupted,
  Unknown,
};

struct ExecutionEvent {
  ExecutionSubject subject{ExecutionSubject::Stream};
  ExecutionPhase phase{ExecutionPhase::Unknown};
  std::string correlation_id;
  std::string native_id;
  std::string native_type;
  std::string status;
  std::string message;
  bool terminal{false};
};

struct UnknownEvent {
  std::string native_type;
};

using EventPayload = std::variant<MessageEvent, ReasoningEvent, ToolCallEvent,
                                  ToolResultEvent, CompactionEvent, UsageEvent,
                                  MetadataEvent, UnknownEvent, ExecutionEvent>;

struct EventIR {
  std::size_t fragment_index{0};
  std::string timestamp;
  EventPayload payload;
};

struct RecordIR {
  std::size_t sequence{0};
  std::size_t source_line{0};
  std::optional<std::uint64_t> native_sequence;
  std::string native_type;
  std::string native_id;
  std::optional<std::string> native_parent_id;
  // Parser-supplied navigation override. nullopt falls back to the native
  // parent, an empty value explicitly means a logical root, and a non-empty
  // value identifies the logical parent.
  std::optional<std::string> navigation_parent_id;
  std::string timestamp;
  std::string raw_json;
  std::vector<EventIR> events;
  // Physical origin when a transcript spans multiple rollout files. Empty for
  // content-only parsing; source_line remains relative to this physical file.
  // Explicit default keeps existing aggregate initializers warning-free.
  // NOLINTNEXTLINE(readability-redundant-member-init)
  std::filesystem::path source_path{};
};

struct SessionIR {
  LogFormat format{LogFormat::Generic};
  std::filesystem::path source_path;
  std::string session_id;
  // Provider-defined reference: this can be an ID, path, or another native
  // locator, so consumers must not assume it is directly joinable as an ID.
  std::optional<std::string> parent_session_ref;
  std::string source_version;
  std::string cwd;
  std::string created_at;
  std::optional<std::string> active_leaf_id;
  std::vector<RecordIR> records;
};

enum class DiagnosticSeverity {
  Info,
  Warning,
  Error,
  Fatal,
};

enum class DiagnosticCode {
  IoError,
  EmptyInput,
  InvalidJson,
  ExpectedObject,
  MissingRequiredField,
  FormatMismatch,
  UnsupportedVersion,
  DuplicateNativeId,
  MissingParent,
  ParentCycle,
  EmptyCallId,
  InconsistentSessionId,
  InvalidFieldType,
  InvalidLifecycle,
  IncompleteStream,
};

struct Diagnostic {
  DiagnosticSeverity severity{DiagnosticSeverity::Warning};
  DiagnosticCode code{DiagnosticCode::MissingRequiredField};
  std::string message;
  std::size_t source_line{0};
};

struct SessionParseResult {
  SessionIR session;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool has_fatal_error() const;
};

template <typename Event>
void
append_event(RecordIR &record, Event event, const std::string &timestamp = {}) {
  record.events.push_back(EventIR{
      .fragment_index = record.events.size(),
      .timestamp = std::move(timestamp),
      .payload = std::move(event),
  });
}

} // namespace lupe

#endif // LUPE_SESSION_IR_HPP_
