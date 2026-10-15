#include "lupe/log_format.hpp"
#include "lupe/log_message.hpp"
#include "lupe/message_projection.hpp"
#include "lupe/session_ir.hpp"
#include "lupe/session_parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

template <typename Event>
const Event *first_event(const lupe::RecordIR &record) {
  for (const auto &event : record.events) {
    if (const auto *value = std::get_if<Event>(&event.payload)) {
      return value;
    }
  }
  return nullptr;
}

template <typename Event>
std::size_t event_count(const lupe::RecordIR &record) {
  return static_cast<std::size_t>(
      std::count_if(record.events.begin(), record.events.end(),
                    [](const lupe::EventIR &event) {
                      return std::holds_alternative<Event>(event.payload);
                    }));
}

const lupe::RecordIR *
record_with_id(const lupe::SessionIR &session, std::string_view id) {
  const auto record = std::ranges::find_if(
      session.records, [id](const lupe::RecordIR &candidate) {
        return candidate.native_id == id;
      });
  return record == session.records.end() ? nullptr : &*record;
}

const lupe::LogMessage *
message_with_content(const std::vector<lupe::LogMessage> &messages,
                     std::string_view content) {
  const auto message = std::ranges::find_if(
      messages, [content](const lupe::LogMessage &candidate) {
        return candidate.content == content;
      });
  return message == messages.end() ? nullptr : &*message;
}

bool has_diagnostic(const lupe::SessionParseResult &parsed,
                    lupe::DiagnosticCode code, std::size_t source_line) {
  return std::ranges::any_of(
      parsed.diagnostics,
      [code, source_line](const lupe::Diagnostic &diagnostic) {
        return diagnostic.code == code && diagnostic.source_line == source_line;
      });
}

} // namespace

TEST_CASE("Pi v3 parsing preserves branches, tools, usage, and recovery",
          "[session_parser][pi]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"session","version":3,"id":"pi-session","timestamp":"2026-07-23T00:00:00Z","cwd":"/repo"}
{"type":"message","id":"u1","parentId":null,"timestamp":"2026-07-23T00:00:01Z","message":{"role":"user","content":"question"}}
{"type":"message","id":"a-old","parentId":"u1","timestamp":"2026-07-23T00:00:02Z","message":{"role":"assistant","content":[{"type":"text","text":"old branch"}]}}
{"type":"message","id":"a-new","parentId":"u1","timestamp":"2026-07-23T00:00:03Z","message":{"role":"assistant","provider":"openai","model":"gpt-5","content":[{"type":"text","text":"new branch"},{"type":"thinking","thinking":"inspect first"},{"type":"toolCall","id":"pi-call","name":"read","arguments":{"path":"README.md"}},{"type":"text","text":"after tool"}],"usage":{"input":11,"cacheRead":3,"cacheWrite":2,"output":7,"reasoning":5,"totalTokens":28,"cost":{"total":0.25}}}}
{"type":"message","id":"broken"
{"type":"message","id":"r1","parentId":"a-new","timestamp":"2026-07-23T00:00:04Z","message":{"role":"toolResult","toolCallId":"pi-call","toolName":"read","content":[{"type":"text","text":"file contents"}],"isError":false}})",
      lupe::LogFormat::Pi);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.session_id == "pi-session");
  REQUIRE(parsed.session.source_version == "3");
  REQUIRE(parsed.session.cwd == "/repo");
  REQUIRE(parsed.session.active_leaf_id == "r1");
  REQUIRE(parsed.session.records.size() == 6);
  REQUIRE(has_diagnostic(parsed, lupe::DiagnosticCode::InvalidJson, 5));

  const auto *assistant_record = record_with_id(parsed.session, "a-new");
  REQUIRE(assistant_record != nullptr);
  REQUIRE(event_count<lupe::MessageEvent>(*assistant_record) == 2);
  REQUIRE(event_count<lupe::ReasoningEvent>(*assistant_record) == 1);
  REQUIRE(event_count<lupe::ToolCallEvent>(*assistant_record) == 1);
  REQUIRE(event_count<lupe::UsageEvent>(*assistant_record) == 1);

  const auto *call = first_event<lupe::ToolCallEvent>(*assistant_record);
  REQUIRE(call != nullptr);
  REQUIRE(call->call_id == "pi-call");
  REQUIRE(call->name == "read");
  REQUIRE(call->input_is_json);
  REQUIRE(call->input.contains("README.md"));

  const auto *usage = first_event<lupe::UsageEvent>(*assistant_record);
  REQUIRE(usage != nullptr);
  REQUIRE(usage->input_tokens == 11);
  REQUIRE(usage->cached_input_tokens == 3);
  REQUIRE(usage->cache_write_tokens == 2);
  REQUIRE(usage->output_tokens == 7);
  REQUIRE(usage->reasoning_tokens == 5);
  REQUIRE(usage->total_tokens == 28);
  REQUIRE(usage->cost == 0.25);

  const auto *result_record = record_with_id(parsed.session, "r1");
  REQUIRE(result_record != nullptr);
  const auto *tool_result = first_event<lupe::ToolResultEvent>(*result_record);
  REQUIRE(tool_result != nullptr);
  REQUIRE(tool_result->call_id == "pi-call");
  REQUIRE(tool_result->name == "read");
  REQUIRE_FALSE(tool_result->is_error);
  REQUIRE(std::get<lupe::TextContent>(tool_result->output.front()).text
          == "file contents");

  const auto displayed = lupe::make_display_messages(
      parsed.session, lupe::DisplayOptions{.show_reasoning = true});
  REQUIRE(message_with_content(displayed, "old branch") == nullptr);
  REQUIRE(message_with_content(displayed, "question") != nullptr);
  const auto *new_branch = message_with_content(displayed, "new branch");
  REQUIRE(new_branch != nullptr);
  REQUIRE(std::ranges::any_of(new_branch->annotations,

                              [](const std::string &annotation) {
                                return annotation.contains("pi::read")
                                    && annotation.contains("pi-call");
                              }));
  REQUIRE(message_with_content(displayed, "inspect first") != nullptr);
  REQUIRE(message_with_content(displayed, "file contents") != nullptr);
}

TEST_CASE("Codex response items take precedence over duplicated event messages",
          "[session_parser][codex]") {
  const auto parsed = lupe::parse_session_content(
      R"({"timestamp":"2026-07-23T00:00:00Z","type":"session_meta","payload":{"id":"codex-session","timestamp":"2026-07-23T00:00:00Z","cwd":"/repo","cli_version":"1.2.3","model_provider":"openai"}}
{"timestamp":"2026-07-23T00:00:01Z","type":"event_msg","payload":{"type":"agent_message","message":"canonical answer","phase":"final_answer"}}
{"timestamp":"2026-07-23T00:00:02Z","type":"response_item","payload":{"type":"message","role":"assistant","phase":"final_answer","content":[{"type":"output_text","text":"canonical answer"}]}}
{"timestamp":"2026-07-23T00:00:03Z","type":"response_item","payload":{"type":"reasoning","summary":[{"type":"summary_text","text":"Plan"}],"content":[{"type":"reasoning_text","text":"Inspect"}],"encrypted_content":"ciphertext"}}
{"timestamp":"2026-07-23T00:00:04Z","type":"response_item","payload":{"type":"function_call","call_id":"codex-call","name":"exec_command","arguments":"{\"cmd\":\"pwd\"}"}}
{"timestamp":"2026-07-23T00:00:05Z","type":"response_item","payload":{"type":"function_call_output","call_id":"codex-call","output":[{"type":"output_text","text":"/repo"}]}}
{"timestamp":"2026-07-23T00:00:06Z","type":"compacted","payload":{"message":"condensed context","tokens_before":4096,"replacement_history":[{"role":"user","content":"kept"}],"trigger":"auto"}})",
      lupe::LogFormat::Codex);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.session_id == "codex-session");
  REQUIRE(parsed.session.source_version == "1.2.3");
  REQUIRE(parsed.session.cwd == "/repo");
  REQUIRE(parsed.session.records.size() == 7);

  // event_msg messages are buffered and omitted when canonical response
  // messages exist, preventing the same semantic message from being shown
  // twice.
  REQUIRE(parsed.session.records[1].events.empty());

  const auto *message =
      first_event<lupe::MessageEvent>(parsed.session.records[2]);
  REQUIRE(message != nullptr);
  REQUIRE(message->role == lupe::Role::Assistant);
  REQUIRE(message->provider == "openai");
  REQUIRE(message->phase == "final_answer");
  REQUIRE(std::get<lupe::TextContent>(message->content.front()).text
          == "canonical answer");

  const auto *reasoning =
      first_event<lupe::ReasoningEvent>(parsed.session.records[3]);
  REQUIRE(reasoning != nullptr);
  REQUIRE(reasoning->summary == "Plan");
  REQUIRE(reasoning->content == "Inspect");
  REQUIRE(reasoning->encrypted);

  const auto *call =
      first_event<lupe::ToolCallEvent>(parsed.session.records[4]);
  REQUIRE(call != nullptr);
  REQUIRE(call->call_id == "codex-call");
  REQUIRE(call->name == "exec_command");
  REQUIRE(call->input_is_json);
  REQUIRE(call->input.contains("\"pwd\""));

  const auto *tool_result =
      first_event<lupe::ToolResultEvent>(parsed.session.records[5]);
  REQUIRE(tool_result != nullptr);
  REQUIRE(tool_result->call_id == "codex-call");
  REQUIRE(tool_result->name == "exec_command");
  REQUIRE(std::get<lupe::TextContent>(tool_result->output.front()).text
          == "/repo");

  const auto *compaction =
      first_event<lupe::CompactionEvent>(parsed.session.records[6]);
  REQUIRE(compaction != nullptr);
  REQUIRE(compaction->summary == "condensed context");
  REQUIRE(compaction->tokens_before == 4096);
  REQUIRE(compaction->trigger == "auto");
  REQUIRE_FALSE(compaction->replacement_context_json.empty());

  const auto displayed = lupe::make_display_messages(
      parsed.session, lupe::DisplayOptions{.show_reasoning = true});
  REQUIRE(message_with_content(displayed, "canonical answer") != nullptr);
  REQUIRE(message_with_content(displayed, "Plan\nInspect") != nullptr);
  REQUIRE(std::count_if(displayed.begin(), displayed.end(),
                        [](const lupe::LogMessage &message) {
                          return message.content == "canonical answer";
                        })
          == 1);
  const auto *output = message_with_content(displayed, "/repo");
  REQUIRE(output != nullptr);
  REQUIRE(output->role == "tool");
  REQUIRE(message_with_content(displayed, "condensed context") != nullptr);
}

TEST_CASE("Codex event messages remain a usable legacy fallback",
          "[session_parser][codex]") {
  const auto parsed = lupe::parse_session_content(
      R"({"timestamp":"2026-07-23T00:00:00Z","type":"session_meta","payload":{"id":"legacy-session","cwd":"/repo"}}
{"timestamp":"2026-07-23T00:00:01Z","type":"event_msg","payload":{"type":"user_message","message":"legacy question"}}
{"timestamp":"2026-07-23T00:00:02Z","type":"event_msg","payload":{"type":"agent_message","message":"legacy answer","phase":"final_answer"}}
{"timestamp":"2026-07-23T00:00:03Z","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":10,"cached_input_tokens":2,"output_tokens":4,"reasoning_output_tokens":1,"total_tokens":17},"total_token_usage":{"input_tokens":30,"output_tokens":12,"total_tokens":42}}}}
{"timestamp":"2026-07-23T00:00:04Z","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":11,"cached_input_tokens":3,"output_tokens":5,"reasoning_output_tokens":2,"total_tokens":21},"total_token_usage":{"input_tokens":31,"output_tokens":13,"total_tokens":44}}}})",
      lupe::LogFormat::Codex);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.records.size() == 5);
  REQUIRE(event_count<lupe::MessageEvent>(parsed.session.records[1]) == 1);
  REQUIRE(event_count<lupe::MessageEvent>(parsed.session.records[2]) == 1);
  REQUIRE(event_count<lupe::UsageEvent>(parsed.session.records[3]) == 2);

  const auto *turn_usage =
      first_event<lupe::UsageEvent>(parsed.session.records[3]);
  REQUIRE(turn_usage != nullptr);
  REQUIRE(turn_usage->scope == lupe::UsageScope::Turn);
  REQUIRE(turn_usage->input_tokens == 10);
  REQUIRE(turn_usage->cached_input_tokens == 2);
  REQUIRE(turn_usage->output_tokens == 4);
  REQUIRE(turn_usage->reasoning_tokens == 1);
  REQUIRE(turn_usage->total_tokens == 17);

  const auto displayed = lupe::make_display_messages(parsed.session);
  const auto *question = message_with_content(displayed, "legacy question");
  const auto *answer = message_with_content(displayed, "legacy answer");
  REQUIRE(question != nullptr);
  REQUIRE(question->role == "user");
  REQUIRE(answer != nullptr);
  REQUIRE(answer->role == "assistant");
  REQUIRE(answer->annotations.size() == 2);
  REQUIRE(std::ranges::any_of(answer->annotations,
                              [](const std::string &annotation) {
                                return annotation.contains("usage input=11");
                              }));
  REQUIRE(std::ranges::any_of(answer->annotations,
                              [](const std::string &annotation) {
                                return annotation.contains("usage input=31");
                              }));
}

TEST_CASE("Claude Code parsing follows UUID branches and compact boundaries",
          "[session_parser][claudecode]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"user","uuid":"u1","parentUuid":null,"sessionId":"claude-session","timestamp":"2026-07-23T00:00:00Z","cwd":"/repo","version":"2.0","message":{"role":"user","content":[{"type":"text","text":"question"}]}}
{"type":"assistant","uuid":"a-old","parentUuid":"u1","sessionId":"claude-session","timestamp":"2026-07-23T00:00:01Z","message":{"role":"assistant","model":"claude-old","content":[{"type":"text","text":"old branch"}]}}
{"type":"assistant","uuid":"a-new","parentUuid":"u1","sessionId":"claude-session","timestamp":"2026-07-23T00:00:02Z","message":{"role":"assistant","model":"claude-sonnet","content":[{"type":"text","text":"new branch"},{"type":"thinking","thinking":"inspect first","summary":"Plan"},{"type":"tool_use","id":"claude-call","name":"Read","input":{"file_path":"README.md"}},{"type":"text","text":"after tool"}],"usage":{"input_tokens":12,"cache_read_input_tokens":4,"cache_creation_input_tokens":3,"output_tokens":8,"thinking_tokens":2,"total_tokens":29}}}
{"type":"assistant","uuid":
{"type":"user","uuid":"t1","parentUuid":"a-new","sessionId":"claude-session","timestamp":"2026-07-23T00:00:03Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"claude-call","content":[{"type":"text","text":"file contents"}],"is_error":false}]}}
{"type":"system","subtype":"compact_boundary","uuid":"c1","parentUuid":null,"logicalParentUuid":"t1","sessionId":"claude-session","timestamp":"2026-07-23T00:00:04Z","content":"Conversation compacted","compactMetadata":{"preTokens":2048,"trigger":"auto"}}
{"type":"user","uuid":"cs1","parentUuid":"c1","sessionId":"claude-session","timestamp":"2026-07-23T00:00:05Z","isCompactSummary":true,"isVisibleInTranscriptOnly":true,"message":{"role":"user","content":[{"type":"text","text":"condensed context"}]}}
{"type":"assistant","uuid":"side1","parentUuid":"a-old","sessionId":"agent-session","timestamp":"2026-07-23T00:00:06Z","isSidechain":true,"agentId":"agent-1","message":{"role":"assistant","content":[{"type":"text","text":"sidechain output"}]}}
{"type":"system","subtype":"compact_boundary","uuid":"side-compact","parentUuid":null,"logicalParentUuid":"side1","sessionId":"agent-session","timestamp":"2026-07-23T00:00:07Z","isSidechain":true,"content":"Sidechain compacted","compactMetadata":{"preTokens":512,"trigger":"auto"}})",
      lupe::LogFormat::ClaudeCode);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.session_id == "claude-session");
  REQUIRE(parsed.session.source_version == "2.0");
  REQUIRE(parsed.session.cwd == "/repo");
  REQUIRE(parsed.session.active_leaf_id == "cs1");
  REQUIRE(parsed.session.records.size() == 9);
  REQUIRE(has_diagnostic(parsed, lupe::DiagnosticCode::InvalidJson, 4));

  const auto *assistant_record = record_with_id(parsed.session, "a-new");
  REQUIRE(assistant_record != nullptr);
  REQUIRE(event_count<lupe::MessageEvent>(*assistant_record) == 2);
  REQUIRE(event_count<lupe::ReasoningEvent>(*assistant_record) == 1);
  REQUIRE(event_count<lupe::ToolCallEvent>(*assistant_record) == 1);
  REQUIRE(event_count<lupe::UsageEvent>(*assistant_record) == 1);

  const auto *call = first_event<lupe::ToolCallEvent>(*assistant_record);
  REQUIRE(call != nullptr);
  REQUIRE(call->call_id == "claude-call");
  REQUIRE(call->name == "Read");
  REQUIRE(call->input_is_json);
  REQUIRE(call->input.contains("README.md"));

  const auto *usage = first_event<lupe::UsageEvent>(*assistant_record);
  REQUIRE(usage != nullptr);
  REQUIRE(usage->input_tokens == 12);
  REQUIRE(usage->cached_input_tokens == 4);
  REQUIRE(usage->cache_write_tokens == 3);
  REQUIRE(usage->output_tokens == 8);
  REQUIRE(usage->reasoning_tokens == 2);
  REQUIRE(usage->total_tokens == 29);

  const auto *result_record = record_with_id(parsed.session, "t1");
  REQUIRE(result_record != nullptr);
  const auto *tool_result = first_event<lupe::ToolResultEvent>(*result_record);
  REQUIRE(tool_result != nullptr);
  REQUIRE(tool_result->call_id == "claude-call");
  REQUIRE(tool_result->name == "Read");
  REQUIRE_FALSE(tool_result->is_error);
  REQUIRE(std::get<lupe::TextContent>(tool_result->output.front()).text
          == "file contents");

  const auto *compaction_record = record_with_id(parsed.session, "c1");
  REQUIRE(compaction_record != nullptr);
  REQUIRE_FALSE(compaction_record->native_parent_id.has_value());
  REQUIRE(compaction_record->navigation_parent_id == "t1");
  const auto *compaction =
      first_event<lupe::CompactionEvent>(*compaction_record);
  REQUIRE(compaction != nullptr);
  REQUIRE(compaction->summary == "Conversation compacted");
  REQUIRE(compaction->tokens_before == 2048);
  REQUIRE(compaction->trigger == "auto");
  REQUIRE(compaction->retained_from_record == "t1");

  const auto *summary_record = record_with_id(parsed.session, "cs1");
  REQUIRE(summary_record != nullptr);
  const auto *summary = first_event<lupe::CompactionEvent>(*summary_record);
  REQUIRE(summary != nullptr);
  REQUIRE(summary->summary == "condensed context");

  const auto selected = lupe::select_conversation_records(parsed.session);
  REQUIRE(selected.size() == 5);
  REQUIRE(parsed.session.records[selected[0]].native_id == "u1");
  REQUIRE(parsed.session.records[selected[1]].native_id == "a-new");
  REQUIRE(parsed.session.records[selected[2]].native_id == "t1");
  REQUIRE(parsed.session.records[selected[3]].native_id == "c1");
  REQUIRE(parsed.session.records[selected[4]].native_id == "cs1");

  const auto displayed = lupe::make_display_messages(
      parsed.session, lupe::DisplayOptions{.show_reasoning = true});
  REQUIRE(message_with_content(displayed, "old branch") == nullptr);
  REQUIRE(message_with_content(displayed, "new branch") != nullptr);
  REQUIRE(message_with_content(displayed, "Plan\ninspect first") != nullptr);
  REQUIRE(message_with_content(displayed, "file contents") != nullptr);
  REQUIRE(message_with_content(displayed, "sidechain output") == nullptr);
  const auto *compact_message =
      message_with_content(displayed, "condensed context");
  REQUIRE(compact_message != nullptr);
  REQUIRE(compact_message->role == "system");
}

TEST_CASE("Claude Code continuation switches to the current session id",
          "[session_parser][claudecode]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"user","uuid":"p1","parentUuid":null,"sessionId":"parent-session","timestamp":"2026-07-22T00:00:00Z","cwd":"/old","version":"1.0","message":{"role":"user","content":"parent prompt"}}
{"type":"assistant","uuid":"p2","parentUuid":"p1","sessionId":"parent-session","timestamp":"2026-07-22T00:00:01Z","message":{"role":"assistant","content":"parent answer"}}
{"type":"user","uuid":"c1","parentUuid":"p2","sessionId":"current-session","message":{"role":"user","content":[{"type":"text","text":"continued prompt"}]}}
{"type":"assistant","uuid":"c2","parentUuid":"c1","sessionId":"current-session","timestamp":"2026-07-23T00:00:01Z","cwd":"/new","version":"2.0","message":{"role":"assistant","content":"current answer"}})",
      lupe::LogFormat::ClaudeCode);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.session_id == "current-session");
  REQUIRE(parsed.session.parent_session_ref == "parent-session");
  REQUIRE(parsed.session.source_version == "2.0");
  REQUIRE(parsed.session.cwd == "/new");
  REQUIRE(parsed.session.created_at == "2026-07-23T00:00:01Z");
  REQUIRE(parsed.session.active_leaf_id == "c2");
  REQUIRE(std::ranges::none_of(
      parsed.diagnostics, [](const lupe::Diagnostic &diagnostic) {
        return diagnostic.code == lupe::DiagnosticCode::InconsistentSessionId;
      }));

  const auto selected = lupe::select_conversation_records(parsed.session);
  REQUIRE(selected == std::vector<std::size_t>{0, 1, 2, 3});
}

TEST_CASE("Claude Code inline images retain their payloads",
          "[session_parser][claudecode]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"user","uuid":"u1","parentUuid":null,"sessionId":"claude-session","timestamp":"2026-07-23T00:00:00Z","cwd":"/repo","message":{"role":"user","content":[{"type":"image","source":{"type":"base64","media_type":"image/png","data":"eA=="}}]}})",
      lupe::LogFormat::ClaudeCode);

  REQUIRE_FALSE(parsed.has_fatal_error());
  const auto *message =
      first_event<lupe::MessageEvent>(parsed.session.records.front());
  REQUIRE(message != nullptr);
  REQUIRE(message->content.size() == 1);
  const auto &image = std::get<lupe::ImageContent>(message->content.front());
  REQUIRE(image.mime_type == "image/png");
  REQUIRE(image.inline_data);
  REQUIRE(image.base64_data);
  REQUIRE(*image.base64_data == "eA==");

  const auto displayed = lupe::make_display_messages(parsed.session);
  REQUIRE(displayed.size() == 1);
  REQUIRE(displayed.front().images.size() == 1);
  CHECK(displayed.front().images.front().base64_data == image.base64_data);
}

TEST_CASE("Claude Code metadata records advance the active branch",
          "[session_parser][claudecode]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"user","uuid":"u1","parentUuid":null,"sessionId":"session","message":{"role":"user","content":"question"}}
{"type":"assistant","uuid":"a-old","parentUuid":"u1","sessionId":"session","message":{"role":"assistant","content":"old branch"}}
{"type":"assistant","uuid":"a-new","parentUuid":"u1","sessionId":"session","message":{"role":"assistant","content":"new branch"}}
{"type":"attachment","uuid":"attachment-1","parentUuid":"a-old","sessionId":"session","timestamp":"2026-07-24T00:00:00Z","fileName":"result.txt"})",
      lupe::LogFormat::ClaudeCode);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.active_leaf_id == "attachment-1");
  REQUIRE(lupe::select_conversation_records(parsed.session)
          == std::vector<std::size_t>{0, 1, 3});

  const auto displayed = lupe::make_display_messages(
      parsed.session, lupe::DisplayOptions{.show_metadata = true});
  REQUIRE(message_with_content(displayed, "old branch") != nullptr);
  REQUIRE(message_with_content(displayed, "new branch") == nullptr);
  REQUIRE(std::ranges::any_of(displayed, [](const lupe::LogMessage &message) {
    return message.raw_type == "attachment";
  }));
}

TEST_CASE("Claude Code rejects unconnected session id jumps",
          "[session_parser][claudecode]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"user","uuid":"u1","parentUuid":null,"sessionId":"main-session","timestamp":"2026-07-23T00:00:00Z","cwd":"/main","message":{"role":"user","content":"main prompt"}}
{"type":"user","uuid":"f1","parentUuid":null,"sessionId":"foreign-session","timestamp":"2026-07-23T00:01:00Z","cwd":"/foreign","message":{"role":"user","content":"foreign prompt"}}
{"type":"assistant","uuid":"f2","parentUuid":"f1","sessionId":"foreign-session","timestamp":"2026-07-23T00:01:01Z","message":{"role":"assistant","content":"foreign answer"}})",
      lupe::LogFormat::ClaudeCode);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.session_id == "main-session");
  REQUIRE_FALSE(parsed.session.parent_session_ref.has_value());
  REQUIRE(parsed.session.cwd == "/main");
  REQUIRE(parsed.session.active_leaf_id == "u1");
  REQUIRE(std::count_if(parsed.diagnostics.begin(), parsed.diagnostics.end(),
                        [](const lupe::Diagnostic &diagnostic) {
                          return diagnostic.code
                              == lupe::DiagnosticCode::InconsistentSessionId;
                        })
          == 1);

  const auto selected = lupe::select_conversation_records(parsed.session);
  REQUIRE(selected == std::vector<std::size_t>{0});
}
