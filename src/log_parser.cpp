#include "lupe/log_parser.hpp"

#include <cstddef>
#include <ios>

#include "lupe/log_message.hpp"
#include "lupe/structured_text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <simdjson.h>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lupe {
namespace {
using simdjson::dom::array;
using simdjson::dom::element;
using simdjson::dom::element_type;

constexpr std::array<std::string_view, 8> kRolePointers{
    "/role",      "/message/role", "/payload/role", "/payload/message/role",
    "/data/role", "/record/role",  "/author/role",  "/type",
};

constexpr std::array<std::string_view, 19> kContentPointers{
    "/content",
    "/text",
    "/message/content",
    "/message/text",
    "/payload/content",
    "/payload/text",
    "/payload/message/content",
    "/payload/message/text",
    "/data/content",
    "/data/text",
    "/record/content",
    "/record/text",
    "/body/content",
    "/body/text",
    "/prompt",
    "/output",
    "/response",
    "/input",
    "/message",
};

constexpr std::array<std::string_view, 7> kTimestampPointers{
    "/timestamp", "/time", "/created_at",        "/updated_at",
    "/created",   "/ts",   "/payload/timestamp",
};

constexpr std::array<std::string_view, 7> kContainerPointers{
    "/messages", "/conversation", "/events",           "/records",
    "/items",    "/data",         "/payload/messages",
};

constexpr std::size_t kMultilineToolArgumentThreshold = 100;

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

std::string lower_copy(std::string_view text) {
  std::string lowered{text};
  std::ranges::transform(lowered, lowered.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return lowered;
}

std::string join_parts(const std::vector<std::string> &parts) {
  std::string joined;
  for (const auto &part : parts) {
    if (part.empty()) {
      continue;
    }
    if (!joined.empty()) {
      joined += '\n';
    }
    joined += part;
  }
  return joined;
}

void append_non_empty(std::vector<std::string> &parts, std::string value) {
  value = trim_copy(value);
  if (!value.empty()) {
    parts.push_back(std::move(value));
  }
}

std::string normalize_role(std::string_view raw_role) {
  std::string role = lower_copy(trim_copy(raw_role));
  if (role.empty()) {
    return "unknown";
  }

  if (role.contains("assistant")) {
    return "assistant";
  }
  if (role.contains("system") || role.contains("developer")) {
    return "system";
  }
  if (role.contains("user") || role.contains("human")) {
    return "user";
  }
  if (role.contains("tool") || role.contains("function")) {
    return "tool";
  }

  return role;
}

std::string value_to_text(element value, int depth);

struct TextField {
  std::string text;
  bool found = false;
};

std::string field_to_text(element value, std::string_view pointer, int depth) {
  element field;
  const auto error = value.at_pointer(pointer).get(field);
  if (error) {
    return {};
  }
  return value_to_text(field, depth + 1);
}

std::string argument_value_to_text(element value, int depth) {
  if (depth > 8) {
    return simdjson::to_string(value);
  }

  switch (value.type()) {
  case element_type::ARRAY:
  case element_type::OBJECT:
    return simdjson::to_string(value);
  case element_type::NULL_VALUE:
    return {};
  default:
    return value_to_text(value, depth);
  }
}

std::string
field_to_argument_text(element value, std::string_view pointer, int depth) {
  element field;
  const auto error = value.at_pointer(pointer).get(field);
  if (error) {
    return {};
  }
  return argument_value_to_text(field, depth + 1);
}

std::string
first_field_to_text(element value,
                    const std::vector<std::string_view> &pointers, int depth) {
  for (const auto pointer : pointers) {
    std::string text = trim_copy(field_to_text(value, pointer, depth));
    if (!text.empty()) {
      return text;
    }
  }
  return {};
}

std::string
first_argument_field_to_text(element value,
                             const std::vector<std::string_view> &pointers,
                             int depth) {
  for (const auto pointer : pointers) {
    std::string text = trim_copy(field_to_argument_text(value, pointer, depth));
    if (!text.empty()) {
      return text;
    }
  }
  return {};
}

template <std::size_t Size>
std::string
first_field_to_text(element value,
                    const std::array<std::string_view, Size> &pointers,
                    int depth) {
  for (const auto pointer : pointers) {
    std::string text = trim_copy(field_to_text(value, pointer, depth));
    if (!text.empty()) {
      return text;
    }
  }
  return {};
}

template <std::size_t Size>
TextField first_text_field(element value,
                           const std::array<std::string_view, Size> &pointers,
                           int depth) {
  TextField result;
  for (const auto pointer : pointers) {
    element field;
    const auto error = value.at_pointer(pointer).get(field);
    if (error) {
      continue;
    }

    result.found = true;
    std::string text = trim_copy(value_to_text(field, depth + 1));
    if (!text.empty()) {
      result.text = std::move(text);
      return result;
    }
  }
  return result;
}

std::string array_to_text(array values, int depth) {
  std::vector<std::string> parts;
  for (const auto item : values) {
    std::string text = trim_copy(value_to_text(item, depth + 1));
    if (!text.empty()) {
      parts.push_back(std::move(text));
    }
  }
  return join_parts(parts);
}

std::string object_to_text(element value, int depth) {
  static const std::vector<std::string_view> nested_content_pointers{
      "/text",        "/content",         "/input_text",
      "/output_text", "/message/content", "/message/text",
      "/value",
  };

  std::string text = first_field_to_text(value, nested_content_pointers, depth);
  if (!text.empty()) {
    return text;
  }

  return simdjson::to_string(value);
}

std::string value_to_text(element value, int depth) {
  if (depth > 8) {
    return simdjson::to_string(value);
  }

  switch (value.type()) {
  case element_type::STRING: {
    std::string_view text;
    if (!value.get_string().get(text)) {
      return std::string{text};
    }
    return {};
  }
  case element_type::ARRAY: {
    array values;
    if (!value.get_array().get(values)) {
      return array_to_text(values, depth);
    }
    return {};
  }
  case element_type::OBJECT:
    return object_to_text(value, depth);
  case element_type::NULL_VALUE:
    return {};
  case element_type::INT64:
  case element_type::UINT64:
  case element_type::DOUBLE:
  case element_type::BOOL:
  case element_type::BIGINT:
    return simdjson::to_string(value);
  }

  return {};
}

bool has_known_role(element value) {
  const std::string role =
      normalize_role(first_field_to_text(value, kRolePointers, 0));
  return role == "assistant"
      || role == "system"
      || role == "user"
      || role == "tool";
}

void
append_element(element value, ParseResult &result, std::size_t source_line);

bool append_container_if_present(element value, ParseResult &result,
                                 std::size_t source_line) {
  const std::size_t before = result.messages.size();
  for (const auto pointer : kContainerPointers) {
    element field;
    const auto error = value.at_pointer(pointer).get(field);
    if (error || field.type() != element_type::ARRAY) {
      continue;
    }
    append_element(field, result, source_line);
    if (result.messages.size() > before) {
      return true;
    }
  }
  return false;
}

std::string call_name(element value) {
  static const std::vector<std::string_view> pointers{
      "/function/name",
      "/function",
      "/name",
      "/tool_name",
  };

  return first_field_to_text(value, pointers, 0);
}

std::string call_arguments(element value) {
  static const std::vector<std::string_view> pointers{
      "/args",
      "/arguments",
      "/function/arguments",
      "/parameters",
  };

  return first_argument_field_to_text(value, pointers, 0);
}

std::string display_call_arguments(std::string_view args) {
  std::string trimmed = trim_copy(args);
  if (trimmed.size() <= kMultilineToolArgumentThreshold) {
    return trimmed;
  }
  return format_structured_text(trimmed);
}

std::string call_id(element value) {
  static const std::vector<std::string_view> pointers{
      "/id",
      "/tool_call_id",
      "/call_id",
  };

  return first_field_to_text(value, pointers, 0);
}

std::string format_tool_call(element value, std::string_view prefix) {
  const std::string name = call_name(value);
  const std::string args = call_arguments(value);
  const std::string id = call_id(value);

  std::string formatted{prefix};
  if (!name.empty()) {
    formatted += " ";
    formatted += name;
  } else {
    formatted += " ";
    formatted += simdjson::to_string(value);
  }

  if (!args.empty() && args != "null") {
    formatted += " ";
    formatted += display_call_arguments(args);
  }
  if (!id.empty()) {
    formatted += " [";
    formatted += id;
    formatted += "]";
  }
  return formatted;
}

std::vector<std::string> tool_annotations(element value) {
  std::vector<std::string> annotations;

  element calls_field;
  if (!value.at_pointer("/tool_calls").get(calls_field)
      && calls_field.type() == element_type::ARRAY) {
    array calls;
    if (!calls_field.get_array().get(calls)) {
      for (const auto call : calls) {
        append_non_empty(annotations, format_tool_call(call, "call"));
      }
    }
  }

  element call_field;
  bool has_tool_call = false;
  if (!value.at_pointer("/tool_call").get(call_field)
      && call_field.type() == element_type::OBJECT) {
    append_non_empty(annotations, format_tool_call(call_field, "result for"));
    has_tool_call = true;
  }

  if (!has_tool_call) {
    append_non_empty(annotations,
                     first_field_to_text(value, {"/tool_call_id"}, 0));
  }

  element error_field;
  if (!value.at_pointer("/error").get(error_field)
      && error_field.type() != element_type::NULL_VALUE) {
    append_non_empty(annotations, "error " + value_to_text(error_field, 0));
  }

  return annotations;
}

LogMessage message_from_object(element value, std::size_t source_line) {
  const std::string raw_role = first_field_to_text(value, kRolePointers, 0);
  const std::string raw_type = field_to_text(value, "/type", 0);
  const TextField content = first_text_field(value, kContentPointers, 0);
  LogMessage message{
      .role = normalize_role(raw_role),
      .content = content.text,
      .annotations = tool_annotations(value),
      .timestamp = first_field_to_text(value, kTimestampPointers, 0),
      .raw_type = trim_copy(raw_type),
      .source_line = source_line,
  };

  if (message.content.empty() && !content.found) {
    message.content = simdjson::to_string(value);
  }

  if (message.raw_type.empty() && message.role != normalize_role(raw_role)) {
    message.raw_type = trim_copy(raw_role);
  }

  return message;
}

void
append_element(element value, ParseResult &result, std::size_t source_line) {
  if (value.type() == element_type::ARRAY) {
    array values;
    if (value.get_array().get(values)) {
      return;
    }

    for (const auto item : values) {
      append_element(item, result, source_line);
    }
    return;
  }

  if (value.type() == element_type::OBJECT) {
    if (!has_known_role(value)
        && append_container_if_present(value, result, source_line)) {
      return;
    }
    result.messages.push_back(message_from_object(value, source_line));
    return;
  }

  const std::string content = trim_copy(value_to_text(value, 0));
  if (!content.empty()) {
    result.messages.push_back(LogMessage{.content = content,
                                         .annotations = {},
                                         .timestamp = {},
                                         .raw_type = {},
                                         .source_line = source_line});
  }
}

bool parse_json_document(std::string_view content, ParseResult &result,
                         std::size_t source_line, std::string *error_message) {
  simdjson::dom::parser parser;
  const simdjson::padded_string json{content};
  element root;
  const auto error = parser.parse(json).get(root);
  if (error) {
    if (error_message != nullptr) {
      *error_message = simdjson::error_message(error);
    }
    return false;
  }

  append_element(root, result, source_line);
  return true;
}

ParseResult parse_json_lines(std::string_view content) {
  ParseResult result;
  std::size_t line_number = 1;
  std::size_t line_begin = 0;

  while (line_begin <= content.size()) {
    const std::size_t line_end = content.find('\n', line_begin);
    const bool at_last_line = line_end == std::string_view::npos;
    const std::string_view line =
        content.substr(line_begin, at_last_line ? std::string_view::npos
                                                : line_end - line_begin);

    const std::string trimmed = trim_copy(line);
    if (!trimmed.empty()) {
      std::string error_message;
      if (!parse_json_document(trimmed, result, line_number, &error_message)) {
        result.errors.push_back(
            "line " + std::to_string(line_number) + ": " + error_message);
      }
    }

    if (at_last_line) {
      break;
    }
    line_begin = line_end + 1;
    ++line_number;
  }

  return result;
}
} // namespace

ParseResult parse_log_content(std::string_view content) {
  ParseResult result;
  const std::string trimmed = trim_copy(content);
  if (trimmed.empty()) {
    result.errors.emplace_back("input is empty");
    return result;
  }

  std::string document_error;
  if (parse_json_document(trimmed, result, 0, &document_error)) {
    return result;
  }

  result = parse_json_lines(content);
  if (!result.messages.empty()) {
    return result;
  }

  if (result.errors.empty()) {
    result.errors.push_back(document_error);
  }
  return result;
}

ParseResult parse_log_file(const std::filesystem::path &path) {
  const std::ifstream input{path, std::ios::binary};
  if (!input) {
    return ParseResult{
        .messages = {},
        .errors = {"failed to open " + path.string()},
    };
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  ParseResult result = parse_log_content(buffer.str());
  return result;
}
} // namespace lupe
