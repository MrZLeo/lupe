#include "lupe/log_format.hpp"
#include "lupe/log_message.hpp"
#include "lupe/message_projection.hpp"
#include "lupe/session_ir.hpp"
#include "lupe/session_parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kTodoStream = R"jsonl(
{"type":"thread.started","thread_id":"t-1"}
{"type":"turn.started"}
{"type":"item.started","item":{"id":"todo","type":"todo_list","items":[{"text":"inspect","completed":false},{"text":"fix","completed":false}]}}
{"type":"item.updated","item":{"id":"todo","type":"todo_list","items":[{"text":"inspect","completed":true},{"text":"fix","completed":false}]}}
{"type":"item.completed","item":{"id":"todo","type":"todo_list","items":[{"text":"inspect","completed":true},{"text":"fix","completed":true}]}}
{"type":"item.completed","item":{"id":"msg","type":"agent_message","text":"done"}}
{"type":"turn.completed","usage":{"input_tokens":1,"cached_input_tokens":0,"output_tokens":1,"reasoning_output_tokens":0}}
)jsonl";

} // namespace

TEST_CASE("Codex Exec todo lists project to checklist messages",
          "[message_projection]") {
  const auto parsed =
      lupe::parse_session_content(kTodoStream, lupe::LogFormat::CodexExec);
  const auto messages = lupe::make_display_messages(parsed.session);

  std::vector<const lupe::LogMessage *> todos;
  for (const auto &message : messages) {
    if (message.raw_type == "todo_list") {
      todos.push_back(&message);
    }
  }
  // Each record changes the list, so every snapshot projects a message.
  REQUIRE(todos.size() == 3);
  REQUIRE(todos[0]->role == "system");
  REQUIRE(todos[0]->content == "[ ] inspect\n[ ] fix");
  REQUIRE(todos[0]->source_line == 4);
  REQUIRE(todos[1]->content == "[x] inspect\n[ ] fix");
  REQUIRE(todos[1]->source_line == 5);
  REQUIRE(todos[2]->content == "[x] inspect\n[x] fix");
  REQUIRE(todos[2]->source_line == 6);
}

TEST_CASE("Todo list projection keeps unparseable payloads verbatim",
          "[message_projection]") {
  lupe::SessionIR session;
  lupe::RecordIR record;
  record.native_type = "item.updated";
  record.source_line = 7;
  record.events.push_back(lupe::EventIR{
      .payload =
          lupe::MetadataEvent{
              .name = "codex_exec.todo_list",
              .value = "not json",
          },
  });
  session.records.push_back(std::move(record));

  const auto messages = lupe::make_display_messages(session);
  REQUIRE(messages.size() == 1);
  REQUIRE(messages.front().raw_type == "todo_list");
  REQUIRE(messages.front().content == "not json");
}

TEST_CASE("Other metadata still follows the show_metadata option",
          "[message_projection]") {
  lupe::SessionIR session;
  lupe::RecordIR record;
  record.native_type = "item.completed";
  record.events.push_back(lupe::EventIR{
      .payload =
          lupe::MetadataEvent{
              .name = "custom",
              .value = "hidden value",
          },
  });
  session.records.push_back(std::move(record));

  REQUIRE(lupe::make_display_messages(session).empty());

  lupe::DisplayOptions options;
  options.show_metadata = true;
  const auto messages = lupe::make_display_messages(session, options);
  REQUIRE(messages.size() == 1);
  REQUIRE(messages.front().content == "hidden value");
}

TEST_CASE("Image references survive display projection",
          "[message_projection]") {
  lupe::SessionIR session;
  lupe::RecordIR record;
  record.native_type = "response_item";
  record.events.push_back(lupe::EventIR{
      .payload =
          lupe::MessageEvent{
              .role = lupe::Role::User,
              .content = {lupe::ImageContent{
                  .mime_type = "image/png",
                  .url = "https://example.com/image.png",
              }},
          },
  });
  session.records.push_back(std::move(record));

  const auto messages = lupe::make_display_messages(session);
  REQUIRE(messages.size() == 1);
  REQUIRE(messages.front().images.size() == 1);
  CHECK(messages.front().images.front().mime_type == "image/png");
  CHECK(messages.front().images.front().url == "https://example.com/image.png");
}

TEST_CASE("Nested call metadata stays on its owning tool result",
          "[message_projection]") {
  lupe::SessionIR session;
  lupe::RecordIR record;
  record.native_type = "tool_result";
  record.source_line = 7;

  lupe::NestedToolCall nested;
  nested.call.call_id = "outer/1";
  nested.call.name = "read";
  nested.call.name_space = "example";
  nested.call.input = R"({"path":"literal\n...)"
                      "\n";
  nested.status = "cancelled";
  nested.duration_ms = 12.5;
  nested.error = "interrupted\npartial work retained";
  nested.omitted_input_bytes = 9000;
  nested.cost = 0.00000001;

  lupe::ToolResultEvent result;
  result.call_id = "outer";
  result.name = "batch";
  result.output = {lupe::TextContent{.text = "output stays unchanged"}};
  result.nested_calls = {nested};
  result.nested_calls_complete = false;
  result.full_output_path = "/tmp/output.txt";
  lupe::append_event(record, std::move(result));
  session.records.push_back(std::move(record));

  const auto messages = lupe::make_display_messages(session);
  REQUIRE(messages.size() == 1);
  const auto &message = messages.front();
  CHECK(message.role == "tool");
  CHECK(message.content == "output stays unchanged");
  CHECK(message.source_line == 7);
  REQUIRE(message.annotations.size() == 4);
  CHECK(message.annotations[0] == "result for batch [outer]");
  CHECK(message.annotations[1] == "nested calls: incomplete");
  CHECK(message.annotations[2]
        == "nested call example::read [outer/1] status=cancelled"
           " duration=12.5ms cost=1e-08\nfull arguments omitted (9000 bytes)\n"
               + nested.call.input
               + "\nerror: interrupted\npartial work retained");
  CHECK(message.annotations[3] == "full output: /tmp/output.txt");
}
