#include "lupe/search.hpp"
#include "lupe/log_message.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lupe {
namespace {
char ascii_lower(char value) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

bool equal_case_insensitive(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }

  for (std::size_t index = 0; index < left.size(); ++index) {
    if (ascii_lower(left[index]) != ascii_lower(right[index])) {
      return false;
    }
  }
  return true;
}
} // namespace

std::vector<SearchMatchRange>
find_text_matches(std::string_view text, std::string_view query) {
  std::vector<SearchMatchRange> matches;
  if (query.empty() || query.size() > text.size()) {
    return matches;
  }

  for (std::size_t index = 0; index + query.size() <= text.size();) {
    if (equal_case_insensitive(text.substr(index, query.size()), query)) {
      matches.push_back(SearchMatchRange{
          .offset = index,
          .length = query.size(),
      });
      index += query.size();
      continue;
    }
    ++index;
  }

  return matches;
}

bool message_matches(const LogMessage &message, std::string_view query) {
  if (!find_text_matches(message.role, query).empty()
      || !find_text_matches(message.content, query).empty()
      || !find_text_matches(message.timestamp, query).empty()
      || !find_text_matches(message.raw_type, query).empty()) {
    return true;
  }

  return std::ranges::any_of(
      message.annotations, [&](const std::string &annotation) {
        return !find_text_matches(annotation, query).empty();
      });
}

std::vector<std::size_t>
find_message_matches(const std::vector<LogMessage> &messages,
                     std::string_view query) {
  std::vector<std::size_t> matches;
  if (query.empty()) {
    return matches;
  }

  for (std::size_t index = 0; index < messages.size(); ++index) {
    if (message_matches(messages[index], query)) {
      matches.push_back(index);
    }
  }
  return matches;
}

std::optional<std::size_t>
find_next_match(const std::vector<std::size_t> &matches, std::size_t selected,
                SearchDirection direction, bool include_selected) {
  if (matches.empty()) {
    return std::nullopt;
  }

  if (direction == SearchDirection::Forward) {
    for (const std::size_t match : matches) {
      if (include_selected ? match >= selected : match > selected) {
        return match;
      }
    }
    return matches.front();
  }

  for (auto iterator = matches.rbegin(); iterator != matches.rend();
       ++iterator) {
    if (include_selected ? *iterator <= selected : *iterator < selected) {
      return *iterator;
    }
  }
  return matches.back();
}

std::optional<std::size_t>
match_ordinal(const std::vector<std::size_t> &matches, std::size_t selected) {
  const auto iterator = std::ranges::find(matches, selected);
  if (iterator == matches.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(std::distance(matches.begin(), iterator));
}

} // namespace lupe
