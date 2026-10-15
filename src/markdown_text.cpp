#include "lupe/markdown_text.hpp"

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lupe {
namespace {
struct InlineStyle {
  bool bold{false};
  bool italic{false};
  bool code{false};
  std::string link_url;
};

bool is_space(char value) {
  return std::isspace(static_cast<unsigned char>(value)) != 0;
}

bool is_digit(char value) {
  return std::isdigit(static_cast<unsigned char>(value)) != 0;
}

bool is_blank(std::string_view text) {
  for (const char value : text) {
    if (!is_space(value)) {
      return false;
    }
  }
  return true;
}

std::string normalize_newlines(std::string_view text) {
  std::string out;
  out.reserve(text.size());

  for (std::size_t index = 0; index < text.size(); ++index) {
    const char value = text[index];
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

std::vector<std::string_view> split_lines(std::string_view text) {
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      lines.push_back(text.substr(start));
      break;
    }
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

std::string trim_copy(std::string_view text) {
  std::size_t first = 0;
  while (first < text.size() && is_space(text[first])) {
    ++first;
  }

  std::size_t last = text.size();
  while (last > first && is_space(text[last - 1])) {
    --last;
  }

  return std::string{text.substr(first, last - first)};
}

std::string_view trim_right(std::string_view text) {
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

std::string_view
strip_leading_spaces(std::string_view text, std::size_t max_spaces) {
  std::size_t removed = 0;
  while (
      removed < max_spaces && removed < text.size() && text[removed] == ' ') {
    ++removed;
  }
  return text.substr(removed);
}

bool starts_with(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size()
      && text.substr(0, prefix.size()) == prefix;
}

bool same_style(const MarkdownSpan &span, const InlineStyle &style) {
  return span.bold == style.bold
      && span.italic == style.italic
      && span.code == style.code
      && span.link_url == style.link_url;
}

void append_span(std::vector<MarkdownSpan> &spans, std::string_view text,
                 const InlineStyle &style) {
  if (text.empty()) {
    return;
  }

  if (!spans.empty() && same_style(spans.back(), style)) {
    spans.back().text += text;
    return;
  }

  spans.push_back(MarkdownSpan{
      .text = std::string{text},
      .bold = style.bold,
      .italic = style.italic,
      .code = style.code,
      .link_url = style.link_url,
  });
}

std::size_t count_run(std::string_view text, std::size_t index, char value) {
  std::size_t count = 0;
  while (index + count < text.size() && text[index + count] == value) {
    ++count;
  }
  return count;
}

std::size_t
find_unescaped(std::string_view text, std::size_t from, char value) {
  bool escaped = false;
  for (std::size_t index = from; index < text.size(); ++index) {
    if (escaped) {
      escaped = false;
      continue;
    }
    if (text[index] == '\\') {
      escaped = true;
      continue;
    }
    if (text[index] == value) {
      return index;
    }
  }
  return std::string_view::npos;
}

std::size_t find_backtick_run(std::string_view text, std::size_t from,
                              std::size_t run_length) {
  for (std::size_t index = from; index < text.size(); ++index) {
    if (text[index] != '`') {
      continue;
    }
    if (count_run(text, index, '`') == run_length) {
      return index;
    }
  }
  return std::string_view::npos;
}

std::size_t find_strong_close(std::string_view text, std::size_t from) {
  bool escaped = false;
  for (std::size_t index = from; index + 1 < text.size(); ++index) {
    if (escaped) {
      escaped = false;
      continue;
    }
    if (text[index] == '\\') {
      escaped = true;
      continue;
    }
    if (starts_with(text.substr(index), "**")) {
      return index;
    }
  }
  return std::string_view::npos;
}

std::size_t find_emphasis_close(std::string_view text, std::size_t from) {
  bool escaped = false;
  for (std::size_t index = from; index < text.size(); ++index) {
    if (escaped) {
      escaped = false;
      continue;
    }
    if (text[index] == '\\') {
      escaped = true;
      continue;
    }
    if (text[index] == '*'
        && (index + 1 >= text.size() || text[index + 1] != '*')) {
      return index;
    }
  }
  return std::string_view::npos;
}

void parse_inline_into(std::string_view text, InlineStyle style,
                       std::vector<MarkdownSpan> &spans, int depth);

void append_link_spans(std::string_view label, std::string_view url,
                       InlineStyle style, std::vector<MarkdownSpan> &spans,
                       int depth) {
  style.link_url = std::string{url};
  parse_inline_into(label, style, spans, depth + 1);
}

void parse_inline_into(std::string_view text, InlineStyle style,
                       std::vector<MarkdownSpan> &spans, int depth) {
  if (depth > 12) {
    append_span(spans, text, style);
    return;
  }

  std::string plain;
  auto flush_plain = [&] {
    append_span(spans, plain, style);
    plain.clear();
  };

  for (std::size_t index = 0; index < text.size();) {
    const char value = text[index];

    if (value == '\\' && index + 1 < text.size()) {
      plain += text[index + 1];
      index += 2;
      continue;
    }

    if (value == '`') {
      const std::size_t run_length = count_run(text, index, '`');
      const std::size_t close =
          find_backtick_run(text, index + run_length, run_length);
      if (close != std::string_view::npos) {
        flush_plain();
        InlineStyle code_style = style;
        code_style.code = true;
        append_span(spans,
                    text.substr(index + run_length, close - index - run_length),
                    code_style);
        index = close + run_length;
        continue;
      }
    }

    if (starts_with(text.substr(index), "**")) {
      const std::size_t close = find_strong_close(text, index + 2);
      if (close != std::string_view::npos && close > index + 2) {
        flush_plain();
        InlineStyle bold_style = style;
        bold_style.bold = true;
        parse_inline_into(text.substr(index + 2, close - index - 2), bold_style,
                          spans, depth + 1);
        index = close + 2;
        continue;
      }
    }

    if (value == '*' && (index + 1 >= text.size() || text[index + 1] != '*')) {
      const std::size_t close = find_emphasis_close(text, index + 1);
      if (close != std::string_view::npos && close > index + 1) {
        flush_plain();
        InlineStyle italic_style = style;
        italic_style.italic = true;
        parse_inline_into(text.substr(index + 1, close - index - 1),
                          italic_style, spans, depth + 1);
        index = close + 1;
        continue;
      }
    }

    if (value == '[') {
      const std::size_t label_end = find_unescaped(text, index + 1, ']');
      if (label_end != std::string_view::npos
          && label_end + 1 < text.size()
          && text[label_end + 1] == '(') {
        const std::size_t url_end = find_unescaped(text, label_end + 2, ')');
        if (url_end != std::string_view::npos) {
          flush_plain();
          append_link_spans(text.substr(index + 1, label_end - index - 1),
                            text.substr(label_end + 2, url_end - label_end - 2),
                            style, spans, depth);
          index = url_end + 1;
          continue;
        }
      }
    }

    plain += value;
    ++index;
  }

  flush_plain();
}

std::vector<MarkdownSpan> parse_inline(std::string_view text) {
  std::vector<MarkdownSpan> spans;
  parse_inline_into(text, InlineStyle{}, spans, 0);
  return spans;
}

bool parse_fence_start(std::string_view line, char &fence_char,
                       std::size_t &fence_length) {
  line = strip_leading_spaces(line, 3);
  if (line.empty() || (line.front() != '`' && line.front() != '~')) {
    return false;
  }

  fence_char = line.front();
  fence_length = count_run(line, 0, fence_char);
  return fence_length >= 3;
}

bool parse_fence_end(std::string_view line, char fence_char,
                     std::size_t fence_length) {
  line = strip_leading_spaces(line, 3);
  if (line.empty() || line.front() != fence_char) {
    return false;
  }

  const std::size_t run_length = count_run(line, 0, fence_char);
  if (run_length < fence_length) {
    return false;
  }

  return is_blank(line.substr(run_length));
}

bool parse_heading(std::string_view line, MarkdownBlock &block) {
  line = strip_leading_spaces(trim_right(line), 3);
  const std::size_t level = count_run(line, 0, '#');
  if (level == 0 || level > 6) {
    return false;
  }
  if (line.size() > level && !is_space(line[level])) {
    return false;
  }

  const std::string content = trim_copy(line.substr(level));
  block.kind = MarkdownBlockKind::Heading;
  block.level = static_cast<int>(level);
  block.spans = parse_inline(content);
  return true;
}

bool parse_quote(std::string_view line, MarkdownBlock &block) {
  line = strip_leading_spaces(trim_right(line), 3);
  if (line.empty() || line.front() != '>') {
    return false;
  }

  line.remove_prefix(1);
  if (!line.empty() && line.front() == ' ') {
    line.remove_prefix(1);
  }

  block.kind = MarkdownBlockKind::Quote;
  block.spans = parse_inline(line);
  return true;
}

bool parse_list_item(std::string_view line, MarkdownBlock &block) {
  line = trim_right(line);

  std::size_t indent = 0;
  while (indent < line.size() && line[indent] == ' ') {
    ++indent;
  }

  std::string_view content = line.substr(indent);
  if (content.size() >= 2
      && (content[0] == '-' || content[0] == '*' || content[0] == '+')
      && is_space(content[1])) {
    content.remove_prefix(2);
    while (!content.empty() && content.front() == ' ') {
      content.remove_prefix(1);
    }
    block.kind = MarkdownBlockKind::ListItem;
    block.level = static_cast<int>(indent / 2);
    block.marker = "-";
    block.spans = parse_inline(content);
    return true;
  }

  std::size_t digits = 0;
  while (digits < content.size() && is_digit(content[digits])) {
    ++digits;
  }
  if (digits > 0
      && digits < content.size()
      && (content[digits] == '.' || content[digits] == ')')
      && digits + 1 < content.size()
      && is_space(content[digits + 1])) {
    block.kind = MarkdownBlockKind::ListItem;
    block.level = static_cast<int>(indent / 2);
    block.marker = std::string{content.substr(0, digits + 1)};
    content.remove_prefix(digits + 2);
    while (!content.empty() && content.front() == ' ') {
      content.remove_prefix(1);
    }
    block.spans = parse_inline(content);
    return true;
  }

  return false;
}

MarkdownBlock parse_paragraph_line(std::string_view line) {
  MarkdownBlock block;
  block.kind = MarkdownBlockKind::Paragraph;
  block.spans = parse_inline(trim_right(line));
  return block;
}

} // namespace

std::vector<MarkdownBlock> parse_markdown_text(std::string_view text) {
  const std::string normalized = normalize_newlines(text);
  const std::vector<std::string_view> lines = split_lines(normalized);
  std::vector<MarkdownBlock> blocks;

  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::string_view line = lines[index];

    char fence_char = '\0';
    std::size_t fence_length = 0;
    if (parse_fence_start(line, fence_char, fence_length)) {
      MarkdownBlock block;
      block.kind = MarkdownBlockKind::CodeBlock;

      ++index;
      while (index < lines.size()
             && !parse_fence_end(lines[index], fence_char, fence_length)) {
        if (!block.code.empty()) {
          block.code += '\n';
        }
        block.code += lines[index];
        ++index;
      }

      blocks.push_back(std::move(block));
      continue;
    }

    if (is_blank(line)) {
      blocks.push_back(MarkdownBlock{.kind = MarkdownBlockKind::Blank});
      continue;
    }

    MarkdownBlock block;
    if (parse_heading(line, block)
        || parse_quote(line, block)
        || parse_list_item(line, block)) {
      blocks.push_back(std::move(block));
      continue;
    }

    blocks.push_back(parse_paragraph_line(line));
  }

  return blocks;
}

} // namespace lupe
