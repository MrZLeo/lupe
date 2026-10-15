#include "lupe/format_detector.hpp"

#include "lupe/log_format.hpp"
#include "lupe/session_parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

const std::string kPiSession =
    R"({"type":"session","version":3,"id":"s-1","cwd":"/tmp/demo","timestamp":"2026-07-27T00:00:00Z"})"
    "\n"
    R"({"type":"message","id":"m-1","parentId":null,"timestamp":"2026-07-27T00:00:01Z","message":{"role":"user","content":[{"type":"text","text":"hi"}]}})"
    "\n";

const std::string kCodexRollout =
    R"({"timestamp":"2026-07-27T00:00:00Z","type":"session_meta","payload":{"id":"r-1","cwd":"/tmp/demo"}})"
    "\n"
    R"({"timestamp":"2026-07-27T00:00:01Z","type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"hi"}]}})"
    "\n";

const std::string kCodexExecStream =
    R"({"type":"thread.started","thread_id":"t-1"})"
    "\n"
    R"({"type":"turn.started"})"
    "\n"
    R"({"type":"item.completed","item":{"id":"item_0","type":"agent_message","text":"hi"}})"
    "\n";

const std::string kClaudeCodeTranscript =
    R"({"type":"user","sessionId":"c-1","uuid":"u-1","cwd":"/tmp/demo","message":{"role":"user","content":"hi"}})"
    "\n"
    R"({"type":"assistant","sessionId":"c-1","uuid":"u-2","parentUuid":"u-1","message":{"role":"assistant","content":[{"type":"text","text":"hello"}]}})"
    "\n";

const std::string kDeepseekHarnessSession =
    R"({"type":"session","version":0,"id":"d-1","createdAt":1786700000000,"cwd":"/tmp/demo","delegationDepth":0})"
    "\n"
    R"({"type":"user/message","seq":0,"time":1786700001000,"data":{"id":"m-1","role":"user","content":[{"type":"text","text":"hi"}],"source":{"kind":"user"}},"surfaceOp":"append"})"
    "\n";

TEST_CASE("detector recognizes each native format", "[format]") {
  REQUIRE(lupe::detect_log_format(kPiSession) == lupe::LogFormat::Pi);
  REQUIRE(lupe::detect_log_format(kCodexRollout) == lupe::LogFormat::Codex);
  REQUIRE(lupe::detect_log_format(kCodexExecStream)
          == lupe::LogFormat::CodexExec);
  REQUIRE(lupe::detect_log_format(kClaudeCodeTranscript)
          == lupe::LogFormat::ClaudeCode);
  REQUIRE(lupe::detect_log_format(kDeepseekHarnessSession)
          == lupe::LogFormat::DeepseekHarness);
}

TEST_CASE("detector anchors on the first record", "[format]") {
  // A codex rollout without session_meta still votes codex from
  // response_item lines.
  const std::string no_meta =
      R"({"timestamp":"2026-07-27T00:00:01Z","type":"response_item","payload":{"type":"message"}})"
      "\n";
  REQUIRE(lupe::detect_log_format(no_meta) == lupe::LogFormat::Codex);

  // Claude Code transcripts may open with a metadata record.
  const std::string metadata_first =
      R"({"type":"file-history-snapshot","messageId":"u-1","snapshot":{}})"
      "\n"
      + kClaudeCodeTranscript;
  REQUIRE(lupe::detect_log_format(metadata_first)
          == lupe::LogFormat::ClaudeCode);

  // A deepseek-harness log without its header still votes from the
  // slash-namespaced event vocabulary and the packed chunk rows.
  const std::string no_header =
      R"({"type":"reasoning-chunks","seq0":0,"time0":1786700001000,"data":{"turn":1,"step":1,"index":0,"dt":[5,5],"texts":["a","b","c"]}})"
      "\n"
      R"({"type":"assistant/message","seq":3,"time":1786700002000,"data":{"turn":1,"step":1,"message":{}}})"
      "\n";
  REQUIRE(lupe::detect_log_format(no_header)
          == lupe::LogFormat::DeepseekHarness);
}

TEST_CASE("detector skips malformed lines", "[format]") {
  const std::string damaged = "{not json\n"
                              "\n"
                              "   \n"
                            + kCodexExecStream;
  REQUIRE(lupe::detect_log_format(damaged) == lupe::LogFormat::CodexExec);
}

TEST_CASE("detector uses structure when the type is foreign", "[format]") {
  // Unknown `type` values still carry the Claude Code envelope.
  const std::string typed_elsewhere =
      R"({"type":"mystery","sessionId":"c-1","uuid":"u-1","message":{}})"
      "\n"
      R"({"type":"mystery","sessionId":"c-1","uuid":"u-2","parentUuid":"u-1"})"
      "\n";
  REQUIRE(lupe::detect_log_format(typed_elsewhere)
          == lupe::LogFormat::ClaudeCode);
}

TEST_CASE("detector refuses ambiguous or unknown content", "[format]") {
  REQUIRE_FALSE(lupe::detect_log_format("").has_value());
  REQUIRE_FALSE(
      lupe::detect_log_format("{\"foo\":1}\n{\"bar\":2}\n").has_value());
  // Legacy generic JSONL stays explicit: the lossy path is never guessed.
  REQUIRE_FALSE(lupe::detect_log_format(R"({"role":"user","content":"hi"})"
                                         "\n")
                    .has_value());
  // A lone weak vocabulary hit is not decisive.
  REQUIRE_FALSE(
      lupe::detect_log_format("{\"type\":\"message\"}\n").has_value());
  // A foreign later record does not override the first-record anchor.
  const std::string mixed = kCodexExecStream + kPiSession;
  REQUIRE(lupe::detect_log_format(mixed) == lupe::LogFormat::CodexExec);

  // A deepseek-harness header is not a Pi header despite sharing the
  // `session` type: createdAt/delegationDepth discriminate it.
  REQUIRE(lupe::detect_log_format(kDeepseekHarnessSession + kPiSession)
          == lupe::LogFormat::DeepseekHarness);
  REQUIRE(lupe::detect_log_format(kPiSession + kDeepseekHarnessSession)
          == lupe::LogFormat::Pi);
}

TEST_CASE("auto format parses through the session entry point", "[format]") {
  auto parsed =
      lupe::parse_session_content(kCodexExecStream, lupe::LogFormat::Auto);
  REQUIRE(parsed.session.format == lupe::LogFormat::CodexExec);
  REQUIRE_FALSE(parsed.has_fatal_error());
}

TEST_CASE("undetectable content is fatal with guidance", "[format]") {
  auto parsed =
      lupe::parse_session_content("{\"foo\":1}\n", lupe::LogFormat::Auto);
  REQUIRE(parsed.has_fatal_error());
  REQUIRE(parsed.session.format == lupe::LogFormat::Auto);
  bool mentions_format_flag = false;
  for (const auto &diagnostic : parsed.diagnostics) {
    if (diagnostic.message.contains("--format")) {
      mentions_format_flag = true;
    }
  }
  REQUIRE(mentions_format_flag);
}

} // namespace
