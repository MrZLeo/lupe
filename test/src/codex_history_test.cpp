#include "lupe/message_projection.hpp"
#include "lupe/session_parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace {

struct TempHistory {
  std::filesystem::path root;

  TempHistory() {
    static std::atomic<unsigned> next{0};
    root = std::filesystem::temp_directory_path()
         / ("lupe-codex-history-"
            + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())
            + "-"
            + std::to_string(next++));
    std::filesystem::create_directories(root);
  }
  TempHistory(const TempHistory &) = delete;
  TempHistory &operator=(const TempHistory &) = delete;
  TempHistory(TempHistory &&) = delete;
  TempHistory &operator=(TempHistory &&) = delete;

  ~TempHistory() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  std::filesystem::path
  write(std::string_view relative, std::string_view content) const {
    const auto path = root / relative;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary};
    output << content;
    output.close();
    REQUIRE(output.good());
    return path;
  }
};

std::string
reference(std::string_view id, std::uint64_t end, std::size_t bytes) {
  return R"({"thread_id":")"
       + std::string{id}
       + R"(","end_ordinal_exclusive":)"
       + std::to_string(end)
       + R"(,"end_byte_offset":)"
       + std::to_string(bytes)
       + "}";
}

std::string meta(std::string_view id, std::uint64_t ordinal = 0,
                 std::string_view base = {}) {
  return R"({"type":"session_meta","ordinal":)"
       + std::to_string(ordinal)
       + R"(,"payload":{"id":")"
       + std::string{id}
       + R"(","cwd":"/work","history_mode":"paginated")"
       + (base.empty() ? "" : R"(,"history_base":)" + std::string{base})
       + "}}\n";
}

std::string message(std::uint64_t ordinal, std::string_view text) {
  return R"({"type":"event_msg","ordinal":)"
       + std::to_string(ordinal)
       + R"(,"payload":{"type":"item_completed","turn_id":"turn-1","item":{"type":"AgentMessage","id":"m-)"
       + std::to_string(ordinal)
       + R"(","content":[{"type":"Text","text":")"
       + std::string{text}
       + R"("}]}}})"
       + "\n";
}

std::vector<std::string> texts(const lupe::SessionParseResult &parsed) {
  std::vector<std::string> values;
  for (const auto &display : lupe::make_display_messages(parsed.session)) {
    values.push_back(display.content);
  }
  return values;
}

bool incomplete(const lupe::SessionParseResult &parsed) {
  return std::ranges::any_of(parsed.diagnostics, [](const auto &diagnostic) {
    return diagnostic.code == lupe::DiagnosticCode::IncompleteStream;
  });
}

} // namespace

TEST_CASE(
    "Codex history follows bounded multi-file prefixes across managed roots",
    "[codex_history]") {
  const TempHistory fixture;
  const auto first = meta("logical-parent") + message(1, "继承的消息");
  const auto parent_path =
      fixture.write("archived_sessions/rollout-2026-parent.jsonl",
                    first + message(2, "excluded parent tail"));
  const auto second =
      meta("logical-fork", 2, reference("parent", 2, first.size()))
      + message(3, "fork message");
  const auto fork_path =
      fixture.write("sessions/2026/09/28/rollout-2026-fork.jsonl",
                    second + message(4, "excluded fork tail"));
  const auto leaf = meta("logical-leaf", 4, reference("fork", 4, second.size()))
                  + message(5, "leaf message");
  const auto path =
      fixture.write("sessions/2026/10/15/rollout-2026-leaf.jsonl", leaf);

  const auto parsed = lupe::parse_session_file(path, lupe::LogFormat::Auto);
  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(parsed.diagnostics.empty());
  REQUIRE(parsed.session.session_id == "logical-leaf");
  REQUIRE(parsed.session.source_path == path);
  REQUIRE(texts(parsed)
          == std::vector<std::string>{"继承的消息", "fork message",
                                      "leaf message"});
  REQUIRE(parsed.session.records.size() == 6);
  for (std::size_t index = 0; index < parsed.session.records.size(); ++index) {
    CHECK(parsed.session.records.at(index).sequence == index);
    CHECK(parsed.session.records.at(index).native_sequence == index);
    CHECK(parsed.session.records.at(index).source_line == (index % 2) + 1);
  }
  CHECK(parsed.session.records.at(1).source_path == parent_path);
  CHECK(parsed.session.records.at(3).source_path == fork_path);
  CHECK(parsed.session.records.at(5).source_path == path);
}

TEST_CASE("Codex inherited prefixes carry model and tool context into the leaf",
          "[codex_history]") {
  const TempHistory fixture;
  const auto parent =
      meta("parent")
      + R"({"ordinal":1,"type":"turn_context","payload":{"turn_id":"t1","model":"gpt-5"}})"
        "\n"
      + R"({"ordinal":2,"type":"response_item","payload":{"type":"function_call","call_id":"call1","name":"read","arguments":"{}"}})"
        "\n";
  fixture.write("rollout-parent.jsonl", parent);
  const auto leaf =
      meta("child", 3, reference("parent", 3, parent.size()))
      + R"({"ordinal":4,"type":"response_item","payload":{"type":"function_call_output","call_id":"call1","output":"contents"}})"
        "\n"
      + message(5, "done");
  const auto path = fixture.write("rollout-child.jsonl", leaf);
  const auto parsed = lupe::parse_session_file(path, lupe::LogFormat::Auto);
  REQUIRE(parsed.diagnostics.empty());
  REQUIRE(parsed.session.session_id == "child");
  const auto &output = std::get<lupe::ToolResultEvent>(
      parsed.session.records.at(4).events.front().payload);
  CHECK(output.name == "read");
  const auto &answer = std::get<lupe::MessageEvent>(
      parsed.session.records.at(5).events.front().payload);
  CHECK(answer.model == "gpt-5");
}

TEST_CASE("Codex history resolves rollout IDs rather than logical thread IDs",
          "[codex_history]") {
  const TempHistory fixture;
  const auto parent = meta("stable-thread") + message(1, "before revert");
  fixture.write("rollout-2026-stable-thread_old-physical-id.jsonl", parent);
  const auto child =
      meta("stable-thread", 2, reference("old-physical-id", 2, parent.size()))
      + message(3, "after revert");
  const auto path =
      fixture.write("rollout-2026-stable-thread_new-physical-id.jsonl", child);
  const auto parsed = lupe::parse_session_file(path, lupe::LogFormat::Codex);
  REQUIRE(parsed.diagnostics.empty());
  REQUIRE(parsed.session.session_id == "stable-thread");
  REQUIRE(texts(parsed)
          == std::vector<std::string>{"before revert", "after revert"});
}

TEST_CASE("Codex reads compressed history and prefers a plain sibling",
          "[codex_history]") {
  // Node zlib.zstdCompressSync(meta("parent") + message(1, "parent")),
  // 277 decoded bytes. Offsets in history_base refer to decoded JSONL bytes.
  static constexpr char compressed[] =
      "\x28\xb5\x2f\xfd\x60\x15\x00\x85\x05\x00\xb2\x89\x1e\x1d\x60\x67"
      "\xab\x03\x06\xaf\xaf\x55\x78\x9b\x08\xdd\x77\x57\x5a\xea\x91\x15"
      "\x47\x20\x9e\x68\x1f\x3d\x21\x06\x61\x07\xf3\x45\x8c\x11\x20\x6e"
      "\xc1\x11\x89\xb3\xa9\x47\xfb\x4a\x10\xfe\xb0\xf9\x65\x30\x36\x06"
      "\x28\xdf\xc0\xda\x1c\x71\x4d\x0f\xd4\xbe\xcc\x58\xb0\xa1\xcd\x13"
      "\xbc\x88\xd1\xea\x9b\x92\xad\x2b\x6d\x48\x79\xba\x90\x05\xd8\x58"
      "\xe0\x69\x42\x48\x0e\x1b\x6f\xfe\x25\xe7\x63\xc5\xbc\x1c\xab\xa4"
      "\x88\x92\x9c\x4d\xe1\x18\x49\xc9\xd4\x53\x1b\xa7\x7d\x43\xa9\x0c"
      "\xc2\x43\x1f\xdf\x29\xeb\x25\x14\x00\x57\x53\xa0\x8a\x7b\x34\x61"
      "\x40\x2d\x7c\x33\x41\x4c\xef\x42\xc6\x42\x50\x36\xad\x26\x68\x8c"
      "\x81\xa3\x9a\x5a\x0b\x10\x4c\x0c\x12\xc1\x29\x26\x0b\x38\xee\x19"
      "\xfc\xa8\xa1\xbc\xa4\x8e\xbd\x58\x13\x0a";
  const TempHistory fixture;
  const auto parent = meta("parent") + message(1, "parent");
  REQUIRE(parent.size() == 277);
  SECTION("compressed source") {
    fixture.write("rollout-parent.jsonl.zst",
                  std::string_view{compressed, sizeof(compressed) - 1});
  }
  SECTION("plain sibling wins even if compressed sibling is corrupt") {
    fixture.write("rollout-parent.jsonl.zst", "broken compressed file");
    fixture.write("rollout-parent.jsonl", parent);
  }
  const auto path =
      fixture.write("rollout-child.jsonl",
                    meta("child", 2, reference("parent", 2, parent.size()))
                        + message(3, "local"));
  const auto parsed = lupe::parse_session_file(path, lupe::LogFormat::Auto);
  REQUIRE(parsed.diagnostics.empty());
  REQUIRE(texts(parsed) == std::vector<std::string>{"parent", "local"});
}

TEST_CASE("content-only Codex parsing reports unresolved inherited history",
          "[codex_history]") {
  const auto parsed = lupe::parse_session_content(
      meta("child", 2, reference("parent", 2, 123)) + message(3, "local"),
      lupe::LogFormat::Auto);
  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(incomplete(parsed));
  REQUIRE(texts(parsed) == std::vector<std::string>{"local"});
}

TEST_CASE("Codex missing history preserves the requested file with a warning",
          "[codex_history]") {
  const TempHistory fixture;
  const auto child =
      meta("child", 2, reference("missing", 2, 123)) + message(3, "local");
  const auto path = fixture.write("rollout-child.jsonl", child);
  const auto parsed = lupe::parse_session_file(path, lupe::LogFormat::Auto);
  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(incomplete(parsed));
  REQUIRE(parsed.session.session_id == "child");
  REQUIRE(texts(parsed) == std::vector<std::string>{"local"});
}

TEST_CASE("Codex rejects invalid byte and ordinal history boundaries",
          "[codex_history]") {
  const TempHistory fixture;
  const auto parent = meta("parent") + message(1, "parent");
  fixture.write("rollout-parent.jsonl", parent);
  std::size_t bytes = parent.size();
  std::uint64_t end = 2;
  SECTION("byte offset inside a record") { --bytes; }
  SECTION("byte offset past EOF") { ++bytes; }
  SECTION("ordinal extends beyond the prefix") { end = 3; }
  SECTION("ordinal excludes a record included by the byte offset") { end = 1; }
  SECTION("resource limit") { bytes = std::size_t{513} * 1024 * 1024; }
  const auto path = fixture.write(
      "rollout-child.jsonl", meta("child", end, reference("parent", end, bytes))
                                 + message(end + 1, "local"));
  const auto parsed = lupe::parse_session_file(path, lupe::LogFormat::Codex);
  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(incomplete(parsed));
  REQUIRE(texts(parsed) == std::vector<std::string>{"local"});
}

TEST_CASE("Codex refuses cycles ambiguous references and unsafe IDs",
          "[codex_history]") {
  const TempHistory fixture;
  const auto parent = meta("parent") + message(1, "parent");
  fixture.write("rollout-parent.jsonl", parent);
  std::string id = "parent";
  SECTION("self cycle") { id = "child"; }
  SECTION("duplicate physical ID") {
    fixture.write("rollout-copy-parent.jsonl", parent);
  }
  SECTION("path traversal") { id = "../parent"; }
  SECTION("symlinked source") {
    std::error_code error;
    std::filesystem::create_symlink(fixture.root / "rollout-parent.jsonl",
                                    fixture.root / "rollout-linked.jsonl",
                                    error);
    if (error) {
      SKIP("symlinks unavailable on this platform");
    }
    id = "linked";
  }
  const auto path = fixture.write(
      "rollout-child.jsonl",
      meta("child", 2, reference(id, 2, parent.size())) + message(3, "local"));
  const auto parsed = lupe::parse_session_file(path, lupe::LogFormat::Codex);
  REQUIRE_FALSE(parsed.has_fatal_error());
  REQUIRE(incomplete(parsed));
  REQUIRE(texts(parsed) == std::vector<std::string>{"local"});
}

TEST_CASE("Codex ignores data beyond the inherited cutoff but rejects broken "
          "prefixes",
          "[codex_history]") {
  const TempHistory fixture;
  auto parent = meta("parent") + message(1, "parent");
  bool broken = false;
  SECTION("invalid suffix is not read") {}
  SECTION("non-paginated source") {
    parent.replace(parent.find("paginated"), 9, "legacy");
    broken = true;
  }
  SECTION("ordinal gap") {
    parent = meta("parent") + message(5, "parent");
    broken = true;
  }
  SECTION("broken ancestor chain") {
    parent =
        meta("parent", 2, reference("missing", 2, 123)) + message(3, "parent");
    broken = true;
  }
  fixture.write("rollout-parent.jsonl", parent + "{broken tail\n");
  const auto end = parent.contains("missing") ? 4U : 2U;
  const auto path =
      fixture.write("rollout-child.jsonl",
                    meta("child", end, reference("parent", end, parent.size()))
                        + message(end + 1, "local"));
  const auto parsed = lupe::parse_session_file(path, lupe::LogFormat::Codex);
  REQUIRE_FALSE(parsed.has_fatal_error());
  if (broken) {
    REQUIRE(incomplete(parsed));
    REQUIRE(texts(parsed) == std::vector<std::string>{"local"});
  } else {
    REQUIRE(parsed.diagnostics.empty());
    REQUIRE(texts(parsed) == std::vector<std::string>{"parent", "local"});
  }
}
