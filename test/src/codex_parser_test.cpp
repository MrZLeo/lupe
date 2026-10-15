#include "loupe/log_format.hpp"
#include "loupe/message_projection.hpp"
#include "loupe/session_ir.hpp"
#include "loupe/session_parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace {

template <typename Event>
std::vector<const Event *> events(const loupe::SessionParseResult &parsed) {
  std::vector<const Event *> found;
  for (const auto &record : parsed.session.records) {
    for (const auto &event : record.events) {
      if (const auto *value = std::get_if<Event>(&event.payload)) {
        found.push_back(value);
      }
    }
  }
  return found;
}

} // namespace

TEST_CASE("parse paginated Codex completed messages reasoning and plans",
          "[codex_parser][paginated]") {
  // Wire shapes follow openai/codex rust-v0.162.1 protocol/src/items.rs.
  const auto parsed = loupe::parse_session_content(
      R"({"ordinal":0,"type":"session_meta","payload":{"id":"thread","history_mode":"paginated","model_provider":"openai"}})"
      "\n"
      R"({"ordinal":1,"type":"event_msg","payload":{"type":"task_started","turn_id":"turn-1"}})"
      "\n"
      R"({"ordinal":2,"type":"turn_context","payload":{"turn_id":"turn-1","model":"gpt-5"}})"
      "\n"
      R"({"ordinal":3,"type":"event_msg","payload":{"type":"item_completed","thread_id":"thread","turn_id":"turn-1","item":{"type":"UserMessage","id":"u1","content":[{"type":"text","text":"question","text_elements":[]},{"type":"image","image_url":"data:image/png;base64,eA=="},{"type":"local_image","path":"/tmp/image.png"}]}}})"
      "\n"
      R"({"ordinal":4,"type":"event_msg","payload":{"type":"item_completed","turn_id":"turn-1","item":{"type":"Reasoning","id":"r1","summary_text":["first","second"],"raw_content":["details"]}}})"
      "\n"
      R"({"ordinal":5,"type":"event_msg","payload":{"type":"item_completed","turn_id":"turn-1","item":{"type":"Plan","id":"p1","text":"the plan"}}})"
      "\n"
      R"({"ordinal":6,"type":"event_msg","payload":{"type":"item_completed","turn_id":"turn-1","item":{"type":"AgentMessage","id":"a1","phase":"final_answer","content":[{"type":"Text","text":"answer"}]}}})",
      loupe::LogFormat::Auto);
  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.diagnostics.empty());
  const auto messages = events<loupe::MessageEvent>(parsed);
  REQUIRE(messages.size() == 3);
  REQUIRE(messages[0]->role == loupe::Role::User);
  REQUIRE(messages[0]->provider == "openai");
  REQUIRE(messages[0]->model == "gpt-5");
  REQUIRE(messages[0]->content.size() == 3);
  const auto &image = std::get<loupe::ImageContent>(messages[0]->content[1]);
  REQUIRE(image.base64_data);
  REQUIRE(*image.base64_data == "eA==");
  REQUIRE(std::get<loupe::ImageContent>(messages[0]->content[2]).url
          == "/tmp/image.png");
  REQUIRE(messages[1]->phase == "plan");
  REQUIRE(messages[2]->role == loupe::Role::Assistant);
  REQUIRE(messages[2]->phase == "final_answer");
  const auto reasoning = events<loupe::ReasoningEvent>(parsed);
  REQUIRE(reasoning.size() == 1);
  REQUIRE(reasoning[0]->summary == "first\nsecond");
  REQUIRE(reasoning[0]->content == "details");
  const auto display = loupe::make_display_messages(parsed.session);
  REQUIRE(display.size() == 3);
  REQUIRE(display.back().content == "answer");
}

TEST_CASE("parse paginated Codex command file and MCP tool completions",
          "[codex_parser][paginated]") {
  const auto parsed = loupe::parse_session_content(
      R"({"type":"session_meta","payload":{"id":"thread","history_mode":"paginated"}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","turn_id":"t1","item":{"type":"CommandExecution","id":"cmd1","command":["sh","-c","echo error"],"cwd":"/work","parsed_cmd":[],"source":"agent","status":"failed","aggregated_output":"error","exit_code":-1}}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","turn_id":"t1","item":{"type":"FileChange","id":"patch1","changes":{"a.txt":{"type":"add","content":"hello"}},"status":"completed","stdout":"applied","stderr":""}}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","turn_id":"t1","item":{"type":"McpToolCall","id":"mcp1","server":"docs","tool":"lookup","arguments":{"key":"x"},"status":"failed","result":{"content":[{"type":"text","text":"not found"}],"isError":true}}}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","turn_id":"t1","item":{"type":"DynamicToolCall","id":"dynamic1","tool":"render","namespace":"ui","arguments":null,"status":"completed","content_items":[{"type":"inputText","text":"rendered"},{"type":"inputImage","imageUrl":"data:image/png;base64,eA=="}],"success":true}}})",
      loupe::LogFormat::Codex);
  REQUIRE(parsed.diagnostics.empty());
  const auto calls = events<loupe::ToolCallEvent>(parsed);
  const auto results = events<loupe::ToolResultEvent>(parsed);
  REQUIRE(calls.size() == 4);
  REQUIRE(results.size() == 4);
  CHECK(calls[0]->input == R"(["sh","-c","echo error"])");
  CHECK(calls[0]->input_is_json);
  CHECK(results[0]->exit_code == -1);
  CHECK(results[0]->is_error);
  CHECK(calls[1]->name == "apply_patch");
  CHECK(calls[2]->name == "lookup");
  CHECK(calls[2]->name_space == "docs");
  CHECK(results[2]->is_error);
  CHECK(std::get<loupe::TextContent>(results[2]->output[0]).text
        == "not found");
  CHECK(calls[3]->input == "null");
  CHECK(calls[3]->input_is_json);
  REQUIRE(results[3]->output.size() == 2);
  CHECK(std::get<loupe::TextContent>(results[3]->output[0]).text == "rendered");
  const auto &image = std::get<loupe::ImageContent>(results[3]->output[1]);
  REQUIRE(image.base64_data);
  CHECK(*image.base64_data == "eA==");
  CHECK_FALSE(results[3]->is_error);
}

TEST_CASE("deduplicate completed Codex items against raw response records in "
          "either order",
          "[codex_parser][paginated]") {
  const std::string header =
      R"({"type":"session_meta","payload":{"id":"thread","history_mode":"paginated"}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"task_started","turn_id":"t1"}})"
      "\n"
      R"({"type":"turn_context","payload":{"turn_id":"t1"}})"
      "\n";
  const std::string raw =
      R"({"type":"response_item","payload":{"type":"message","id":"a1","role":"assistant","phase":"final_answer","content":[{"type":"output_text","text":"answer"}]}})"
      "\n"
      R"({"type":"response_item","payload":{"type":"reasoning","id":"r1","summary":[{"type":"summary_text","text":"summary"}],"encrypted_content":"secret"}})"
      "\n"
      R"({"type":"response_item","payload":{"type":"function_call","call_id":"cmd1","name":"exec_command","arguments":"{\"cmd\":\"ls\"}"}})"
      "\n"
      R"({"type":"response_item","payload":{"type":"function_call_output","call_id":"cmd1","output":"full untruncated output"}})"
      "\n";
  const std::string completed =
      R"({"type":"event_msg","payload":{"type":"item_completed","turn_id":"t1","item":{"type":"AgentMessage","id":"a1","phase":"final_answer","content":[{"type":"Text","text":"answer"}]}}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","turn_id":"t1","item":{"type":"Reasoning","id":"r1","summary_text":["summary"],"raw_content":[]}}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","turn_id":"t1","item":{"type":"CommandExecution","id":"cmd1","command":["ls"],"status":"failed","aggregated_output":"truncated","exit_code":2}}})"
      "\n";
  for (const bool completed_first : {false, true}) {
    const auto parsed = loupe::parse_session_content(
        header + (completed_first ? completed + raw : raw + completed),
        loupe::LogFormat::Codex);
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(events<loupe::MessageEvent>(parsed).size() == 1);
    const auto reasoning = events<loupe::ReasoningEvent>(parsed);
    REQUIRE(reasoning.size() == 1);
    REQUIRE(reasoning[0]->encrypted);
    const auto calls = events<loupe::ToolCallEvent>(parsed);
    const auto results = events<loupe::ToolResultEvent>(parsed);
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0]->name == "exec_command");
    REQUIRE(results.size() == 1);
    REQUIRE(results[0]->exit_code == 2);
    REQUIRE(results[0]->is_error);
    REQUIRE(std::get<loupe::TextContent>(results[0]->output[0]).text
            == "full untruncated output");
    REQUIRE(parsed.session.records.size() == 10);
    for (const auto &record : parsed.session.records) {
      REQUIRE_FALSE(record.raw_json.empty());
    }
  }
}

TEST_CASE("Codex binds the first completed turn ID to preceding raw records",
          "[codex_parser][paginated]") {
  const auto parsed = loupe::parse_session_content(
      R"({"type":"session_meta","payload":{"id":"thread"}})"
      "\n"
      R"({"type":"response_item","payload":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"answer"}]}})"
      "\n"
      R"({"type":"response_item","payload":{"type":"function_call","call_id":"cmd","name":"exec_command","arguments":"{}"}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","turn_id":"t1","item":{"type":"AgentMessage","id":"a1","content":[{"type":"Text","text":"answer"}]}}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","turn_id":"t1","item":{"type":"CommandExecution","id":"cmd","command":["ls"],"status":"completed","aggregated_output":"files","exit_code":0}}})",
      loupe::LogFormat::Codex);
  REQUIRE(parsed.diagnostics.empty());
  REQUIRE(events<loupe::MessageEvent>(parsed).size() == 1);
  REQUIRE(events<loupe::ToolCallEvent>(parsed).size() == 1);
  REQUIRE(events<loupe::ToolResultEvent>(parsed).size() == 1);
  const auto &completion = parsed.session.records.back();
  REQUIRE(completion.events.size() == 1);
  REQUIRE(completion.events.front().fragment_index == 0);
  REQUIRE(
      std::get<loupe::ToolResultEvent>(completion.events.front().payload).name
      == "exec_command");
}

TEST_CASE(
    "parse additional Codex TurnItem variants and preserve future extensions",
    "[codex_parser][paginated]") {
  const auto parsed = loupe::parse_session_content(
      R"({"type":"session_meta","payload":{"id":"thread","history_mode":"paginated"}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","item":{"type":"WebSearch","id":"web","query":"Codex","action":{"type":"search","query":"Codex"},"results":[{"title":"result"}]}}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","item":{"type":"ImageGeneration","id":"image","status":"completed","result":"data:image/png;base64,eA=="}}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","item":{"type":"CollabAgentToolCall","id":"agent","tool":"spawn_agent","status":"completed","sender_thread_id":"thread","receiver_thread_ids":["child"],"agents_states":{"child":"running"}}}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","item":{"type":"FunctionCallOutput","id":"call-output","name":"read","output":"file contents"}}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","item":{"type":"HookPrompt","id":"hook","fragments":[{"text":"hook input","hookRunId":"h1"}]}}})"
      "\n"
      R"({"type":"compacted","payload":{"message":"preserved summary"}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","item":{"type":"ContextCompaction","id":"compact"}}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","item":{"type":"Extension","id":"future","payload":{"type":"new_extension"}}}})",
      loupe::LogFormat::Codex);
  REQUIRE(parsed.diagnostics.empty());
  const auto calls = events<loupe::ToolCallEvent>(parsed);
  REQUIRE(calls.size() == 3);
  REQUIRE(calls.at(0)->name == "web_search");
  REQUIRE(calls.at(1)->name == "image_generation");
  REQUIRE(calls.at(2)->name == "spawn_agent");
  REQUIRE(events<loupe::ToolResultEvent>(parsed).size() == 4);
  const auto messages = events<loupe::MessageEvent>(parsed);
  REQUIRE(messages.size() == 1);
  REQUIRE(messages.front()->role == loupe::Role::Developer);
  REQUIRE(std::get<loupe::TextContent>(messages.front()->content.front()).text
          == "hook input");
  const auto compactions = events<loupe::CompactionEvent>(parsed);
  REQUIRE(compactions.size() == 1);
  REQUIRE(compactions.front()->summary == "preserved summary");
  REQUIRE(events<loupe::UnknownEvent>(parsed).size() == 1);
}

TEST_CASE("Codex completed-message deduplication is one-to-one and turn scoped",
          "[codex_parser][paginated]") {
  const auto parsed = loupe::parse_session_content(
      R"({"type":"session_meta","payload":{"id":"thread"}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"task_started","turn_id":"t1"}})"
      "\n"
      R"({"type":"response_item","payload":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"same"}]}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","turn_id":"t1","item":{"type":"AgentMessage","id":"a1","content":[{"type":"Text","text":"same"}]}}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","turn_id":"t1","item":{"type":"AgentMessage","id":"a2","content":[{"type":"Text","text":"same"}]}}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"turn_started","turn_id":"t2"}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","turn_id":"t2","item":{"type":"AgentMessage","id":"a3","content":[{"type":"Text","text":"same"}]}}})",
      loupe::LogFormat::Codex);
  REQUIRE(parsed.diagnostics.empty());
  REQUIRE(events<loupe::MessageEvent>(parsed).size() == 3);
  REQUIRE(parsed.session.records[3].events.empty());
  REQUIRE_FALSE(parsed.session.records[4].events.empty());
  REQUIRE_FALSE(parsed.session.records[6].events.empty());
}

TEST_CASE("retain unknown and malformed Codex completed items without losing "
          "following messages",
          "[codex_parser][paginated]") {
  const auto parsed = loupe::parse_session_content(
      R"({"type":"session_meta","payload":{"id":"thread"}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","item":null}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","item":{"type":"FutureItem","id":"future","data":{"keep":true}}}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","item":{"type":"AgentMessage","id":"bad","content":"wrong type"}}})"
      "\n"
      R"({"type":"event_msg","payload":{"type":"item_completed","item":{"type":"AgentMessage","content":[{"type":"Text","text":"recover"}]}}})",
      loupe::LogFormat::Codex);
  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.records.size() == 5);
  REQUIRE(events<loupe::UnknownEvent>(parsed).size() == 3);
  REQUIRE(parsed.diagnostics.size() == 3);
  const auto messages = loupe::make_display_messages(parsed.session);
  REQUIRE(messages.size() == 1);
  REQUIRE(messages[0].content == "recover");
}

TEST_CASE("parse Codex rollout records into semantic events",
          "[codex_parser]") {
  const auto parsed = loupe::parse_session_content(
      R"({"timestamp":"2026-07-23T00:00:00Z","ordinal":1,"type":"session_meta","payload":{"id":"thread-1","session_id":"root-1","timestamp":"2026-07-23T00:00:00Z","cwd":"/work","cli_version":"1.2.3","model_provider":"openai"}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:01Z","ordinal":2,"type":"turn_context","payload":{"cwd":"/work","model":"gpt-5"}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:02Z","ordinal":3,"type":"response_item","payload":{"id":"msg-1","type":"message","role":"assistant","phase":"final_answer","content":[{"type":"output_text","text":"done"}]}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:03Z","ordinal":4,"type":"response_item","payload":{"type":"reasoning","summary":[{"type":"summary_text","text":"summary"}],"content":[{"type":"reasoning_text","text":"details"}],"encrypted_content":"encrypted"}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:04Z","ordinal":5,"type":"response_item","payload":{"type":"function_call","call_id":"call-1","name":"read","namespace":"fs","arguments":"{\"path\":\"a\"}"}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:05Z","ordinal":6,"type":"response_item","payload":{"type":"function_call_output","call_id":"call-1","output":[{"type":"input_text","text":"contents"}]}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:06Z","ordinal":7,"type":"event_msg","payload":{"type":"agent_message","message":"commentary-only","phase":"commentary"}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:07Z","ordinal":8,"type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":1,"cached_input_tokens":2,"cache_write_input_tokens":3,"output_tokens":4,"reasoning_output_tokens":5,"total_tokens":15},"total_token_usage":{"input_tokens":10,"cached_input_tokens":20,"cache_write_input_tokens":30,"output_tokens":40,"reasoning_output_tokens":50,"total_tokens":150}},"rate_limits":null}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:08Z","ordinal":9,"type":"compacted","payload":{"message":"compact summary","replacement_history":[{"type":"message","role":"user","content":[]}]}})",
      loupe::LogFormat::Codex);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.diagnostics.empty());
  REQUIRE(parsed.session.session_id == "thread-1");
  REQUIRE(parsed.session.source_version == "1.2.3");
  REQUIRE(parsed.session.cwd == "/work");
  REQUIRE(parsed.session.records.size() == 9);
  REQUIRE(parsed.session.records[2].native_id == "msg-1");

  const auto &message = std::get<loupe::MessageEvent>(
      parsed.session.records[2].events[0].payload);
  REQUIRE(message.role == loupe::Role::Assistant);
  REQUIRE(message.provider == "openai");
  REQUIRE(message.model == "gpt-5");
  REQUIRE(message.phase == "final_answer");

  const auto &reasoning = std::get<loupe::ReasoningEvent>(
      parsed.session.records[3].events[0].payload);
  REQUIRE(reasoning.summary == "summary");
  REQUIRE(reasoning.content == "details");
  REQUIRE(reasoning.encrypted);

  const auto &call = std::get<loupe::ToolCallEvent>(
      parsed.session.records[4].events[0].payload);
  REQUIRE(call.call_id == "call-1");
  REQUIRE(call.input == R"({"path":"a"})");
  REQUIRE(call.input_is_json);

  const auto &tool_result = std::get<loupe::ToolResultEvent>(
      parsed.session.records[5].events[0].payload);
  REQUIRE(tool_result.name == "read");
  REQUIRE(std::get<loupe::TextContent>(tool_result.output[0]).text
          == "contents");

  const auto &commentary = std::get<loupe::MessageEvent>(
      parsed.session.records[6].events[0].payload);
  REQUIRE(commentary.phase == "commentary");
  REQUIRE(std::get<loupe::TextContent>(commentary.content[0]).text
          == "commentary-only");
  REQUIRE(parsed.session.records[7].events.size() == 2);
  REQUIRE(
      std::get<loupe::UsageEvent>(parsed.session.records[7].events[0].payload)
          .scope
      == loupe::UsageScope::Turn);
  REQUIRE(
      std::get<loupe::UsageEvent>(parsed.session.records[7].events[1].payload)
          .scope
      == loupe::UsageScope::Session);

  const auto &compaction = std::get<loupe::CompactionEvent>(
      parsed.session.records[8].events[0].payload);
  REQUIRE(compaction.summary == "compact summary");
  REQUIRE_FALSE(compaction.replacement_context_json.empty());
}

TEST_CASE("use legacy Codex messages only when response messages are absent",
          "[codex_parser]") {
  const auto parsed = loupe::parse_session_content(
      R"({"timestamp":"2026-07-23T00:00:00Z","type":"session_meta","payload":{"id":"thread-1","timestamp":"2026-07-23T00:00:00Z","cwd":"/work","cli_version":"1.2.3"}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:01Z","type":"event_msg","payload":{"type":"user_message","message":"question","images":["https://example.com/a.png"]}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:02Z","type":"event_msg","payload":{"type":"agent_message","message":"answer","phase":"final_answer"}})",
      loupe::LogFormat::Codex);

  REQUIRE_FALSE(parsed.has_fatal_error());
  const auto &user = std::get<loupe::MessageEvent>(
      parsed.session.records[1].events[0].payload);
  REQUIRE(user.role == loupe::Role::User);
  REQUIRE(user.content.size() == 2);

  const auto &assistant = std::get<loupe::MessageEvent>(
      parsed.session.records[2].events[0].payload);
  REQUIRE(assistant.role == loupe::Role::Assistant);
  REQUIRE(assistant.phase == "final_answer");
}

TEST_CASE("report malformed Codex function arguments without dropping records",
          "[codex_parser]") {
  const auto parsed = loupe::parse_session_content(
      R"({"timestamp":"2026-07-23T00:00:00Z","type":"session_meta","payload":{"id":"thread-1","timestamp":"2026-07-23T00:00:00Z","cwd":"/work","cli_version":"1.2.3"}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:01Z","type":"response_item","payload":{"type":"function_call","call_id":"call-1","name":"read","arguments":"not-json"}})",
      loupe::LogFormat::Codex);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.records.size() == 2);
  REQUIRE(parsed.diagnostics.size() == 1);
  REQUIRE(parsed.diagnostics[0].code == loupe::DiagnosticCode::InvalidJson);

  const auto &call = std::get<loupe::ToolCallEvent>(
      parsed.session.records[1].events[0].payload);
  REQUIRE(call.input == "not-json");
  REQUIRE_FALSE(call.input_is_json);
}

TEST_CASE("keep legacy Codex messages when image content differs",
          "[codex_parser]") {
  const auto parsed = loupe::parse_session_content(
      R"({"timestamp":"2026-07-23T00:00:00Z","type":"session_meta","payload":{"id":"thread-1","timestamp":"2026-07-23T00:00:00Z","cwd":"/work","cli_version":"1.2.3"}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:01Z","type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":""},{"type":"input_image","image_url":"https://example.com/canonical.png"}]}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:02Z","type":"event_msg","payload":{"type":"user_message","message":"","images":["https://example.com/legacy.png"]}})",
      loupe::LogFormat::Codex);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.records[2].events.size() == 1);
  const auto &legacy = std::get<loupe::MessageEvent>(
      parsed.session.records[2].events[0].payload);
  REQUIRE(legacy.content.size() == 2);
  REQUIRE(std::get<loupe::ImageContent>(legacy.content[1]).url
          == "https://example.com/legacy.png");
}

TEST_CASE(
    "retain Codex inline image payloads when deduplicating legacy messages",
    "[codex_parser]") {
  for (const std::string legacy_data : {"eA==", "eQ=="}) {
    const auto parsed = loupe::parse_session_content(
        std::string{
            R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"question"},{"type":"input_image","image_url":"data:image/png;base64,eA=="}]}})"}
            + "\n"
            + R"({"type":"event_msg","payload":{"type":"user_message","message":"question","images":["data:image/png;base64,)"
            + legacy_data
            + R"("]}})",
        loupe::LogFormat::Codex);
    REQUIRE_FALSE(parsed.has_fatal_error());
    const auto &canonical = std::get<loupe::MessageEvent>(
        parsed.session.records[0].events[0].payload);
    const auto &image = std::get<loupe::ImageContent>(canonical.content[1]);
    REQUIRE(image.base64_data);
    CHECK(*image.base64_data == "eA==");
    CHECK(image.url.empty());
    if (legacy_data == "eA==") {
      CHECK(parsed.session.records[1].events.empty());
    } else {
      REQUIRE(parsed.session.records[1].events.size() == 1);
      const auto &legacy = std::get<loupe::MessageEvent>(
          parsed.session.records[1].events[0].payload);
      const auto &legacy_image =
          std::get<loupe::ImageContent>(legacy.content[1]);
      REQUIRE(legacy_image.base64_data);
      CHECK(*legacy_image.base64_data == legacy_data);
    }
  }
}

TEST_CASE("parse extended Codex response items into semantic events",
          "[codex_parser]") {
  const auto parsed = loupe::parse_session_content(
      R"({"timestamp":"2026-07-23T00:00:00Z","type":"session_meta","payload":{"id":"thread-1","timestamp":"2026-07-23T00:00:00Z","cwd":"/work","cli_version":"1.2.3"}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:01Z","type":"response_item","payload":{"id":"web-1","type":"web_search_call","status":"completed","action":{"type":"search","query":"Codex"}}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:02Z","type":"response_item","payload":{"id":"search-item","type":"tool_search_call","call_id":"search-1","status":"completed","execution":"search","arguments":{"query":"tool"}}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:03Z","type":"response_item","payload":{"type":"tool_search_output","call_id":"search-1","status":"completed","execution":"search","tools":[{"name":"read"}]}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:04Z","type":"response_item","payload":{"id":"image-1","type":"image_generation_call","status":"completed","revised_prompt":"a cat","result":"data:image/png;base64,eA=="}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:05Z","type":"response_item","payload":{"type":"compaction","encrypted_content":"encrypted"}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:06Z","type":"response_item","payload":{"type":"context_compaction","encrypted_content":"encrypted"}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:07Z","type":"response_item","payload":{"type":"compaction_trigger"}})",
      loupe::LogFormat::Codex);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.diagnostics.empty());
  REQUIRE(parsed.session.records.size() == 8);

  const auto &web = std::get<loupe::ToolCallEvent>(
      parsed.session.records[1].events[0].payload);
  REQUIRE(web.call_id == "web-1");
  REQUIRE(web.name == "web_search");
  REQUIRE(web.name_space == "codex");
  REQUIRE(web.input == R"({"type":"search","query":"Codex"})");
  REQUIRE(web.input_is_json);

  const auto &search = std::get<loupe::ToolCallEvent>(
      parsed.session.records[2].events[0].payload);
  REQUIRE(search.call_id == "search-1");
  REQUIRE(search.name == "tool_search");
  REQUIRE(search.input == R"({"query":"tool"})");
  REQUIRE(search.input_is_json);

  const auto &search_result = std::get<loupe::ToolResultEvent>(
      parsed.session.records[3].events[0].payload);
  REQUIRE(search_result.call_id == "search-1");
  REQUIRE(search_result.name == "tool_search");
  REQUIRE(std::get<loupe::UnknownContent>(search_result.output[0]).json
          == R"([{"name":"read"}])");

  REQUIRE(parsed.session.records[4].events.size() == 2);
  const auto &image_call = std::get<loupe::ToolCallEvent>(
      parsed.session.records[4].events[0].payload);
  REQUIRE(image_call.call_id == "image-1");
  REQUIRE(image_call.input == "a cat");
  const auto &image_result = std::get<loupe::ToolResultEvent>(
      parsed.session.records[4].events[1].payload);
  const auto &image = std::get<loupe::ImageContent>(image_result.output[0]);
  REQUIRE(image.mime_type == "image/png");
  REQUIRE(image.url.empty());
  REQUIRE(image.inline_data);
  REQUIRE(image.base64_data);
  REQUIRE(*image.base64_data == "eA==");

  REQUIRE(std::get<loupe::CompactionEvent>(
              parsed.session.records[5].events[0].payload)
              .trigger
          == "compaction");
  REQUIRE(std::get<loupe::CompactionEvent>(
              parsed.session.records[6].events[0].payload)
              .trigger
          == "context_compaction");
  REQUIRE(std::get<loupe::CompactionEvent>(
              parsed.session.records[7].events[0].payload)
              .trigger
          == "compaction_trigger");

  for (std::size_t index = 1; index < parsed.session.records.size(); ++index) {
    REQUIRE_FALSE(parsed.session.records[index].raw_json.empty());
    REQUIRE_FALSE(std::holds_alternative<loupe::UnknownEvent>(
        parsed.session.records[index].events[0].payload));
  }
}

TEST_CASE("parse pre-envelope Codex rollouts and preserve native records",
          "[codex_parser]") {
  const auto parsed = loupe::parse_session_content(
      R"({"id":"legacy-session","timestamp":"2025-08-26T00:00:00Z","instructions":null,"git":{"commit_hash":"abc","branch":"main","repository_url":null}})"
      "\n"
      R"({"record_type":"state"})"
      "\n"
      R"({"type":"ghost_snapshot","ghost_commit":{"id":"ghost-1","parent":null}})"
      "\n"
      R"({"id":"user-1","type":"message","role":"user","content":[{"type":"input_text","text":"question"}]})"
      "\n"
      R"({"id":"reasoning-1","type":"reasoning","summary":[{"type":"summary_text","text":"plan"}],"content":[{"type":"reasoning_text","text":"details"}],"encrypted_content":"ciphertext"})"
      "\n"
      R"({"id":"call-item-1","type":"function_call","call_id":"call-1","name":"read","arguments":"{\"path\":\"README.md\"}"})"
      "\n"
      R"({"type":"function_call_output","call_id":"call-1","output":"contents"})"
      "\n"
      R"({"id":"assistant-1","type":"message","role":"assistant","content":[{"type":"output_text","text":"answer"}]})",
      loupe::LogFormat::Codex);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.diagnostics.empty());
  REQUIRE(parsed.session.session_id == "legacy-session");
  REQUIRE(parsed.session.created_at == "2025-08-26T00:00:00Z");
  REQUIRE(parsed.session.records.size() == 8);
  REQUIRE(parsed.session.records[0].native_type == "session_meta");
  REQUIRE(parsed.session.records[0].native_id.empty());

  REQUIRE(parsed.session.records[1].native_type == "state");
  REQUIRE_FALSE(parsed.session.records[1].raw_json.empty());
  REQUIRE(
      std::get<loupe::UnknownEvent>(parsed.session.records[1].events[0].payload)
          .native_type
      == "state");

  REQUIRE(parsed.session.records[2].native_type == "ghost_snapshot");
  REQUIRE_FALSE(parsed.session.records[2].raw_json.empty());
  REQUIRE(
      std::get<loupe::UnknownEvent>(parsed.session.records[2].events[0].payload)
          .native_type
      == "response_item.ghost_snapshot");

  REQUIRE(parsed.session.records[3].native_id == "user-1");
  const auto &user = std::get<loupe::MessageEvent>(
      parsed.session.records[3].events[0].payload);
  REQUIRE(user.role == loupe::Role::User);
  REQUIRE(std::get<loupe::TextContent>(user.content[0]).text == "question");

  const auto &reasoning = std::get<loupe::ReasoningEvent>(
      parsed.session.records[4].events[0].payload);
  REQUIRE(reasoning.summary == "plan");
  REQUIRE(reasoning.content == "details");
  REQUIRE(reasoning.encrypted);

  const auto &call = std::get<loupe::ToolCallEvent>(
      parsed.session.records[5].events[0].payload);
  REQUIRE(call.call_id == "call-1");
  REQUIRE(call.name == "read");
  REQUIRE(call.input == R"({"path":"README.md"})");
  REQUIRE(call.input_is_json);

  const auto &result = std::get<loupe::ToolResultEvent>(
      parsed.session.records[6].events[0].payload);
  REQUIRE(result.call_id == "call-1");
  REQUIRE(result.name == "read");
  REQUIRE(std::get<loupe::TextContent>(result.output[0]).text == "contents");

  const auto &assistant = std::get<loupe::MessageEvent>(
      parsed.session.records[7].events[0].payload);
  REQUIRE(assistant.role == loupe::Role::Assistant);
  REQUIRE(std::get<loupe::TextContent>(assistant.content[0]).text == "answer");
}

TEST_CASE("do not use Codex session IDs as native record IDs",
          "[codex_parser]") {
  const auto parsed = loupe::parse_session_content(
      R"({"timestamp":"2026-07-23T00:00:00Z","type":"session_meta","payload":{"id":"thread-1","cwd":"/work"}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:01Z","type":"session_meta","payload":{"id":"thread-1","cwd":"/work"}})",
      loupe::LogFormat::Codex);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.diagnostics.empty());
  REQUIRE(parsed.session.session_id == "thread-1");
  REQUIRE(parsed.session.records.size() == 2);
  REQUIRE(parsed.session.records[0].native_id.empty());
  REQUIRE(parsed.session.records[1].native_id.empty());
}

TEST_CASE("deduplicate Codex messages across same-turn bookkeeping records",
          "[codex_parser]") {
  const auto parsed = loupe::parse_session_content(
      R"({"timestamp":"2026-07-23T00:00:00Z","type":"session_meta","payload":{"id":"thread-1"}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:01Z","type":"event_msg","payload":{"type":"agent_message","message":"answer","phase":"final_answer"}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:02Z","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":1}}}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:03Z","type":"response_item","payload":{"type":"reasoning","summary":[{"type":"summary_text","text":"plan"}]}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:04Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":2}}}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:05Z","type":"response_item","payload":{"type":"message","role":"assistant","phase":"final_answer","content":[{"type":"output_text","text":"answer"}]}})",
      loupe::LogFormat::Codex);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.diagnostics.empty());
  REQUIRE(parsed.session.records[1].events.empty());
  REQUIRE(std::holds_alternative<loupe::MessageEvent>(
      parsed.session.records[5].events[0].payload));
}

TEST_CASE("keep identical Codex messages from different turns",
          "[codex_parser]") {
  const auto parsed = loupe::parse_session_content(
      R"({"timestamp":"2026-07-23T00:00:00Z","type":"session_meta","payload":{"id":"thread-1"}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:01Z","type":"event_msg","payload":{"type":"agent_message","message":"same answer","phase":"final_answer"}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:02Z","type":"turn_context","payload":{"model":"gpt-5"}})"
      "\n"
      R"({"timestamp":"2026-07-23T00:00:03Z","type":"response_item","payload":{"type":"message","role":"assistant","phase":"final_answer","content":[{"type":"output_text","text":"same answer"}]}})",
      loupe::LogFormat::Codex);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.diagnostics.empty());
  REQUIRE(std::holds_alternative<loupe::MessageEvent>(
      parsed.session.records[1].events[0].payload));
  REQUIRE(std::holds_alternative<loupe::MessageEvent>(
      parsed.session.records[3].events[0].payload));
}
