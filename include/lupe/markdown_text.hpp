#ifndef PROJECT_MARKDOWN_TEXT_HPP_
#define PROJECT_MARKDOWN_TEXT_HPP_

#include <string>
#include <string_view>
#include <vector>

namespace lupe {

enum class MarkdownBlockKind {
  Paragraph,
  Heading,
  ListItem,
  Quote,
  CodeBlock,
  Blank,
};

struct MarkdownSpan {
  std::string text;
  bool bold{false};
  bool italic{false};
  bool code{false};
  std::string link_url;
};

struct MarkdownBlock {
  MarkdownBlockKind kind{MarkdownBlockKind::Paragraph};
  int level{0};
  std::string marker{};
  std::string code{};
  std::vector<MarkdownSpan> spans{};
};

std::vector<MarkdownBlock> parse_markdown_text(std::string_view text);

} // namespace lupe

#endif // PROJECT_MARKDOWN_TEXT_HPP_
