#include "lupe/structured_text.hpp"

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace lupe {
namespace {
std::string trim_copy(std::string_view text) {
  auto is_space = [](unsigned char value) { return std::isspace(value) != 0; };

  std::size_t first = 0;
  while (first < text.size()
         && is_space(static_cast<unsigned char>(text[first]))) {
    ++first;
  }

  std::size_t last = text.size();
  while (last > first && is_space(static_cast<unsigned char>(text[last - 1]))) {
    --last;
  }

  return std::string{text.substr(first, last - first)};
}

bool is_opening(char value) { return value == '{' || value == '['; }

bool is_closing(char value) { return value == '}' || value == ']'; }

bool likely_structured(std::string_view text) {
  if (text.size() < 2
      || !is_opening(text.front())
      || !is_closing(text.back())) {
    return false;
  }

  bool in_string = false;
  char quote = '\0';
  bool escaped = false;
  for (const char value : text) {
    if (escaped) {
      escaped = false;
      continue;
    }
    if (in_string) {
      if (value == '\\') {
        escaped = true;
      } else if (value == quote) {
        in_string = false;
      }
      continue;
    }
    if (value == '\'' || value == '"') {
      in_string = true;
      quote = value;
      continue;
    }
    if (value == ':' || value == ',') {
      return true;
    }
  }
  return false;
}

std::size_t next_non_space(std::string_view text, std::size_t index) {
  while (index < text.size()
         && std::isspace(static_cast<unsigned char>(text[index])) != 0) {
    ++index;
  }
  return index;
}

void append_indent(std::string &out, int indent) {
  for (int index = 0; index < indent; ++index) {
    out += "  ";
  }
}

void append_newline(std::string &out, int indent) {
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  out += '\n';
  append_indent(out, indent);
}

std::string normalize_physical_newlines(std::string_view text) {
  std::string out;
  out.reserve(text.size());

  for (std::size_t index = 0; index < text.size(); ++index) {
    const char value = text[index];
    if (value == '\\' && index + 1 < text.size()) {
      const char next = text[index + 1];
      if (next == '\n') {
        out += '\n';
        ++index;
        continue;
      }
      if (next == '\r') {
        out += '\n';
        index += index + 2 < text.size() && text[index + 2] == '\n' ? 2 : 1;
        continue;
      }
    }

    if (value == '\r') {
      out += '\n';
      if (index + 1 < text.size() && text[index + 1] == '\n') {
        ++index;
      }
      continue;
    }

    out += value;
  }

  return out;
}

std::string normalize_text_newlines(std::string_view text) {
  std::string out;
  out.reserve(text.size());

  for (std::size_t index = 0; index < text.size(); ++index) {
    const char value = text[index];
    if (value == '\\' && index + 1 < text.size() && text[index + 1] == 'n') {
      out += '\n';
      ++index;
      continue;
    }
    out += value;
  }

  return out;
}
} // namespace

std::string format_structured_text(std::string_view text) {
  const std::string normalized = normalize_physical_newlines(text);
  const std::string trimmed = trim_copy(normalized);
  if (!likely_structured(trimmed)) {
    return normalize_text_newlines(normalized);
  }

  std::string out;
  out.reserve(trimmed.size() + trimmed.size() / 4);

  int indent = 0;
  bool in_string = false;
  char quote = '\0';
  bool escaped = false;

  for (std::size_t index = 0; index < trimmed.size(); ++index) {
    const char value = trimmed[index];

    if (escaped) {
      out += value;
      escaped = false;
      continue;
    }

    if (in_string) {
      if (value == '\n') {
        out += '\n';
        append_indent(out, indent + 1);
        continue;
      }
      if (value == '\\'
          && index + 1 < trimmed.size()
          && trimmed[index + 1] == 'n') {
        out += '\n';
        append_indent(out, indent + 1);
        ++index;
        continue;
      }
      out += value;
      if (value == '\\') {
        escaped = true;
      } else if (value == quote) {
        in_string = false;
      }
      continue;
    }

    if (value == '\'' || value == '"') {
      in_string = true;
      quote = value;
      out += value;
      continue;
    }

    if (std::isspace(static_cast<unsigned char>(value)) != 0) {
      continue;
    }

    if (is_opening(value)) {
      out += value;
      ++indent;
      const std::size_t next = next_non_space(trimmed, index + 1);
      if (next < trimmed.size() && !is_closing(trimmed[next])) {
        append_newline(out, indent);
      }
      continue;
    }

    if (is_closing(value)) {
      --indent;
      if (!out.empty() && out.back() != '\n') {
        append_newline(out, indent);
      } else {
        append_indent(out, indent);
      }
      out += value;
      continue;
    }

    if (value == ',') {
      out += value;
      append_newline(out, indent);
      continue;
    }

    if (value == ':') {
      out += ": ";
      continue;
    }

    out += value;
  }

  return out;
}
} // namespace lupe
