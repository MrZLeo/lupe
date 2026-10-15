#include "loupe/log_format.hpp"
#include "loupe/log_message.hpp"
#include "loupe/message_projection.hpp"
#include "loupe/session_ir.hpp"
#include "loupe/session_parser.hpp"

#include <catch2/catch_test_macros.hpp>

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
      loupe::parse_session_content(kTodoStream, loupe::LogFormat::CodexExec);
  const auto messages = loupe::make_display_messages(parsed.session);

  std::vector<const loupe::LogMessage *> todos;
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
  loupe::SessionIR session;
  loupe::RecordIR record;
  record.native_type = "item.updated";
  record.source_line = 7;
  record.events.push_back(loupe::EventIR{
      .payload =
          loupe::MetadataEvent{
              .name = "codex_exec.todo_list",
              .value = "not json",
          },
  });
  session.records.push_back(std::move(record));

  const auto messages = loupe::make_display_messages(session);
  REQUIRE(messages.size() == 1);
  REQUIRE(messages.front().raw_type == "todo_list");
  REQUIRE(messages.front().content == "not json");
}

TEST_CASE("Other metadata still follows the show_metadata option",
          "[message_projection]") {
  loupe::SessionIR session;
  loupe::RecordIR record;
  record.native_type = "item.completed";
  record.events.push_back(loupe::EventIR{
      .payload =
          loupe::MetadataEvent{
              .name = "custom",
              .value = "hidden value",
          },
  });
  session.records.push_back(std::move(record));

  REQUIRE(loupe::make_display_messages(session).empty());

  loupe::DisplayOptions options;
  options.show_metadata = true;
  const auto messages = loupe::make_display_messages(session, options);
  REQUIRE(messages.size() == 1);
  REQUIRE(messages.front().content == "hidden value");
}

TEST_CASE("Image references survive display projection",
          "[message_projection]") {
  loupe::SessionIR session;
  loupe::RecordIR record;
  record.native_type = "response_item";
  record.events.push_back(loupe::EventIR{
      .payload =
          loupe::MessageEvent{
              .role = loupe::Role::User,
              .content = {loupe::ImageContent{
                  .mime_type = "image/png",
                  .url = "https://example.com/image.png",
              }},
          },
  });
  session.records.push_back(std::move(record));

  const auto messages = loupe::make_display_messages(session);
  REQUIRE(messages.size() == 1);
  REQUIRE(messages.front().images.size() == 1);
  CHECK(messages.front().images.front().mime_type == "image/png");
  CHECK(messages.front().images.front().url == "https://example.com/image.png");
}
