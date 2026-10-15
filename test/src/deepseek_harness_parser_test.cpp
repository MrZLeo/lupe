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

const lupe::LogMessage *
message_with_content(const std::vector<lupe::LogMessage> &messages,
                     std::string_view content) {
  const auto message = std::ranges::find_if(
      messages, [content](const lupe::LogMessage &candidate) {
        return candidate.content == content;
      });
  return message == messages.end() ? nullptr : &*message;
}

bool
has_diagnostic(const lupe::SessionParseResult &parsed,
               lupe::DiagnosticCode code, lupe::DiagnosticSeverity severity) {
  return std::ranges::any_of(parsed.diagnostics,
                             [code, severity](const lupe::Diagnostic &d) {
                               return d.code == code && d.severity == severity;
                             });
}

const std::string kConversation =
    R"({"type":"session","version":0,"id":"dsh-session","createdAt":1786700000000,"cwd":"/repo","delegationDepth":0})"
    "\n"
    R"({"type":"turn/start","seq":0,"time":1786700001000,"data":{"turn":1}})"
    "\n"
    R"({"type":"step/start","seq":1,"time":1786700001010,"data":{"turn":1,"step":1}})"
    "\n"
    R"({"type":"user/message","seq":2,"time":1786700001020,"data":{"id":"m-u1","role":"user","content":[{"type":"text","text":"run the tests"}],"source":{"kind":"user"}},"surfaceOp":"append"})"
    "\n"
    R"({"type":"reasoning-chunks","seq0":3,"time0":1786700001100,"data":{"turn":1,"step":1,"index":0,"dt":[50,50],"texts":["I"," will"," check"]}})"
    "\n"
    R"({"type":"assistant/message","seq":6,"time":1786700002000,"data":{"turn":1,"step":1,"message":{"id":"m-a1","role":"assistant","content":[{"type":"reasoning","text":"I will check"},{"type":"tool-call","id":"call-1","name":"bash","arguments":"{\"cmd\":\"pnpm test\"}"}],"source":{"kind":"model","provider":"deepseek-official","model":"deepseek-chat"}},"usage":{"inputTokens":120,"outputTokens":31,"cacheReadTokens":8,"reasoningTokens":12}},"sourceEventSeqs":[3,4,5],"surfaceOp":"append"})"
    "\n"
    R"({"type":"tool/call","seq":7,"time":1786700002010,"data":{"turn":1,"step":1,"callId":"call-1","name":"bash","arguments":"{\"cmd\":\"pnpm test\"}"}})"
    "\n"
    R"({"type":"tool/result","seq":8,"time":1786700003000,"data":{"turn":1,"step":1,"message":{"id":"m-t1","role":"user","content":[{"type":"tool-result","toolCallId":"call-1","content":[{"type":"text","text":"tests passed"}]}],"source":{"kind":"tool","callId":"call-1"}}},"surfaceOp":"append"})"
    "\n"
    R"({"type":"step/end","seq":9,"time":1786700003010,"data":{"turn":1,"step":1}})"
    "\n"
    R"({"type":"turn/end","seq":10,"time":1786700003020,"data":{"turn":1,"reason":{"kind":"completed"}}})"
    "\n";

TEST_CASE("deepseek-harness parsing maps header, messages, tools, and usage",
          "[session_parser][deepseek-harness]") {
  const auto parsed = lupe::parse_session_content(
      kConversation, lupe::LogFormat::DeepseekHarness);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.format == lupe::LogFormat::DeepseekHarness);
  REQUIRE(parsed.session.session_id == "dsh-session");
  REQUIRE(parsed.session.source_version == "0");
  REQUIRE(parsed.session.cwd == "/repo");
  REQUIRE(parsed.session.created_at == "2026-08-14T09:33:20.000Z");

  // 10 physical rows, with the packed reasoning-chunks row expanded into
  // three decoded assistant/chunk records.
  REQUIRE(parsed.session.records.size() == 12);

  const auto &header = parsed.session.records[0];
  REQUIRE(header.native_type == "session");
  REQUIRE(header.timestamp == "2026-08-14T09:33:20.000Z");

  const auto &user_record = parsed.session.records[3];
  REQUIRE(user_record.native_type == "user/message");
  REQUIRE(user_record.native_sequence == 2);
  REQUIRE(user_record.timestamp == "2026-08-14T09:33:21.020Z");
  const auto *user_message = first_event<lupe::MessageEvent>(user_record);
  REQUIRE(user_message != nullptr);
  REQUIRE(user_message->role == lupe::Role::User);
  REQUIRE(std::get<lupe::TextContent>(user_message->content.front()).text
          == "run the tests");

  // Packed deltas decode to individual chunk records with reconstructed
  // sequence numbers and timestamps.
  for (std::size_t index = 4; index <= 6; ++index) {
    const auto &chunk = parsed.session.records[index];
    REQUIRE(chunk.native_type == "assistant/chunk");
    REQUIRE(chunk.native_sequence == index - 1);
    const auto *execution = first_event<lupe::ExecutionEvent>(chunk);
    REQUIRE(execution != nullptr);
    REQUIRE(execution->subject == lupe::ExecutionSubject::Stream);
    REQUIRE(execution->phase == lupe::ExecutionPhase::Updated);
    REQUIRE(execution->status == "reasoning-delta");
  }
  REQUIRE(parsed.session.records[4].timestamp == "2026-08-14T09:33:21.100Z");
  REQUIRE(parsed.session.records[5].timestamp == "2026-08-14T09:33:21.150Z");
  REQUIRE(parsed.session.records[6].timestamp == "2026-08-14T09:33:21.200Z");
  REQUIRE(parsed.session.records[4].raw_json.contains("\"text\":\"I\""));
  REQUIRE(parsed.session.records[5].raw_json.contains("\"text\":\" will\""));
  REQUIRE(parsed.session.records[6].raw_json.contains("\"text\":\" check\""));

  const auto &assistant_record = parsed.session.records[7];
  const auto *assistant_message =
      first_event<lupe::MessageEvent>(assistant_record);
  REQUIRE(assistant_message != nullptr);
  REQUIRE(assistant_message->role == lupe::Role::Assistant);
  REQUIRE(assistant_message->provider == "deepseek-official");
  REQUIRE(assistant_message->model == "deepseek-chat");

  const auto *reasoning = first_event<lupe::ReasoningEvent>(assistant_record);
  REQUIRE(reasoning != nullptr);
  REQUIRE(reasoning->content == "I will check");

  const auto *call = first_event<lupe::ToolCallEvent>(assistant_record);
  REQUIRE(call != nullptr);
  REQUIRE(call->call_id == "call-1");
  REQUIRE(call->name == "bash");
  REQUIRE(call->input_is_json);
  REQUIRE(call->input.contains("pnpm test"));

  const auto *usage = first_event<lupe::UsageEvent>(assistant_record);
  REQUIRE(usage != nullptr);
  REQUIRE(usage->input_tokens == 120);
  REQUIRE(usage->output_tokens == 31);
  REQUIRE(usage->cached_input_tokens == 8);
  REQUIRE(usage->reasoning_tokens == 12);

  // tool/call is an audit copy: kept as metadata, not a duplicate call.
  const auto &audit_record = parsed.session.records[8];
  REQUIRE(event_count<lupe::MetadataEvent>(audit_record) == 1);
  REQUIRE(event_count<lupe::ToolCallEvent>(audit_record) == 0);

  const auto &result_record = parsed.session.records[9];
  const auto *tool_result = first_event<lupe::ToolResultEvent>(result_record);
  REQUIRE(tool_result != nullptr);
  REQUIRE(tool_result->call_id == "call-1");
  REQUIRE(tool_result->name == "bash");
  REQUIRE_FALSE(tool_result->is_error);
  REQUIRE(std::get<lupe::TextContent>(tool_result->output.front()).text
          == "tests passed");

  const auto &turn_end = parsed.session.records[11];
  const auto *execution = first_event<lupe::ExecutionEvent>(turn_end);
  REQUIRE(execution != nullptr);
  REQUIRE(execution->subject == lupe::ExecutionSubject::Turn);
  REQUIRE(execution->phase == lupe::ExecutionPhase::Completed);

  const auto displayed = lupe::make_display_messages(
      parsed.session, lupe::DisplayOptions{.show_reasoning = true});
  REQUIRE(message_with_content(displayed, "run the tests") != nullptr);
  REQUIRE(message_with_content(displayed, "I will check") != nullptr);
  REQUIRE(message_with_content(displayed, "tests passed") != nullptr);
  // Streaming chunks stay off the conversation timeline by default.
  REQUIRE(message_with_content(displayed, "I will check") != nullptr);
  REQUIRE(std::ranges::none_of(displayed, [](const lupe::LogMessage &m) {
    return m.raw_type == "assistant/chunk";
  }));
}

TEST_CASE("deepseek-harness usage chunks flush at step end without a message",
          "[session_parser][deepseek-harness]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"session","version":0,"id":"dsh-usage","createdAt":1786700000000,"cwd":"/repo","delegationDepth":0}
{"type":"step/start","seq":0,"time":1786700001000,"data":{"turn":1,"step":1}}
{"type":"assistant/chunk","seq":1,"time":1786700001100,"data":{"turn":1,"step":1,"chunk":{"type":"usage","usage":{"inputTokens":10,"outputTokens":2}}}}
{"type":"step/end","seq":2,"time":1786700001200,"data":{"turn":1,"step":1}})",
      lupe::LogFormat::DeepseekHarness);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.records.size() == 4);

  // The usage chunk itself carries no event while it is pending.
  REQUIRE(parsed.session.records[2].events.empty());

  const auto &step_end = parsed.session.records[3];
  const auto *usage = first_event<lupe::UsageEvent>(step_end);
  REQUIRE(usage != nullptr);
  REQUIRE(usage->input_tokens == 10);
  REQUIRE(usage->output_tokens == 2);
}

TEST_CASE("deepseek-harness orphan usage survives a torn tail",
          "[session_parser][deepseek-harness]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"session","version":0,"id":"dsh-torn","createdAt":1786700000000,"cwd":"/repo","delegationDepth":0}
{"type":"assistant/chunk","seq":0,"time":1786700001000,"data":{"turn":1,"step":1,"chunk":{"type":"usage","usage":{"inputTokens":5,"outputTokens":1}}}}
{"type":"assistant/chunk","seq":1,"time":1786700001)",
      lupe::LogFormat::DeepseekHarness);

  REQUIRE(parsed.session.records.size() == 3);
  REQUIRE(has_diagnostic(parsed, lupe::DiagnosticCode::InvalidJson,
                         lupe::DiagnosticSeverity::Error));
  // The orphan usage is attached to the last intact record, not dropped.
  REQUIRE(event_count<lupe::UsageEvent>(parsed.session.records[1]) == 1);
}

TEST_CASE("deepseek-harness compaction and unknown events are loss-aware",
          "[session_parser][deepseek-harness]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"session","version":0,"id":"dsh-misc","createdAt":1786700000000,"cwd":"/repo","delegationDepth":0}
{"type":"compaction/summary","seq":0,"time":1786700001000,"data":{"compactionId":"c-1","turn":1,"summary":[{"type":"text","text":"earlier context"}],"shadowedRange":{"start":2,"end":6},"shadowedSeqs":[2,4,6],"shadowedTokenCount":4096}}
{"type":"plugin/future","seq":1,"time":1786700001100,"data":{"x":1},"ignorable":true}
{"type":"plugin/required","seq":2,"time":1786700001200,"data":{"x":2}}
{"type":"user/message","seq":9,"time":1786700001300,"data":{"id":"m-1","role":"user","content":[{"type":"text","text":"hi"}],"source":{"kind":"user"}},"surfaceOp":"append"})",
      lupe::LogFormat::DeepseekHarness);

  REQUIRE_FALSE(parsed.has_fatal_error());

  const auto &compaction = parsed.session.records[1];
  const auto *event = first_event<lupe::CompactionEvent>(compaction);
  REQUIRE(event != nullptr);
  REQUIRE(event->summary == "earlier context");
  REQUIRE(event->tokens_before == 4096);
  REQUIRE(event->replacement_context_json.contains("\"shadowedSeqs\":[2,4,6]"));

  // Unknown ignorable events parse quietly; unknown required events are an
  // error per the format's reconstruction contract.
  REQUIRE(event_count<lupe::UnknownEvent>(parsed.session.records[2]) == 1);
  REQUIRE(event_count<lupe::UnknownEvent>(parsed.session.records[3]) == 1);
  REQUIRE(has_diagnostic(parsed, lupe::DiagnosticCode::FormatMismatch,
                         lupe::DiagnosticSeverity::Error));

  // The seq jump from 2 to 9 is reported but parsing continues.
  REQUIRE(has_diagnostic(parsed, lupe::DiagnosticCode::InvalidLifecycle,
                         lupe::DiagnosticSeverity::Warning));
}

TEST_CASE("deepseek-harness requires a v0 session header",
          "[session_parser][deepseek-harness]") {
  const auto no_header = lupe::parse_session_content(
      R"({"type":"user/message","seq":0,"time":1786700001000,"data":{"id":"m-1","role":"user","content":[],"source":{"kind":"user"}}})",
      lupe::LogFormat::DeepseekHarness);
  REQUIRE(no_header.has_fatal_error());
  REQUIRE(has_diagnostic(no_header, lupe::DiagnosticCode::FormatMismatch,
                         lupe::DiagnosticSeverity::Fatal));

  const auto future_version = lupe::parse_session_content(
      R"({"type":"session","version":1,"id":"dsh-future","createdAt":1786700000000,"cwd":"/repo","delegationDepth":0}
{"type":"turn/start","seq":0,"time":1786700001000,"data":{"turn":1}})",
      lupe::LogFormat::DeepseekHarness);
  REQUIRE_FALSE(future_version.has_fatal_error());
  REQUIRE(future_version.session.source_version == "1");
  REQUIRE(has_diagnostic(future_version,
                         lupe::DiagnosticCode::UnsupportedVersion,
                         lupe::DiagnosticSeverity::Warning));
}

TEST_CASE("deepseek-harness packed tool-call chunks decode call deltas",
          "[session_parser][deepseek-harness]") {
  const auto parsed = lupe::parse_session_content(
      R"({"type":"session","version":0,"id":"dsh-pack","createdAt":1786700000000,"cwd":"/repo","delegationDepth":0}
{"type":"tool-call-chunks","seq0":0,"time0":1786700001000,"data":{"turn":1,"step":1,"index":0,"id":"call-9","name":"bash","dt":[10],"args":["{\"cmd\"",":\"ls\"}"]}}
{"type":"tool/result","seq":2,"time":1786700002000,"data":{"turn":1,"step":1,"message":{"id":"m-t1","role":"user","content":[{"type":"tool-result","toolCallId":"call-9","content":[{"type":"text","text":"ok"}]}],"source":{"kind":"tool","callId":"call-9"}}},"surfaceOp":"append"})",
      lupe::LogFormat::DeepseekHarness);

  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.session.records.size() == 4);

  const auto &first = parsed.session.records[1];
  REQUIRE(first.native_sequence == 0);
  REQUIRE(first.timestamp == "2026-08-14T09:33:21.000Z");
  REQUIRE(first.raw_json.contains("\"tool-call-delta\""));
  REQUIRE(first.raw_json.contains("\"id\":\"call-9\""));
  REQUIRE(first.raw_json.contains("\"name\":\"bash\""));
  REQUIRE(first.raw_json.contains("\"argumentsDelta\":\"{\\\"cmd\\\"\""));

  const auto &second = parsed.session.records[2];
  REQUIRE(second.native_sequence == 1);
  REQUIRE(second.timestamp == "2026-08-14T09:33:21.010Z");
  // Only the first delta of a run carries the tool name.
  REQUIRE(second.raw_json.contains("\"id\":\"call-9\""));
  REQUIRE_FALSE(second.raw_json.contains("\"name\""));

  // The packed call registered the name for the later result.
  const auto *tool_result =
      first_event<lupe::ToolResultEvent>(parsed.session.records[3]);
  REQUIRE(tool_result != nullptr);
  REQUIRE(tool_result->name == "bash");
}

} // namespace
