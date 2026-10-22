#include "lupe/log_format.hpp"
#include "lupe/message_projection.hpp"
#include "lupe/session_ir.hpp"
#include "lupe/session_parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace {

bool has_diagnostic(const lupe::SessionParseResult &parsed,
                    lupe::DiagnosticCode code, std::size_t source_line,
                    std::string_view message_part) {
  return std::ranges::any_of(parsed.diagnostics,
                             [&](const lupe::Diagnostic &diagnostic) {
                               return diagnostic.code == code
                                   && diagnostic.source_line == source_line
                                   && diagnostic.message.contains(message_part);
                             });
}

} // namespace

TEST_CASE("parse Pi messages into ordered semantic events", "[pi_parser]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"session","version":3,"id":"session-1","timestamp":"2026-07-23T00:00:00Z","cwd":"/work"}
{"type":"message","id":"u1","parentId":null,"timestamp":"2026-07-23T00:00:01Z","message":{"role":"user","content":[{"type":"text","text":"hello"},{"type":"image","data":"eA==","mimeType":"image/png"}],"timestamp":1000}}
{"type":"message","id":"a1","parentId":"u1","timestamp":"2026-07-23T00:00:02Z","message":{"role":"assistant","provider":"openai","model":"gpt-5","timestamp":2000,"content":[{"type":"text","text":"before"},{"type":"thinking","thinking":"reason"},{"type":"toolCall","id":"call-1","name":"read","arguments":{"path":"a"}},{"type":"text","text":"after"}],"usage":{"input":10,"output":5,"cacheRead":2,"cacheWrite":1,"reasoning":3,"totalTokens":18,"cost":{"total":0.5}}}})",
      lupe::LogFormat::Pi);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.session_id == "session-1");
  REQUIRE(parsed.session.source_version == "3");
  REQUIRE(parsed.session.cwd == "/work");
  REQUIRE(parsed.session.active_leaf_id == "a1");
  REQUIRE(parsed.session.records.size() == 3);

  const auto &user_events = parsed.session.records[1].events;
  REQUIRE(user_events.size() == 1);
  const auto &user = std::get<lupe::MessageEvent>(user_events[0].payload);
  REQUIRE(user.role == lupe::Role::User);
  REQUIRE(user.content.size() == 2);
  const auto &image = std::get<lupe::ImageContent>(user.content[1]);
  REQUIRE(image.inline_data);
  REQUIRE(image.base64_data);
  REQUIRE(*image.base64_data == "eA==");

  const auto &assistant_events = parsed.session.records[2].events;
  REQUIRE(assistant_events.size() == 5);
  REQUIRE(
      std::get<lupe::TextContent>(
          std::get<lupe::MessageEvent>(assistant_events[0].payload).content[0])
          .text
      == "before");
  REQUIRE(std::get<lupe::ReasoningEvent>(assistant_events[1].payload).content
          == "reason");
  REQUIRE(std::get<lupe::ToolCallEvent>(assistant_events[2].payload).call_id
          == "call-1");
  REQUIRE(
      std::get<lupe::TextContent>(
          std::get<lupe::MessageEvent>(assistant_events[3].payload).content[0])
          .text
      == "after");
  REQUIRE(
      std::get<lupe::UsageEvent>(assistant_events[4].payload).reasoning_tokens
      == 3);

  const auto messages = lupe::make_display_messages(parsed.session);
  REQUIRE(messages.front().images.size() == 1);
  REQUIRE(messages.front().images.front().base64_data == image.base64_data);
}

TEST_CASE("parse Pi tools, compaction, metadata, and active branches",
          "[pi_parser]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"session","version":3,"id":"session-2","timestamp":"2026-07-23T00:00:00Z","cwd":"/work"}
{"type":"message","id":"t1","parentId":null,"timestamp":"2026-07-23T00:00:01Z","message":{"role":"toolResult","toolCallId":"call-1","toolName":"read","content":[{"type":"text","text":"ok"}],"isError":false}}
{"type":"message","id":"b1","parentId":"t1","timestamp":"2026-07-23T00:00:02Z","message":{"role":"bashExecution","command":"false","output":"failed","exitCode":1,"cancelled":false}}
{"type":"compaction","id":"c1","parentId":"b1","timestamp":"2026-07-23T00:00:03Z","summary":"summary","tokensBefore":100,"retainedTail":[{"role":"user","content":"last"}]}
{"type":"model_change","id":"m1","parentId":"c1","timestamp":"2026-07-23T00:00:04Z","provider":"openai","modelId":"gpt-5"}
{"type":"custom_message","id":"x1","parentId":"t1","timestamp":"2026-07-23T00:00:05Z","customType":"fixture","content":"alternate branch","display":false})",
      lupe::LogFormat::Pi);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.active_leaf_id == "x1");

  const auto &tool = std::get<lupe::ToolResultEvent>(
      parsed.session.records[1].events[0].payload);
  REQUIRE(tool.call_id == "call-1");

  const auto &bash_events = parsed.session.records[2].events;
  REQUIRE(bash_events.size() == 2);
  REQUIRE(std::get<lupe::ToolCallEvent>(bash_events[0].payload).name
          == "bash");
  REQUIRE(std::get<lupe::ToolResultEvent>(bash_events[1].payload).is_error);

  const auto &compaction = std::get<lupe::CompactionEvent>(
      parsed.session.records[3].events[0].payload);
  REQUIRE(compaction.tokens_before == 100);
  REQUIRE_FALSE(compaction.replacement_context_json.empty());
  REQUIRE(std::holds_alternative<lupe::MetadataEvent>(
      parsed.session.records[4].events[0].payload));

  const auto &custom_events = parsed.session.records[5].events;
  REQUIRE(custom_events.size() == 2);
  REQUIRE(std::get<lupe::MessageEvent>(custom_events[0].payload).phase
          == "fixture");
  const auto &display =
      std::get<lupe::MetadataEvent>(custom_events[1].payload);
  REQUIRE(display.name == "custom_message.display");
  REQUIRE(display.value == "false");

  const auto selected = lupe::select_conversation_records(parsed.session);
  REQUIRE(selected.size() == 2);
  REQUIRE(parsed.session.records[selected[0]].native_id == "t1");
  REQUIRE(parsed.session.records[selected[1]].native_id == "x1");

  const auto displayed = lupe::make_display_messages(parsed.session);
  REQUIRE(displayed.size() == 1);
  REQUIRE(displayed.front().content == "ok");
}

TEST_CASE("parse nested Pi custom and legacy hook messages", "[pi_parser]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"session","version":2,"id":"custom-session","timestamp":"2026-07-23T00:00:00Z","cwd":"/work"}
{"type":"message","id":"c1","parentId":null,"timestamp":"2026-07-23T00:00:01Z","message":{"role":"custom","customType":"hidden-extension","content":"hidden context","display":false}}
{"type":"message","id":"h1","parentId":"c1","timestamp":"2026-07-23T00:00:02Z","message":{"role":"hookMessage","customType":"legacy-hook","content":"visible context","display":true}})",
      lupe::LogFormat::Pi);

  REQUIRE_FALSE(parsed.has_fatal_error());

  const auto &custom_events = parsed.session.records[1].events;
  REQUIRE(custom_events.size() == 2);
  const auto &custom = std::get<lupe::MessageEvent>(custom_events[0].payload);
  REQUIRE(custom.raw_role == "custom");
  REQUIRE(custom.phase == "hidden-extension");
  REQUIRE(std::get<lupe::MetadataEvent>(custom_events[1].payload).value
          == "false");

  const auto &hook_events = parsed.session.records[2].events;
  REQUIRE(hook_events.size() == 2);
  const auto &hook = std::get<lupe::MessageEvent>(hook_events[0].payload);
  REQUIRE(hook.raw_role == "hookMessage");
  REQUIRE(hook.phase == "legacy-hook");
  REQUIRE(std::get<lupe::MetadataEvent>(hook_events[1].payload).value
          == "true");

  const auto displayed = lupe::make_display_messages(parsed.session);
  REQUIRE(displayed.size() == 1);
  REQUIRE(displayed.front().role == "hookMessage");
  REQUIRE(displayed.front().content == "visible context");
}

TEST_CASE("ignore Pi leaf entries without non-empty targets", "[pi_parser]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"session","version":3,"id":"leaf-session","timestamp":"2026-07-23T00:00:00Z","cwd":"/work"}
{"type":"message","id":"root","parentId":null,"timestamp":"2026-07-23T00:00:01Z","message":{"role":"user","content":"root"}}
{"type":"message","id":"left","parentId":"root","timestamp":"2026-07-23T00:00:02Z","message":{"role":"assistant","content":"left branch"}}
{"type":"message","id":"right","parentId":"root","timestamp":"2026-07-23T00:00:03Z","message":{"role":"assistant","content":"right branch"}}
{"type":"leaf","id":"empty-leaf","parentId":"right","timestamp":"2026-07-23T00:00:04Z","targetId":""}
{"type":"leaf","id":"missing-leaf","parentId":"right","timestamp":"2026-07-23T00:00:05Z"})",
      lupe::LogFormat::Pi);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.active_leaf_id == "right");
  REQUIRE(has_diagnostic(parsed, lupe::DiagnosticCode::MissingRequiredField, 5,
                         "`targetId` must not be empty"));
  REQUIRE(has_diagnostic(parsed, lupe::DiagnosticCode::MissingRequiredField, 6,
                         "missing required `targetId`"));

  const auto selected = lupe::select_conversation_records(parsed.session);
  REQUIRE(selected.size() == 2);
  REQUIRE(parsed.session.records[selected[0]].native_id == "root");
  REQUIRE(parsed.session.records[selected[1]].native_id == "right");

  const auto displayed = lupe::make_display_messages(parsed.session);
  REQUIRE(displayed.size() == 2);
  REQUIRE(displayed[0].content == "root");
  REQUIRE(displayed[1].content == "right branch");
}

TEST_CASE("reject non-Pi input when the explicit format is Pi", "[pi_parser]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"assistant","message":{"role":"assistant","content":[]}})",
      lupe::LogFormat::Pi);

  REQUIRE(parsed.has_fatal_error());
  REQUIRE_FALSE(parsed.session.records.empty());
  REQUIRE(std::holds_alternative<lupe::UnknownEvent>(
      parsed.session.records.front().events.front().payload));
}

TEST_CASE("parse legacy linear Pi sessions without tree ids", "[pi_parser]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"session","id":"legacy-session","timestamp":"2026-07-23T00:00:00Z","cwd":"/work"}
{"type":"message","timestamp":"2026-07-23T00:00:01Z","message":{"role":"user","content":"question","timestamp":1000}}
{"type":"message","timestamp":"2026-07-23T00:00:02Z","message":{"role":"assistant","content":[{"type":"text","text":"answer"}],"timestamp":2000}})",
      lupe::LogFormat::Pi);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.source_version == "1");
  REQUIRE(parsed.session.records.front().native_id.empty());

  const auto displayed = lupe::make_display_messages(parsed.session);
  REQUIRE(displayed.size() == 2);
  REQUIRE(displayed[0].timestamp == "2026-07-23T00:00:01Z");
  REQUIRE(displayed[1].timestamp == "2026-07-23T00:00:02Z");
}

TEST_CASE("diagnose missing and invalid Pi v2/v3 parent ids", "[pi_parser]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"session","version":3,"id":"session-3","timestamp":"2026-07-23T00:00:00Z","cwd":"/work"}
{"type":"message","id":"u1","timestamp":"2026-07-23T00:00:01Z","message":{"role":"user","content":"missing parent"}}
{"type":"message","id":"a1","parentId":42,"timestamp":"2026-07-23T00:00:02Z","message":{"role":"assistant","content":[{"type":"text","text":"invalid parent"}]}})",
      lupe::LogFormat::Pi);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(has_diagnostic(parsed, lupe::DiagnosticCode::MissingRequiredField, 2,
                         "missing required `parentId`"));
  REQUIRE(has_diagnostic(parsed, lupe::DiagnosticCode::FormatMismatch, 3,
                         "`parentId` must be null or a string"));
  REQUIRE_FALSE(parsed.session.records[1].native_parent_id);
  REQUIRE_FALSE(parsed.session.records[2].native_parent_id);
}

TEST_CASE("Pi codemode scripts preserve source rather than JSON escaping",
          "[pi_parser][codemode]") {
  const auto parsed = lupe::parse_session_content(
      R"jsonl({"type":"session","version":3,"id":"scripts","timestamp":"2026-10-22T00:00:00Z","cwd":"/work"}
{"type":"message","id":"a1","parentId":null,"message":{"role":"assistant","content":[{"type":"toolCall","id":"cm","name":"codemode","arguments":{"code":"// @options: {\"max_output_tokens\": 2000}\nconst line = \"\\n\";\ntext(await tools.read({path: \"file.txt\"}));"}},{"type":"toolCall","id":"empty","name":"codemode","arguments":{"code":""}},{"type":"toolCall","id":"invalid","name":"codemode","arguments":{"code":42}},{"type":"toolCall","id":"missing","name":"codemode","arguments":{}},{"type":"toolCall","id":"regular","name":"custom_tool","arguments":{"code":"keep as JSON"}}]}})jsonl",
      lupe::LogFormat::Pi);

  REQUIRE(parsed.diagnostics.empty());
  const auto &events = parsed.session.records[1].events;
  REQUIRE(events.size() == 5);
  const std::string source = R"js(// @options: {"max_output_tokens": 2000}
const line = "\n";
text(await tools.read({path: "file.txt"}));)js";
  const auto &script = std::get<lupe::ToolCallEvent>(events[0].payload);
  CHECK(script.call_id == "cm");
  CHECK(script.name == "codemode");
  CHECK(script.name_space == "pi");
  CHECK(script.input == source);
  CHECK_FALSE(script.input_is_json);

  const auto &empty = std::get<lupe::ToolCallEvent>(events[1].payload);
  CHECK(empty.input.empty());
  CHECK_FALSE(empty.input_is_json);
  const auto &invalid = std::get<lupe::ToolCallEvent>(events[2].payload);
  CHECK(invalid.input == R"({"code":42})");
  CHECK(invalid.input_is_json);
  const auto &missing = std::get<lupe::ToolCallEvent>(events[3].payload);
  CHECK(missing.input == "{}");
  CHECK(missing.input_is_json);
  const auto &regular = std::get<lupe::ToolCallEvent>(events[4].payload);
  CHECK(regular.input == R"({"code":"keep as JSON"})");
  CHECK(regular.input_is_json);

  const auto displayed = lupe::make_display_messages(parsed.session);
  REQUIRE(displayed.size() == 1);
  REQUIRE(displayed[0].annotations.size() == 5);
  CHECK(displayed[0].annotations[0] == "call pi::codemode [cm]\n" + source);
  CHECK(displayed[0].annotations[1] == "call pi::codemode [empty]");
}

TEST_CASE("Pi codemode merges nested calls without inventing tool outputs",
          "[pi_parser][codemode]") {
  const auto parsed = lupe::parse_session_content(
      R"jsonl({"type":"session","version":3,"id":"nested","timestamp":"2026-10-22T00:00:00Z","cwd":"/work"}
{"type":"message","id":"a1","parentId":null,"message":{"role":"assistant","content":[{"type":"toolCall","id":"cm","name":"codemode","arguments":{"code":"return await tools.read({path: 'file.txt'});"}}]}}
{"type":"message","id":"r1","parentId":"a1","timestamp":"2026-10-22T00:00:01Z","message":{"role":"toolResult","toolCallId":"cm","toolName":"codemode","content":[{"type":"text","text":"Script completed\nWall time 0.1 seconds\nOutput:\n"},{"type":"text","text":"==> text 1/2 <==\nfile contents\n==> text 2/2 <==\nfiltered result\n<console_output>\nliteral: \\n\n</console_output>"},{"type":"image","data":"eA==","mimeType":"image/png"}],"isError":false,"nestedCalls":{"complete":true,"calls":[{"id":"cm/1","name":"read","arguments":{"path":"file.txt","offset":1},"status":"ok","durationMs":3},{"id":"cm/2","name":"bash","arguments":{"command":"false"},"status":"error","durationMs":4,"error":"command failed"}]},"details":{"calls":[{"id":"cm/2","name":"bash","args":"truncated preview","status":"error","durationMs":4.5,"error":"short error"},{"id":"cm/1","name":"read","args":"truncated preview","status":"ok","durationMs":3.5},{"id":"cm/model/1","name":"models.classify","args":"{\"provider\":\"typesafe\"}","status":"ok","durationMs":5.5,"cost":0.125}],"fullOutputPath":"/tmp/full-output.txt"},"usage":{"input":10,"output":2,"totalTokens":12,"cost":{"total":0.125}}}}
{"type":"custom","id":"store1","parentId":"r1","customType":"codemode-store","data":{"set":{"cursor":42},"delete":[]}})jsonl",
      lupe::LogFormat::Pi);

  REQUIRE(parsed.diagnostics.empty());
  const auto &record = parsed.session.records[2];
  REQUIRE(record.events.size() == 2);
  const auto &result =
      std::get<lupe::ToolResultEvent>(record.events[0].payload);
  REQUIRE(result.nested_calls.size() == 3);
  CHECK(result.call_id == "cm");
  CHECK_FALSE(result.is_error);
  CHECK(result.nested_calls_complete == true);
  CHECK(result.full_output_path == "/tmp/full-output.txt");

  const auto &read = result.nested_calls[0];
  CHECK(read.call.call_id == "cm/1");
  CHECK(read.call.name == "read");
  CHECK(read.call.input == R"({"path":"file.txt","offset":1})");
  CHECK(read.call.input_is_json);
  CHECK(read.duration_ms == 3);
  CHECK(read.status == "ok");
  const auto &bash = result.nested_calls[1];
  CHECK(bash.call.call_id == "cm/2");
  CHECK(bash.status == "error");
  CHECK(bash.error == "command failed");
  const auto &model = result.nested_calls[2];
  CHECK(model.call.name == "models.classify");
  CHECK_FALSE(model.call.input_is_json);
  CHECK(model.duration_ms == 5.5);
  CHECK(model.cost == 0.125);

  REQUIRE(result.output.size() == 3);
  const auto &image = std::get<lupe::ImageContent>(result.output[2]);
  REQUIRE(image.base64_data);
  CHECK(*image.base64_data == "eA==");
  const auto &usage = std::get<lupe::UsageEvent>(record.events[1].payload);
  CHECK(usage.total_tokens == 12);
  CHECK(usage.cost == 0.125);
  CHECK(record.raw_json.contains("truncated preview"));
  CHECK(record.source_line == 3);
  CHECK(record.native_parent_id == "a1");
  const auto &store = std::get<lupe::MetadataEvent>(
      parsed.session.records[3].events[0].payload);
  CHECK(store.name == "custom:codemode-store");
  CHECK(store.value.contains("\"cursor\":42"));

  const auto displayed = lupe::make_display_messages(parsed.session);
  REQUIRE(displayed.size() == 2);
  const auto &tool = displayed[1];
  CHECK(tool.role == "tool");
  CHECK(tool.timestamp == "2026-10-22T00:00:01Z");
  CHECK(tool.source_line == 3);
  CHECK(tool.content.starts_with("Script completed\n"));
  CHECK(tool.content.contains("==> text 2/2 <==\nfiltered result"));
  CHECK(tool.content.contains("literal: \\n\n"));
  REQUIRE(tool.images.size() == 1);
  CHECK(tool.images[0].base64_data == image.base64_data);
  REQUIRE(tool.annotations.size() == 6);
  CHECK(tool.annotations[0] == "result for codemode [cm]");
  CHECK(
      tool.annotations[1].starts_with("nested call pi::read [cm/1] status=ok"));
  CHECK(tool.annotations[1].contains("duration=3ms"));
  CHECK_FALSE(tool.annotations[1].contains("truncated preview"));
  CHECK(tool.annotations[2].contains("error: command failed"));
  CHECK(tool.annotations[3].contains("cost=0.125"));
  CHECK(tool.annotations[4] == "full output: /tmp/full-output.txt");
  CHECK(tool.annotations[5].starts_with("usage input=10"));
}

TEST_CASE("Pi codemode keeps model calls between their recorded tool anchors",
          "[pi_parser][codemode]") {
  const auto parsed = lupe::parse_session_content(
      R"jsonl({"type":"session","version":3,"id":"call-order","timestamp":"2026-10-22T00:00:00Z","cwd":"/work"}
{"type":"message","id":"r1","parentId":null,"message":{"role":"toolResult","toolCallId":"cm","toolName":"codemode","content":"output","isError":false,"nestedCalls":{"complete":true,"calls":[{"id":"cm/1","name":"read","arguments":{"path":"full-path"},"status":"ok"},{"id":"cm/1/1","name":"inner","arguments":{},"status":"ok"},{"id":"cm/2","name":"write","arguments":{},"status":"ok"}]},"details":{"calls":[{"id":"cm/models.classify/1","name":"models.classify","args":"before","status":"ok"},{"id":"cm/1","name":"read","args":"preview","status":"ok"},{"id":"cm/models.classify/2","name":"models.classify","args":"middle","status":"ok"},{"id":"cm/models.classify/2","name":"models.classify","args":"duplicate","status":"ok"},{"id":"cm/?","name":"pending","args":"first","status":"cancelled"},{"id":"cm/?","name":"pending","args":"second","status":"cancelled"},{"id":"cm/2","name":"write","args":"{}","status":"ok"},{"id":"cm/models.generateImages/3","name":"models.generateImages","args":"after","status":"ok"}]}}})jsonl",
      lupe::LogFormat::Pi);

  REQUIRE(parsed.diagnostics.empty());
  const auto &result = std::get<lupe::ToolResultEvent>(
      parsed.session.records[1].events[0].payload);
  const auto &calls = result.nested_calls;
  REQUIRE(calls.size() == 8);
  CHECK(calls[0].call.call_id == "cm/models.classify/1");
  CHECK(calls[1].call.call_id == "cm/1");
  CHECK(calls[1].call.input == R"({"path":"full-path"})");
  CHECK(calls[1].call.input_is_json);
  CHECK(calls[2].call.call_id == "cm/1/1");
  CHECK(calls[3].call.call_id == "cm/models.classify/2");
  CHECK(calls[3].call.input == "middle");
  CHECK(calls[4].call.input == "first");
  CHECK(calls[5].call.input == "second");
  CHECK(calls[6].call.call_id == "cm/2");
  CHECK(calls[7].call.call_id == "cm/models.generateImages/3");

  const auto displayed = lupe::make_display_messages(parsed.session);
  REQUIRE(displayed.size() == 1);
  REQUIRE(displayed[0].annotations.size() == 9);
  CHECK(displayed[0].annotations[1].contains("[cm/models.classify/1]"));
  CHECK(displayed[0].annotations[2].contains("[cm/1]"));
  CHECK(displayed[0].annotations[4].contains("[cm/models.classify/2]"));
  CHECK(displayed[0].annotations[7].contains("[cm/2]"));
}

TEST_CASE("Pi codemode marks omitted arguments and unfinished calls",
          "[pi_parser][codemode]") {
  const auto parsed = lupe::parse_session_content(
      R"jsonl({"type":"session","version":3,"id":"incomplete","timestamp":"2026-10-22T00:00:00Z","cwd":"/work"}
{"type":"message","id":"r1","parentId":null,"message":{"role":"toolResult","toolCallId":"cm","toolName":"codemode","content":[{"type":"text","text":"Script failed\npartial output\nScript error:\ntimeout"}],"isError":true,"nestedCalls":{"complete":false,"calls":[{"id":"cm/1","name":"write","argumentsBytes":9000,"status":"error","error":"aborted"},{"id":"cm/2","name":"read","status":"unfinished"}]},"details":{"calls":[{"id":"cm/1","name":"write","args":"{\"content\":\"partial...","status":"cancelled","durationMs":10.5},{"id":"cm/?","name":"bash","args":"first","status":"cancelled"},{"id":"cm/?","name":"bash","args":"second","status":"cancelled"}]}}})jsonl",
      lupe::LogFormat::Pi);

  REQUIRE(parsed.diagnostics.empty());
  const auto &result = std::get<lupe::ToolResultEvent>(
      parsed.session.records[1].events[0].payload);
  REQUIRE(result.nested_calls.size() == 4);
  CHECK(result.nested_calls_complete == false);
  const auto &write = result.nested_calls[0];
  CHECK(write.omitted_input_bytes == 9000);
  CHECK(write.call.input == R"({"content":"partial...)");
  CHECK_FALSE(write.call.input_is_json);
  CHECK(write.status == "cancelled");
  CHECK(write.duration_ms == 10.5);
  const auto &unfinished = result.nested_calls[1];
  CHECK(unfinished.status == "unfinished");
  CHECK(unfinished.call.input.empty());
  CHECK_FALSE(unfinished.call.input_is_json);
  CHECK_FALSE(unfinished.duration_ms);
  CHECK(result.nested_calls[2].call.input == "first");
  CHECK(result.nested_calls[3].call.input == "second");

  const auto displayed = lupe::make_display_messages(parsed.session);
  REQUIRE(displayed.size() == 1);
  REQUIRE(displayed[0].annotations.size() == 6);
  CHECK(displayed[0].content.contains("partial output"));
  CHECK(displayed[0].annotations[0] == "result for codemode [cm] error");
  CHECK(displayed[0].annotations[1] == "nested calls: incomplete");
  CHECK(displayed[0].annotations[2].contains("status=cancelled"));
  CHECK(displayed[0].annotations[2].contains(
      "full arguments omitted (9000 bytes)"));
  CHECK(displayed[0].annotations[2].contains("error: aborted"));
  CHECK(displayed[0].annotations[3].contains("status=unfinished"));
}

TEST_CASE("Pi codemode supports details-only results and branch selection",
          "[pi_parser][codemode]") {
  const auto parsed = lupe::parse_session_content(
      R"jsonl({"type":"session","version":3,"id":"legacy-codemode","timestamp":"2026-10-22T00:00:00Z","cwd":"/work"}
{"type":"message","id":"u1","parentId":null,"message":{"role":"user","content":"start"}}
{"type":"message","id":"left","parentId":"u1","message":{"role":"toolResult","toolCallId":"cm-left","toolName":"codemode","content":[{"type":"text","text":"left output"}],"details":{"calls":[{"id":"cm-left/1","name":"read","args":"{\"path\":\"a\"}","status":"ok","durationMs":0}]},"isError":false}}
{"type":"custom","id":"left-store","parentId":"left","customType":"codemode-store","data":{"set":{"branch":"left"},"delete":[]}}
{"type":"message","id":"right","parentId":"u1","message":{"role":"toolResult","toolCallId":"cm-right","toolName":"codemode","content":[{"type":"text","text":"right output"}],"details":{"calls":[{"id":"cm-right/model/1","name":"models.generateImages","args":"preview","status":"error","error":"model failed","cost":0}]},"isError":true}})jsonl",
      lupe::LogFormat::Pi);

  REQUIRE(parsed.diagnostics.empty());
  const auto &left_result = std::get<lupe::ToolResultEvent>(
      parsed.session.records[2].events[0].payload);
  CHECK_FALSE(left_result.nested_calls_complete.has_value());
  REQUIRE(left_result.nested_calls.size() == 1);
  CHECK(left_result.nested_calls[0].duration_ms == 0);

  const auto displayed = lupe::make_display_messages(parsed.session);
  REQUIRE(displayed.size() == 2);
  CHECK(displayed[1].content == "right output");
  REQUIRE(displayed[1].annotations.size() == 2);
  CHECK(displayed[1].annotations[1].contains("models.generateImages"));
  CHECK(displayed[1].annotations[1].contains("cost=0"));
  CHECK(displayed[1].annotations[1].contains("error: model failed"));

  lupe::DisplayOptions options;
  options.leaf_id = "left-store";
  const auto left = lupe::make_display_messages(parsed.session, options);
  REQUIRE(left.size() == 2);
  CHECK(left[1].content == "left output");
  REQUIRE(left[1].annotations.size() == 2);
  CHECK(left[1].annotations[1].contains("nested call pi::read"));
  CHECK(left[1].annotations[1].contains("duration=0ms"));
}

TEST_CASE("Pi keeps generic nested calls separate from tool-specific details",
          "[pi_parser][codemode]") {
  const auto parsed = lupe::parse_session_content(
      R"jsonl({"type":"session","version":3,"id":"regular","timestamp":"2026-10-22T00:00:00Z","cwd":"/work"}
{"type":"message","id":"r1","parentId":null,"message":{"role":"toolResult","toolCallId":"batch","toolName":"batch","content":"ok","isError":false,"nestedCalls":{"complete":true,"calls":[{"id":"batch/1","name":"read","arguments":{},"status":"ok"}]},"details":{"calls":[{"id":"unrelated","name":"not a nested tool"}],"fullOutputPath":"unrelated"}}})jsonl",
      lupe::LogFormat::Pi);

  REQUIRE(parsed.diagnostics.empty());
  const auto &result = std::get<lupe::ToolResultEvent>(
      parsed.session.records[1].events[0].payload);
  REQUIRE(result.nested_calls.size() == 1);
  CHECK(result.nested_calls[0].call.name == "read");
  CHECK(result.nested_calls[0].call.input == "{}");
  CHECK(result.full_output_path.empty());
  const auto displayed = lupe::make_display_messages(parsed.session);
  REQUIRE(displayed.size() == 1);
  CHECK(displayed[0].content == "ok");
  CHECK(displayed[0].annotations.size() == 2);
}

TEST_CASE("Pi codemode tolerates absent and malformed optional call metadata",
          "[pi_parser][codemode]") {
  for (
      const std::string_view metadata : {
          "",
          R"(,"nestedCalls":null,"details":null)",
          R"(,"nestedCalls":{"calls":"bad"},"details":{"calls":42})",
          R"(,"nestedCalls":{"calls":[null,42,"bad",[]]},"details":{"calls":[false,null]})",
          R"(,"nestedCalls":{"calls":[],"complete":false})",
      }) {
    INFO(metadata);
    const std::string content =
        R"jsonl({"type":"session","version":3,"id":"empty","timestamp":"2026-10-22T00:00:00Z","cwd":"/work"}
{"type":"message","id":"r1","parentId":null,"message":{"role":"toolResult","toolCallId":"cm","toolName":"codemode","content":"output","isError":false)jsonl"
        + std::string{metadata}
        + "}}";
    const auto parsed =
        lupe::parse_session_content(content, lupe::LogFormat::Pi);
    REQUIRE_FALSE(parsed.has_fatal_error());
    const auto &result = std::get<lupe::ToolResultEvent>(
        parsed.session.records[1].events[0].payload);
    CHECK(result.nested_calls.empty());
    CHECK(parsed.session.records[1].raw_json.ends_with(std::string{metadata}
                                                       + "}}"));
    const auto displayed = lupe::make_display_messages(parsed.session);
    REQUIRE(displayed.size() == 1);
    CHECK(displayed[0].content == "output");
    if (result.nested_calls_complete == false) {
      REQUIRE(displayed[0].annotations.size() == 2);
      CHECK(displayed[0].annotations[1] == "nested calls: incomplete");
    } else {
      CHECK(displayed[0].annotations.size() == 1);
    }
  }
}
