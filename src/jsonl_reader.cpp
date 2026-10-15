#include "jsonl_reader.hpp"

#include <cctype>
#include <cstddef>
#include <string_view>

namespace lupe::detail {

std::string_view trim_json_whitespace(std::string_view value) {
  auto is_space = [](char character) {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
  };

  while (!value.empty() && is_space(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_space(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

JsonlReader::JsonlReader(std::string_view content) : content_(content) {}

bool JsonlReader::next(JsonlLine &line) {
  while (!finished_) {
    const std::size_t line_end = content_.find('\n', offset_);
    const bool last_line = line_end == std::string_view::npos;
    const std::size_t length =
        last_line ? content_.size() - offset_ : line_end - offset_;
    std::string_view raw = content_.substr(offset_, length);
    const std::size_t current_line = source_line_;

    if (last_line) {
      finished_ = true;
      offset_ = content_.size();
    } else {
      offset_ = line_end + 1;
      ++source_line_;
    }

    if (!raw.empty() && raw.back() == '\r') {
      raw.remove_suffix(1);
    }

    std::string_view json = trim_json_whitespace(raw);
    if (current_line == 1
        && json.size() >= 3
        && static_cast<unsigned char>(json[0]) == 0xEF
        && static_cast<unsigned char>(json[1]) == 0xBB
        && static_cast<unsigned char>(json[2]) == 0xBF) {
      json.remove_prefix(3);
      json = trim_json_whitespace(json);
    }

    if (json.empty()) {
      continue;
    }

    line = JsonlLine{
        .sequence = sequence_++,
        .source_line = current_line,
        .raw = raw,
        .json = json,
    };
    return true;
  }

  return false;
}

} // namespace lupe::detail
