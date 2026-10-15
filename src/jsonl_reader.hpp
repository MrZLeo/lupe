#ifndef LUPE_JSONL_READER_HPP_
#define LUPE_JSONL_READER_HPP_

#include <cstddef>
#include <string_view>

namespace lupe::detail {

struct JsonlLine {
  std::size_t sequence{0};
  std::size_t source_line{0};
  std::string_view raw;
  std::string_view json;
};

class JsonlReader {
public:
  explicit JsonlReader(std::string_view content);

  bool next(JsonlLine &line);

private:
  std::string_view content_;
  std::size_t offset_{0};
  std::size_t source_line_{1};
  std::size_t sequence_{0};
  bool finished_{false};
};

std::string_view trim_json_whitespace(std::string_view value);

} // namespace lupe::detail

#endif // LUPE_JSONL_READER_HPP_
