#ifndef PROJECT_SEARCH_HPP_
#define PROJECT_SEARCH_HPP_

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "lupe/log_message.hpp"

namespace lupe {

enum class SearchDirection {
  Forward,
  Backward,
};

struct SearchMatchRange {
  std::size_t offset{0};
  std::size_t length{0};

  bool operator==(const SearchMatchRange &) const = default;
};

std::vector<SearchMatchRange>
find_text_matches(std::string_view text, std::string_view query);

bool message_matches(const LogMessage &message, std::string_view query);

std::vector<std::size_t>
find_message_matches(const std::vector<LogMessage> &messages,
                     std::string_view query);

std::optional<std::size_t>
find_next_match(const std::vector<std::size_t> &matches, std::size_t selected,
                SearchDirection direction, bool include_selected);

std::optional<std::size_t>
match_ordinal(const std::vector<std::size_t> &matches, std::size_t selected);

} // namespace lupe

#endif // PROJECT_SEARCH_HPP_
