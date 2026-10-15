#include "loupe/log_format.hpp"
#include "loupe/message_projection.hpp"
#include "loupe/session_ir.hpp"
#include "loupe/session_parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace {

bool has_diagnostic(const loupe::SessionParseResult &parsed,
                    loupe::DiagnosticCode code, std::size_t source_line,
                    std::string_view message_part) {
  return std::ranges::any_of(parsed.diagnostics,
                             [&](const loupe::Diagnostic &diagnostic) {
                               return diagnostic.code == code
                                   && diagnostic.source_line == source_line
                                   && diagnostic.message.contains(message_part);
                             });
}

} // namespace

TEST_CASE("parse Pi messages into ordered semantic events", "[pi_parser]") {
  const auto parsed = loupe::parse_session_content(
      R"({"type":"session","version":3,"id":"session-1","timestamp":"2026-07-23T00:00:00Z","cwd":"/work"}
{"type":"message","id":"u1","parentId":null,"timestamp":"2026-07-23T00:00:01Z","message":{"role":"user","content":[{"type":"text","text":"hello"},{"type":"image","data":"eA==","mimeType":"image/png"}],"timestamp":1000}}
{"type":"message","id":"a1","parentId":"u1","timestamp":"2026-07-23T00:00:02Z","message":{"role":"assistant","provider":"openai","model":"gpt-5","timestamp":2000,"content":[{"type":"text","text":"before"},{"type":"thinking","thinking":"reason"},{"type":"toolCall","id":"call-1","name":"read","arguments":{"path":"a"}},{"type":"text","text":"after"}],"usage":{"input":10,"output":5,"cacheRead":2,"cacheWrite":1,"reasoning":3,"totalTokens":18,"cost":{"total":0.5}}}})",
      loupe::LogFormat::Pi);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.session_id == "session-1");
  REQUIRE(parsed.session.source_version == "3");
  REQUIRE(parsed.session.cwd == "/work");
  REQUIRE(parsed.session.active_leaf_id == "a1");
  REQUIRE(parsed.session.records.size() == 3);

  const auto &user_events = parsed.session.records[1].events;
  REQUIRE(user_events.size() == 1);
  const auto &user = std::get<loupe::MessageEvent>(user_events[0].payload);
  REQUIRE(user.role == loupe::Role::User);
  REQUIRE(user.content.size() == 2);
  const auto &image = std::get<loupe::ImageContent>(user.content[1]);
  REQUIRE(image.inline_data);
  REQUIRE(image.base64_data);
  REQUIRE(*image.base64_data == "eA==");

  const auto &assistant_events = parsed.session.records[2].events;
  REQUIRE(assistant_events.size() == 5);
  REQUIRE(
      std::get<loupe::TextContent>(
          std::get<loupe::MessageEvent>(assistant_events[0].payload).content[0])
          .text
      == "before");
  REQUIRE(std::get<loupe::ReasoningEvent>(assistant_events[1].payload).content
          == "reason");
  REQUIRE(std::get<loupe::ToolCallEvent>(assistant_events[2].payload).call_id
          == "call-1");
  REQUIRE(
      std::get<loupe::TextContent>(
          std::get<loupe::MessageEvent>(assistant_events[3].payload).content[0])
          .text
      == "after");
  REQUIRE(
      std::get<loupe::UsageEvent>(assistant_events[4].payload).reasoning_tokens
      == 3);

  const auto messages = loupe::make_display_messages(parsed.session);
  REQUIRE(messages.front().images.size() == 1);
  REQUIRE(messages.front().images.front().base64_data == image.base64_data);
}

TEST_CASE("parse Pi tools, compaction, metadata, and active branches",
          "[pi_parser]") {
  const auto parsed = loupe::parse_session_content(
      R"({"type":"session","version":3,"id":"session-2","timestamp":"2026-07-23T00:00:00Z","cwd":"/work"}
{"type":"message","id":"t1","parentId":null,"timestamp":"2026-07-23T00:00:01Z","message":{"role":"toolResult","toolCallId":"call-1","toolName":"read","content":[{"type":"text","text":"ok"}],"isError":false}}
{"type":"message","id":"b1","parentId":"t1","timestamp":"2026-07-23T00:00:02Z","message":{"role":"bashExecution","command":"false","output":"failed","exitCode":1,"cancelled":false}}
{"type":"compaction","id":"c1","parentId":"b1","timestamp":"2026-07-23T00:00:03Z","summary":"summary","tokensBefore":100,"retainedTail":[{"role":"user","content":"last"}]}
{"type":"model_change","id":"m1","parentId":"c1","timestamp":"2026-07-23T00:00:04Z","provider":"openai","modelId":"gpt-5"}
{"type":"custom_message","id":"x1","parentId":"t1","timestamp":"2026-07-23T00:00:05Z","customType":"fixture","content":"alternate branch","display":false})",
      loupe::LogFormat::Pi);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.active_leaf_id == "x1");

  const auto &tool = std::get<loupe::ToolResultEvent>(
      parsed.session.records[1].events[0].payload);
  REQUIRE(tool.call_id == "call-1");

  const auto &bash_events = parsed.session.records[2].events;
  REQUIRE(bash_events.size() == 2);
  REQUIRE(std::get<loupe::ToolCallEvent>(bash_events[0].payload).name
          == "bash");
  REQUIRE(std::get<loupe::ToolResultEvent>(bash_events[1].payload).is_error);

  const auto &compaction = std::get<loupe::CompactionEvent>(
      parsed.session.records[3].events[0].payload);
  REQUIRE(compaction.tokens_before == 100);
  REQUIRE_FALSE(compaction.replacement_context_json.empty());
  REQUIRE(std::holds_alternative<loupe::MetadataEvent>(
      parsed.session.records[4].events[0].payload));

  const auto &custom_events = parsed.session.records[5].events;
  REQUIRE(custom_events.size() == 2);
  REQUIRE(std::get<loupe::MessageEvent>(custom_events[0].payload).phase
          == "fixture");
  const auto &display =
      std::get<loupe::MetadataEvent>(custom_events[1].payload);
  REQUIRE(display.name == "custom_message.display");
  REQUIRE(display.value == "false");

  const auto selected = loupe::select_conversation_records(parsed.session);
  REQUIRE(selected.size() == 2);
  REQUIRE(parsed.session.records[selected[0]].native_id == "t1");
  REQUIRE(parsed.session.records[selected[1]].native_id == "x1");

  const auto displayed = loupe::make_display_messages(parsed.session);
  REQUIRE(displayed.size() == 1);
  REQUIRE(displayed.front().content == "ok");
}

TEST_CASE("parse nested Pi custom and legacy hook messages", "[pi_parser]") {
  const auto parsed = loupe::parse_session_content(
      R"({"type":"session","version":2,"id":"custom-session","timestamp":"2026-07-23T00:00:00Z","cwd":"/work"}
{"type":"message","id":"c1","parentId":null,"timestamp":"2026-07-23T00:00:01Z","message":{"role":"custom","customType":"hidden-extension","content":"hidden context","display":false}}
{"type":"message","id":"h1","parentId":"c1","timestamp":"2026-07-23T00:00:02Z","message":{"role":"hookMessage","customType":"legacy-hook","content":"visible context","display":true}})",
      loupe::LogFormat::Pi);

  REQUIRE_FALSE(parsed.has_fatal_error());

  const auto &custom_events = parsed.session.records[1].events;
  REQUIRE(custom_events.size() == 2);
  const auto &custom = std::get<loupe::MessageEvent>(custom_events[0].payload);
  REQUIRE(custom.raw_role == "custom");
  REQUIRE(custom.phase == "hidden-extension");
  REQUIRE(std::get<loupe::MetadataEvent>(custom_events[1].payload).value
          == "false");

  const auto &hook_events = parsed.session.records[2].events;
  REQUIRE(hook_events.size() == 2);
  const auto &hook = std::get<loupe::MessageEvent>(hook_events[0].payload);
  REQUIRE(hook.raw_role == "hookMessage");
  REQUIRE(hook.phase == "legacy-hook");
  REQUIRE(std::get<loupe::MetadataEvent>(hook_events[1].payload).value
          == "true");

  const auto displayed = loupe::make_display_messages(parsed.session);
  REQUIRE(displayed.size() == 1);
  REQUIRE(displayed.front().role == "hookMessage");
  REQUIRE(displayed.front().content == "visible context");
}

TEST_CASE("ignore Pi leaf entries without non-empty targets", "[pi_parser]") {
  const auto parsed = loupe::parse_session_content(
      R"({"type":"session","version":3,"id":"leaf-session","timestamp":"2026-07-23T00:00:00Z","cwd":"/work"}
{"type":"message","id":"root","parentId":null,"timestamp":"2026-07-23T00:00:01Z","message":{"role":"user","content":"root"}}
{"type":"message","id":"left","parentId":"root","timestamp":"2026-07-23T00:00:02Z","message":{"role":"assistant","content":"left branch"}}
{"type":"message","id":"right","parentId":"root","timestamp":"2026-07-23T00:00:03Z","message":{"role":"assistant","content":"right branch"}}
{"type":"leaf","id":"empty-leaf","parentId":"right","timestamp":"2026-07-23T00:00:04Z","targetId":""}
{"type":"leaf","id":"missing-leaf","parentId":"right","timestamp":"2026-07-23T00:00:05Z"})",
      loupe::LogFormat::Pi);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.active_leaf_id == "right");
  REQUIRE(has_diagnostic(parsed, loupe::DiagnosticCode::MissingRequiredField, 5,
                         "`targetId` must not be empty"));
  REQUIRE(has_diagnostic(parsed, loupe::DiagnosticCode::MissingRequiredField, 6,
                         "missing required `targetId`"));

  const auto selected = loupe::select_conversation_records(parsed.session);
  REQUIRE(selected.size() == 2);
  REQUIRE(parsed.session.records[selected[0]].native_id == "root");
  REQUIRE(parsed.session.records[selected[1]].native_id == "right");

  const auto displayed = loupe::make_display_messages(parsed.session);
  REQUIRE(displayed.size() == 2);
  REQUIRE(displayed[0].content == "root");
  REQUIRE(displayed[1].content == "right branch");
}

TEST_CASE("reject non-Pi input when the explicit format is Pi", "[pi_parser]") {
  const auto parsed = loupe::parse_session_content(
      R"({"type":"assistant","message":{"role":"assistant","content":[]}})",
      loupe::LogFormat::Pi);

  REQUIRE(parsed.has_fatal_error());
  REQUIRE_FALSE(parsed.session.records.empty());
  REQUIRE(std::holds_alternative<loupe::UnknownEvent>(
      parsed.session.records.front().events.front().payload));
}

TEST_CASE("parse legacy linear Pi sessions without tree ids", "[pi_parser]") {
  const auto parsed = loupe::parse_session_content(
      R"({"type":"session","id":"legacy-session","timestamp":"2026-07-23T00:00:00Z","cwd":"/work"}
{"type":"message","timestamp":"2026-07-23T00:00:01Z","message":{"role":"user","content":"question","timestamp":1000}}
{"type":"message","timestamp":"2026-07-23T00:00:02Z","message":{"role":"assistant","content":[{"type":"text","text":"answer"}],"timestamp":2000}})",
      loupe::LogFormat::Pi);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.source_version == "1");
  REQUIRE(parsed.session.records.front().native_id.empty());

  const auto displayed = loupe::make_display_messages(parsed.session);
  REQUIRE(displayed.size() == 2);
  REQUIRE(displayed[0].timestamp == "2026-07-23T00:00:01Z");
  REQUIRE(displayed[1].timestamp == "2026-07-23T00:00:02Z");
}

TEST_CASE("diagnose missing and invalid Pi v2/v3 parent ids", "[pi_parser]") {
  const auto parsed = loupe::parse_session_content(
      R"({"type":"session","version":3,"id":"session-3","timestamp":"2026-07-23T00:00:00Z","cwd":"/work"}
{"type":"message","id":"u1","timestamp":"2026-07-23T00:00:01Z","message":{"role":"user","content":"missing parent"}}
{"type":"message","id":"a1","parentId":42,"timestamp":"2026-07-23T00:00:02Z","message":{"role":"assistant","content":[{"type":"text","text":"invalid parent"}]}})",
      loupe::LogFormat::Pi);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(has_diagnostic(parsed, loupe::DiagnosticCode::MissingRequiredField, 2,
                         "missing required `parentId`"));
  REQUIRE(has_diagnostic(parsed, loupe::DiagnosticCode::FormatMismatch, 3,
                         "`parentId` must be null or a string"));
  REQUIRE_FALSE(parsed.session.records[1].native_parent_id);
  REQUIRE_FALSE(parsed.session.records[2].native_parent_id);
}
