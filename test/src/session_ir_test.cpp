#include "lupe/log_format.hpp"
#include "lupe/message_projection.hpp"
#include "lupe/session_ir.hpp"
#include "lupe/session_parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

TEST_CASE("parse explicit log format names", "[session_ir]") {
  REQUIRE(lupe::parse_log_format("pi") == lupe::LogFormat::Pi);
  REQUIRE(lupe::parse_log_format("codex") == lupe::LogFormat::Codex);
  REQUIRE(lupe::parse_log_format("codex-exec") == lupe::LogFormat::CodexExec);
  REQUIRE(lupe::parse_log_format("claudecode")
          == lupe::LogFormat::ClaudeCode);
  REQUIRE(lupe::parse_log_format("generic") == lupe::LogFormat::Generic);
  REQUIRE(lupe::parse_log_format("auto") == lupe::LogFormat::Auto);
  REQUIRE_FALSE(lupe::parse_log_format("yaml").has_value());
  REQUIRE(lupe::log_format_name(lupe::LogFormat::CodexExec) == "codex-exec");
  REQUIRE(lupe::log_format_name(lupe::LogFormat::Auto) == "auto");
}

TEST_CASE("select active branch from parent-linked records", "[session_ir]") {
  lupe::SessionIR session{
      .format = lupe::LogFormat::Pi,
      .active_leaf_id = "right",
      .records =
          {
              lupe::RecordIR{.sequence = 0,
                              .native_id = "root",
                              .native_parent_id = std::nullopt},
              lupe::RecordIR{.sequence = 1,
                              .native_id = "left",
                              .native_parent_id = "root"},
              lupe::RecordIR{.sequence = 2,
                              .native_id = "right",
                              .native_parent_id = "root"},
          },
  };

  const auto selected = lupe::select_conversation_records(session);

  REQUIRE(selected == std::vector<std::size_t>{0, 2});
  REQUIRE(lupe::select_conversation_records(session, "missing").empty());

  session.active_leaf_id = "missing";
  REQUIRE(lupe::select_conversation_records(session).empty());
  const auto diagnostics = lupe::validate_session(session);
  REQUIRE(
      std::ranges::any_of(diagnostics, [](const lupe::Diagnostic &diagnostic) {
        return diagnostic.code == lupe::DiagnosticCode::MissingParent
            && diagnostic.message.contains("active conversation");
      }));
}

TEST_CASE("navigation parent can explicitly override a native edge with root",
          "[session_ir]") {
  const lupe::SessionIR session{
      .format = lupe::LogFormat::Pi,
      .active_leaf_id = "child",
      .records =
          {
              lupe::RecordIR{
                  .sequence = 0,
                  .native_id = "root",
              },
              lupe::RecordIR{
                  .sequence = 1,
                  .native_id = "child",
                  .native_parent_id = "root",
                  .navigation_parent_id = "",
              },
          },
  };

  REQUIRE(lupe::select_conversation_records(session)
          == std::vector<std::size_t>{1});
}

TEST_CASE("validate duplicate ids, missing parents, and empty call ids",
          "[session_ir]") {
  lupe::RecordIR first{
      .sequence = 0,
      .source_line = 1,
      .native_id = "same",
  };
  lupe::append_event(first, lupe::ToolCallEvent{});

  const lupe::SessionIR session{
      .format = lupe::LogFormat::Pi,
      .records =
          {
              std::move(first),
              lupe::RecordIR{
                  .sequence = 1,
                  .source_line = 2,
                  .native_id = "same",
                  .native_parent_id = "missing",
              },
          },
  };

  const auto diagnostics = lupe::validate_session(session);

  REQUIRE(diagnostics.size() == 3);
}

TEST_CASE("project structured events into display messages", "[session_ir]") {
  lupe::RecordIR record{
      .sequence = 0,
      .source_line = 7,
      .native_type = "message",
      .timestamp = "2026-07-23T00:00:00Z",
  };
  lupe::append_event(record,
                      lupe::MessageEvent{
                          .role = lupe::Role::Assistant,
                          .raw_role = "assistant",
                          .content = {lupe::TextContent{.text = "Checking."}},
                      });
  lupe::append_event(record, lupe::ToolCallEvent{
                                  .call_id = "call-1",
                                  .name = "bash",
                                  .input = R"({"cmd":"pwd"})",
                                  .input_is_json = true,
                              });

  const lupe::SessionIR session{
      .format = lupe::LogFormat::Codex,
      .records = {std::move(record)},
  };

  const auto messages = lupe::make_display_messages(session);

  REQUIRE(messages.size() == 1);
  REQUIRE(messages.front().role == "assistant");
  REQUIRE(messages.front().content == "Checking.");
  REQUIRE(messages.front().source_line == 7);
  REQUIRE(messages.front().annotations.size() == 1);
  REQUIRE(messages.front().annotations.front().contains("call bash"));
}

TEST_CASE("hide unknown content blocks unless explicitly requested",
          "[session_ir]") {
  lupe::RecordIR record{
      .sequence = 0,
      .source_line = 3,
      .native_type = "response_item",
  };
  lupe::append_event(
      record,
      lupe::MessageEvent{
          .role = lupe::Role::Agent,
          .raw_role = "agent",
          .content =
              {
                  lupe::TextContent{.text = "readable"},
                  lupe::UnknownContent{
                      .native_type = "encrypted_content",
                      .json = R"({"type":"encrypted_content","data":"cipher"})",
                  },
              },
      });

  const lupe::SessionIR session{
      .format = lupe::LogFormat::Codex,
      .records = {std::move(record)},
  };

  const auto default_messages = lupe::make_display_messages(session);
  REQUIRE(default_messages.size() == 1);
  REQUIRE(default_messages.front().content == "readable");

  const auto messages_with_unknown = lupe::make_display_messages(
      session, lupe::DisplayOptions{.show_unknown = true});
  REQUIRE(messages_with_unknown.size() == 1);
  REQUIRE(messages_with_unknown.front().content.contains("cipher"));
}

TEST_CASE("content parsing includes shared semantic validation",
          "[session_ir]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"session","version":3,"id":"session","timestamp":"2026-07-23T00:00:00Z","cwd":"/work"}
{"type":"message","id":"a1","parentId":null,"timestamp":"2026-07-23T00:00:01Z","message":{"role":"assistant","content":[{"type":"toolCall","id":"","name":"read","arguments":{}}]}})",
      lupe::LogFormat::Pi);

  REQUIRE(std::ranges::any_of(
      parsed.diagnostics, [](const lupe::Diagnostic &diagnostic) {
        return diagnostic.code == lupe::DiagnosticCode::EmptyCallId;
      }));
}

TEST_CASE("usage after a hidden event stays attached to the visible turn",
          "[session_ir]") {
  lupe::RecordIR message_record{
      .sequence = 0,
      .source_line = 1,
      .native_type = "response_item",
  };
  lupe::append_event(
      message_record,
      lupe::MessageEvent{
          .role = lupe::Role::Assistant,
          .raw_role = "assistant",
          .content = {lupe::TextContent{.text = "visible answer"}},
      });

  lupe::RecordIR usage_record{
      .sequence = 1,
      .source_line = 2,
      .native_type = "event_msg",
  };
  lupe::append_event(usage_record, lupe::ReasoningEvent{
                                        .summary = "hidden plan",
                                        .content = "hidden details",
                                    });
  lupe::append_event(usage_record, lupe::UsageEvent{
                                        .scope = lupe::UsageScope::Turn,
                                        .input_tokens = 12,
                                        .output_tokens = 4,
                                    });

  const lupe::SessionIR session{
      .format = lupe::LogFormat::Codex,
      .records = {std::move(message_record), std::move(usage_record)},
  };

  const auto messages = lupe::make_display_messages(session);

  REQUIRE(messages.size() == 1);
  REQUIRE(messages.front().content == "visible answer");
  REQUIRE(messages.front().annotations.size() == 1);
  REQUIRE(messages.front().annotations.front().contains("input=12"));
}
