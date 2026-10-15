#include "session_parser_internal.hpp"

#include "json_helpers.hpp"
#include "jsonl_reader.hpp"
#include "zstd_decode.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <simdjson.h>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace loupe::detail {
namespace {

constexpr std::size_t kMaxHistoryDepth = 64;
constexpr std::uint64_t kMaxHistoryBytes = std::uint64_t{512} * 1024 * 1024;

struct HistoryBase {
  std::string rollout_id;
  std::uint64_t end_ordinal{0};
  std::uint64_t end_byte_offset{0};
};

struct HistoryHeader {
  bool paginated{false};
  bool has_reference{false};
  std::optional<std::uint64_t> ordinal;
  std::optional<HistoryBase> base;
  std::string error;
};

bool safe_rollout_id(std::string_view id) {
  return !id.empty() && std::ranges::all_of(id, [](char character) {
    return (character >= 'a' && character <= 'z')
        || (character >= 'A' && character <= 'Z')
        || (character >= '0' && character <= '9')
        || character == '-';
  });
}

HistoryHeader read_header(std::string_view content) {
  HistoryHeader header;
  JsonlReader reader{content};
  JsonlLine line;
  simdjson::dom::parser parser;
  simdjson::dom::element document;
  if (!reader.next(line)
      || parser.parse(simdjson::padded_string{line.json}).get(document)
             != simdjson::SUCCESS
      || string_at(document, "/type") != "session_meta") {
    header.error = "source rollout has no valid session_meta header";
    return header;
  }
  header.paginated =
      string_at(document, "/payload/history_mode") == "paginated";
  header.ordinal = uint_at(document, "/ordinal");
  simdjson::dom::element base;
  if (!element_at(document, "/payload/history_base", base)
      || base.type() == simdjson::dom::element_type::NULL_VALUE) {
    return header;
  }
  header.has_reference = true;
  const auto id = string_at(base, "/thread_id");
  const auto end = uint_at(base, "/end_ordinal_exclusive");
  const auto offset = uint_at(base, "/end_byte_offset");
  if (!header.paginated
      || !id
      || !safe_rollout_id(*id)
      || !end
      || *end == 0
      || !offset
      || *offset == 0
      || !header.ordinal
      || *header.ordinal != *end) {
    header.error = "invalid paginated history_base or session_meta ordinal";
    return header;
  }
  header.base = HistoryBase{
      .rollout_id = *id,
      .end_ordinal = *end,
      .end_byte_offset = *offset,
  };
  return header;
}

// Only search the owning Codex home's managed roots. Exported rollouts can
// resolve siblings, but must not silently import unrelated ~/.codex history.
std::vector<std::filesystem::path>
search_roots(const std::filesystem::path &path) {
  std::error_code error;
  auto parent = std::filesystem::absolute(path, error).parent_path();
  if (error) {
    return {path.parent_path()};
  }
  for (auto current = parent; !current.empty();) {
    if (current.filename() == "sessions"
        || current.filename() == "archived_sessions") {
      return {
          current.parent_path() / "sessions",
          current.parent_path() / "archived_sessions",
      };
    }
    const auto next = current.parent_path();
    if (next == current) {
      break;
    }
    current = next;
  }
  return {parent};
}

bool is_rollout_file(const std::filesystem::path &path) {
  const auto name = path.filename().string();
  return name.starts_with("rollout-")
      && (name.ends_with(".jsonl")
          || name.ends_with(".jsonl.zst")
          || name.ends_with(".jsonl.zstd"));
}

std::vector<std::filesystem::path>
find_rollouts(const std::vector<std::filesystem::path> &roots,
              std::string &error) {
  std::vector<std::filesystem::path> paths;
  for (const auto &root : roots) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) && !ec) {
      continue;
    }
    // Do not follow nested symlinks or accept symlinked files from log input.
    std::filesystem::recursive_directory_iterator iterator{root, ec};
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && iterator != end) {
      const auto &entry = *iterator;
      if (roots.size() == 1) {
        iterator.disable_recursion_pending();
      }
      const auto status = entry.symlink_status(ec);
      if (ec) {
        break;
      }
      if (std::filesystem::is_regular_file(status)
          && is_rollout_file(entry.path())) {
        paths.push_back(entry.path());
      }
      iterator.increment(ec);
    }
    if (ec) {
      error = "cannot search " + root.string() + ": " + ec.message();
      return {};
    }
  }
  return paths;
}

std::filesystem::path plain_path(const std::filesystem::path &path) {
  return path.extension() == ".zst" || path.extension() == ".zstd"
           ? path.parent_path() / path.stem()
           : path;
}

std::optional<std::filesystem::path>
locate_rollout(const std::vector<std::filesystem::path> &paths,
               std::string_view id, std::string &error) {
  std::optional<std::filesystem::path> found;
  const std::string suffix = "-" + std::string{id};
  for (const auto &path : paths) {
    const auto plain = plain_path(path);
    const std::string stem = plain.stem().string();
    // Reverts use rollout-<timestamp>-<thread_id>_<rollout_id>.jsonl.
    // Do not match the stable thread ID in place of the physical rollout ID.
    const auto separator = stem.rfind('_');
    const bool matches = separator == std::string::npos
                           ? stem.ends_with(suffix)
                           : std::string_view{stem}.substr(separator + 1) == id;
    if (!matches) {
      continue;
    }
    if (found) {
      if (plain_path(*found) != plain) {
        error = "ambiguous source rollout ID " + std::string{id};
        return std::nullopt;
      }
      // Codex prefers the plain sibling during compression/materialization.
      if (path == plain) {
        found = path;
      }
    } else {
      found = path;
    }
  }
  if (!found) {
    error = "missing source rollout " + std::string{id};
  }
  return found;
}

std::optional<std::string>
read_prefix(const std::filesystem::path &path, const HistoryBase &base,
            std::uint64_t remaining_bytes, std::string &error) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  std::ifstream input{path, std::ios::binary};
  if (ec || !input) {
    error = "cannot read source rollout " + path.string();
    return std::nullopt;
  }
  std::array<char, 4> magic{};
  input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
  const bool compressed =
      looks_like_zstd(std::string_view{magic.data(), magic.size()});
  input.clear();
  input.seekg(0);
  const auto bytes = compressed ? size : base.end_byte_offset;
  if (base.end_byte_offset > remaining_bytes || bytes > kMaxHistoryBytes) {
    error = "inherited history exceeds the 512 MiB read limit";
    return std::nullopt;
  }
  if (bytes > size) {
    error = "history_base byte offset is past the source rollout";
    return std::nullopt;
  }
  std::string content(static_cast<std::size_t>(bytes), '\0');
  input.read(content.data(), static_cast<std::streamsize>(bytes));
  if (!input || std::cmp_not_equal(input.gcount(), bytes)) {
    error = "failed while reading source rollout " + path.string();
    return std::nullopt;
  }
  if (compressed) {
    auto decoded = decompress_zstd_frames(content, kMaxHistoryBytes);
    if (!decoded.error.empty()) {
      error = "cannot decompress source rollout: " + decoded.error;
      return std::nullopt;
    }
    content = std::move(decoded.text);
  }
  if (base.end_byte_offset > content.size()) {
    error = "history_base byte offset is past the decoded source rollout";
    return std::nullopt;
  }
  content.resize(static_cast<std::size_t>(base.end_byte_offset));
  if (content.empty() || content.back() != '\n') {
    error = "history_base byte offset is not a JSONL record boundary";
    return std::nullopt;
  }
  return content;
}

bool valid_prefix(std::string_view content, const HistoryHeader &header,
                  const HistoryBase &base, std::string &error) {
  if (!header.error.empty() || !header.paginated || !header.ordinal) {
    error =
        header.error.empty() ? "source rollout is not paginated" : header.error;
    return false;
  }
  const auto expected_start = header.base ? header.base->end_ordinal : 0;
  if (*header.ordinal != expected_start) {
    error = "source rollout has an invalid starting ordinal";
    return false;
  }
  std::uint64_t next = expected_start;
  JsonlReader reader{content};
  JsonlLine line;
  simdjson::dom::parser parser;
  while (reader.next(line)) {
    simdjson::dom::element document;
    if (parser.parse(simdjson::padded_string{line.json}).get(document)
            != simdjson::SUCCESS
        || uint_at(document, "/ordinal") != next
        || next >= base.end_ordinal) {
      error = "history_base byte and ordinal boundaries do not match the "
              "source rollout";
      return false;
    }
    ++next;
  }
  if (next != base.end_ordinal) {
    error = "history_base ordinal is past the selected source prefix";
    return false;
  }
  return true;
}

} // namespace

SessionParseResult parse_codex_rollout_file(const std::filesystem::path &path,
                                            std::string_view content) {
  struct OwnedSource {
    std::filesystem::path path;
    std::string content;
  };
  std::vector<OwnedSource> inherited;
  auto header = read_header(content);
  // A malformed/legacy header without a history reference is handled by the
  // regular parser, without inventing an incomplete-history warning.
  std::string error = header.has_reference ? header.error : "";
  std::vector<std::filesystem::path> candidates;
  if (header.base && error.empty()) {
    candidates = find_rollouts(search_roots(path), error);
  }
  std::unordered_set<std::string> visited;
  std::error_code ec;
  visited.insert(std::filesystem::weakly_canonical(path, ec).string());
  std::uint64_t bytes = content.size();
  while (header.base && error.empty()) {
    if (inherited.size() >= kMaxHistoryDepth || bytes >= kMaxHistoryBytes) {
      error = "history_base chain exceeds the depth or size limit";
      break;
    }
    const auto base = *header.base;
    const auto source = locate_rollout(candidates, base.rollout_id, error);
    if (!source) {
      break;
    }
    const auto canonical = std::filesystem::weakly_canonical(*source, ec);
    if (ec || !visited.insert(canonical.string()).second) {
      error = "history_base chain contains a cycle or an inaccessible path";
      break;
    }
    auto prefix = read_prefix(*source, base, kMaxHistoryBytes - bytes, error);
    if (!prefix) {
      break;
    }
    header = read_header(*prefix);
    if (!valid_prefix(*prefix, header, base, error)) {
      break;
    }
    bytes += prefix->size();
    inherited.push_back({.path = *source, .content = std::move(*prefix)});
  }

  std::vector<CodexRolloutSource> sources;
  if (error.empty()) {
    for (const auto &source : std::views::reverse(inherited)) {
      sources.push_back({.content = source.content, .path = source.path});
    }
  }
  sources.push_back({.content = content, .path = path});
  auto result = parse_codex_rollout_sources(sources);
  if (!error.empty()) {
    add_diagnostic(result, DiagnosticSeverity::Warning,
                   DiagnosticCode::IncompleteStream,
                   "Codex inherited history could not be loaded: "
                       + error
                       + "; showing only "
                       + path.string());
  }
  return result;
}

} // namespace loupe::detail
